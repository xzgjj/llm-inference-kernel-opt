"""矩阵配置与聚合：CSV 列、不可行档、两轮偏差判定（不启服务）。"""

from __future__ import annotations

import json
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

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

    def test_gpu_and_kv_flags_are_reproducible_in_argv(self) -> None:
        data = config_dict()
        data.update({
            "n_gpu_layers": 28, "threads": 12, "kv_cache_type": "q8_0",
            "donor_slots": 0, "disable_prefix_cache": True, "chunk_size": 64,
        })
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            cfg = matrix.load_config(path)
        argv = matrix.server_argv(cfg, 4, 18080)
        self.assertEqual(argv[argv.index("--n-gpu-layers") + 1], "28")
        self.assertEqual(argv[argv.index("--threads") + 1], "12")
        self.assertEqual(argv[argv.index("--kv-cache-type") + 1], "q8_0")
        self.assertEqual(argv[argv.index("--donor-slots") + 1], "0")
        self.assertEqual(argv[argv.index("--chunk-size") + 1], "64")
        self.assertIn("--disable-prefix-cache", argv)

    def test_default_donor_is_explicit_in_argv(self) -> None:
        """回归：服务默认 donor=4，矩阵不能因为默认省略而让 manifest 与实跑不一致。"""
        data = config_dict()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            cfg = matrix.load_config(path)
        self.assertEqual(cfg.donor_slots, 4)
        argv = matrix.server_argv(cfg, 1, 18080)
        self.assertEqual(argv[argv.index("--donor-slots") + 1], "4")

    def test_phase8b_configs_declare_kv_type(self) -> None:
        """回归：短/长档曾省略 KV 类型，manifest 只记默认值，复现口径不清。"""
        root = Path(__file__).resolve().parents[2]
        for name in ("phase8b_short.json", "phase8b_long.json"):
            cfg = matrix.load_config(root / "scripts" / "bench" / "matrix" / name)
            self.assertEqual(cfg.kv_cache_type, "f16", name)

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
        report, deviations = matrix.summarize(
            cfg, rows, metrics, {1: matrix.parse_vram_budgets("CUDA0 model buffer size = 100 MiB")})
        self.assertEqual(len(report), 1)
        self.assertEqual(set(report[0]), set(matrix.REPORT_FIELDS))
        self.assertEqual(report[0]["status"], "ok")
        self.assertEqual(report[0]["deviation_flag"], "ok")
        self.assertIn(report[0]["kv_internal_frag_ratio"], (0.04, 0.05))
        self.assertEqual(report[0]["vram_peak_mib"], 100.0)
        self.assertEqual(report[0]["vram_peak_scope"], "buffers_partial")
        self.assertEqual(report[0]["vram_device_peak_mib"], 0.0)
        self.assertEqual(deviations[0]["verdict"], "ok")

    def test_prefill_and_prefix_hit_tokens_override_char_estimate(self) -> None:
        """回归：中文 prompt 的字符数不是 token 数；命中前缀的 token 也要算逻辑 prompt。"""
        cfg = self.cfg()
        rows = many(1, "short", 1, n=2, tokens=40)
        metrics = {(1, 1): {"kv": 0.04, "sched": 0.02,
                           "prefill_tokens": 60.0, "prefix_hit_tokens": 40.0}}
        report, _ = matrix.summarize(cfg, rows, metrics, {})
        self.assertEqual(report[0]["tok_accounting"], "prompt_metrics")
        # (60 实算 + 40 命中 + 80 生成) / 1.0s；若退回字符估算会是 (32×2 + 80) / 1.0s。
        self.assertAlmostEqual(report[0]["total_tok_s"], 180.0)

    def test_missing_prefill_metric_falls_back_and_is_labelled(self) -> None:
        cfg = self.cfg()
        rows = many(1, "short", 1, n=2, tokens=40)
        report, _ = matrix.summarize(cfg, rows, {(1, 1): {"kv": 0.04, "sched": 0.02}}, {})
        self.assertEqual(report[0]["tok_accounting"], "char_estimate")
        self.assertAlmostEqual(report[0]["total_tok_s"], 144.0)

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


