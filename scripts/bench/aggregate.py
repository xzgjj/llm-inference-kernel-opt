#!/usr/bin/env python3
"""aggregate.py — 阶段 4 归档聚合与检查（4-G2 / 4-G3 / N-07）。

用法：
    python scripts/bench/aggregate.py --dir bench/results/phase4/<run>
    python scripts/bench/aggregate.py --dir ... --check-deviation
    python scripts/bench/aggregate.py --dir ... --check-metrics
    python scripts/bench/aggregate.py --offline <归档目录>
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.bench import report, plot  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="阶段 4 聚合与检查")
    parser.add_argument("--dir", default=None, help="一次矩阵运行的归档目录")
    parser.add_argument("--offline", default=None, help="仅凭归档重建 CSV/PNG")
    parser.add_argument("--check-deviation", action="store_true")
    parser.add_argument("--check-metrics", action="store_true")
    parser.add_argument("--plot", action="store_true", help="生成 PNG 图表")
    args = parser.parse_args(argv)
    directory = Path(args.offline or args.dir or "")
    if not str(directory) or not directory.is_dir():
        parser.error("--dir/--offline 必须指向存在的归档目录")

    code = 0
    if args.offline or not (args.check_deviation or args.check_metrics):
        code |= report.write_offline(directory)
    if args.check_deviation:
        code |= report.check_deviation(directory / "deviation.csv")
    if args.check_metrics:
        code |= report.check_metrics(directory)
    if args.offline or args.plot:
        code |= plot.plot_directory(directory)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
