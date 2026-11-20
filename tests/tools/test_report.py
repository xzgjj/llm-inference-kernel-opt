"""聚合检查与离线重建：偏差判定、/metrics 对账、PNG 图表。"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.bench import matrix, plot, report


def manifest() -> dict:
    return {
        "seed": 7,
        "concurrency": [1, 2],
        "rounds": 1,
        "tiers": [
            {"name": "short", "max_prompt_tokens": 8, "n": 2},
            {"name": "long", "max_prompt_tokens": 16, "n": 2},
        ],
        "model_path": "m.gguf",
        "server_path": "s.exe",
        "backend": "stub",
    }


def write_report(directory: Path, rows: list[dict]) -> None:
    matrix.write_report(directory / "matrix_report.csv", rows, matrix.REPORT_FIELDS)


class ReportTest(unittest.TestCase):
    def test_offline_rebuild_from_jsonl(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "repro_manifest.json").write_text(json.dumps(manifest()), encoding="utf-8")
            records = []
            for concurrency in (1, 2):
                for round_no in (1,):
                    for tier in ("short", "long"):
                        for index in range(30):
                            records.append(_record(concurrency, tier, round_no, index))
            (directory / "requests.jsonl").write_text(
                "\n".join(json.dumps(row) for row in records) + "\n", encoding="utf-8")
            rebuilt, deviations = report.rebuild(directory)
            self.assertEqual(len(rebuilt), 4)
            self.assertEqual(len(deviations), 4)
            self.assertEqual(report.write_offline(directory), 0)

    def test_plot_directory_writes_four_png(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            rows = [_row(concurrency, tier) for concurrency in (1, 2) for tier in ("short", "long")]
            write_report(directory, rows)
            self.assertEqual(plot.plot_directory(directory), 0)
            pngs = sorted((directory / "plots").glob("*.png"))
            self.assertEqual(len(pngs), 4)
            for png in pngs:
                blob = png.read_bytes()
                self.assertTrue(blob.startswith(b"\x89PNG\r\n\x1a\n"))
                self.assertGreater(len(blob), 1000)

    def test_check_metrics_detects_count_drift(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            data = manifest()
            data["rounds"] = 1
            (directory / "repro_manifest.json").write_text(json.dumps(data), encoding="utf-8")
            records = [_record(1, "short", 1, 0), _record(1, "short", 1, 1)]
            (directory / "requests.jsonl").write_text(
                "\n".join(json.dumps(row) for row in records) + "\n", encoding="utf-8")
            (directory / "metrics_p1_r1.prom").write_text(
                'llmopt_requests_total{status="ok"} 3\n'
                "llmopt_tokens_generated_total 9\n", encoding="utf-8")
            self.assertEqual(report.check_metrics(directory), 1)

    def test_check_deviation_high_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            matrix.write_report(directory / "deviation.csv",
                                [{"concurrency_p": 1, "prompt_tier": "short", "verdict": "high"}],
                                matrix.DEVIATION_FIELDS)
            self.assertEqual(report.check_deviation(directory / "deviation.csv"), 1)


def _record(concurrency: int, tier: str, round_no: int, index: int) -> dict:
    return {
        "concurrency": concurrency,
        "tier": tier,
        "round": round_no,
        "prompt_id": f"{tier}-{index:03d}",
        "dropped": False,
        "status": "ok",
        "error_code": None,
        "t_enqueue_ms": float(index),
        "t_first_ms": 10.0 + index,
        "t_end_ms": 100.0 + index,
        "ttft_ms": 10.0 + index,
        "itl_ms": [2.0, 3.0],
        "n_prompt": 4,
        "n_gen": 8,
    }


def _row(concurrency: int, tier: str) -> dict:
    return {
        "concurrency_p": concurrency,
        "prompt_tier": tier,
        "status": "ok",
        "n_requests": 30,
        "ttft_p50_ms": 10.0 + concurrency,
        "ttft_p95_ms": 20.0 + concurrency,
        "itl_p99_ms": 5.0,
        "total_tok_s": 100.0 / concurrency,
        "kv_internal_frag_ratio": 0.04,
        "sched_cpu_ratio": 0.02,
        "reason": "na",
        "deviation_flag": "ok",
    }


if __name__ == "__main__":
    unittest.main()
