"""SPEC-5 判据复算：阈值组合、数据缺口、trace 代理、CLI 落盘与预留不越界。"""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "spec5" / "recompute_spec5.py"
DATA = ROOT / "tests" / "data" / "spec5"

_SPEC = importlib.util.spec_from_file_location("recompute_spec5", SCRIPT)
spec5 = importlib.util.module_from_spec(_SPEC)
sys.modules["recompute_spec5"] = spec5  # dataclasses 在 exec 时按模块名回查命名空间
_SPEC.loader.exec_module(spec5)


def row(p: int, tier: str = "short", itl: float = 60.0, n: int = 30,
        status: str = "ok", tok_s: float = 100.0) -> dict[str, str]:
    return {
        "concurrency_p": str(p),
        "prompt_tier": tier,
        "status": status,
        "n_requests": str(n),
        "ttft_p50_ms": "1",
        "ttft_p95_ms": "2",
        "itl_p99_ms": str(itl),
        "total_tok_s": str(tok_s),
        "kv_internal_frag_ratio": "0.5",
        "sched_cpu_ratio": "0.0",
        "reason": "na",
        "deviation_flag": "ok",
    }


def trace(share: float, p99: float = 200.0) -> dict[str, object]:
    return {"share": share, "itl_p99_ms": p99, "decode_steps": 10,
            "long_steps": 2, "prefill_long_steps": int(share * 2)}


def evaluate(p: int = 4, tier: str = "long", itl: float = 60.0, n: int = 30,
             status: str = "ok", shares: dict[int, dict[str, object]] | None = None,
             gains: dict[tuple[int, str], float] | None = None,
             specs: dict[tuple[int, str], dict[str, float | None]] | None = None,
             migration: dict[str, object] | None = None) -> dict[str, str]:
    return spec5.evaluate_row(
        row(p, tier, itl=itl, n=n, status=status),
        spec5.Thresholds(),
        shares or {p: trace(1.0)},
        gains or {},
        specs or {},
        migration,
    )