class ReportPrecisionTest(unittest.TestCase):
    def test_ratio_fields_keep_small_sched_overhead(self) -> None:
        """回归：K-4 常在 0.01% 量级，3 位小数会写成一列 0。"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.csv"
            matrix.write_report(path, [{"kv_internal_frag_ratio": 0.5,
                                        "sched_cpu_ratio": 0.000123}],
                                ["kv_internal_frag_ratio", "sched_cpu_ratio"])
            text = path.read_text(encoding="utf-8")
        self.assertIn("0.000123", text)
        self.assertIn("0.5", text)


class VramBudgetTest(unittest.TestCase):
    def test_parse_cuda_buffers_as_process_peak(self) -> None:
        text = (
            "load_tensors:   CPU_Mapped model buffer size =  1773.61 MiB\n"
            "load_tensors:        CUDA0 model buffer size =  3015.58 MiB\n"
            "llama_kv_cache:        CPU KV buffer size =   416.00 MiB\n"
            "llama_kv_cache:      CUDA0 KV buffer size =   736.00 MiB\n"
            "sched_reserve:      CUDA0 compute buffer size =   328.75 MiB\n"
            "sched_reserve:  CUDA_Host compute buffer size =    33.02 MiB\n"
        )
        parsed = matrix.parse_vram_budgets(text)
        self.assertEqual(parsed["vram_peak_scope"], "buffers")
        self.assertAlmostEqual(parsed["vram_peak_mib"], 4080.33)
        self.assertAlmostEqual(parsed["cuda0_model_mib"], 3015.58)
        self.assertAlmostEqual(parsed["cuda0_kv_mib"], 736.0)
        self.assertAlmostEqual(parsed["cuda0_compute_mib"], 328.75)
        self.assertAlmostEqual(parsed["host_kv_mib"], 416.0)

    def test_missing_compute_marks_partial_not_unknown(self) -> None:
        parsed = matrix.parse_vram_budgets(
            "load_tensors: CUDA0 model buffer size = 1000.00 MiB\n"
            "llama_kv_cache: CUDA0 KV buffer size = 200.00 MiB\n")
        self.assertEqual(parsed["vram_peak_scope"], "buffers_partial")
        self.assertAlmostEqual(parsed["vram_peak_mib"], 1200.0)

    def test_stub_log_is_unknown(self) -> None:
        parsed = matrix.parse_vram_budgets("stub backend ready")
        self.assertEqual(parsed["vram_peak_scope"], "unknown")
        self.assertEqual(parsed["vram_peak_mib"], 0.0)

    def test_read_missing_log_is_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            parsed = matrix.read_vram_budgets(Path(tmp) / "absent.log")
        self.assertEqual(parsed["vram_peak_scope"], "unknown")


class LiveMetricSamplerTest(unittest.TestCase):
    def test_keeps_peak_when_cell_end_snapshot_is_drained(self) -> None:
        """回归：格结束快照在排空后为 0，采样器必须保留压测窗口内的峰值。"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            path.write_text(json.dumps(config_dict()), encoding="utf-8")
            cfg = matrix.load_config(path)
        text = (
            "# TYPE llmopt_kv_internal_frag_ratio gauge\n"
            "llmopt_kv_internal_frag_ratio 0.0700\n"
            "# TYPE llmopt_sched_cpu_ratio gauge\n"
            "llmopt_sched_cpu_ratio 0.0021\n"
            "# TYPE llmopt_vram_device_mib gauge\n"
            "llmopt_vram_device_mib 6554.0000\n"
            "llmopt_kv_used_blocks 37\n"
            "llmopt_vram_peak_mib NaN\n"
        )
        sampler = matrix._LiveMetricSampler(cfg, 1, interval_s=0.05)
        with mock.patch.object(matrix, "fetch_metrics", return_value=text):
            sampler.start()
            # 确定性等待首轮回调写入，不再靠固定 sleep 赌线程调度；2s 上限兜底防挂死。
            deadline = time.monotonic() + 2.0
            while sampler.kv_used == 0 and time.monotonic() < deadline:
                time.sleep(0.01)
            sampler.stop()
        self.assertAlmostEqual(sampler.kv, 0.07)
        self.assertAlmostEqual(sampler.sched, 0.0021)
        self.assertAlmostEqual(sampler.device_vram, 6554.0)
        self.assertEqual(sampler.kv_used, 37)


if __name__ == "__main__":
    unittest.main()
