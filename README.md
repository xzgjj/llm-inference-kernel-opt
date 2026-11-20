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
- 当前限制：官方 7×2 矩阵已在 CPU 主机跑完（14/14 行 `ok`，指标对账 PASS），但两轮偏差 12/14 行超差、P8 后吞吐不再增长，属 CPU 饱和负结果；不据此宣称性能达标。chat 端点仍 404（501 待契约批准）。

## 阶段 5：可选预研（判据与预留，无实现）

- 判据复算：`python scripts/spec5/recompute_spec5.py --csv <matrix_report.csv> --traces <dir>`，只读十二列 CSV 与 `steps-p<N>.jsonl`，输出 `criteria.csv/md`；n<30、非 ok 档、缺 trace/缺 chunk 对照一律记数据缺口，不估算收益。
- 真实数据：官方 14 行复算 **0 触发、12 候选、2 不可判**，结论“本期数据不支持翻案”；PD 停在“缺同档 chunk 调参/Δstall”，投机缺草稿模型实测。报告在 `docs/notes/phase5_预研评估.md`，判据表在 `docs/evidence/phase5/`（本地证据，不入库）。
- 预留接口：`include/llmopt/p5_reserved.hpp`（`llmopt::p5`，C-P5-01…03），默认值等价 v0，`src` 零引用；用例 `unit_p5_reserved` + `tests/tools/test_spec5.py`。
- 边界：不建 PD 双进程、不做跨进程 KV、不接草稿模型、不引 CUDA；接口契约待 review 后再冻结。
