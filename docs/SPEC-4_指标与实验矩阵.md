# SPEC-4 指标与实验矩阵

> 上游：规划/03 阶段 4（4-G1…）；联动：SPEC-0 的 CSV/JSONL 口径、SPEC-1 C-09/C-10
> 版本：v0.6（阶段 8：显存证据列 + 真实 token 口径）｜状态：draft｜契约基线：C-09、C-10、C-13（修订）｜更新：2026-11-21



## 0 开工前置：架构校正与裁决（4-G0）

> 阶段 4 的第一件事**不是补指标，是先把架构上的硬伤修掉**。4-G0 八项（架构五项 + 代码质量评审新增三项）全过才允许进 4-G1；§0.3/§0.4 八项裁决未回复时按"建议默认"执行，回复后按裁决结果覆盖并在此记录一行。

### 0.1 评估摘要（开工口径）

| 维度 | 现状 | 关键数字 |
|---|---|---|
| 进度 | 阶段 0–3 已落地并推送（`origin/1/2/3`） | 分支 `3` @ `70f3a62`，工作区干净 |
| KPI | K-4/5/6/7/8/9 达标；K-1/2 仅 P1 档；K-3 未达 | 吞吐 289.4→251.7 tok/s（0.87×）；KV 85.4%；调度 0.212% |
| 代码 | 4,646 行 / 11 模块 / 依赖 6 | 低于 6,000 下限；单文件最大 `scheduler.cpp` 1,004 行 |
| 测试 | CTest 17/17（4.35 s）、Python 19/19、CI 三闸门 PASS | ASan/TSan 未进自动闸门 |
| 指标 | 40 余个 `llmopt_*`，缺 K-1/K-2 sum/count、K-5 碎片、K-6 峰值、K-8 峰值、K-9 verdict | 与本文 §1.3 的 `llm_*` 命名不一致 |
| 证据 | 23 份入库，未标定写 `NA`、不宣称碎片率为 0 | GPU 峰值是全卡口径；原始两轮数据在 `/tmp` |
| 风险 | 零锁模型缺 TSan；热路径存在阻塞与全局锁 | 见 §0.2 |

结论：**控制面正确性可信，服务边界与指标口径不可信**。阶段 4 若直接跑矩阵，等于用一套自己都没对齐的口径测一遍，再返工。

### 0.2 架构校正八项（4-G0-1…4-G0-8）

| # | 问题（证据） | 参考做法 | 本项目目标需求 | 优先选择 | 验收 | 归属 |
|---|---|---|---|---|---|---|
| 4-G0-1 | 指标热路径全局锁 + 字符串键（`metrics.cpp:56-79`；调度每 tick 15+ 次 `set`） | Prometheus client 的每线程分片计数 + 渲染归并；vLLM 的 per-process counter | R-01：热路径零锁、零分配；`/metrics` 能被标准抓取端解析 | 保留字符串 API 给冷路径，热路径改固定 `MetricSlot` 原子数组；渲染补 `# TYPE`/`# HELP` | `metrics_format_test`：TYPE 全覆盖；并发压测下调度 p99 不退化；`/metrics` 与 CSV 计数差 ≤0.1% | 阶段 4 |
| 4-G0-2 | 回包阻塞 2 s（`sink.cpp:12-19`，调用点 `scheduler.cpp:961`） | Envoy/nginx 的 slow-consumer 策略：慢客户端断开，不阻塞生产者 | R-02/R-06：执行线程不得阻塞；一路慢读不能拖累他人 | `try_push` 非阻塞；队列满即标 `Aborted` + 断开 + `llmopt_sink_overflow_total`，**不丢帧** | `backpressure_test`：慢读 1 帧/s 时另一路 ITL p99 退化 ≤10%；无悬挂连接 | 阶段 4 |
| 4-G0-3 | 峰值显存是全卡口径（`phase3_manifest.json` 4095 MiB） | cgroup / 进程级 RSS；`nvidia-smi --query-compute-apps` 按 pid | K-8 必须可归因到本进程，采不到写 `unknown` | 进程级 RSS + 本进程 VRAM；两条都进 `/metrics` | `resource_peak_test`：RSS/VRAM 有值或 `unknown`，无编造；30 min 采样无跳变 | 阶段 4 |
| 4-G0-4 | 依赖表失真（`concurrentqueue` 已引入但 Mailbox 用 `deque+mutex`；Google Benchmark 未接入） | 依赖表 = 实际链接表；dev-only 依赖单列 | N-08/依赖预算：≤5 个直接链接，纸面项一律删 | 删 `concurrentqueue`（保留兜底实现）；Google Benchmark 要么接入微基准，要么从 §1.3 撤掉 | `size_dep_check`：依赖清单与 CMake 实际链接一致 | 阶段 4 |
| 4-G0-5 | 术语口径漂移：`llm_*` vs `llmopt_*`；`used` 含 cached donor；K-2 把纯 decode 与 mixed 步混算 | vLLM 的 ITL 只统计 decode 步；Prometheus 命名带单位后缀 | 同一指标名在文档、代码、CSV 三处一致；K-2 可判 | 冻结 `llmopt_*`；`used/cached/held` 定义写死；K-2 只统计纯 decode 步并另报 mixed 占比 | 文档与代码逐字对齐；chunk 128 的 ITL 按新口径复算 | 阶段 4 |
| 4-G0-6 | 准入拒绝发生在调度线程：`scheduler.cpp:307-311` 只 `finish(Error)`，`http_server.cpp:167` 非流式错误一律 200。实测 64 并发：64×200，11 ok / 53 error / queue_full=0 | OpenAI/Envoy 在入口限流，回 429 + `Retry-After`，不进推理队列 | C-03：客户端必须能区分「被限流」与「执行失败」；K-4 并发档不得把限流算作失败 | **准入预算前移**：HTTP 层先查 `running_+waiting_` 配额，满则 `send_error(QueueFull)` 并计入 `queue_full`；调度内拒绝保留为兜底 | 64 并发 × 256 token：429 数量 = 超出配额数；`queue_full` 与 `error` 计数不混；客户端退避后成功 | 阶段 4 |
| 4-G0-7 | runner 五级句柄手工逆序释放（`runner_llama.cpp:193-218`），异常路径无兜底 | `unique_ptr` + 自定义 deleter，声明顺序即释放逆序 | R-03 资源单点可控；soak 后 RSS 不涨 | 五个句柄改 RAII 包装，`release()` 只做 `reset()`，析构调 `release()` | ASan 下注入异常/中途返回不泄漏；soak 200 请求 RSS 持平（已实测 577,348→577,380 kB） | 阶段 4 |
| 4-G0-8 | 错误面三处：空 prompt 回 `PROMPT_TOO_LONG`；采样越界只说「非法或越界」；chat 端点 404＋「阶段 1 未启用」 | JSON Schema 逐字段 `loc`；已存在未开放端点回 501 | 错误可定位、语义码不混用；C-03 里 NotSupported 目前锁 404 | 空 prompt→`InvalidJson`（**已落地**）；越界消息带字段名与合法区间（**已落地**）；文案改「阶段 4 未开放」（**已落地**）；`NotSupported`→501 属 **C-03 契约修订**，未获批准前保持 404 | 17 例探测脚本语义断言通过；`http_status` 单测覆盖 11 个码；501 待契约修订后补 | 阶段 4 |

