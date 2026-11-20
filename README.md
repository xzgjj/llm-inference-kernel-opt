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
- 当前限制：官方 7×2 矩阵已在 CPU 主机跑完（14/14 行 `ok`，指标对账 PASS），但两轮偏差 12/14 行超差、P8 后吞吐不再增长，属 CPU 饱和负结果；不据此宣称性能达标。

## 阶段 5：可选预研（判据与预留，无实现）

- 判据复算：`python scripts/spec5/recompute_spec5.py --csv <matrix_report.csv> --traces <dir>`，只读十二列 CSV 与 `steps-p<N>.jsonl`，输出 `criteria.csv/md`；n<30、非 ok 档、缺 trace/缺 chunk 对照一律记数据缺口，不估算收益。
- 真实数据：官方 14 行复算 **0 触发、12 候选、2 不可判**，结论“本期数据不支持翻案”；PD 停在“缺同档 chunk 调参/Δstall”，投机缺草稿模型实测。报告在 `docs/notes/phase5_预研评估.md`，判据表在 `docs/evidence/phase5/`（本地证据，不入库）。
- 预留接口：`include/llmopt/p5_reserved.hpp`（`llmopt::p5`，C-P5-01…03），默认值等价 v0，`src` 零引用；用例 `unit_p5_reserved` + `tests/tools/test_spec5.py`。
- 边界：不建 PD 双进程、不做跨进程 KV、不接草稿模型、不引 CUDA；接口契约待 review 后再冻结。

## 阶段 6：软件与服务完成度

- 兼容面：`POST /v1/chat/completions`（`messages` 走 GGUF 内嵌 chat template，缺失回退拼接）、`POST /v1/completions`（`stop`/`n`/`seed`/`usage`）、`GET /v1/models`；旧 `/completion` 保留。兼容面外字段返回 400 `unsupported_field`。
- 配置：CLI > `LLMOPT_*` > JSON 文件 > 默认四层优先级，逐项打印 `config source`；未知键/越界/坏 JSON 退出码 2。
- 服务：`/readyz` 就绪探针 + `Starting/Ready/Draining/Stopped` 四态；SIGTERM 后停止准入、等在飞收尾，超 `--drain-deadline-seconds` 记 `llmopt_drain_timeout_total`。
- 限流：`--rate-limit-rps/--rate-limit-burst` 原子令牌桶，超额 429 + `Retry-After: 1`，不进队列。
- 指标：全部 family 带 `# TYPE/# HELP`；请求 id 进该请求日志行；`llmopt_rate_limited_total`、`llmopt_drain_timeout_total` 已登记。
- 部署：`scripts/release/Dockerfile`、`llmopt.service`、`docker_smoke.sh`；`soak.py` 30 min + `.github/workflows/nightly.yml`（TSan 10 轮 / ASan / soak）。说明见 `scripts/release/README.md`。
- 边界：单 token Bearer（D6-2）仍未启用，待用户确认是否修订 01 §2.3；慢客户端 ≤10% 退化与 sanitizer 结论以 Linux/真后端为准。
