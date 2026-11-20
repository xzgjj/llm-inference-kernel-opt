#!/usr/bin/env python3
"""peak_sampler.py — 按 pid 采进程级 RSS 与本进程 VRAM，落 CSV（4-G0-3 / K-8）。

采样实现复用 tools/bench/peak.py：VRAM 只认 nvidia-smi compute-apps 里 pid
匹配的行，采不到写空并在 vram_known=0 标记，不拿整卡占用冒充。

用法：
    python scripts/bench/peak_sampler.py --pid 1234 --duration 30 --interval 0.5 \
        --out bench/results/peak_samples.csv
"""
from __future__ import annotations

import argparse
import csv
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.bench import peak  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="sample RSS/VRAM of one pid")
    parser.add_argument("--pid", type=int, required=True, help="target process id")
    parser.add_argument("--duration", type=float, default=30.0, help="seconds to sample")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between samples")
    parser.add_argument("--out", required=True, help="CSV output path")
    args = parser.parse_args(argv)
    if args.duration <= 0 or args.interval <= 0:
        parser.error("duration/interval must be positive")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, object]] = []
    deadline = time.monotonic() + args.duration
    while True:
        row = peak.sample_row(args.pid)
        row["ts_ms"] = int(time.time() * 1000)
        rows.append(row)
        if time.monotonic() >= deadline:
            break
        time.sleep(args.interval)

    fieldnames = ["ts_ms", "pid", "rss_mib", "vram_mib", "vram_known"]
    with out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    rss_values = [float(row["rss_mib"]) for row in rows if row["rss_mib"] != ""]
    vram_values = [float(row["vram_mib"]) for row in rows if row["vram_mib"] != ""]
    print(
        "samples={} rss_peak_mib={} vram_peak_mib={} vram_known={}/{}".format(
            len(rows),
            round(max(rss_values), 3) if rss_values else "unknown",
            round(max(vram_values), 3) if vram_values else "unknown",
            len(vram_values),
            len(rows),
        )
    )
    return 0 if rss_values else 2


if __name__ == "__main__":
    raise SystemExit(main())