### 0.3 待裁决四件（附参考做法与建议默认）

> **裁决记录（2026-11-20）**：用户确认「建议默认，好调整完就行」，D4-A…D4-D **全部按建议默认执行**，不再等待回复；后续如反向，按本表「若反向选择」列回滚并在此追加一行。

| # | 事项 | 参考做法 | 目标需求 | 建议默认（已采纳） | 若反向选择 |
|---|---|---|---|---|---|
| D4-A | 指标命名 | Prometheus 命名规范（`_total`/`_ms`/`_ratio` 后缀）；项目历史一致优先于规范完美 | 历史证据可追溯、脚本不返工 | **冻结 `llmopt_*`**，本文 §1.3 表改名 | 改 6 个脚本 + 23 份 evidence 的解析键，约 1 天 |
| D4-B | K-3 负结果怎么表述 | vLLM/SGLang 的组批收益随模型增大而上升；小模型 decode 受固定开销支配 | 给出"收益边界"而非"补考达标" | **矩阵加长 prompt 与更高并发对照；7B 档仅在 L2 放行时跑**，不造达标 | 只测 0.5B，则结论必须写成"本档不成立"，且 K-3 标记为 negative result |
| D4-C | A-11 行数下限 | 原区间 6–10k 的意图是"防范围失守"，上限才是硬约束 | 上限硬、下限软 | **下限改 4,500 为止损线**；阶段 6 服务化补回 6k | 为凑行数加功能，违反"不为指标写代码" |
| D4-D | K-2 在 chunked prefill 下的口径 | vLLM 的 ITL 只统计 decode；prefill 另算 TTFT 分段 | 门槛可判、不被 mixed 步污染 | **只统计纯 decode 步**，mixed 步单列占比 | 保留混算，则 3×p50 门槛失效，需改绝对 p99 |

### 0.4 架构评审四问（附参考做法与建议默认）

> **裁决记录（2026-11-20）**：与 §0.3 同步确认，D4-E…D4-H **全部按建议默认执行**。

| # | 问题 | 参考做法 | 目标需求 | 建议默认（已采纳） | 反对方的代价 |
|---|---|---|---|---|---|
| D4-E | Core 与 Executor 是否合并为一个线程 | llama.cpp server 单执行线程；vLLM 把 executor 独立出去 | 少一跳、少一条死锁路径 | **不合并**，先加 step trace；交接占比 >3% 再议 | 现在合并 = 改 C-12 契约 + 重刷 TSan 基线 |
| D4-F | sink 溢出策略 | Envoy 慢消费者直接断流；gRPC 的 flow control 背压 | 语义正确优先于吞吐 | **断开并计数**，不丢帧 | 降级非流式：实现更复杂、首 token 延迟翻倍 |
| D4-G | 指标重写放阶段 4 还是 6 | 先定口径再跑数，是 benchmark 的常规顺序 | 矩阵数据只跑一遍 | **放阶段 4（4-G0-1）** | 放阶段 6：矩阵要用旧口径跑两遍 |
| D4-H | 兼容面边界 | OpenAI 兼容层的常用子集（chat/completions/models） | 服务完成度 75%，不做产品化 | **锁死三点**，tools/vision 返回 400 | 扩面 = 进入产品化，越 01 §2.3 边界 |

