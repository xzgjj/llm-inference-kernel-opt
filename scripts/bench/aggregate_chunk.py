#!/usr/bin/env python3
"""aggregate_chunk.py — 3-G2 四档 chunk 两轮取中。

读取 bench_chunk.sh 产生的 chunk_<size>_r<round>.csv，每档按列取两轮中位数
（偶数轮取靠中间两值的均值），写出 chunk_tradeoff.csv 供报告引用。不推测缺失值。
"""
from __future__ import annotations

import argparse
import csv
import statistics
from pathlib import Path

COLUMNS = [
    "ttft_p50_ms",
    "ttft_p95_ms",
    "itl_p50_ms",
    "itl_p99_ms",
    "tokens_per_s",
    "kv_util_median_pct",
    "batch_size_peak",
    "running_peak",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    raw_dir = Path(args.raw_dir)
    rows = []
    for chunk in (32, 64, 128, 256):
        files = sorted(raw_dir.glob(f"chunk_{chunk}_r*.csv"))
        if len(files) < 2:
            raise SystemExit(f"chunk {chunk} 不足两轮: {len(files)}")
        samples = []
        for path in files:
            with path.open(encoding="utf-8", newline="") as handle:
                samples.extend(list(csv.DictReader(handle)))
        if not samples:
            raise SystemExit(f"chunk {chunk} 无样本")
        row = {
            "chunk": chunk,
            "rounds": len(files),
            "requests_per_round": samples[0]["requests"],
        }
        for column in COLUMNS:
            values = [float(sample[column]) for sample in samples]
            row[column] = round(statistics.median(values), 2)
        rows.append(row)

    with Path(args.output).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
