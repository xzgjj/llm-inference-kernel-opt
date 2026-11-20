"""归档重建与一致性检查：从 JSONL + .prom 快照重算 CSV，不依赖在线服务（N-07）。"""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any

from . import matrix

REPORT_FIELDS = matrix.REPORT_FIELDS


def read_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line]


def read_metrics_dir(directory: Path, concurrency: list[int], repeats: int) -> dict:
    result: dict[tuple[int, int], dict[str, float | None]] = {}
    for value in concurrency:
        for round_no in range(1, repeats + 1):
            path = directory / f"metrics_p{value}_r{round_no}.prom"
            text = path.read_text(encoding="utf-8") if path.exists() else ""
            result[(value, round_no)] = {
                "kv": matrix.parse_named(text, "llmopt_kv_internal_frag_ratio"),
                "sched": matrix.parse_named(text, "llmopt_sched_cpu_ratio"),
            }
    return result


def config_from_manifest(manifest: dict) -> matrix.MatrixConfig:
    """只用清单里的并发/档位/轮次重建聚合上下文，不需要原配置。"""
    tiers = [matrix.Tier(item["name"], int(item["max_prompt_tokens"]), int(item["n"]))
             for item in manifest["tiers"]]
    return matrix.MatrixConfig(
        server=Path(str(manifest.get("server_path", "server"))),
        model=Path(str(manifest.get("model_path", "model"))),
        backend=str(manifest.get("backend", "stub")),
        concurrency=[int(value) for value in manifest["concurrency"]],
        tiers=tiers,
        repeats=int(manifest["rounds"]),
        warmup_requests=0,
        max_tokens=0,
        ctx_tokens=0,
        max_num_batched_tokens=0,
        queue_capacity=0,
        tier_timeout_ms=0,
        request_timeout_s=0.0,
        seed=int(manifest.get("seed", 0)),
        host="127.0.0.1",
        mixed=False,
        idle_between_cells_s=0.0,
        raw=manifest,
    )


def rebuild(directory: Path) -> tuple[list[dict], list[dict]]:
    manifest = json.loads((directory / "repro_manifest.json").read_text(encoding="utf-8"))
    cfg = config_from_manifest(manifest)
    records = read_jsonl(directory / "requests.jsonl")
    metrics = read_metrics_dir(directory, cfg.concurrency, cfg.repeats)
    return matrix.summarize(cfg, records, metrics)


def check_deviation(path: Path) -> int:
    with path.open(encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    high = [row for row in rows if row.get("verdict") == "high"]
    missing = [row for row in rows if row.get("verdict") not in {"ok", "high", "insufficient", "na"}]
    for row in high:
        print(f"[deviation] P={row['concurrency_p']} tier={row['prompt_tier']} 超差")
    if missing:
        print(f"[deviation] {len(missing)} 行 verdict 非法")
    print(f"[deviation] {len(rows)} 行，超差 {len(high)} 行")
    return 0 if not high and not missing else 1


def check_metrics(directory: Path) -> int:
    """对账：最后一次 /metrics 的 ok 计数与 token 总数 vs JSONL 成功请求。"""
    manifest = json.loads((directory / "repro_manifest.json").read_text(encoding="utf-8"))
    records = read_jsonl(directory / "requests.jsonl")
    failures: list[str] = []
    for concurrency in [int(value) for value in manifest["concurrency"]]:
        path = directory / f"metrics_p{concurrency}_r{int(manifest['rounds'])}.prom"
        if not path.exists():
            failures.append(f"P={concurrency} 缺 .prom 快照")
            continue
        text = path.read_text(encoding="utf-8")
        metric_ok = matrix.parse_named(text, 'llmopt_requests_total{status="ok"}')
        metric_tokens = matrix.parse_named(text, "llmopt_tokens_generated_total")
        records_ok = sum(1 for record in records
                         if record["concurrency"] == concurrency and record["status"] == "ok")
        tokens = sum(int(record.get("n_gen", 0)) for record in records
                     if record["concurrency"] == concurrency and record["status"] == "ok")
        if metric_ok is None or metric_tokens is None:
            failures.append(f"P={concurrency} 快照缺计数序列")
            continue
        for label, metric, expected in (
            ("requests_ok", metric_ok, records_ok),
            ("tokens", metric_tokens, tokens),
        ):
            delta = abs(metric - expected)
            ratio = delta / max(expected, 1)
            if ratio > 0.001:
                failures.append(f"P={concurrency} {label} 差 {delta}（{ratio:.4%} > 0.1%）")
    for item in failures:
        print(f"[metrics] {item}")
    print(f"[metrics] {'PASS' if not failures else 'FAIL'}，归档 {directory}")
    return 0 if not failures else 1


def write_offline(directory: Path) -> int:
    report, deviations = rebuild(directory)
    matrix.write_report(directory / "matrix_report_rebuilt.csv", report, REPORT_FIELDS)
    matrix.write_report(directory / "deviation_rebuilt.csv", deviations, matrix.DEVIATION_FIELDS)
    print(f"[offline] 重建 {len(report)} 行 -> matrix_report_rebuilt.csv")
    return 0


def check_columns(directory: Path) -> int:
    path = directory / "matrix_report.csv"
    if not path.exists():
        print(f"[columns] 缺 {path}")
        return 1
    with path.open(encoding="utf-8") as handle:
        header = next(csv.reader(handle))
    missing = [field for field in REPORT_FIELDS if field not in header]
    if missing:
        print(f"[columns] 缺列: {missing}")
        return 1
    print("[columns] PASS")
    return 0
