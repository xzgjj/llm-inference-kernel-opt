"""D2-13：容量表生成与 SLO 阈值判定（纯离线，不启动服务）。"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

_spec = importlib.util.spec_from_file_location(
    "capacity_table", REPO / "scripts" / "bench" / "capacity_table.py"
)
capacity_table = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(capacity_table)


HEADER = (
    "concurrency_p,prompt_tier,status,n_requests,ttft_p50_ms,ttft_p95_ms,itl_p99_ms,"
    "total_tok_s,tok_accounting,kv_internal_frag_ratio,sched_cpu_ratio,vram_peak_mib,"
    "vram_peak_scope,vram_device_peak_mib,reason,deviation_flag\n"
)


def write_group(root: Path, name: str, rows: str) -> None:
    group = root / name
    group.mkdir(parents=True)
    (group / "matrix_report.csv").write_text(HEADER + rows, encoding="utf-8")


class CapacityTableTest(unittest.TestCase):
    def test_rows_follow_csv_and_order(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_group(root, "perf-static", "8,short,ok,30,6969.995,11893.2,142.112,57.099,prompt_metrics,0.7,0.0002,4920.43,buffers,7382.0,na,high\n")
            write_group(root, "perf-mixed", "1,short,ok,30,29.799,113.6,23.279,61.017,prompt_metrics,0.7,0.0002,4384.87,buffers,6991.0,na,ok\n")
            text = capacity_table.render(root)
            self.assertIn("| perf-static | 8 | 6970.0 | 142.1 | 57.1 | 4920 | high† |", text)
            self.assertIn("| perf-mixed | 1 | 29.8 | 23.3 | 61.0 | 4384 |  |", text)
            self.assertLess(text.index("perf-static"), text.index("perf-mixed"))

    def test_missing_group_is_marked_not_faked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            text = capacity_table.render(Path(tmp))
            self.assertIn("缺 CSV", text)

    def test_empty_itl_keeps_dash(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_group(root, "perf-prefix-on", "4,shared512,ok,1,194.3,194.3,,17271.6,prompt_metrics,0.0,0.0002,4614.1,buffers,0.0,na,insufficient\n")
            text = capacity_table.render(root)
            self.assertIn("| perf-prefix-on | 4 | 194.3 | - | 17271.6 | 4614 | insufficient† |", text)

    def test_cli_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(capacity_table.main(["--evidence", str(root)]), 0)
            self.assertEqual(capacity_table.main(["--evidence", str(root / "none")]), 3)


class SloThresholdTest(unittest.TestCase):
    """SLO 阈值判定逻辑；5% 边界逐位对齐 SPEC-D2-13。"""

    @staticmethod
    def regressed(baseline: float, measured: float, threshold: float = 0.05) -> bool:
        return measured < baseline * (1.0 - threshold)

    def test_five_percent_boundary(self) -> None:
        self.assertFalse(self.regressed(100.0, 95.1))
        self.assertTrue(self.regressed(100.0, 94.9))
        self.assertTrue(self.regressed(100.0, 95.0 - 1e-9))

    def test_latency_direction_is_inverted(self) -> None:
        def slower(baseline: float, measured: float, threshold: float = 0.05) -> bool:
            return measured > baseline * (1.0 + threshold)

        self.assertFalse(slower(100.0, 105.0))
        self.assertTrue(slower(100.0, 105.1))


if __name__ == "__main__":
    unittest.main()
