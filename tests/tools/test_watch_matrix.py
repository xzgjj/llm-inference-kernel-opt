"""watch_matrix：进度快照不依赖服务，坏行/缺文件都不能崩。"""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from scripts.bench import watch_matrix


class SnapshotTest(unittest.TestCase):
    def test_counts_cells_and_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            rows = [
                {"concurrency": 1, "tier": "long", "round": 1, "status": "ok"},
                {"concurrency": 2, "tier": "long", "round": 1, "status": "error"},
            ]
            (out / "requests.jsonl").write_text(
                "\n".join(json.dumps(row) for row in rows) + "\nnot-json\n", encoding="utf-8")
            state = watch_matrix.snapshot(out)
        self.assertEqual(state["rows"], 2)
        self.assertEqual(state["ok"], 1)
        self.assertEqual(state["failed"], 1)
        self.assertEqual(state["cells"], {"p1/long/r1": 1, "p2/long/r1": 1})
        self.assertFalse(state["report"])

    def test_report_without_manifest_is_not_complete(self) -> None:
        """回归：矩阵先写 CSV 再写 manifest，监控不能在 manifest 缺失时提前退出。"""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "matrix_report.csv").write_text("header\n", encoding="utf-8")
            state = watch_matrix.snapshot(out)
        self.assertFalse(state["report"])

    def test_missing_dir_is_named_not_crashing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            state = watch_matrix.snapshot(Path(tmp) / "absent")
        self.assertEqual(state["rows"], 0)
        self.assertIsNone(state["last_event_age_s"])
        self.assertIn("no-cell", watch_matrix.render(state, "00:00:00"))


class DirtyRowTest(unittest.TestCase):
    def test_dirty_fields_do_not_crash(self) -> None:
        """null/字符串/缺字段的脏行归默认值，监控进程不崩；非对象行跳过。"""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "requests.jsonl").write_text(
                json.dumps({"concurrency": None, "round": "x", "status": "ok"}) + "\n"
                + "[1, 2, 3]\n", encoding="utf-8")
            state = watch_matrix.snapshot(out)
        self.assertEqual(state["rows"], 1)
        self.assertEqual(state["cells"], {"p0//r0": 1})


class DecideTest(unittest.TestCase):
    def test_stall_label_when_age_exceeds_threshold(self) -> None:
        state = {"last_event_age_s": 150.0, "report": False}
        suffix, exit_now = watch_matrix.decide(state, 120.0, False)
        self.assertIn("STALL", suffix)
        self.assertFalse(exit_now)

    def test_no_stall_below_threshold(self) -> None:
        state = {"last_event_age_s": 10.0, "report": False}
        suffix, exit_now = watch_matrix.decide(state, 120.0, False)
        self.assertEqual(suffix, "")
        self.assertFalse(exit_now)

    def test_no_stall_when_age_unknown(self) -> None:
        state = {"last_event_age_s": None, "report": False}
        suffix, _ = watch_matrix.decide(state, 120.0, False)
        self.assertEqual(suffix, "")

    def test_until_report_exits_only_with_report(self) -> None:
        ready = {"last_event_age_s": 1.0, "report": True}
        _, exit_ready = watch_matrix.decide(ready, 120.0, True)
        self.assertTrue(exit_ready)
        missing = {"last_event_age_s": 1.0, "report": False}
        _, exit_missing = watch_matrix.decide(missing, 120.0, True)
        self.assertFalse(exit_missing)
        off = {"last_event_age_s": 1.0, "report": True}
        _, exit_off = watch_matrix.decide(off, 120.0, False)
        self.assertFalse(exit_off)


if __name__ == "__main__":
    unittest.main()
