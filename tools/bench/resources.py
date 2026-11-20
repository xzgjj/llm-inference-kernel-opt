"""资源采样：按 pid 归因的 RSS 峰值与本进程 VRAM，1s 一次，退出时取峰值。"""

from __future__ import annotations

import threading
from dataclasses import dataclass

from . import peak

MIB = 1024 * 1024


@dataclass
class ResourcePeak:
    rss_peak_bytes: int = 0
    vram_peak_bytes: int = 0
    samples: int = 0
    vram_known: bool = False


def _read_rss_bytes(pid: int) -> int:
    value = peak.read_rss_mib(pid)
    return int(value * MIB) if value is not None else 0


def _read_vram_bytes(pid: int) -> int:
    value = peak.read_vram_mib(pid)
    return int(value * MIB) if value is not None else 0


class ResourceSampler:
    """后台线程采样；stop() 后返回峰值。VRAM 按 pid，不采整卡占用。"""

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
            vram = _read_vram_bytes(self.pid)
            self.peak.rss_peak_bytes = max(self.peak.rss_peak_bytes, rss)
            if vram > 0:
                self.peak.vram_peak_bytes = max(self.peak.vram_peak_bytes, vram)
                self.peak.vram_known = True
            self.peak.samples += 1
            self._stop.wait(self.interval_s)


def sample_once(pid: int) -> ResourcePeak:
    """单次采样，供每格结束时的快速读数。"""
    vram = _read_vram_bytes(pid)
    return ResourcePeak(
        rss_peak_bytes=_read_rss_bytes(pid),
        vram_peak_bytes=vram,
        samples=1,
        vram_known=vram > 0,
    )