### 0.5 顺序与门禁

- 顺序：**4-G0（1…8）→ 4-G1…4-G6 → 4-G8 收口**；4-G0 不改对外契约（除 C-13 修订），不动调度语义；4-G0-6 只前移准入判定，不改调度策略。
- 门禁：4-G0 每项配用例；八项全过后 `ctest -L fast` 与 `run_ci.py --mode fast` 全绿，才允许开始矩阵工具开发。
- 记录：4-G0 完成后在 `docs/阶段4-实现记录.md` 写"校正前后对比"（延迟、锁、口径三列），作为阶段 4 的第一份证据。

**当前进度（2026-11-20，分支 `5`）**

- 4-G0-2 已落地：`BoundedSink::push_frame` 非阻塞、满即关闭并计数；`llmopt_sink_overflow_total` 已预置；`unit_sink` 四例。
- 4-G0-1/5 已落地：`MetricId` 固定表 + 8 路线程分片原子计数；`IMetricsSink` 字符串 API 只留冷路径；`# TYPE`/`# HELP` 每 family 一次；补 K-1/K-2 sum/count、K-5 碎片率、K-6 峰值、K-7 带 reason、K-9 verdict；`unit_metrics` 9 例 + `integration_server_metrics`。
- 4-G0-6 已落地：准入预算前移为 `max_concurrency + queue_capacity`，含在飞提交；HTTP 429 + `Retry-After: 1`；`queue_full` 与 `error` 分开计数；`admission_test` 2 例 + `integration_server_admission`（8 并发、预算 3）。
- 4-G0-3 已落地：`ResourceSampler` 每 500 ms 采进程 RSS（Windows `GetProcessMemoryInfo` / Linux `/proc/self/statm`），VRAM 每 4 tick 按 pid 查 `nvidia-smi --query-compute-apps`；采不到写 NaN + `llmopt_vram_probe_ok=0`；`unit_resource` 5 例 + `integration_server_metrics` K-8 断言 + `tests/tools/test_peak_sampler.py`；`scripts/bench/peak_sampler.py` 落 `peak_samples.csv`。
- 4-G0-4 已落地：`concurrentqueue` 从 `cmake/deps.cmake` 与链接表移除，Google Benchmark 撤出依赖表；`docs/ci/ci-spec.md` 的 SYSTEM 豁免句同步删除。
- 4-G0-7 已落地：`runner_llama.cpp` 的 model/context/batch/sampler 全部改 `unique_ptr` + deleter；`release()` 只 reset，不再手工配对 `llama_*_free`；`runner_raii_test`（仅 `LLMOPT_ENABLE_LLAMA=ON` 构建，WSL ASan 跑）。
- 4-G0-8 未完成：前三项已落地，chat 501 仍待 C-03 修订批准，当前保持 404 + 「阶段 4 未开放」文案。
- 4-G1/4-G2/4-G3/4-G6 已落地：`scripts/bench/matrix_runner.py` + `tools/bench/matrix.py`（JSON 配置 `scripts/bench/matrix/phase4.json`；7 并发档 × 2 prompt 档、两轮、档间重启、不可行档 `infeasible`）；`matrix_report.csv` 列与 §1.6 一致；`deviation.csv` 判 TTFT/ITL p50 ≤10%、吞吐 ≤7%；`metrics_p*_r*.prom` 归档 + `aggregate.py --check-metrics` 对账；`--step-trace` JSONL 归档后由 `step_summary.csv` 给步级 p50/p99；`--mixed` 长短 1:1 分组入表。
- 4-G4/4-G5 已落地：`check_deps.py`（声明 = 实际链接，直接依赖 4 个）、`size_dep_check.py`（5,299 行 / 4 模块 / 4 依赖）、`scope_scan.py`（无分布式/CUDA/训练/PD，测试全登记）；三者已并入 `check_code.py`，每次代码闸门自动跑。
- 4-G0-2 证据补全：`scripts/bench/slow_client.py` 落 `backpressure.csv`（快客户端 ITL p99 + 溢出计数）。
- 4-G8 收口：验收映射、三次 review、偏差口径与复现入口写入 `docs/阶段4-实现记录.md` §6/§7。

**验证状态（2026-11-20，阶段 0–3 作为整体）**

