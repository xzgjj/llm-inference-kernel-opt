"""聚合：JSONL 原始记录 → baseline.csv 与两轮偏差表。

分位数口径与 numpy.percentile 默认一致（线性插值，n≥30 才出数）。
"""

from __future__ import annotations

import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

BASELINE_FIELDS = [
    "run_id",
    "concurrency_p",
    "tier",
    "round",
    "n_requests",
    "n_errors",
    "ttft_p50_ms",
    "ttft_p95_ms",
    "ttft_p99_ms",
    "itl_p50_ms",
    "itl_p99_ms",
    "tg_agg_tok_s",
    "pp_tok_s",
    "total_tok_s",
    "single_decode_tok_s",
    "note",
]

DEVIATION_FIELDS = [
    "run_id",
    "concurrency_p",
    "tier",
    "pp_dev_pct",
    "tg_dev_pct",
    "ttft_p50_dev_pct",
    "verdict",
]

MIN_SAMPLES = 30
PP_LIMIT_PCT = 2.0
TG_LIMIT_PCT = 7.0


@dataclass
class CellStats:
    """一个 (run_side, concurrency, tier, round) 格的聚合结果。"""

    concurrency: int
    tier: str
    round: int
    n_requests: int
    n_errors: int
    ttft_p50_ms: float | None
    ttft_p95_ms: float | None
    ttft_p99_ms: float | None
    itl_p50_ms: float | None
    itl_p99_ms: float | None
    tg_agg_tok_s: float | None
    pp_tok_s: float | None
    total_tok_s: float | None
    single_decode_tok_s: float | None
    note: str
    time_enqueue: float = 0.0
    time_end: float = 0.0


def percentile(values: list[float], p: float) -> float:
    """线性插值分位数；空列表抛 ValueError，避免悄悄返回 0。"""
    if not values:
        raise ValueError("percentile 需要非空样本")
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * p
    low = int(rank)
    high = min(low + 1, len(ordered) - 1)
    weight = rank - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def _cell(records: list[dict], concurrency: int, tier: str, round_no: int) -> CellStats:
    valid = [
        r
        for r in records
        if r.get("concurrency") == concurrency
        and r.get("tier") == tier
        and r.get("round") == round_no
        and not r.get("dropped")
        and r.get("status") == "ok"
        and r.get("ttft_ms") is not None
    ]
    errors = [
        r
        for r in records
        if r.get("concurrency") == concurrency
        and r.get("tier") == tier
        and r.get("round") == round_no
        and not r.get("dropped")
        and r.get("status") != "ok"
    ]
    notes: list[str] = []
    if len(valid) < MIN_SAMPLES:
        notes.append("insufficient_samples")
    if errors:
        notes.append("n_errors")
    if not valid:
        return CellStats(concurrency, tier, round_no, 0, len(errors), None, None, None,
                         None, None, None, None, None, None, ";".join(notes))

    ttft = [float(r["ttft_ms"]) for r in valid]
    itl: list[float] = []
    for r in valid:
        itl.extend(float(x) for x in r.get("itl_ms", []))
    t_enqueue = min(float(r["t_enqueue_ms"]) for r in valid)
    t_end = max(float(r["t_end_ms"]) for r in valid)
    wall_s = max((t_end - t_enqueue) / 1000.0, 1e-9)
    sum_prompt = sum(int(r.get("n_prompt", 0)) for r in valid)
    sum_gen = sum(int(r.get("n_gen", 0)) for r in valid)
    tg = sum_gen / wall_s
    pp = sum_prompt / wall_s
    total = (sum_prompt + sum_gen) / wall_s
    return CellStats(
        concurrency=concurrency,
        tier=tier,
        round=round_no,
        n_requests=len(valid),
        n_errors=len(errors),
        ttft_p50_ms=percentile(ttft, 0.50),
        ttft_p95_ms=percentile(ttft, 0.95),
        ttft_p99_ms=percentile(ttft, 0.99),
        itl_p50_ms=percentile(itl, 0.50) if itl else None,
        itl_p99_ms=percentile(itl, 0.99) if itl else None,
        tg_agg_tok_s=tg,
        pp_tok_s=pp,
        total_tok_s=total,
        single_decode_tok_s=tg / concurrency if concurrency else None,
        note=";".join(notes),
        time_enqueue=t_enqueue,
        time_end=t_end,
    )


