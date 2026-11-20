#!/usr/bin/env python3
"""peak_sampler.py — 按 pid 采进程级 RSS 与本进程 VRAM，落 CSV（4-G0-3 / K-8）。

只信可归因到该进程的数字：VRAM 走 nvidia-smi 的 compute-apps 列表按 pid 匹配，
采不到写空并在 vram_known=0 标记，不拿整卡占用冒充。

用法：
    python scripts/bench/peak_sampler.py --pid 1234 --duration 30 --interval 0.5 \
        --out bench/results/peak_samples.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import time
from pathlib import Path

MIB = 1024 * 1024


def read_rss_mib(pid: int) -> float | None:
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes

        class ProcessMemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        handle = kernel32.OpenProcess(0x1000 | 0x0400, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            return None
        try:
            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb
            ):
                return None
            return counters.WorkingSetSize / MIB
        finally:
            kernel32.CloseHandle(handle)
    try:
        with open(f"/proc/{pid}/statm", "r", encoding="ascii") as handle:
            _, resident = handle.read().split()[:2]
        return int(resident) * os.sysconf("SC_PAGE_SIZE") / MIB
    except (OSError, ValueError):
        return None


def read_vram_mib(pid: int) -> float | None:
    try:
        result = subprocess.run(
            [
                "nvidia-smi",
                "--query-compute-apps=pid,used_memory",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    for line in result.stdout.splitlines():
        parts = [part.strip() for part in line.split(",")]
        if len(parts) != 2:
            continue
        try:
            row_pid, used = int(parts[0]), float(parts[1])
        except ValueError:
            continue
        if row_pid == pid and used > 0:
            return used
    return None


def sample(pid: int) -> dict[str, object]:
    rss = read_rss_mib(pid)
    vram = read_vram_mib(pid)
    return {
        "ts_ms": int(time.time() * 1000),
        "pid": pid,
        "rss_mib": "" if rss is None else round(rss, 3),
        "vram_mib": "" if vram is None else round(vram, 3),
        "vram_known": 0 if vram is None else 1,
    }


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
        rows.append(sample(args.pid))
        if time.monotonic() >= deadline:
            break
        time.sleep(args.interval)

    with out.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
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
