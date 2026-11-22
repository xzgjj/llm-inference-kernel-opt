"""性能回归闸门（D2-13）：拿当前矩阵 CSV 与入库基线比较，劣化 >5% 即失败。

为什么用离线 CSV 而不是现场压测：CI 机器没有 GPU、也没有固定模型，现场压测的
数字本身不可比；矩阵 CSV 由固定 prompt 集/固定轮次/固定二进制采集，才是可复现基线。
基线文件 `ci/baseline/perf-baseline.json` 记录 P4 混长档的 `ttft_p50_ms` 与 `total_tok_s`。
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

THRESHOLD = 0.05  # 5% 劣化即失败（SPEC-D2-13 写死）


def load_baseline(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def read_row(csv_path: Path, concurrency: str) -> dict:
    with csv_path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            if row.get("concurrency_p") == concurrency and row.get("status") == "ok":
                return row
    raise KeyError(f"{csv_path} 中找不到 P={concurrency} 的 ok 行")


def compare(baseline: dict, row: dict) -> list[str]:
    """返回劣化项；空列表即通过。吞吐越低越差，TTFT 越高越差。"""
    problems: list[str] = []
    base_tok = float(baseline["total_tok_s"])
    got_tok = float(row["total_tok_s"])
    if got_tok < base_tok * (1.0 - THRESHOLD):
        problems.append(f"吞吐 {got_tok:.1f} < 基线 {base_tok:.1f} 的 95%")
    base_ttft = float(baseline["ttft_p50_ms"])
    got_ttft = float(row["ttft_p50_ms"])
    if got_ttft > base_ttft * (1.0 + THRESHOLD):
        problems.append(f"TTFT p50 {got_ttft:.1f} > 基线 {base_ttft:.1f} 的 105%")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="D2-13 性能回归闸门")
    parser.add_argument("--baseline", type=Path, default=Path("ci/baseline/perf-baseline.json"))
    parser.add_argument("--csv", type=Path, default=Path("evidence/phase13/perf-mixed/matrix_report.csv"))
    parser.add_argument("--concurrency", default="4")
    args = parser.parse_args(argv)
    if not args.baseline.is_file():
        print(f"基线文件不存在: {args.baseline}", file=sys.stderr)
        return 3
    if not args.csv.is_file():
        print(f"矩阵 CSV 不存在: {args.csv}", file=sys.stderr)
        return 3
    try:
        row = read_row(args.csv, args.concurrency)
    except KeyError as exc:
        print(str(exc), file=sys.stderr)
        return 3
    problems = compare(load_baseline(args.baseline), row)
    if problems:
        for item in problems:
            print(f"[FAIL] {item}")
        return 1
    print(f"[PASS] P={args.concurrency} 未劣化超过 {THRESHOLD:.0%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
