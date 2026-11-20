"""peak_sampler CLI：真 pid 采样、CSV 列契约、峰值不止损、未知进程返回非零。"""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "bench" / "peak_sampler.py"


class PeakSamplerTest(unittest.TestCase):
    def test_samples_sleeping_process(self) -> None:
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(2)"])
        try:
            with tempfile.TemporaryDirectory() as tmp:
                out = Path(tmp) / "peak_samples.csv"
                result = subprocess.run(
                    [sys.executable, str(SCRIPT), "--pid", str(child.pid),
                     "--duration", "0.6", "--interval", "0.1", "--out", str(out)],
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                with out.open(encoding="utf-8") as handle:
                    rows = list(csv.DictReader(handle))
                self.assertGreaterEqual(len(rows), 2)
                self.assertEqual(
                    list(rows[0].keys()), ["ts_ms", "pid", "rss_mib", "vram_mib", "vram_known"]
                )
                rss = [float(row["rss_mib"]) for row in rows if row["rss_mib"] != ""]
                self.assertTrue(rss, "RSS 必须有值")
                self.assertTrue(all(value > 0.0 for value in rss))
                self.assertTrue(all(row["vram_known"] in {"0", "1"} for row in rows))
                for row in rows:
                    if row["vram_known"] == "1":
                        self.assertGreater(float(row["vram_mib"]), 0.0)
                    else:
                        self.assertEqual(row["vram_mib"], "")
                self.assertIn("rss_peak_mib=", result.stdout)
        finally:
            child.terminate()
            child.wait(timeout=5)

    def test_missing_pid_returns_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "peak_samples.csv"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--pid", "999999999",
                 "--duration", "0.1", "--interval", "0.1", "--out", str(out)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(out.exists())


if __name__ == "__main__":
    unittest.main()
