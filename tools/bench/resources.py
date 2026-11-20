"""资源采样：SUT 进程 RSS 峰值与显存占用，1s 一次，退出时取峰值。"""

from __future__ import annotations

import subprocess
import threading
import time
from dataclasses import dataclass


@dataclass
class ResourcePeak:
    rss_peak_bytes: int = 0
    vram_peak_bytes: int = 0
    samples: int = 0


def _read_rss_bytes(pid: int) -> int:
    """Linux /proc/<pid>/status 的 VmHWM（峰值 RSS），单位 kB。"""
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        return 0


def _read_vram_bytes() -> int:
    """nvidia-smi 当前显存占用；无 GPU 时返回 0，不抛异常。"""
    try:
        proc = subprocess.run(
            [
                "nvidia-smi",
                "--query-gpu=memory.used",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        first = (proc.stdout or "").strip().splitlines()
        return int(first[0].strip()) * 1024 * 1024 if first else 0
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
        return 0


class ResourceSampler:
    """后台线程采样；stop() 后返回峰值。"""

    def __init__(self, pid: int, interval_s: float = 1.0) -> None:
        self.pid = pid
        self.interval_s = max(0.2, interval_s)
        self.peak = ResourcePeak()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> ResourcePeak:
        self._stop.set()
        self._thread.join(timeout=self.interval_s * 2)
        return self.peak

    def _loop(self) -> None:
        while not self._stop.is_set():
            rss = _read_rss_bytes(self.pid)
            vram = _read_vram_bytes()
            self.peak.rss_peak_bytes = max(self.peak.rss_peak_bytes, rss)
            self.peak.vram_peak_bytes = max(self.peak.vram_peak_bytes, vram)
            self.peak.samples += 1
            self._stop.wait(self.interval_s)


def sample_once(pid: int) -> ResourcePeak:
    """单次采样，供每格结束时的快速读数。"""
    return ResourcePeak(rss_peak_bytes=_read_rss_bytes(pid), vram_peak_bytes=_read_vram_bytes(), samples=1)