| 入口 | 范围 | 结果 |
|---|---|---|
| Windows `ctest -L fast`（分支 5） | unit 78 + contract 35 + integration 8 | **121/121 通过** |
| Windows `run_ci.py --mode fast`（分支 5） | docs / tests / code 三闸门 | **PASS**（重复率 3.23% 警告） |
| Linux（WSL）`ctest -DLLMOPT_COVERAGE=ON` | unit 60 + contract 33 + integration 3 | **96/96 通过，1.9 s** |
| WSL llama.cpp `ctest -L fast`（分支 5） | unit 81 + contract 35 + integration 8 | **124/124 通过，11.2 s**（含 3 例 `runner_raii`） |
| WSL llama.cpp 官方矩阵 | 7 档 × 2 prompt 档 × 2 轮 × 30 条 × 64 token | **14/14 行 `ok`；`--check-metrics` PASS；`--check-deviation` 12/14 `high`**（CPU 饱和，重复性不达标） |
| Linux nightly `run_ci.py --mode nightly --base origin/3` | docs / tests / code / diff / coverage | **五闸门全 PASS；`src/` 行覆盖 2,091/2,447 = 85.5%** |
| Windows nightly（同参数） | 同上，coverage 因无 gcov SKIP | **四 PASS + coverage SKIP（留痕）** |

说明：增量为 0/0=100%，因为当前 HEAD 与 `origin/3` 相同、改动尚未提交；提交后按 `origin/3...HEAD` 重算。覆盖率闸门、e2e 真实服务契约、`docs/testing.md` 登记已同时落地，后续测试必须先登记再进闸门。



### 0.6 代码质量评审结论（2026-11-20）

> 全文：`docs/代码质量评审.md`（10 条，含 `file:line` 与参考做法）。五维评分：内存 C+、参数 B、错误 C、RAII C+、抽象 B−。

| 维度 | 结论 | 代表证据 | 处置 |
|---|---|---|---|
| 内存管理 | 无裸 new/delete；C 句柄手工逆序释放 | `runner_llama.cpp:193-218` | 4-G0-7 |
| 参数校验 | 配置 11 项全有范围与中文报错；协议 6 个采样参数有 min/max | `config.cpp:261`、`protocol.cpp` | 保留；补字段级报错（4-G0-8） |
| 错误信息 | `ErrCode`→状态码单点映射；但错误发生在 handler 之后无回写通道 | `scheduler.cpp:307` + `http_server.cpp:167` | 4-G0-6 结构性修 |
| 指针与 RAII | `LogitsView`/`running_ptrs()` 裸指针有生命周期注释约束，限于单次 decode/pump | `include/llmopt/scheduler.hpp` | 阶段 4 尾补断言 + TSan |
| 抽象与接口 | `ModelRunner`/`ResponseSink` 可注入；缺 `IMetricsSink` | `metrics.cpp:56-79` | 随 4-G0-1 |
| 依赖 | Mailbox 已改 `deque+mutex+cv`，仍链 `concurrentqueue` | `cmake/deps.cmake` | 4-G0-4 |

真正阻塞矩阵的是三条：**准入拒绝回 200**（4-G0-6）、**runner 无 RAII 兜底**（4-G0-7）、**错误码与文案误导用户**（4-G0-8）。其余为 P1/P2，已在评审文档里给出归属。

### 0.7 真实运行路径测试结论（2026-11-20）

> 全文：`docs/真实路径测试记录.md`；原始明细：`evidence/phase4/realpath.json`。WSL + 真 Qwen2.5-0.5B Q4 + CUDA，17 例探针，两轮一致。

| 项 | 结果 |
|---|---|
| 通过率 | **16/17**（唯一 FAIL 是 `queue_pressure`，16 并发恰好吃满 8+8，未触发 429） |
| 首 token / 稳态 | TTFT 76.4 ms；ITL 3.11–4.80 ms |
| 并发 8 × 200 token | **756.7 tok/s**（阶段 2 并发 1 为 289.4 tok/s，2.6×） |
| 慢客户端 | 快端 p99 16.54 ms、无错误、`sink_overflow=0`（队列未被填满，策略未真触发） |
| soak | 200 请求：507,016 → 577,348 → **577,380 kB**（持平，不判泄漏） |
| 关闭 | SIGTERM 4 s 内退出，`server stopped` + `kv invariants=ok`，无 abort |
| 反面证据 | 64 并发 × 256 token：**64×200**、11 ok / 53 error / queue_full=0 → 4-G0-6 的直接证据 |

两点必须写进矩阵口径：
1. **K-3 负结果不作定论**。并发 8 的 756.7 tok/s 高于并发 1 的 289.4，说明阶段 2 的 0.87× 是负载/配置所致；矩阵必须加「同 prompt、同输出长度、仅改并发」对照档（D4-B 建议默认）。
2. **429 语义在修好 4-G0-6 前无法测**。现在的「限流」表现为 200+error 体，K-4 的并发档会把限流算成失败样本；4-G0-6 落地后本探针的 `queue_pressure` 才能改成确定性断言（并发 = 2×(运行+排队)+1）。



## 1.1 功能范围