class TraceAndPercentileTest(unittest.TestCase):
    def test_percentile_matches_linear_interpolation(self) -> None:
        self.assertAlmostEqual(spec5.percentile([1, 2, 3, 4], 50), 2.5)
        self.assertAlmostEqual(spec5.percentile([10, 20], 99), 19.9)
        self.assertEqual(spec5.percentile([], 99), 0.0)

    def test_fixture_trace_share_one_when_long_interval_has_prefill(self) -> None:
        profile = spec5.load_trace(DATA / "steps-p4.jsonl")
        self.assertIsNotNone(profile)
        self.assertEqual(profile["share"], 1.0)
        self.assertEqual(profile["long_steps"], 1)
        self.assertGreater(profile["itl_p99_ms"], 100.0)

    def test_fixture_trace_share_zero_without_prefill(self) -> None:
        profile = spec5.load_trace(DATA / "steps-p1.jsonl")
        self.assertIsNotNone(profile)
        self.assertEqual(profile["share"], 0.0)

    def test_truncated_trace_line_is_dropped(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "steps-p2.jsonl"
            path.write_text('{"ts_ns":0,"decode_tokens":1}\n{"ts_ns":5000000,"dec',
                            encoding="utf-8")
            self.assertIsNone(spec5.load_trace(path))


class InputValidationTest(unittest.TestCase):
    def test_missing_column_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.csv"
            path.write_text("concurrency_p,prompt_tier\n1,short\n", encoding="utf-8")
            with self.assertRaises(spec5.DataError):
                spec5.read_csv_rows(path, spec5.REQUIRED_COLUMNS)

    def test_below_min_samples_is_insufficient(self) -> None:
        result = evaluate(n=29)
        self.assertEqual(result["pd_verdict"], "data_insufficient")
        self.assertEqual(result["spec_verdict"], "data_insufficient")
        self.assertEqual(result["row_verdict"], "data_insufficient")
        self.assertTrue(result["reason"].startswith("n=29<30"))

    def test_non_ok_status_is_insufficient(self) -> None:
        result = evaluate(status="tier_timeout")
        self.assertEqual(result["row_verdict"], "data_insufficient")


class PdCriteriaTest(unittest.TestCase):
    def test_itl_within_slo_not_triggered(self) -> None:
        result = evaluate(itl=49.9)
        self.assertEqual(result["pd_verdict"], "not_triggered")
        self.assertEqual(result["pd_reason"], "itl_within_slo")

    def test_share_low_not_triggered(self) -> None:
        result = evaluate(shares={4: trace(0.4)})
        self.assertEqual(result["pd_verdict"], "not_triggered")
        self.assertEqual(result["pd_reason"], "share_low")

    def test_candidate_when_chunk_gain_missing(self) -> None:
        result = evaluate()
        self.assertEqual(result["pd_verdict"], "candidate_pending_chunk")
        self.assertEqual(result["pd_reason"], "chunk_gain_missing")

    def test_chunk_tuning_sufficient_blocks_trigger(self) -> None:
        result = evaluate(gains={(4, "long"): 0.10})
        self.assertEqual(result["pd_verdict"], "not_triggered")
        self.assertEqual(result["chunk_gain_pct"], "10.000000")

    def test_candidate_when_delta_stall_unmeasured(self) -> None:
        result = evaluate(gains={(4, "long"): 0.02})
        self.assertEqual(result["pd_verdict"], "candidate_pending_migration")
        self.assertEqual(result["pd_reason"], "delta_stall_unmeasured")

    def test_negative_gain_when_transfer_exceeds_stall(self) -> None:
        migration = {"worth": False, "kv_mib": 48.0, "t_xfer_ms": 46.8, "budget_ms": 5.0}
        result = evaluate(gains={(4, "long"): 0.02}, migration=migration)
        self.assertEqual(result["pd_verdict"], "negative_gain")

    def test_pd_triggered_when_three_conditions_hold(self) -> None:
        migration = {"worth": True, "kv_mib": 48.0, "t_xfer_ms": 0.46, "budget_ms": 5.0}
        result = evaluate(itl=60.0, gains={(4, "long"): 0.02}, migration=migration)
        self.assertEqual(result["pd_verdict"], "pd_triggered")
        self.assertEqual(result["row_verdict"], "pd_triggered")

    def test_migration_formula_matches_spec(self) -> None:
        estimate = spec5.migration_estimate(spec5.Migration(
            kv_kib_per_token=12.0, ctx_tokens=4096, link_gibps=10.0,
            route_ms=0.0, delta_stall_ms=10.0, overlap_ms=0.0))
        self.assertAlmostEqual(estimate["kv_mib"], 48.0)
        self.assertAlmostEqual(estimate["t_xfer_ms"], 4.6875, places=3)
        self.assertTrue(estimate["worth"])
        missing = spec5.migration_estimate(spec5.Migration(
            kv_kib_per_token=12.0, ctx_tokens=4096, link_gibps=0.0,
            route_ms=0.0, delta_stall_ms=10.0, overlap_ms=0.0))
        self.assertIsNone(missing)


class SpecCriteriaTest(unittest.TestCase):
    def test_missing_draft_measurement_is_insufficient(self) -> None:
        result = evaluate()
        self.assertEqual(result["spec_verdict"], "data_insufficient")

    def test_gain_below_threshold_is_batch_decay(self) -> None:
        result = evaluate(specs={(4, "long"): {"baseline_tok_s": 484.29, "draft_tok_s": 512.0,
                                               "itl_delta_pct": 3.1}})
        self.assertEqual(result["spec_verdict"], "batch_decay")
        self.assertAlmostEqual(float(result["spec_gain_pct"]), 5.7219, places=3)

    def test_triggered_requires_itl_tolerance(self) -> None:
        hit = evaluate(specs={(4, "long"): {"baseline_tok_s": 100.0, "draft_tok_s": 120.0,
                                            "itl_delta_pct": 7.0}})
        miss = evaluate(specs={(4, "long"): {"baseline_tok_s": 100.0, "draft_tok_s": 120.0,
                                             "itl_delta_pct": 7.1}})
        self.assertEqual(hit["spec_verdict"], "spec_triggered")
        self.assertEqual(miss["spec_verdict"], "not_triggered")
        self.assertEqual(miss["spec_reason"], "itl_degraded")

    def test_alpha_k_cost_only_estimates_not_evidence(self) -> None:
        # α=0.7、k=4：E[a]=1.7731，除以 (1+c·k)=1.8 得 0.985；仅展示，不构成翻案证据。
        self.assertAlmostEqual(spec5.speculative_speedup(0.7, 4, 0.2), 0.985056, places=5)
        self.assertIsNone(spec5.speculative_speedup(None, 4, 0.2))


class CliTest(unittest.TestCase):
    def test_cli_writes_criteria_without_report_when_not_triggered(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "spec5"
            code = spec5.main(["--csv", str(DATA / "matrix_report.csv"),
                               "--traces", str(DATA), "--out", str(out), "--quiet"])
            self.assertEqual(code, 0)
            self.assertTrue((out / "criteria.csv").exists())
            text = (out / "criteria.md").read_text(encoding="utf-8")
            self.assertIn("本期数据不支持翻案", text)
            self.assertFalse((out / "phase5_预研评估.md").exists())

    def test_cli_writes_report_on_request(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "phase5_预研评估.md"
            code = spec5.main(["--csv", str(DATA / "matrix_report.csv"),
                               "--traces", str(DATA), "--out", str(Path(tmp) / "o"),
                               "--report", str(report), "--quiet"])
            self.assertEqual(code, 0)
            text = report.read_text(encoding="utf-8")
            self.assertIn("本期数据不支持翻案", text)
            self.assertIn("[ ] 投机值得翻案", text)

    def test_cli_bad_csv_returns_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.csv"
            path.write_text("p,tier\n1,a\n", encoding="utf-8")
            code = spec5.main(["--csv", str(path), "--out", str(Path(tmp) / "o"), "--quiet"])
            self.assertEqual(code, 1)


class GoldenTest(unittest.TestCase):
    def test_official_matrix_fixture_verdicts(self) -> None:
        rows = spec5.read_csv_rows(DATA / "matrix_report.csv", spec5.REQUIRED_COLUMNS)
        traces = spec5.load_traces(DATA)
        results = [spec5.evaluate_row(r, spec5.Thresholds(), traces, {}, {}, None) for r in rows]
        expected = json.loads((DATA / "expected_verdicts.json").read_text(encoding="utf-8"))
        actual = {f"{r['concurrency_p']}-{r['prompt_tier']}":
                  {"pd": r["pd_verdict"], "row": r["row_verdict"]} for r in results}
        self.assertEqual(actual, expected)
        summary = spec5.summarize(results, spec5.Thresholds())
        self.assertEqual(summary["fired"], 0)
        self.assertEqual(summary["candidates"], 2)


class ReservedBoundaryTest(unittest.TestCase):
    def test_reserved_header_is_not_in_v0_hot_path(self) -> None:
        offenders = []
        for directory in ("src", "include"):
            for path in (ROOT / directory).rglob("*.hpp"):
                if path.name == "p5_reserved.hpp":
                    continue
                if "p5_reserved" in path.read_text(encoding="utf-8"):
                    offenders.append(str(path.relative_to(ROOT)))
            for path in (ROOT / directory).rglob("*.cpp"):
                if "p5_reserved" in path.read_text(encoding="utf-8"):
                    offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [], "预留头不得进入 v0 编译路径（R-02）")


if __name__ == "__main__":
    unittest.main()
