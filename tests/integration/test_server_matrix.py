# test_server_matrix.py — 4-G1 端到端：小矩阵起真 stub 服务、档间重启、报告/清单可复盘。
from __future__ import annotations

import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "bench" / "matrix_runner.py"
sys.path.insert(0, str(ROOT))
from tools.bench import matrix as matrix_tool  # noqa: E402

REPORT_FIELDS = matrix_tool.REPORT_FIELDS


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        out = Path(tmp) / "matrix"
        env = dict(os.environ)
        env["LLMOPT_ALLOW_STUB"] = "1"
        result = subprocess.run(
            [sys.executable, str(RUNNER), "--smoke", "--server", str(server),
             "--model", str(model), "--out", str(out), "--run-id", "matrix-test"],
            capture_output=True, text=True, timeout=180, env=env,
        )
        check(result.returncode == 0, f"矩阵失败 rc={result.returncode}\n{result.stdout}\n{result.stderr}")

        with (out / "matrix_report.csv").open(encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        check(len(rows) == 4, f"2 并发 × 2 档 应有 4 行，实际 {len(rows)}")
        check(list(rows[0].keys()) == REPORT_FIELDS, "CSV 列必须与 SPEC §1.6 一致")
        for row in rows:
            check(row["status"] == "ok", f"冒烟档应成功：{row}")
            check(int(row["n_requests"]) == 3, "每档应有 3 条有效请求")
            check(float(row["ttft_p50_ms"]) > 0, "TTFT 应为正")
            check(float(row["total_tok_s"]) > 0, "吞吐应为正")
            # stub 构建没有 CUDA 缓冲区日志：显存证据必须显式写 unknown，不能写 0 冒充实测。
            check(row["vram_peak_scope"] == "unknown", f"stub 显存口径错误：{row}")
            check(float(row["vram_peak_mib"]) == 0.0, f"stub 不应编造显存：{row}")

        lines = (out / "requests.jsonl").read_text(encoding="utf-8").splitlines()
        check(len(lines) == 16, f"warmup 2×2 + 正式 2×2×3 = 16，实际 {len(lines)}")
        warmups = [json.loads(line) for line in lines if json.loads(line)["dropped"]]
        check(len(warmups) == 4, f"warmup 应 4 条，实际 {len(warmups)}")

        manifest = json.loads((out / "repro_manifest.json").read_text(encoding="utf-8"))
        check(len(manifest["config_hash"]) == 64, "配置哈希应为 sha256")
        check(len(manifest["prompt_set_hash"]) == 64, "prompt 集哈希应为 sha256")
        check(manifest["seed"] == 7, "种子应入清单")
        check(manifest["model_sha256"] == kit.sha256(model), "模型 sha256 应一致")
        check(set(manifest["vram_buffers"]) == {"1", "2"}, "每个并发档都应有显存分项")
        for concurrency in ("1", "2"):
            peak = manifest["resource_peak"][concurrency]
            check(peak["rss_peak_bytes"] > 0, "RSS 峰值应可归因到本进程")
            check(isinstance(peak["vram_known"], bool), "VRAM 未知应显式标记")

        metrics = (out / "metrics_p1_r1.prom").read_text(encoding="utf-8")
        check("llmopt_kv_internal_frag_ratio" in metrics, "应归档 /metrics 快照")
        check("llmopt_sched_cpu_ratio" in metrics, "应归档调度占比")

        with (out / "step_summary.csv").open(encoding="utf-8") as handle:
            step_rows = list(csv.DictReader(handle))
        check(len(step_rows) == 2, "每个并发档一行步级汇总")
        for row in step_rows:
            check(float(row["step_ns_p99_ms"]) > 0, "步级 p99 应由 step trace 离线算出")

        aggregate = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "bench" / "aggregate.py"),
             "--dir", str(out), "--check-metrics", "--check-deviation"],
            capture_output=True, text=True, timeout=60,
        )
        check(aggregate.returncode == 0,
              f"计数对账/偏差检查失败\n{aggregate.stdout}\n{aggregate.stderr}")
        print("OK integration_server_matrix")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