- 三条路径采集 K-1…K-9：引擎内步级打点、HTTP 层请求生命周期、进程外周期采样；统一暴露到 `GET /metrics`（K-1…K-9）。
- 实验矩阵驱动：并发 {1,2,4,8,16,32,64} × prompt 两档（short ≤128 / long ≤512）+ 长短混合负载；档间 warmup、重复轮次、超时与失败判定（K-1/K-2/K-3/K-6）。
- 结果聚合：JSONL + metrics 快照 + 采样 CSV → 格级 CSV，分位数 / 吞吐 / 利用率 / 调度占比；混合长短分组不混算（K-3/K-4/K-5）。
- 复现收口：配置快照 + 种子 + 软件版本 + 门控快照哈希归档；两轮同档偏差对账；一条命令从零到出图（K-9、N-02）。
- 规模与边界复核：生产行数 / 模块数 / 依赖数核对；非目标清单复核（4-G4、4-G5）。
- 不做新调度功能、不做 PD 分离、不改 SPEC-1 共享契约；缺打点回写对应 SPEC，不事后补口径。
- 指标分两套：在线 `/metrics` 仅 counter+gauge（含 sum/count），不含分位数；离线 Benchmark Results 存原始时间戳/样本与 p50/p95/p99、吞吐、配置快照，分位数由基准工具算，不从 `/metrics` 取。



## 1.2 架构

```
 bench 客户端 ──HTTP/SSE(P0/P1/Pn)──▶ http 池线程 ──b 生命周期计数 ──┐
                                                                     │
 SUT 进程 ──a 调度/执行回写(步级耗时/块计数/抢占) ────────────────────┼─▶ registry
                                                                     │   (per-thread 计数归并, 热路径无锁)
 进程外采样器 ──c CPU 时间 / RSS / VRAM, 500ms ──▶ samples.csv ───────┤        │
                                                                     │        ▼
 JSONL 逐请求记录 ──────────────────────────────────────────────────┘  expose_text()
                                                                          │ .prom 归档
                                                                          ▼
                                              aggregate.py ──▶ matrix_report.csv ──▶ plot.py ──▶ PNG
```

- **a 引擎内**：调度线程 `schedule()` 前后取单调时钟；runner 回写忙序列 / 块计数；状态迁移记抢占事件。
- **b HTTP 层**：HTTP 线程只记入队 / 出队时刻，不读 KV/context（ADR-06）；handler 只做 atomic 计数与 gauge 回写，不调推理接口；TTFT/ITL 样本由客户端记 JSONL，服务端不算分位数。
- **c 进程外**：独立线程 / 子进程读 `/proc`、`GetProcessTimes`、`nvidia-smi`；采不到写 `unknown`，不编造。峰值对照 K-8 硬门限 ≤4.5 GiB；启动预算按 05 §5 快照：free VRAM 4.855 GiB × `safety_ratio` 0.80 ≈ 3.884 GiB。
- **聚合方**：`bench/aggregate.py` 只读归档文件，分位数在聚合侧算；`registry` 不回写调度状态。
- **落盘**：`bench/results/phase4/` 下 JSONL、`.prom` 快照、`samples/`、`matrix_report.csv`、`plots/`、`repro_manifest.json`。
- **时钟**：同一条时间线差值只用单调时钟（Linux `CLOCK_MONOTONIC_RAW` / Windows QPC）；墙钟 UTC 只入 manifest。



## 1.3 接口设计

```cpp
namespace metrics {
using Labels=std::vector<std::pair<std::string,std::string>>;
struct Snapshot{};
class IMetricsSink {
public: virtual ~IMetricsSink()=default;
  virtual void inc_counter(const std::string&,uint64_t,Labels)=0;
  virtual void set_gauge(const std::string&,double,Labels)=0;
  virtual void observe_sample(const std::string&,double,Labels)=0;
  virtual Snapshot snapshot() const=0;
  virtual std::string expose_text() const=0; };
class EngineStats { public: void record_schedule_step(double) noexcept; Snapshot snapshot() const; void reset_for_tier(int); };
}
```

在线 `/metrics` 仅 counter+gauge（sum/count 与窗口 gauge），无分位数；离线结果存原始样本与分位数、吞吐、配置快照。

> 命名口径（D4-A 冻结）：全部使用 `llmopt_*`；下表已按代码实际序列名逐行对齐。历史 evidence 里的旧键只读，不再改名。

