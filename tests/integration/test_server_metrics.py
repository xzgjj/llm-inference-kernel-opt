# test_server_metrics.py — 4-G0-1/5 进程级契约：TYPE/HELP 全覆盖，KPI 与请求计数一致。
from __future__ import annotations

import math
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import server_kit as kit


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def stream_once(port: int, prompt: str, max_tokens: int, seed: int) -> None:
    status, body, _ = kit.post(port, "/completion",
                               {"prompt": prompt, "stream": True,
                                "max_tokens": max_tokens, "seed": seed})
    check(status == 200, f"流式请求应 200，实际 {status}")
    lines = [line for line in body.decode().splitlines() if line.startswith("data: ")]
    check(lines[-1] == "data: [DONE]", "流式响应缺少 [DONE]")


def main() -> int:
    server = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        model = Path(tmp) / "fake.gguf"
        kit.make_gguf(model)
        with kit.running_server(server, model,
                                ["--ctx-tokens", "512", "--max-concurrency", "1",
                                 "--max-num-batched-tokens", "256", "--log-level", "warn"]) as port:
            status, body, headers = kit.get(port, "/metrics")
            check(status == 200, f"/metrics 应 200，实际 {status}")
            check("text/plain" in headers.get("Content-Type", ""), "Content-Type 应为 text/plain")
            check(b"# HELP" in body and b"# TYPE" in body, "缺少 TYPE/HELP")

            for index in range(3):
                stream_once(port, f"metrics-{index}", 3, 100 + index)

            status, body, _ = kit.get(port, "/metrics")
            check(status == 200, "/metrics 二次抓取失败")
            samples, types, helps, duplicate_help = kit.parse_metrics(body.decode())
            check(not duplicate_help, f"同一 family 出现多组 HELP：{duplicate_help}")
            check(len(types) == len(helps), "每个 family 必须同时有 TYPE 与 HELP")
            for name, value in samples.items():
                family = name.split("{", 1)[0]
                check(family in types, f"指标 {name} 缺少 TYPE")
                check(family in helps, f"指标 {name} 缺少 HELP")
                check(value >= 0 or str(value) == "nan", f"指标 {name} 为负值 {value}")

            # K-3 / 请求计数：3 条成功，共 9 个 token；accepted 与 ok 一一对应。
            check(samples['llmopt_requests_total{status="accepted"}'] == 3,
                  "accepted 应等于 3")
            check(samples['llmopt_requests_total{status="ok"}'] == 3, "ok 应等于 3")
            check(samples['llmopt_requests_total{status="error"}'] == 0, "不应出现执行错误")
            check(samples['llmopt_requests_total{status="queue_full"}'] == 0, "不应出现队列满")
            check(samples["llmopt_tokens_generated_total"] == 9, "生成 token 总数应为 9")
            check(samples["llmopt_http_requests_total"] == 3, "HTTP 计数应为 3")

            # K-1 / K-2：每条请求 1 个 TTFT，max_tokens=3 时纯 decode 间隔各 2 个。
            check(samples["llmopt_ttft_ms_count"] == 3, "TTFT 计数应为 3")
            check(samples["llmopt_ttft_ms_sum"] > 0, "TTFT 累计应为正")
            check(samples["llmopt_itl_ms_count"] >= 6, "ITL 计数至少 6")
            check(samples["llmopt_itl_ms_sum"] > 0, "ITL 累计应为正")
            check(samples["llmopt_decode_steps_total"] >= 6, "纯 decode 步数至少 6")

            # K-4 / K-6 / K-5：调度占比、运行峰值、KV 碎片率口径。
            check(0.0 <= samples["llmopt_sched_cpu_ratio"] <= 1.0, "调度占比应在 [0,1]")
            check(samples["llmopt_requests_running"] == 0, "收尾后运行数应为 0")
            check(samples["llmopt_requests_queued"] == 0, "收尾后队列应为 0")
            check(samples["llmopt_requests_running_peak"] >= 1, "运行峰值至少 1")
            check(0.0 <= samples["llmopt_kv_internal_frag_ratio"] <= 1.0, "碎片率应在 [0,1]")
            check(samples["llmopt_tokens_per_sec"] > 0, "平均吞吐应为正")
            check(samples["llmopt_kv_held_blocks"] == 0, "收尾后 held 应为 0")

            # KV 峰值：排空后当前占用回到 0，峰值必须保留运行期水位，且不低于当前占用。
            check("llmopt_kv_used_blocks_peak" in samples, "缺少 llmopt_kv_used_blocks_peak 序列")
            check(samples["llmopt_kv_used_blocks_peak"] >= samples["llmopt_kv_used_blocks"],
                  "KV 峰值不应低于当前占用")
            check(samples["llmopt_kv_used_blocks_peak"] >= 1, "排空后峰值应保留运行期水位")

            # K-9：stub + mode=off 应判 ELIGIBLE 且未降级。
            check(samples['llmopt_gate_verdict_total{verdict="ELIGIBLE"}'] == 1,
                  "门控应记录 1 次 ELIGIBLE")
            check(samples["llmopt_degraded"] == 0, "未降级时 degraded 应为 0")

            # K-8：RSS 为进程级实数，峰值不低于当前；VRAM 未知时必须 NaN + probe_ok=0。
            check(samples["llmopt_rss_mib"] > 0, "RSS 应为正")
            check(samples["llmopt_rss_peak_mib"] >= samples["llmopt_rss_mib"],
                  "RSS 峰值不应低于当前值")
            probe = samples["llmopt_vram_probe_ok"]
            check(probe in (0, 1), "vram_probe_ok 只能是 0/1")
            if probe == 0:
                check(math.isnan(samples["llmopt_vram_mib"]), "未知 VRAM 应写 NaN")
                check(math.isnan(samples["llmopt_vram_peak_mib"]), "未知 VRAM 峰值应写 NaN")

            # 设备级整卡显存序列必须发射；未知探针口径下为 NaN，其余为非负实数。
            check("llmopt_vram_device_mib" in samples, "缺少 llmopt_vram_device_mib 序列")
            device_vram = samples["llmopt_vram_device_mib"]
            check(device_vram >= 0 or math.isnan(device_vram),
                  f"设备显存口径应为非负或 NaN，实际 {device_vram}")

            # 反例：未知指标名不得被动态注册成新序列。
            check("llmopt_unknown_metric" not in samples, "未知指标不应出现在 /metrics")
            check(samples["llmopt_schedule_step_ns_last"] >= 0, "最近步耗时应为非负")
            print("OK integration_server_metrics")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
