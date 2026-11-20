"""聚合口径：分位数、错误剔除、样本不足与两轮偏差判定。"""

from __future__ import annotations

import unittest

from tools.bench import aggregate


def make_row(
    concurrency: int = 1,
    tier: str = "short",
    round_no: int = 1,
    status: str = "ok",
    dropped: bool = False,
    ttft: float = 100.0,
    gen: int = 64,
    prompt: int = 120,
    start: float = 0.0,
    end: float = 1000.0,
) -> dict:
    return {
        "concurrency": concurrency,
        "tier": tier,
        "round": round_no,
        "status": status,
        "dropped": dropped,
        "ttft_ms": ttft if status == "ok" else None,
        "itl_ms": [10.0, 12.0] if status == "ok" else [],
        "t_enqueue_ms": start,
        "t_end_ms": end,
        "n_prompt": prompt,
        "n_gen": gen,
    }


class PercentileTest(unittest.TestCase):
    def test_linear_interpolation(self) -> None:
        self.assertEqual(aggregate.percentile([1.0, 2.0, 3.0, 4.0], 0.5), 2.5)
        self.assertAlmostEqual(aggregate.percentile([1.0, 2.0, 3.0, 4.0], 0.99), 3.97)

    def test_empty_raises(self) -> None:
        with self.assertRaises(ValueError):
            aggregate.percentile([], 0.5)


class AggregateTest(unittest.TestCase):
    def test_filters_dropped_and_errors(self) -> None:
        rows = [make_row() for _ in range(30)]
        rows.append(make_row(status="error", end=900.0))
        rows.append(make_row(status="error", end=900.0))
        rows.append(make_row(dropped=True))
        baseline, _ = aggregate.aggregate(rows, "r1", [1], ["short"], 1)
        cell = baseline[0]
        self.assertEqual(cell["n_requests"], 30)
        self.assertEqual(cell["n_errors"], 2)
        self.assertIn("n_errors", cell["note"])
        # 30 * 64 tokens / 1s 墙钟
        self.assertAlmostEqual(float(cell["tg_agg_tok_s"]), 1920.0, places=1)

    def test_insufficient_samples(self) -> None:
        rows = [make_row() for _ in range(5)]
        baseline, _ = aggregate.aggregate(rows, "r1", [1], ["short"], 1)
        self.assertIn("insufficient_samples", baseline[0]["note"])

    def test_single_decode_divides_by_concurrency(self) -> None:
        rows = [make_row(concurrency=4, end=2000.0) for _ in range(30)]
        baseline, _ = aggregate.aggregate(rows, "r1", [4], ["short"], 1)
        self.assertAlmostEqual(
            float(baseline[0]["single_decode_tok_s"]),
            float(baseline[0]["tg_agg_tok_s"]) / 4.0,
            places=3,
        )


class DeviationTest(unittest.TestCase):
    def _baseline(self, pp1: float, tg1: float, pp2: float, tg2: float) -> list[dict]:
        return [
            {"concurrency_p": 1, "tier": "short", "round": 1, "pp_tok_s": pp1,
             "tg_agg_tok_s": tg1, "ttft_p50_ms": 100.0},
            {"concurrency_p": 1, "tier": "short", "round": 2, "pp_tok_s": pp2,
             "tg_agg_tok_s": tg2, "ttft_p50_ms": 102.0},
        ]

    def test_within_limit_passes(self) -> None:
        rows = aggregate._deviation_rows("r1", self._baseline(300, 100, 305, 104))
        self.assertEqual(rows[0]["verdict"], "pass")

    def test_tg_over_limit_exceeds(self) -> None:
        rows = aggregate._deviation_rows("r1", self._baseline(300, 100, 301, 110))
        self.assertEqual(rows[0]["verdict"], "exceeded")

    def test_pp_over_limit_exceeds(self) -> None:
        rows = aggregate._deviation_rows("r1", self._baseline(300, 100, 310, 101))
        self.assertEqual(rows[0]["verdict"], "exceeded")

    def test_three_rounds_use_last_two(self) -> None:
        baseline = self._baseline(300, 100, 305, 104)
        baseline.append({"concurrency_p": 1, "tier": "short", "round": 3,
                         "pp_tok_s": 306, "tg_agg_tok_s": 105, "ttft_p50_ms": 103.0})
        rows = aggregate._deviation_rows("r1", baseline, 2, 3)
        self.assertEqual(rows[0]["verdict"], "pass")
        self.assertGreater(float(rows[0]["pp_dev_pct"]), 0.0)


if __name__ == "__main__":
    unittest.main()