| K | 指标名（口径） | 在线/离线 |
|---|---|---|
| K-1 | 在线 `llmopt_ttft_ms_sum`（毫秒，counter float）/ `llmopt_ttft_ms_count`；TTFT p50/p95 离线 | 两者 |
| K-2 | 在线 `llmopt_itl_ms_sum` / `llmopt_itl_ms_count`（仅纯 decode 步）；`llmopt_decode_steps_total` / `llmopt_mixed_steps_total` 报污染比；ITL p50/p99 离线 | 两者 |
| K-3 | 在线 `llmopt_tokens_generated_total`、`llmopt_tokens_per_sec`（启动至今均值）；窗口吞吐离线 | 两者 |
| K-4 | 在线 `llmopt_scheduler_ns_total` / `llmopt_scheduler_steps_total`、`llmopt_schedule_step_ns_last`、`llmopt_sched_cpu_ratio`；步级 p99 由 `--step-trace` JSONL 离线复算 | 两者 |
| K-5 | `llmopt_kv_used_blocks` / `llmopt_kv_cached_blocks` / `llmopt_kv_free_blocks` / `llmopt_kv_held_blocks`（= used-cached）/ `llmopt_kv_internal_frag_ratio`（含 max_tokens 预留） | 在线 |
| K-6 | `llmopt_requests_running` / `llmopt_requests_running_peak` / `llmopt_requests_queued`（旧名 `llmopt_running_sequences` / `llmopt_queue_depth` 保留为 deprecated 别名） | 在线 |
| K-7 | `llmopt_preemptions_total{reason="admission\|chunk_plan"}` | 在线 |
| K-8 | `llmopt_rss_peak_mib` / `llmopt_vram_peak_mib`（NaN = 本平台采不到）、`llmopt_vram_probe_ok`；峰值对照 K-8 硬门限 ≤4.5 GiB | 在线 |
| K-9 | `llmopt_gate_verdict_total{verdict}`（ELIGIBLE/DEGRADED/BLOCKED）、`llmopt_degraded`；门控默认 `off` | 在线 |



| 错误变体 | 触发场景 | 建议动作 |
|---|---|---|
| CounterOverflow | uint64 计数回绕 | 回绕归零记日志；进程寿命内不预期触发，不崩 |
| SampleInsufficient | 有效样本 n<30 | 分位数列 `NA`，`note=insufficient_samples`，不插值 |
| AggregateMissingColumn | CSV 缺扩展列 / 旧列被改名 | 聚合中止报缺列；旧列只读不改名 |
| TierTimeout | 单档超时 / 队列 429 / 门控拒绝 | 记 `failed`/`infeasible` + reason，继续下一档；失败样本不进分母 |



**1.3.1 复用件落点**

| 复用件 | 来源 | 处置 | 落在本阶段接口/模块 | 改造点(≤12字) |
|---|---|---|---|---|
| 自研文本 exposition（约 100–150 行 + `IMetricsSink`） | 02 ADR-08 / 04 §4.4 | 自研 | `/metrics` 输出、`llmopt_*` 指标族 | counter/gauge only |
| spdlog 1.17.0 | 02 §4 / 04 §4.5 | 直接复用 | 跑数日志、异常 | 不引独立 fmt |
| Google Benchmark 1.8.3 | 04 §6 | 直接复用 | 单步时延/调度占比微基准 | 禁跑长测 |
| doctest 2.4.11 | 04 §6 | 直接复用 | 指标聚合单测 | 无 |
| nlohmann/json 3.11.3 | 02 ADR-03 | 直接复用 | manifest/快照落盘 | 无 |
| 自研 CSV 聚合 | — | 自研 | `experiment_matrix.csv` | 列名冻结 |

改造细节与验证证据见 [04 库选型与验证记录](规划/04_库选型与验证记录.md) §4.4、[05 能力门控](规划/05_能力探测与容量门控.md) §5。



## 1.4 核心逻辑骨架

```
# 实验运行循环
读 matrix.yaml → 校验模型 sha256 / 配置哈希 / 种子 → 起 SUT 至 Ready
for tier in 并发档 × prompt 档 × (chunk 档, 抢占开关):
  warmup 5 请求 (dropped=true, 不聚合)
  for round in repeats: 并发发压 → 落 JSONL(P0/P1/Pn) → 路径 a/b 打点
  抓 /metrics 快照归档; 路径 c 采样落 samples.csv; 档间 reset_for_tier(policy)
混合负载: 长短 1:1 一组, 分组出分位数
aggregate.py → matrix_report.csv → 两轮对账 → plot.py 出图 → 写 manifest
```

```
# 指标快照与清零
record_step(ms): 写当前线程局部样本缓冲 (无锁, 无堆分配)
snapshot(): 短临界区归并各线程计数 → 不可变 Snapshot; 渲染不持锁
reset_for_tier: 仅无在飞序列时; gauge/counter 按档清; token 累计 counter 按 policy 保留
退出前输出最后一次快照 (对齐 C-10 关闭顺序)
```

```
# 复现性校验（两轮偏差）
仅对 status=ok 且两轮 n≥30 的档对账; dev(x)=|r1-r2|/max(r1,r2)
TTFT/ITL p50: dev ≤ 0.10; 吞吐: dev ≤ 0.07; PP 参照 <2%
超差 → 加跑第 3 轮取中位数; 仍超差 → deviation_flag=high, 标低置信度
校验和/config_hash/种子/prompt_set_hash 不一致 → 不对账, 记 na
```



## 1.5 红线

