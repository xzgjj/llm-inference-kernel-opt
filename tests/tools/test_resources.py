"""resources.ResourcesSampler：start/stop 必须各留一次同步采样，峰值不得因线程调度变 0。"""

from __future__ import annotations

import subprocess
import sys
import time
import unittest

from tools.bench.resources import ResourceSampler


class ResourceSamplerTest(unittest.TestCase):
    def spawn_child(self) -> subprocess.Popen[bytes]:
        return subprocess.Popen([sys.executable, "-c", "import time; time.sleep(3)"])

    def test_immediate_stop_still_has_two_samples(self) -> None:
        """回归：并行 ctest 下采样线程没被调度，stop 曾返回 samples=0 / rss=0，
        让 integration_server_matrix 的 RSS 断言随机变红。"""
        child = self.spawn_child()
        try:
            sampler = ResourceSampler(child.pid, interval_s=0.5)
            sampler.start()
            peak = sampler.stop()
        finally:
            child.terminate()
            child.wait(timeout=5)
        self.assertGreaterEqual(peak.samples, 2)
        self.assertGreater(peak.rss_peak_bytes, 0)

    def test_peak_survives_child_exit(self) -> None:
        """进程退出后的终采读到 None，也不能把已有峰值抹成 0。"""
        child = self.spawn_child()
        sampler = ResourceSampler(child.pid, interval_s=0.2)
        sampler.start()
        time.sleep(0.3)
        child.terminate()
        child.wait(timeout=5)
        peak = sampler.stop()
        self.assertGreater(peak.rss_peak_bytes, 0)
        self.assertGreaterEqual(peak.samples, 2)

    def test_interval_is_clamped(self) -> None:
        sampler = ResourceSampler(1, interval_s=0.0)
        self.assertEqual(sampler.interval_s, 0.2)


if __name__ == "__main__":
    unittest.main()
