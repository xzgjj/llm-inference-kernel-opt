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
    """后台线程采样；start/stop 各同步采一次，保证 samples≥2。VRAM 按 pid，不采整卡占用。"""

    def __init__(self, pid: int, interval_s: float = 1.0) -> None:
        self.pid = pid
        self.interval_s = max(0.2, interval_s)
        self.peak = ResourcePeak()
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def start(self) -> None:
        # 同步首采：线程可能还没被调度，不能把"还没轮到采样"记成 0 样本/0 RSS。
        self._capture()
        self._thread.start()

    def stop(self) -> ResourcePeak:
        self._stop.set()
        self._thread.join(timeout=max(10.0, self.interval_s * 2))
        # 同步终采：进程此刻仍由调用方持有，必须留下一次真实读数。
        self._capture()
        return self.peak

    def _capture(self) -> None:
        rss = _read_rss_bytes(self.pid)
        vram = _read_vram_bytes(self.pid)
        with self._lock:
            if rss > 0:
                # 只把成功读到的 RSS 计入样本数：进程已退出时的 0 不能冒充一次有效采样。
                self.peak.rss_peak_bytes = max(self.peak.rss_peak_bytes, rss)
                self.peak.samples += 1
            if vram > 0:
                self.peak.vram_peak_bytes = max(self.peak.vram_peak_bytes, vram)
                self.peak.vram_known = True

    def _loop(self) -> None:
        while not self._stop.is_set():
            self._capture()
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