- R-01：调度 / 执行 / handler 热路径禁止加互斥锁；高频回写只用 atomic 与线程局部样本缓冲。
- R-02：插桩不得改变被测对象；步级插桩开销预算 ≤2% 步墙钟，超预算先降采样高频样本，不丢抢占 / 拒绝事件。
- R-03：`dropped` / `error` / `failed` / `infeasible` 样本禁止进入分位数与吞吐分母；不可行档不回填降级配置数字。
- R-04：禁止跨时钟源算延迟；TTFT 失真（负间隔 / 离群长间隔）样本丢弃并计数，不用墙钟补算。
- R-05：标签取值封闭枚举；请求 ID / 时间戳禁止进 metrics 标签（防基数膨胀）。
- R-06：分位数只在聚合侧由逐请求样本算；`/metrics` 不暴露服务端算好的 p50/p95/p99。



## 1.6 示例

CSV 表头 + 两行【示例数据】（0.5B 历史示例，只用于字段/精度示意，**不是 8B 结论**；8B 实测见 `docs/8B-重测记录.md` §5）：

```csv
concurrency_p,prompt_tier,status,n_requests,ttft_p50_ms,ttft_p95_ms,itl_p99_ms,total_tok_s,tok_accounting,kv_internal_frag_ratio,sched_cpu_ratio,vram_peak_mib,vram_peak_scope,vram_device_peak_mib,reason,deviation_flag
1,short,ok,30,86.4,121.7,18.6,304.28,prompt_metrics,0.04,0.021,4080.33,buffers,6554.0,na,ok
4,short,ok,30,142.9,218.4,41.5,484.29,prompt_metrics,0.07,0.038,4080.33,buffers,6554.0,na,ok
```

吞吐口径（阶段 8 修订）：

- `total_tok_s` 的 prompt 部分必须来自服务端：`llmopt_prefill_tokens_total`（实算）+ `llmopt_prefix_hit_tokens_total`（命中复用），再加生成 token。**禁止用字符数估算**——中文 BPE 下字符数约为真实 token 的 2.6 倍，会把吞吐整体抬高。
- `tok_accounting`：`prompt_metrics` 表示上述真实口径；`char_estimate` 只允许在指标缺失时出现，且该行吞吐不得单独引用。

显存三列口径（阶段 8 新增）：

- `vram_peak_mib`：`CUDA0 model + KV + compute buffer` 之和，是**本进程可归因**的常驻显存；WSL 下 `nvidia-smi --query-compute-apps` 拿不到 pid，故以 llama.cpp 加载日志为准。
- `vram_peak_scope`：`buffers`（三项齐全）/ `buffers_partial`（加载中断或缺 compute）/ `unknown`（stub 或无 CUDA 日志，禁止把 0 当实测）。
- `vram_device_peak_mib`：`/metrics` 的 `llmopt_vram_device_mib` 峰值，**全卡口径**，只用于观察桌面/其它进程扰动，不能当前者使用。

`/metrics` 片段【示例数据】：

```prometheus
# HELP llmopt_tokens_generated_total 已采样并下发的生成 token 总数
# TYPE llmopt_tokens_generated_total counter
llmopt_tokens_generated_total 18432
# HELP llmopt_kv_internal_frag_ratio 块表内部碎片率（含 max_tokens 预留）
# TYPE llmopt_kv_internal_frag_ratio gauge
llmopt_kv_internal_frag_ratio 0.0700
```

配置项表（`matrix.json`；项目不引 YAML 库，§0.3 D4-D 已记录偏差）：

| 配置项 | 默认 | 说明 |
|---|---|---|
| `matrix.concurrency` | `[1,2,4,8,16,32,64]` | 并发档清单，顺序即执行顺序 |
| `matrix.repeats` | `2` | 重复轮次；超差加跑第 3 轮 |
| `matrix.warmup_requests` | `5` | 每档丢弃的预热请求数 |
| `matrix.tier_timeout_ms` | `600000` | 单档墙钟超时（10 分钟） |
| `matrix.dev_ttft_itl_p50` | `0.10` | 两轮 TTFT/ITL p50 偏差阈值 |
| `matrix.dev_throughput` | `0.07` | 两轮吞吐偏差阈值 |
| `matrix.n_gpu_layers` | `0` | 上卡层数；>0 时写入 argv，保证 manifest 可复现 |
| `matrix.threads` | `0` | CPU 线程；0 表示交给后端默认 |
| `matrix.kv_cache_type` | `f16` | `f16/q8_0/f32`；写入 argv 与容量估算 |
| `matrix.donor_slots` | `4` | 与服务默认一致；**始终写入 argv**，避免“省略即默认”导致 manifest 与实跑不一致；0 必须同时关前缀缓存 |
| `matrix.disable_prefix_cache` | `false` | 关前缀复用，KV 压力档使用 |
| `matrix.chunk_size` | `128` | 32/64/128/256；压力档调小以触发 chunk 权衡 |



## 1.7 验收与证据链

