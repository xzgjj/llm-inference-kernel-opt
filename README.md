# llm-inference-kernel-opt 大模型推理服务



- 本项目和llam相关主**要围绕C++ 大模型推理服务：**


偏算子 / 后端：**ggml 后端插件化：自定义 Backend 与算子加速**

偏服务调度：**C++ 大模型推理服务：Continuous Batching 与请求调度**

偏算法加速：**LLM 投机解码落地与端到端加速评估**

偏低比特：**低比特量化下的推理精度 — 吞吐权衡（INT4/FP8）**



- CUDA toolkit

Qwen2.5-0.5B

## 阶段 4：指标与实验矩阵

- 指标：固定 `MetricId` 表 + 8 路分片计数；`/metrics` 暴露 TTFT/ITL sum+count、KV 碎片率、峰值、抢占原因、准入 verdict、RSS/VRAM。
- 准入：预算 = `max_concurrency + queue_capacity`，超限返回 429 + `Retry-After: 1`。
- 实验矩阵：`scripts/bench/matrix_runner.py` 按 JSON 配置跑并发档 × prompt 档，档间重启；产物含 `requests.jsonl`、`matrix_report.csv`、`deviation.csv`、`step_summary.csv`、`repro_manifest.json`。
- 复现：`bash scripts/repro.sh --smoke`（stub，秒级）；真机用 `--backend llama` 并覆盖 `--server/--model`。
- 当前限制：官方 7×2 两轮尚未在 CPU 主机跑完；已跑 4 档 × 2 档小样，结果与限制见 `docs/阶段4-实现记录.md` §5。chat 端点仍 404（501 待契约批准）。
