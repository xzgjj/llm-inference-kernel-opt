"""矩阵配置与聚合：CSV 列、不可行档、两轮偏差判定（不启服务）。"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.bench import matrix


def record(concurrency: int, tier: str, round_no: int, status: str = "ok",
           ttft: float = 100.0, itl: float = 10.0, tokens: int = 64,
           start: float = 0.0, end: float = 1000.0) -> dict:
    return {
        "concurrency": concurrency,
        "tier": tier,
        "round": round_no,
        "prompt_id": f"{tier}-000",
        "dropped": False,
        "status": status,
        "error_code": None if status == "ok" else "boom",
        "t_enqueue_ms": start,
        "t_first_ms": start + ttft if status == "ok" else None,
        "t_end_ms": end if status == "ok" else None,
        "ttft_ms": ttft if status == "ok" else None,
        "itl_ms": [itl] if status == "ok" else [],
        "n_prompt": 32,
        "n_gen": tokens,
    }


def many(concurrency: int, tier: str, round_no: int, n: int = 30, **kwargs) -> list[dict]:
    return [record(concurrency, tier, round_no, **kwargs) for _ in range(n)]


def config_dict() -> dict:
    return {
        "server": "server.exe",
        "model": "model.gguf",
        "concurrency": [1],
        "repeats": 2,
        "tiers": [{"name": "short", "max_prompt_tokens": 64, "n": 30}],
    }


class MatrixConfigTest(unittest.TestCase):
    def test_load_and_override(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            path.write_text(json.dumps(config_dict()), encoding="utf-8")
            cfg = matrix.load_config(path, {"concurrency": [1, 4], "max_tokens": 9})
        self.assertEqual(cfg.concurrency, [1, 4])
        self.assertEqual(cfg.max_tokens, 9)
        self.assertEqual(cfg.tiers[0].name, "short")
        self.assertEqual(len(cfg.config_hash()), 64)

    def test_missing_key_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            data = config_dict()
            del data["concurrency"]
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(SystemExit):
                matrix.load_config(path)


class SummarizeTest(unittest.TestCase):
    def cfg(self) -> matrix.MatrixConfig:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            path.write_text(json.dumps(config_dict()), encoding="utf-8")
            return matrix.load_config(path)

    def test_two_rounds_produce_spec_columns_and_ok_deviation(self) -> None:
        cfg = self.cfg()
        rows = many(1, "short", 1) + many(1, "short", 2)
        metrics = {(1, 1): {"kv": 0.04, "sched": 0.02}, (1, 2): {"kv": 0.05, "sched": 0.03}}
        report, deviations = matrix.summarize(cfg, rows, metrics)
        self.assertEqual(len(report), 1)
        self.assertEqual(set(report[0]), set(matrix.REPORT_FIELDS))
        self.assertEqual(report[0]["status"], "ok")
        self.assertEqual(report[0]["deviation_flag"], "ok")
        self.assertIn(report[0]["kv_internal_frag_ratio"], (0.04, 0.05))
        self.assertEqual(deviations[0]["verdict"], "ok")

    def test_deviation_above_threshold_is_high(self) -> None:
        cfg = self.cfg()
        rows = many(1, "short", 1, ttft=100.0) + many(1, "short", 2, ttft=200.0)
        report, deviations = matrix.summarize(cfg, rows, {})
        self.assertEqual(report[0]["deviation_flag"], "high")
        self.assertEqual(deviations[0]["verdict"], "high")

    def test_all_errors_are_failed_not_ok(self) -> None:
        cfg = self.cfg()
        rows = [record(1, "short", round_no, status="error") for round_no in (1, 2)]
        report, _ = matrix.summarize(cfg, rows, {})
        self.assertEqual(report[0]["status"], "failed")
        self.assertEqual(report[0]["n_requests"], 0)

    def test_infeasible_records_keep_row(self) -> None:
        cfg = self.cfg()
        rows = []
        for round_no in (1, 2):
            row = record(1, "short", round_no, status="infeasible")
            row["error_code"] = "startup_failed"
            rows.append(row)
        report, _ = matrix.summarize(cfg, rows, {})
        self.assertEqual(report[0]["status"], "infeasible")
        self.assertIn("startup_failed", report[0]["reason"])

    def test_small_sample_marks_insufficient(self) -> None:
        cfg = self.cfg()
        rows = many(1, "short", 1, n=3) + many(1, "short", 2, n=3)
        report, deviations = matrix.summarize(cfg, rows, {})
        self.assertEqual(report[0]["deviation_flag"], "insufficient")
        self.assertEqual(deviations[0]["verdict"], "insufficient")


if __name__ == "__main__":
    unittest.main()
