#!/usr/bin/env python3
"""cmp_mode.py — 3-G7 协作/自主模式判据采样。

协作模式（当前实现）：同一长 prompt 连续两次请求，第二次应命中满块前缀，
记录冷/热 TTFT 与命中 token；自主模式需要 patch llama.cpp 的 cell 选择入口，
本仓库未实现，只按源码耦合点填“待定补丁面”，不编造 TTFT。

输出 mode_compare.csv，列语义见 docs/SPEC-3 §1.7。
"""
from __future__ import annotations

import argparse
import csv
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_cb import metrics_snapshot, stream_once  # noqa: E402

BASE = (
    "连续批处理把多个请求的 prefill 与 decode 组合到同一步。"
    "调度器必须在组批之前决定 token 预算、块需求与抢占候选，"
    "执行器只负责把不可变批次写入 KV 并回报采样结果。"
)


def request_once(port: int, prompt: str, max_tokens: int) -> dict:
    result: dict = {}
    errors: list[Exception] = []
    stream_once(port, prompt, max_tokens, result, errors)
    if errors:
        raise RuntimeError(f"请求失败: {errors}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--repeat", type=int, default=10)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--max-tokens", type=int, default=16)
    parser.add_argument("--output", required=True)
    parser.add_argument("--patch-files", type=int, default=4)
    parser.add_argument("--patch-loc-min", type=int, default=120)
    parser.add_argument("--maintenance-hours", type=float, default=8.0)
    args = parser.parse_args()

    prompt = BASE * args.repeat
    cold: list[float] = []
    warm: list[float] = []
    hit_tokens = 0
    hits = 0
    requests = 0
    for _ in range(args.rounds):
        before = metrics_snapshot(args.port)
        cold_result = request_once(args.port, prompt, args.max_tokens)
        after_cold = metrics_snapshot(args.port)
        warm_result = request_once(args.port, prompt, args.max_tokens)
        after_warm = metrics_snapshot(args.port)
        cold.append(float(cold_result["ttft_ms"]))
        warm.append(float(warm_result["ttft_ms"]))
        hit_tokens += int(after_warm.get("llmopt_prefix_hit_tokens_total", 0) -
                          after_cold.get("llmopt_prefix_hit_tokens_total", 0))
        hits += int(after_warm.get("llmopt_prefix_hits_total", 0) -
                   before.get("llmopt_prefix_hits_total", 0))
        requests += 2
        if after_cold.get("llmopt_prefix_hits_total", 0) < before.get(
                "llmopt_prefix_hits_total", 0):
            raise RuntimeError("prefix 指标回退")

    cold_median = statistics.median(cold)
    warm_median = statistics.median(warm)
    gain = cold_median - warm_median
    gain_pct = gain / cold_median * 100.0 if cold_median else 0.0

    rows = [
        {
            "mode": "collaboration",
            "evidence": "measured",
            "requests": requests,
            "prefix_hits": hits,
            "prefix_hit_rate": round(hits / requests, 3) if requests else 0.0,
            "prefix_hit_tokens": hit_tokens,
            "ttft_cold_ms": round(cold_median, 2),
            "ttft_warm_ms": round(warm_median, 2),
            "ttft_gain_ms": round(gain, 2),
            "ttft_gain_pct": round(gain_pct, 2),
            "patch_files": 0,
            "patch_loc_min": 0,
            "maintenance_hours": 0,
            "decision": "维持",
        },
        {
            "mode": "autonomous",
            "evidence": "not_implemented",
            "requests": "",
            "prefix_hits": "",
            "prefix_hit_rate": "",
            "prefix_hit_tokens": "",
            "ttft_cold_ms": "",
            "ttft_warm_ms": "",
            "ttft_gain_ms": "",
            "ttft_gain_pct": "",
            "patch_files": args.patch_files,
            "patch_loc_min": args.patch_loc_min,
            "maintenance_hours": args.maintenance_hours,
            "decision": "不立项",
        },
    ]
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