def aggregate(
    records: Iterable[dict],
    run_id: str,
    concurrencies: list[int],
    tiers: list[str],
    rounds: int,
) -> tuple[list[dict], list[dict]]:
    """返回 (baseline 行, 两轮偏差行)；只读入参，不改原始记录。"""
    all_records = list(records)
    baseline: list[dict] = []
    for concurrency in concurrencies:
        for tier in tiers:
            for round_no in range(1, rounds + 1):
                cell = _cell(all_records, concurrency, tier, round_no)
                baseline.append(_baseline_row(run_id, cell))
    # 3 轮起首轮视为冷启动，用最后两轮判偏差；2 轮仍按 1 vs 2。
    deviations = []
    if rounds >= 2:
        deviations = _deviation_rows(run_id, baseline, max(1, rounds - 1), rounds)
    return baseline, deviations


def _baseline_row(run_id: str, cell: CellStats) -> dict:
    return {
        "run_id": run_id,
        "concurrency_p": cell.concurrency,
        "tier": cell.tier,
        "round": cell.round,
        "n_requests": cell.n_requests,
        "n_errors": cell.n_errors,
        "ttft_p50_ms": _fmt(cell.ttft_p50_ms),
        "ttft_p95_ms": _fmt(cell.ttft_p95_ms),
        "ttft_p99_ms": _fmt(cell.ttft_p99_ms),
        "itl_p50_ms": _fmt(cell.itl_p50_ms),
        "itl_p99_ms": _fmt(cell.itl_p99_ms),
        "tg_agg_tok_s": _fmt(cell.tg_agg_tok_s),
        "pp_tok_s": _fmt(cell.pp_tok_s),
        "total_tok_s": _fmt(cell.total_tok_s),
        "single_decode_tok_s": _fmt(cell.single_decode_tok_s),
        "note": cell.note or "ok",
    }


def _deviation_rows(run_id: str, baseline: list[dict], first_round: int = 1,
                    second_round: int = 2) -> list[dict]:
    """逐格比较指定两轮；rounds≥3 时取最后两轮，避开首轮冷启动。"""
    index = {(r["concurrency_p"], r["tier"], r["round"]): r for r in baseline}
    rows: list[dict] = []
    keys = {(r["concurrency_p"], r["tier"]) for r in baseline}
    for concurrency, tier in sorted(keys):
        first = index.get((concurrency, tier, first_round))
        second = index.get((concurrency, tier, second_round))
        if not first or not second:
            continue
        pp_dev = _pct_dev(first["pp_tok_s"], second["pp_tok_s"])
        tg_dev = _pct_dev(first["tg_agg_tok_s"], second["tg_agg_tok_s"])
        ttft_dev = _pct_dev(first["ttft_p50_ms"], second["ttft_p50_ms"])
        verdict = "pass"
        if pp_dev is None or tg_dev is None:
            verdict = "insufficient"
        elif pp_dev >= PP_LIMIT_PCT or tg_dev > TG_LIMIT_PCT:
            verdict = "exceeded"
        rows.append(
            {
                "run_id": run_id,
                "concurrency_p": concurrency,
                "tier": tier,
                "pp_dev_pct": _fmt(pp_dev),
                "tg_dev_pct": _fmt(tg_dev),
                "ttft_p50_dev_pct": _fmt(ttft_dev),
                "verdict": verdict,
            }
        )
    return rows


def _pct_dev(a, b) -> float | None:
    if a in (None, "") or b in (None, ""):
        return None
    base = float(a)
    if base == 0.0:
        return None
    return abs(float(b) - base) / base * 100.0


def _fmt(value: float | None) -> str | float:
    return "" if value is None else round(float(value), 3)


def write_csv(path: Path, rows: list[dict], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
