"""SPEC-5 判据复算：只读 SPEC-4 矩阵 CSV 与 step trace，不触发任何实现路径。

输入：`matrix_report.csv`（十二列，见 SPEC-4 §1.6）与同目录 `steps-p<N>.jsonl`。
输出：`criteria.csv`、`criteria.md`，可选 `phase5_预研评估.md`。
退出码：0 计算完成（含“数据不足/不触发”）、1 输入不可用、2 用法错误。
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

REQUIRED_COLUMNS = (
    "concurrency_p",
    "prompt_tier",
    "status",
    "n_requests",
    "itl_p99_ms",
    "total_tok_s",
)
REPORT_FIELDS = (
    "concurrency_p",
    "prompt_tier",
    "status",
    "n_requests",
    "itl_p99_ms",
    "itl_over",
    "prefill_share",
    "prefill_share_source",
    "chunk_gain_pct",
    "pd_verdict",
    "pd_reason",
    "spec_gain_pct",
    "spec_itl_delta_pct",
    "spec_verdict",
    "spec_reason",
    "row_verdict",
    "reason",
)
CRITERIA_SCOPE = (1, 4, 8)
VERDICT_ORDER = (
    "pd_triggered",
    "spec_triggered",
    "negative_gain",
    "candidate_pending_chunk",
    "candidate_pending_migration",
    "batch_decay",
    "not_triggered",
    "data_insufficient",
)


class DataError(RuntimeError):
    """输入文件不可用：缺列、不可解析或路径不存在。"""


@dataclass(frozen=True)
class Thresholds:
    slo_itl_ms: float = 50.0
    share: float = 0.50
    chunk_gain: float = 0.10
    spec_gain: float = 0.10
    spec_itl_tolerance: float = 0.07
    min_samples: int = 30


@dataclass(frozen=True)
class Migration:
    kv_kib_per_token: float
    ctx_tokens: int
    link_gibps: float
    route_ms: float
    delta_stall_ms: float | None
    overlap_ms: float


def percentile(values: Iterable[float], percent: float) -> float:
    """线性插值分位数，与 `tools/bench/aggregate.py` 口径一致。"""
    ordered = sorted(float(v) for v in values)
    if not ordered:
        return 0.0
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * percent / 100.0
    lower = math.floor(rank)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (rank - lower)


def parse_number(text: Any) -> float | None:
    """空串、nan、inf 一律视为缺失，避免把无意义数值带进判据。"""
    if text is None:
        return None
    try:
        value = float(str(text).strip())
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def read_csv_rows(path: Path, required: tuple[str, ...]) -> list[dict[str, str]]:
    """读 CSV 并校验必需列；缺列直接判输入不可用，不做猜测性填充。"""
    if not path.exists():
        raise DataError(f"文件不存在：{path}")
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        header = tuple(reader.fieldnames or ())
        missing = [name for name in required if name not in header]
        if missing:
            raise DataError(f"{path} 缺少必需列：{','.join(missing)}")
        return [dict(row) for row in reader if any(str(value or "").strip() for value in row.values())]


def _int_field(record: dict[str, Any], name: str) -> int:
    """JSONL 字段容错：坏值按 0 处理，不让一条脏样本毁掉整档证据。"""
    try:
        return int(record.get(name, 0) or 0)
    except (TypeError, ValueError):
        return 0


def load_trace(path: Path) -> dict[str, Any] | None:
    """按 step 间隔估算 ITL p99 与“长间隔中含 prefill 的批次占比”。

    JSONL 每行一个调度 step（ts_ns/prefill_tokens/decode_tokens）。
    间隔取“上一步→当前步”的时间差，仅统计当前步有 decode 的样本；
    prefill 占比是 §1.4 “前一步批次含新准入 prefill”的批次级代理。
    """
    if not path.exists():
        return None
    steps: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue  # 进程被强杀时末行可能截断：丢弃半行，不伪造样本
            if isinstance(record, dict) and "ts_ns" in record:
                steps.append(record)
    intervals: list[float] = []
    mixed: list[bool] = []
    for index in range(1, len(steps)):
        current, previous = steps[index], steps[index - 1]
        if _int_field(current, "decode_tokens") <= 0:
            continue
        try:
            delta = (float(current["ts_ns"]) - float(previous["ts_ns"])) / 1e6
        except (TypeError, ValueError, KeyError):
            continue
        if delta <= 0:
            continue
        intervals.append(delta)
        mixed.append(_int_field(current, "prefill_tokens") > 0)
    if len(intervals) < 2:
        return None
    bound = percentile(intervals, 99)
    long_indices = [i for i, value in enumerate(intervals) if value >= bound]
    share = sum(1 for i in long_indices if mixed[i]) / len(long_indices) if long_indices else 0.0
    return {
        "decode_steps": len(intervals),
        "itl_p99_ms": round(bound, 3),
        "long_steps": len(long_indices),
        "prefill_long_steps": sum(1 for i in long_indices if mixed[i]),
        "share": share,
    }


def chunk_gains(rows: list[dict[str, str]], baseline: list[dict[str, str]],
                min_samples: int = 30) -> dict[tuple[int, str], float]:
    """调参后矩阵相对基线的 ITL 改善比例：只接受同 P、同档、同口径的十二列报告。"""
    base_map: dict[tuple[int, str], float] = {}
    for row in baseline:
        value = parse_number(row.get("itl_p99_ms"))
        try:
            key = (int(row["concurrency_p"]), str(row["prompt_tier"]))
        except (KeyError, ValueError):
            continue
        if value and value > 0:
            base_map[key] = value
    gains: dict[tuple[int, str], float] = {}
    for row in rows:
        value = parse_number(row.get("itl_p99_ms"))
        samples = parse_number(row.get("n_requests"))
        try:
            key = (int(row["concurrency_p"]), str(row["prompt_tier"]))
        except (KeyError, ValueError):
            continue
        before = base_map.get(key)
        if (row.get("status") or "").strip() != "ok":
            continue
        if samples is None or samples < min_samples:
            continue
        if value is None or not before:
            continue
        gains[key] = (before - value) / before
    return gains


def load_spec_rows(path: Path | None) -> dict[tuple[int, str], dict[str, float | None]]:
    """投机行：`draft_tok_s` 是唯一可作为翻案依据的实测值，alpha/k/c 只用于估算展示。"""
    if path is None:
        return {}
    rows = read_csv_rows(path, ("concurrency_p", "prompt_tier"))
    result: dict[tuple[int, str], dict[str, float | None]] = {}
    for row in rows:
        try:
            key = (int(row["concurrency_p"]), str(row["prompt_tier"]))
        except (KeyError, ValueError):
            continue
        result[key] = {
            "baseline_tok_s": parse_number(row.get("baseline_tok_s")),
            "draft_tok_s": parse_number(row.get("draft_tok_s")),
            "alpha": parse_number(row.get("alpha")),
            "k": parse_number(row.get("k")),
            "c": parse_number(row.get("cost_ratio")),
            "itl_delta_pct": parse_number(row.get("itl_p99_delta_pct")),
        }
    return result


def migration_estimate(migration: Migration) -> dict[str, Any] | None:
    """迁移代价：t_xfer = B_kv / B_eff；无实测链路带宽或 Δstall 时不出结论。"""
    if migration.link_gibps <= 0 or migration.delta_stall_ms is None:
        return None
    kv_kib = migration.kv_kib_per_token * migration.ctx_tokens
    if kv_kib <= 0:
        return None
    t_xfer_ms = kv_kib * 1000.0 / (migration.link_gibps * 1024.0 * 1024.0)
    budget_ms = migration.delta_stall_ms - migration.overlap_ms
    worth = (t_xfer_ms + migration.route_ms) < budget_ms
    return {
        "kv_mib": kv_kib / 1024.0,
        "t_xfer_ms": t_xfer_ms,
        "budget_ms": budget_ms,
        "worth": worth,
    }


def speculative_speedup(alpha: float | None, k: float | None, cost: float | None) -> float | None:
    """E[a] = Σ α^i；估算加速比 ≈ E[a] / (1 + c·k)，仅展示，不作翻案依据。"""
    if alpha is None or k is None or cost is None:
        return None
    if not 0.0 <= alpha <= 1.0 or k < 1 or cost < 0:
        return None
    expected = sum(alpha ** i for i in range(1, int(k) + 1))
    return expected / (1.0 + cost * k)


def _pct(value: float | None) -> str:
    return "" if value is None else f"{value * 100.0:.6f}"


def _num(value: float | None, digits: int = 3) -> str:
    return "" if value is None else f"{value:.{digits}f}"


def evaluate_row(
    row: dict[str, str],
    thresholds: Thresholds,
    traces: dict[int, dict[str, Any]],
    gains: dict[tuple[int, str], float],
    specs: dict[tuple[int, str], dict[str, float | None]],
    migration: dict[str, Any] | None,
) -> dict[str, str]:
    """按 SPEC-5 §1.4 计算一行：PD 三条件、投机两条件与数据缺口。"""
    status = (row.get("status") or "").strip() or "unknown"
    try:
        concurrency = int(row["concurrency_p"])
    except (KeyError, ValueError):
        concurrency = -1
    tier = (row.get("prompt_tier") or "").strip()
    n_requests = parse_number(row.get("n_requests"))
    itl = parse_number(row.get("itl_p99_ms"))
    out = {field: "" for field in REPORT_FIELDS}
    out.update({
        "concurrency_p": str(concurrency),
        "prompt_tier": tier,
        "status": status,
        "n_requests": "" if n_requests is None else str(int(n_requests)),
        "itl_p99_ms": _num(itl),
    })
    reasons: list[str] = []

    # 先判输入是否可判：非 ok 档与样本不足直接出 "数据不足"，不估算收益。
    if status != "ok":
        out.update(pd_verdict="data_insufficient", pd_reason=f"status={status}",
                   spec_verdict="data_insufficient", spec_reason=f"status={status}",
                   row_verdict="data_insufficient", reason=f"status={status}")
        return out
    if n_requests is None or int(n_requests) < thresholds.min_samples:
        reason = f"n={'' if n_requests is None else int(n_requests)}<{thresholds.min_samples}"
        out.update(pd_verdict="data_insufficient", pd_reason=reason,
                   spec_verdict="data_insufficient", spec_reason=reason,
                   row_verdict="data_insufficient", reason=reason)
        return out
    if itl is None:
        out.update(pd_verdict="data_insufficient", pd_reason="itl_p99_ms 缺失",
                   spec_verdict="data_insufficient", spec_reason="itl_p99_ms 缺失",
                   row_verdict="data_insufficient", reason="itl_p99_ms 缺失")
        return out

    itl_over = itl > thresholds.slo_itl_ms
    out["itl_over"] = "1" if itl_over else "0"
    trace = traces.get(concurrency)
    if trace is None:
        out["prefill_share_source"] = "missing"
    else:
        out["prefill_share"] = f"{trace['share']:.6f}"
        out["prefill_share_source"] = f"batch_trace_p{concurrency}"

    pd_verdict, pd_reason = "not_triggered", "itl_within_slo"
    if itl_over:
        if trace is None:
            pd_verdict, pd_reason = "data_insufficient", "trace_missing"
        elif trace["share"] <= thresholds.share:
            pd_verdict, pd_reason = "not_triggered", "share_low"
        else:
            gain = gains.get((concurrency, tier))
            if gain is None:
                pd_verdict, pd_reason = "candidate_pending_chunk", "chunk_gain_missing"
            elif gain >= thresholds.chunk_gain:
                out["chunk_gain_pct"] = _pct(gain)
                pd_verdict, pd_reason = "not_triggered", "chunk_tuning_suffices"
            else:
                out["chunk_gain_pct"] = _pct(gain)
                if migration is None:
                    pd_verdict, pd_reason = "candidate_pending_migration", "delta_stall_unmeasured"
                elif not migration["worth"]:
                    pd_verdict, pd_reason = "negative_gain", "transfer_exceeds_stall"
                else:
                    pd_verdict, pd_reason = "pd_triggered", "three_conditions_met"
    out.update(pd_verdict=pd_verdict, pd_reason=pd_reason)

    spec = specs.get((concurrency, tier))
    spec_verdict, spec_reason = "data_insufficient", "no_draft_measurement"
    if spec is not None:
        # 优先用对照行自带的基线吞吐；缺失时回落到同一矩阵行，避免两套基线。
        base = spec.get("baseline_tok_s") or parse_number(row.get("total_tok_s"))
        draft = spec.get("draft_tok_s")
        if base and draft is not None and base > 0:
            gain = (draft - base) / base
            out["spec_gain_pct"] = _pct(gain)
            delta = spec.get("itl_delta_pct")
            if delta is None:
                delta = 0.0
            out["spec_itl_delta_pct"] = _num(delta, 3)
            if gain > thresholds.spec_gain and delta <= thresholds.spec_itl_tolerance * 100.0:
                spec_verdict, spec_reason = "spec_triggered", "gain_and_itl_ok"
            elif gain <= thresholds.spec_gain:
                spec_verdict, spec_reason = "batch_decay", "gain_below_threshold"
            else:
                spec_verdict, spec_reason = "not_triggered", "itl_degraded"
    out.update(spec_verdict=spec_verdict, spec_reason=spec_reason)

    # 行判定按“可执行结论优先”：触发/候选/回退先出；两者都不可判才记数据不足。
    actionable = {pd_verdict, spec_verdict} - {"not_triggered", "data_insufficient"}
    if actionable:
        chosen = next(v for v in VERDICT_ORDER if v in actionable)
    elif "data_insufficient" in {pd_verdict, spec_verdict}:
        chosen = "data_insufficient"
    else:
        chosen = "not_triggered"
    out["row_verdict"] = chosen
    reasons = []
    if pd_verdict not in {"not_triggered"} or spec_verdict == "data_insufficient":
        reasons.append(pd_reason)
    if spec_verdict != "not_triggered":
        reasons.append(spec_reason)
    out["reason"] = ";".join(filter(None, dict.fromkeys(reasons))) or "all_conditions_unmet"
    return out


def summarize(rows: list[dict[str, str]], thresholds: Thresholds) -> dict[str, Any]:
    """整表结论：只有真正触发才写“值得翻案”，候选项不算翻案依据。"""
    counts: dict[str, int] = {}
    for row in rows:
        counts[row["row_verdict"]] = counts.get(row["row_verdict"], 0) + 1
    fired = [r for r in rows if r["row_verdict"] in {"pd_triggered", "spec_triggered"}]
    candidates = [r for r in rows if r["row_verdict"].startswith("candidate")]
    return {
        "rows": len(rows),
        "counts": counts,
        "fired": len(fired),
        "candidates": len(candidates),
        "conclusion": "存在触发项：按 SPEC-5 §1.6 立项评估" if fired else "本期数据不支持翻案",
        "slo_itl_ms": thresholds.slo_itl_ms,
    }


def _row_cells(row: dict[str, str]) -> str:
    values = [
        row["concurrency_p"],
        row["prompt_tier"],
        row["n_requests"],
        row["itl_p99_ms"],
        row["itl_over"] or "-",
        (f"{float(row['prefill_share']):.3f}" if row["prefill_share"] else "-"),
        (f"{float(row['chunk_gain_pct']):.1f}%" if row["chunk_gain_pct"] else "-"),
        row["pd_verdict"],
        (f"{float(row['spec_gain_pct']):.1f}%" if row["spec_gain_pct"] else "-"),
        (f"{float(row['spec_itl_delta_pct']):.2f}%" if row["spec_itl_delta_pct"] else "-"),
        row["spec_verdict"],
        row["row_verdict"],
    ]
    return "| " + " | ".join(values) + " |"


def render_markdown(rows: list[dict[str, str]], summary: dict[str, Any], meta: dict[str, str],
                    thresholds: Thresholds) -> str:
    """判据表：结论、逐行判定、复算口径与数据缺口同页，便于逐行回查 CSV。"""
    lines = [
        "# 阶段 5 预研判据表",
        "",
        f"> 输入：`{meta['csv']}`｜trace：`{meta['traces']}`｜行数：{summary['rows']}",
        f"> 口径：ITL p99 > {thresholds.slo_itl_ms:g} ms，prefill 占比 > {thresholds.share:.2f}，"
        f"chunk 改善 < {thresholds.chunk_gain * 100:.0f}%；投机增益 > {thresholds.spec_gain * 100:.0f}% "
        f"且 ITL 偏差 ≤ {thresholds.spec_itl_tolerance * 100:.0f}%",
        "",
        "## 结论",
        "",
        f"**{summary['conclusion']}**",
        "",
        f"- 触发：{summary['fired']} 行；候选：{summary['candidates']} 行；"
        f"不可判：{summary['counts'].get('data_insufficient', 0)} 行。",
        "- 候选项不等于翻案：缺 chunk 同档调参或迁移对照时，只能保留候选状态。",
        "",
        "## 判据表",
        "",
        "| P | 档 | n | ITL p99 (ms) | 超 SLO | prefill 占比 | chunk 改善 | PD | 投机增益 | 投机 ITL 变化 | 投机 | 行判定 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    in_scope, extra = [], []
    for row in rows:
        (in_scope if int(row["concurrency_p"]) in CRITERIA_SCOPE else extra).append(row)
    lines.extend(_row_cells(row) for row in in_scope)
    if extra:
        lines.extend(["", "### 非主档（P 不在 1/4/8；只留档，不参与阶段门槛同口径）", "",
                      "| P | 档 | n | ITL p99 (ms) | 超 SLO | prefill 占比 | chunk 改善 | PD | 投机增益 | 投机 ITL 变化 | 投机 | 行判定 |",
                      "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"])
        lines.extend(_row_cells(row) for row in extra)
    gaps = _gap_lines(rows)
    lines.extend(["", "## 数据缺口", ""])
    lines.extend(gaps or ["- 无。"])
    lines.extend([
        "",
        "## 复算口径",
        "",
        "- ITL 与吞吐取 `matrix_report.csv` 的实测列；n<30 或 `status!=ok` 直接记“数据不足”。",
        "- prefill 占比取同并发档 `steps-p<N>.jsonl`：decode step 的到达间隔 p99 为“长间隔”，"
        "其中当前步 `prefill_tokens>0` 的批次占比即代理值。",
        "- chunk 改善必须来自同 P、同档的调参后十二列报告；缺档不估算。",
        "- 投机增益只用 `draft_tok_s` 实测；α/k/c 只进估算，不作翻案依据。",
        "- 复算命令：`python scripts/spec5/recompute_spec5.py --csv <matrix_report.csv>`。",
    ])
    return "\n".join(lines) + "\n"


def _gap_lines(rows: list[dict[str, str]]) -> list[str]:
    gaps = sorted({row["pd_reason"] for row in rows if row["pd_reason"].endswith(("_missing", "_unmeasured"))}
                  | {row["spec_reason"] for row in rows if row["spec_reason"] == "no_draft_measurement"})
    labels = {
        "trace_missing": "缺 step trace，无法计算 prefill 占比",
        "chunk_gain_missing": "缺同档 chunk 调参前后对照，PD 只能停在候选",
        "delta_stall_unmeasured": "缺混批/隔离对照，无法估算 Δstall，PD 不落结论",
        "no_draft_measurement": "无草稿模型实测吞吐，投机不可判",
    }
    return [f"- `{gap}`：{labels.get(gap, gap)}" for gap in gaps]


def render_report(rows: list[dict[str, str]], summary: dict[str, Any], meta: dict[str, str]) -> str:
    """SPEC-5 §1.6 ② 一页评估报告；未触发时结论固定为“本期数据不支持翻案”。"""
    fired_pd = [r for r in rows if r["pd_verdict"] == "pd_triggered"]
    fired_spec = [r for r in rows if r["spec_verdict"] == "spec_triggered"]
    candidates = [r for r in rows if r["row_verdict"].startswith("candidate")]
    boxes = [
        ("PD 值得翻案", bool(fired_pd)),
        ("投机值得翻案", bool(fired_spec)),
        ("本期数据不支持翻案", not fired_pd and not fired_spec),
    ]
    lines = [
        "# phase5_预研评估",
        "",
        "结论: " + "   ".join(f"{'[x]' if on else '[ ]'} {label}" for label, on in boxes),
        "",
        "数据支撑:",
    ]
    if fired_pd:
        for row in fired_pd[:4]:
            lines.append(f"- PD: P={row['concurrency_p']} {row['prompt_tier']} "
                         f"ITL p99={row['itl_p99_ms']} ms / prefill 占比={row['prefill_share']} / "
                         f"chunk 改善={row['chunk_gain_pct']}%；行见 `{Path(meta['csv']).name}`")
    else:
        pd_rows = [r for r in rows if r["itl_over"] == "1"]
        lines.append(f"- PD: 超 SLO 行 {len(pd_rows)} 条；"
                     f"候选 {summary['candidates']} 条；"
                     + ("缺 chunk 调参或 Δstall 实测，不落翻案结论。" if summary['candidates'] else
                        "prefill 占比或 chunk 改善未过阈值。"))
    if fired_spec:
        for row in fired_spec[:4]:
            lines.append(f"- 投机: P={row['concurrency_p']} {row['prompt_tier']} "
                         f"增益={row['spec_gain_pct']}% / ITL 偏差={row['spec_itl_delta_pct']}%")
    else:
        lines.append("- 投机: 无草稿模型实测吞吐，按 R-05 不以文献加速比替代，判不可判。")
    lines.extend([
        "",
        "翻案条件勾选: [{}] PD 不等式成立   [{}] 投机增益 >10% 且 p99 未恶化   [{}] 第二机/双进程成本已评估".format(
            "x" if fired_pd else " ", "x" if fired_spec else " ", " "),
        "",
        f"遗留: 未触发条目保留 SPEC-5 §1.3 接口草案；判据表见 `{meta['out']}/criteria.csv`。",
    ])
    if candidates:
        lines.append(f"候选（不构成翻案）: {len(candidates)} 行；"
                     + "、".join(f"P={r['concurrency_p']}/{r['prompt_tier']}" for r in candidates[:6]))
    return "\n".join(lines) + "\n"


def write_outputs(out_dir: Path, rows: list[dict[str, str]], summary: dict[str, Any],
                  meta: dict[str, str], thresholds: Thresholds, report_path: Path | None) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    criteria_csv = out_dir / "criteria.csv"
    with criteria_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(REPORT_FIELDS))
        writer.writeheader()
        writer.writerows(rows)
    criteria_md = out_dir / "criteria.md"
    criteria_md.write_text(render_markdown(rows, summary, meta, thresholds), encoding="utf-8")
    written = [criteria_csv, criteria_md]
    if report_path is None and summary["fired"]:
        report_path = out_dir / "phase5_预研评估.md"
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(render_report(rows, summary, meta), encoding="utf-8")
        written.append(report_path)
    return written


def load_traces(directory: Path) -> dict[int, dict[str, Any]]:
    traces: dict[int, dict[str, Any]] = {}
    if not directory.exists():
        return traces
    for path in sorted(directory.glob("steps-p*.jsonl")):
        try:
            concurrency = int(path.stem.split("-p")[1])
        except (IndexError, ValueError):
            continue
        profile = load_trace(path)
        if profile is not None:
            traces[concurrency] = profile
    return traces


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="SPEC-5 判据复算（只读，不改 v0）")
    parser.add_argument("--csv", required=True, type=Path, help="matrix_report.csv（SPEC-4 十二列）")
    parser.add_argument("--traces", type=Path, default=None, help="steps-p<N>.jsonl 所在目录")
    parser.add_argument("--chunk-csv", type=Path, default=None, help="同 P 同档的调参后矩阵报告")
    parser.add_argument("--spec-csv", type=Path, default=None, help="草稿模型实测行")
    parser.add_argument("--out", type=Path, default=None, help="输出目录（默认 CSV 同目录 spec5/）")
    parser.add_argument("--report", type=Path, default=None, help="评估报告路径（默认仅触发时写）")
    parser.add_argument("--slo-itl-ms", type=float, default=50.0, help="ITL p99 SLO（D5-1）")
    parser.add_argument("--share-threshold", type=float, default=0.50)
    parser.add_argument("--chunk-gain-threshold", type=float, default=0.10)
    parser.add_argument("--spec-gain-threshold", type=float, default=0.10)
    parser.add_argument("--spec-itl-tolerance", type=float, default=0.07)
    parser.add_argument("--min-samples", type=int, default=30)
    parser.add_argument("--kv-kib-per-token", type=float, default=12.0)
    parser.add_argument("--ctx-tokens", type=int, default=4096)
    parser.add_argument("--link-gibps", type=float, default=0.0, help="跨实例链路实测带宽；0 表示未测")
    parser.add_argument("--route-ms", type=float, default=0.0)
    parser.add_argument("--delta-stall-ms", type=float, default=None, help="混批-隔离的 ITL 差；缺省不出结论")
    parser.add_argument("--overlap-ms", type=float, default=0.0)
    parser.add_argument("--quiet", action="store_true")
    return parser


def run(args: argparse.Namespace) -> int:
    csv_path: Path = args.csv
    traces_dir = args.traces or csv_path.parent
    out_dir = args.out or (csv_path.parent / "spec5")
    try:
        baseline = read_csv_rows(csv_path, REQUIRED_COLUMNS)
        gains = chunk_gains(read_csv_rows(args.chunk_csv, REQUIRED_COLUMNS), baseline,
                            args.min_samples) if args.chunk_csv else {}
        specs = load_spec_rows(args.spec_csv)
    except DataError as exc:
        print(f"[spec5] 输入不可用：{exc}", file=sys.stderr)
        return 1

    thresholds = Thresholds(
        slo_itl_ms=args.slo_itl_ms,
        share=args.share_threshold,
        chunk_gain=args.chunk_gain_threshold,
        spec_gain=args.spec_gain_threshold,
        spec_itl_tolerance=args.spec_itl_tolerance,
        min_samples=args.min_samples,
    )
    traces = load_traces(traces_dir)
    migration = migration_estimate(Migration(
        kv_kib_per_token=args.kv_kib_per_token,
        ctx_tokens=args.ctx_tokens,
        link_gibps=args.link_gibps,
        route_ms=args.route_ms,
        delta_stall_ms=args.delta_stall_ms,
        overlap_ms=args.overlap_ms,
    ))
    rows = [evaluate_row(row, thresholds, traces, gains, specs, migration) for row in baseline]
    summary = summarize(rows, thresholds)
    meta = {"csv": str(csv_path).replace("\\", "/"),
            "traces": str(traces_dir).replace("\\", "/"),
            "out": str(out_dir).replace("\\", "/")}
    written = write_outputs(out_dir, rows, summary, meta, thresholds, args.report)
    if migration is not None and not args.quiet:
        print(f"[spec5] 迁移估算：B_kv={migration['kv_mib']:.1f} MiB，"
              f"t_xfer={migration['t_xfer_ms']:.3f} ms，worth={migration['worth']}")
    if not args.quiet:
        print(f"[spec5] {summary['conclusion']}；rows={summary['rows']} "
              f"fired={summary['fired']} candidates={summary['candidates']}")
        for path in written:
            print(f"[spec5] 写入 {path}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
