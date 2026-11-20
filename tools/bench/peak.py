"""进程级 RSS/VRAM 采样：按 pid 归因，采不到返回 None（4-G0-3 / K-8）。

Windows 用 psapi，Linux 读 /proc/<pid>/statm；VRAM 只认 nvidia-smi compute-apps
里 pid 匹配的行，不拿整卡占用冒充。
"""

from __future__ import annotations

import os
import subprocess
import sys

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
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
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


def sample_row(pid: int) -> dict[str, object]:
    """一行 CSV 记录；unknown 一律留空 + vram_known=0。"""
    rss = read_rss_mib(pid)
    vram = read_vram_mib(pid)
    return {
        "pid": pid,
        "rss_mib": "" if rss is None else round(rss, 3),
        "vram_mib": "" if vram is None else round(vram, 3),
        "vram_known": 0 if vram is None else 1,
    }