| 门 | 验收口径 | 测试登记名 | 命令 | 证据物 |
|---|---|---|---|---|
| 4-G0-1 | 热路径零锁；`# TYPE`/`# HELP` 覆盖全部指标；`/metrics` 与离线 CSV 计数差 ≤0.1% | `metrics_format_test` | `ctest -R metrics` | `metrics_format.txt` |
| 4-G0-2 | 慢读 1 帧/s 时另一路 ITL p99 退化 ≤10%；溢出计数可见；无悬挂连接 | `backpressure_test` | `scripts/bench/slow_client.py` | `backpressure.csv` |
| 4-G0-3 | RSS/VRAM 为进程级或 `unknown`；30 min 采样无跳变 | `resource_peak_test` | `scripts/bench/peak_sampler.py` | `peak_samples.csv` |
| 4-G0-4 | 依赖清单与 CMake 实际链接一致；无纸面依赖 | `size_dep_check` | `scripts/ci/check_deps.py` | 依赖核对记录 |
| 4-G0-5 | 文档、代码、CSV 三处指标名逐字一致；ITL 按纯 decode 口径可复算 | `metrics_format_test` | 同上 | `metrics_format.txt` |
| 4-G0-6 | 超配额并发返回 429 + `Retry-After`；`queue_full` 与 `error` 计数不混；退避后成功 | `admission_test` | `scripts/bench/realpath_probe.py --case queue_pressure` | `admission.csv` |
| 4-G0-7 | 异常/中途返回不泄漏；析构即释放；ASan 无 leak | `runner_raii_test` | `ctest -R runner`（ASan 构建） | ASan 日志 |
| 4-G0-8 | 空 prompt→`InvalidJson`；越界消息含字段名；chat 文案改「阶段 4 未开放」（501 待 C-03 修订） | `protocol_error_test` | `ctest -R protocol` | 错误体快照 |
| 4-G1 | 七档并发 × 两档全部出 CSV 行；不可行档有拒绝/退出记录，进程无崩溃 | `matrix_run_test` | `scripts/repro.sh bench/matrix/phase4.yaml` | `matrix_report.csv` |
| 4-G2 | 两轮 TTFT/ITL p50 偏差 ≤10%，吞吐 ≤7%；模型/配置/种子归档 | `repro_deviation_test` | `aggregate.py --check-deviation` | 偏差表 + `repro_manifest.json` |
| 4-G3 | 在线 counter/gauge/sum-count 可取且与 CSV 计数一致；分位数与步级 p99 由离线 CSV 复算 | `metrics_exposure_test` | `curl localhost:8080/metrics` | `.prom` 快照 |
| 4-G4 | 生产代码 6,000–10,000 行；模块 ≤10；直接链接第三方依赖 ≤5 | `size_dep_check` | 行数 / 依赖扫描脚本 | 核对记录 |
| 4-G5 | 无分布式通信、无自定义 CUDA 算子、无训练通路、无 PD 分离形态 | `scope_scan` | 依赖扫描 + 目录复核 | 目录清单 |
| 4-G6 | 混合长短一轮完成；长/短 TTFT、ITL 分组入表不混算 | `mixed_load_test` | `matrix_runner.py --mixed` | CSV 分组列 |
| N-07 | 无 GPU 仅凭归档 JSONL/快照/采样重建 CSV 与图表 | `aggregate_offline_test` | `aggregate.py --offline <归档目录>` | 重建 CSV/PNG |

**阶段 4 完成判定** = 七档×两档全跑完 且 K-1…K-9 可从 `/metrics` 复算一致。



## 1.8 开放决策点

| # | 决策 | 建议默认 |
|---|---|---|
| D4-1 | 指标后端 | v0 自研文本 exposition（`IMetricsSink` 隔离，约 100–150 行）；prometheus-cpp 1.3.0 备选，未采用（ADR-08 已翻案为自研文本） |
| D4-2 | 原始样本记录口径 | TTFT/ITL 参考阈值 `[1,5,10,25,50,100,250,500,1000]` ms；步级耗时 `[0.5,1,2,5,10]` ms；样本全量落离线文件，在线不聚合分位数 |
| D4-3 | 并发 64 档是否默认硬跑 | 标记探测档；并发上限由 L2 判据（ELIGIBLE/UNSUPPORTED/BLOCKED 三态）决定，不写死路数；预判不可行则记 `infeasible` 不硬跑，实测边界点入 manifest |
| D4-4 | 档间清零策略 | gauge/counter 按档清；token 累计 counter 跨档保留（`reset_policy` 可配） |
| D4-5 | 日志后端 | spdlog 1.17.0（header-only，内嵌 fmt）；metrics 层不直接依赖日志库 |
| D4-6 | 分位数是否允许在线近似 | 否，v0 一律离线精确聚合，在线只给 sum/count |

## 变更

- v0.4 · 2026-11-20 · 并入代码质量评审（§0.6）与真实路径测试（§0.7）：4-G0 由五项扩为八项（新增准入 429、runner RAII、错误面收敛），§1.7 补三行验收；补 K-3 负结果反证与 RSS 平台证据。
- v0.3 · 2026-11-20 · 新增 §0 开工前置（评估摘要、4-G0 架构校正五项、八项裁决与参考做法），1.7 补 4-G0 验收行；阶段 4 顺序改为先校正后跑矩阵。
