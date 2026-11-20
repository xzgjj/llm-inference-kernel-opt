# 测试策略（唯一事实来源）

本文件是测试的注册表与策略说明。CI 不读 workflow 里的硬编码命令，只读这里的两张表：新增测试必须登记，未登记即失败。

## 1 分工

- **本文件**：有哪些测试、在哪、跑什么命令、需要什么环境。测试策略的唯一事实来源。
- **`tests/`**：产品测试。调度器、KV cache、后端、算子、端到端，随功能开发增长，计入覆盖率。
- **`ci/`**：CI 工具自测。验证闸门脚本本身（字数口径、注册表解析、Diff 判定），不计入产品覆盖率，也不代表功能正确。
- **`docs/ci.md` / `docs/ci/ci-spec.md`**：闸门与判定规则，不描述测试内容。

两类测试同一份注册表、同一套五列，但分表登记、分开统计。混在一起会让"覆盖率"把规则自测算成产品能力，这正是要避免的。



## 2 注册表



### 2.1 产品测试（`tests/`）

| Scope | Level | Runner | Args | Metadata |
| --- | --- | --- | --- | --- |
| `tests/tools` | `unit` | `python` | `-m unittest discover -s tests/tools -t . -v` | `fast`, `hermetic` |
| `tests/unit` | `unit` | `ctest` | `--test-dir build --output-on-failure -L unit` | `fast`, `hermetic` |
| `tests/contract` | `contract` | `ctest` | `--test-dir build --output-on-failure -L contract` | `fast`, `hermetic` |
| `tests/integration` | `integration` | `ctest` | `--test-dir build --output-on-failure -L integration` | `fast`, `hermetic` |
| `tests/e2e` | `e2e` | `ctest` | `--test-dir build --output-on-failure -L e2e` | `slow`, `needs-gpu` |

C++ 用例由 CTest 登记，`Args` 使用仓库根的 `build/`（CI 与本机约定一致）；本机另建目录时手动执行 `ctest --test-dir <dir>`，不改本表。CI 的非默认构建目录（sanitizer、覆盖率）用 `run_registry.py --build-dir <dir>` 覆盖表里的路径，表本身仍保持一条可独立执行的命令。

CTest 粒度是**用例**不是可执行文件：`doctest_discover_tests` 在构建后枚举 `--list-test-cases`，一个 `TEST_CASE` 注册成一条 CTest（形如 `unit_config.端口、上下文…`），当前近百条（另有 4 个进程级脚本）。因此 `ctest -N` 的数字就是用例数，`-R unit_config.` 可以单跑一个用例，`-j` 可以并行。

目录与命名约定见 [../tests/README.md](../tests/README.md)：`unit/` `contract/` `integration/` `e2e/` `perf/` 按层级分目录，Scope 一律写目录而不是单个文件，目录内新增用例自动被覆盖。



### 2.2 CI 工具自测（`ci/`）

| Scope | Level | Runner | Args | Metadata |
| --- | --- | --- | --- | --- |
| `ci` | `unit` | `python` | `-m unittest discover -s ci -t . -v` | `fast`, `hermetic`, `protected`, `infra` |

列定义：

- **Scope**：仓库相对目录，决定覆盖范围。CI 用它判断"某个测试文件是否已登记"。
- **Level**：`unit` / `contract` / `integration` / `e2e` / `perf` / `sanitizer`。表达"失败时能定位到多远"，不表达重要性。
- **Runner**：可执行程序，限定词表 `python` / `ctest` / `cmake` / `bash` / `nvcc`。禁止写绝对路径。
- **Args**：直接传给 Runner 的参数，必须能独立执行；不依赖 shell 通配符与环境变量。
- **Metadata**：逗号分隔标签，至少包含一个速度标签和一个隔离标签。

标签词表：

| 类别 | 取值 | 含义 |
| --- | --- | --- |
| 速度 | `fast`（≤30 s）、`slow`（>30 s） | 决定进入 PR 闸门还是 nightly |
| 环境 | `hermetic`、`needs-network`、`needs-gpu`、`needs-vm` | 决定由哪类 runner 承接 |
| 性质 | `protected`、`infra`、`perf-baseline`、`quarantine:<到期日>:<owner>` | 受保护/CI 自测/基线/隔离 |

`protected` 表示锁定已被确认的行为。删除、跳过或改期望值都需要 `behavior-change` 标签与行为负责人 review。`infra` 表示这是闸门自身的测试：它失败说明规则坏了，不是产品坏了。



## 3 分层策略

| Level | 覆盖什么 | 数量预期 | 运行位置 |
| --- | --- | --- | --- |
| unit | 单个函数/类，无 IO，无时钟依赖 | 多 | L1/L2，`fast` |
| contract | 模块边界不变量：调度器 ↔ 后端、请求 ↔ KV cache | 中 | L2 |
| integration | 多模块协同：一批请求在连续批处理下的生命周期 | 少 | L2 |
| e2e | 进程级：起服务、发请求、校验输出与吞吐 | 很少 | nightly / GPU 队列 |
| perf | 吞吐、首 token 延迟、显存占用 | 每指标一条 | nightly |
| sanitizer | 复用 unit/contract 用例，在 ASan/UBSan/TSan 下重跑 | 与上游同集 | L3 |

原则：下层能测的不要放到上层。一个 e2e 失败若无法指出是哪个模块坏了，说明缺 contract 层用例。



## 4 登记流程

1. 先确定归属：产品行为进 `tests/`，闸门脚本进 `ci/`，不要互相借目录。
2. 确定 Level：测试失败时，希望一眼看出坏在哪一层。
3. 建文件，命名 `test_*.py` / `*_test.cc` / `*_test.cpp`。
4. 在对应表里加一行，Scope 覆盖该目录；Args 先在本地原样跑通。
5. 本地执行 `python scripts/ci/check_tests.py`，确认覆盖检查通过。
6. 还没写好就不要加行——写进第 7 节待补齐清单。



## 5 一致性检查

`check_tests.py` 在 L0/L1 执行，逐项校验：

1. 两张表的表头五列齐全，每行五列，Level 与 Metadata 取值在词表内。
2. `tests/**` 与 `ci/**` 下每个测试文件必须被某条 Scope 覆盖；未覆盖则失败并打印路径。
3. 每条 Scope 内至少有一个测试文件；空 Scope 视为过期登记，必须删除。
4. `needs-gpu` / `needs-vm` / `needs-network` 不得进入 PR fast 计划。
5. `quarantine:` 标签必须带到期日与 owner，到期日不超过 14 天。
6. 有测试文件却没有任何登记 → 失败；仓库完全无测试 → 允许（初始化期）。

`run_registry.py` 读同一份注册表执行测试，表里的 Args 就是运行命令。两处不一致即 CI-DOC-006 违规。

覆盖率闸门（`scripts/ci/check_coverage.py`，规则 CI-COV-001/002）：nightly 用 `-DLLMOPT_COVERAGE=ON` 的构建目录跑完注册用例后取证，判 `src/` 行覆盖 ≥70%、本分支改动行 ≥80%。Windows 无 gcov 时该步 SKIP 并留痕，Linux/WSL 必须实跑；SKIP 不等于通过。2026-11-20 实测：`src/` 2,091/2,447 = **85.5%**，96 个用例全绿。

### 5.1 覆盖率闸门（CI-COV-001 / CI-COV-002）

覆盖率不是口号，是两条会红的规则：

| 规则 | 口径 | 阈值 | 实现 |
| --- | --- | --- | --- |
| CI-COV-001 | `src/` 总体行覆盖率 | ≥70% | `scripts/ci/check_coverage.py --min-line` |
| CI-COV-002 | 本次改动新增/修改的可执行行覆盖率 | ≥80% | `scripts/ci/check_coverage.py --min-diff` |

- 插桩：`cmake -DLLMOPT_COVERAGE=ON`（GCC/Clang 的 `--coverage`），只对 `src/` 计分；`include/`、`tests/`、`third_party/` 不计——头文件里的模板会按 TU 摊开，混进总分会虚高。
- 采集：CI `coverage` 作业装 `gcovr`，跑 `run_registry.py --build-dir build-cov` 后调 `check_coverage.py`；本地同样 `pip install gcovr` 后可用一条命令复核。
- 增量覆盖率是"最小测试"的执行标准：新功能只写能跑到自己新增分支的用例即可达标，不必为凑总数补无关用例；但改动没有可执行行时按 100% 处理，不奖励空改动。
- 阈值调整属于规则变更，按 §8 走 `ci-rule-change`。



## 6 隔离与 flaky

| 测试 | 隔离原因 | 到期日 | Owner | Issue |
| --- | --- | --- | --- | --- |
| （空） | | | | |

隔离期内该测试不参与闸门判定，但每天在 nightly 重跑并记录。到期未修复自动回到闸门并失败——不允许靠延期维持绿灯。



## 7 待补齐（不属于注册表）

按项目方向列出的预期产品测试，**未实现前不得写成注册表行**。实现后移入 2.1 并删除此处条目。已在 2.1 落地的项（批次构成、KV 前缀命中、并发 8 路、进程级冒烟）不再留在清单里——清单只写缺口，不写成绩。

- [ ] contract：`tests/contract/scheduler_backend` —— 组批时 token 预算与 slot 分配的不变量；现由 `contract_continuous_batch` 间接覆盖，缺独立边界
- [ ] contract：`tests/contract/kv_cache` —— 真实后端并发下的块表不泄漏、不重叠；纯逻辑侧已有 `unit_kv`
- [ ] integration：`tests/integration/batching` —— 长短请求混跑的饥饿与公平性；`--fairness-aging` 目前只有单测，没有进程级验证
- [ ] e2e：`tests/e2e/smoke` —— 固定请求集的 golden 输出逐 token 比对；现有 3 个脚本只校验协议、指标与等价性，不比对文本
- [ ] perf：`tests/perf/throughput`、`tests/perf/latency` —— 吞吐与首 token 延迟基线，供 nightly 判回退
- [ ] unit：`tests/unit/quant` —— INT4/FP8 量化误差界；量化路径未实现，先不建用例
- [ ] unit：`runner_llama` 的批内决策逻辑 —— 真实后端仍只有 `verify_prefix.sh` / `verify_chunk.py` 手工脚本，`llama-compile` 作业只保证能编译



### 7.1 缺陷回归的存放约定

修复缺陷必须把触发场景固化成用例，跟着修复一起提交，不允许“改完即算”。用例放在对应层级的目录（逻辑判定进 `unit`/`contract`，进程行为进 `integration`），名称或注释要写明修复前的症状与触发条件，便于后来者判断它锁的是什么行为。

2026-11-20 实例（ASan 作业第一次真抓到东西）：`src/kv.cpp:evict_donor` 里 `donor` 可能正是 `donor_order_` 中被 `erase` 的元素，`dropped_.push_back(donor)` 构成 heap-use-after-free；短 id 走 SSO 掩盖了问题，只有 17 字节的请求 id 才暴露。回归用例 `长 donor id 淘汰后仍安全写入待释放列表` 进 `unit`，注释写明症状与触发条件，修复与用例同一次提交。

已落地的例子：真实 llama 纯 CPU 后端没有统一 KV stream，`llama_memory_seq_cp` 会跨 stream 整段复制并 reset 目标 cells，与批内写入冲突，实测 4 条请求 `ExecutionFailed`。回归用例在 `tests/unit/config_test.cc` 的「后端不支持共享时前缀复用降级为关闭」，锁定 `effective_prefix_cache(true, false) == false`；真实后端用 `scripts/bench/verify_prefix.sh` 配 `NGL=0 EXPECT_SHARE=0` 复核“零命中、零崩溃、零对账差异”。

另一个真实后端缺陷：`n_ctx_total = ctx × concurrency`，但 `n_seq_max = concurrency + donor_slots`，llama.cpp 会把总数均分给所有槽，8 并发 + 4 donor 时单序列只剩 `8/12` 上下文，约 840 token 的 prompt 直接 `failed to find a memory slot`。修复为 `n_ctx_total = ctx × (concurrency + 实际需要的 donor 槽)`，且 CPU（前缀共享已关闭）不再预留 donor。回归证据是 2-G2 的 8 路约 920-token prompt：修复前 `decode` 报 batch of size 840 失败，修复后 24/24 成功、利用率 85.39%、`reconcile=0`（`docs/阶段2-证据.md`）。

阶段 3 的切块行为由三层用例锁定：`tests/unit/scheduler/chunk_planner_test.cc` 锁 128/128/64、预算截断与批次构成快照（5 例）；`tests/contract/continuous_batch_test.cc` 锁“切块输出与整段 prefill 逐 token 一致”“decode 保位混批”“chunk 宽大于预算跨 tick”“中途取消 Aborted”（14 例中含 4 例切块）；`tests/integration/test_server_chunked.py` 锁进程级 SSE 一致、`budget < ctx` 可推进与 7 个指标键真实存在。`unit_config` 另覆盖四档/非法值/CLI 覆盖 env、预算等于并发与小于 ctx 两组边界。真实后端另用 `scripts/bench/verify_chunk.py` 在 CPU/GPU 两档复核同一 token 序列，结果见 `docs/阶段3-1-实现记录.md`。

阶段 3 的抢占/恢复/生命周期再补四组用例：

- `tests/unit/scheduler/cost_model_test.cc`（6 例）：`context_tokens` 的“末 token 未写回”口径、fcfs_tail 选最晚准入、aging 豁免、weighted 打分、recompute/swap 代价未标定返回 -1、`choose` 回落 recompute、四态迁移合法性。
- `tests/contract/preempt_test.cc`（3 例）：基线档零抢占；压力档每次抢占写 trace，断言 victim/`free_before`/`need_blocks`/释放块数/重算 token/`recompute` 路径与代价；连续 cap 后请求以 `Error` 终止且块全归还。
- `tests/contract/state_test.cc`（2 例）：recompute 恢复后的输出与无抢占基线逐 token 一致、长度不超 `max_tokens`、stale completion 不重复计进度。
- `tests/contract/lifecycle_test.cc`（5 例）：断连 `abort()` 后下一步 `Aborted` 并归还块；排队请求取消不占块；超上下文请求以 `Error` 终止且不留在等待队列；生成达到 `max_tokens` 后按长度收尾并归还块；压力抢占后关停无悬挂序列、无泄漏块（`check_invariants`）。

`unit_config` 同时覆盖 `--fairness-aging` 无值开关、`--aging-threshold-ms` / `--max-consecutive-preemptions` / `--preempt-trace` 的 CLI/env 覆盖与范围校验。调度不变量“抢占只在组批前、已组批序列不得成为 victim、被抢占组不得反向抢占”由 `preempt_test` / `state_test` / `lifecycle_test` 共同锁定。阶段 3 的完整证据链与三次 review 见 `docs/阶段3-实现记录.md`。

阶段 4 的回包队列改造按同一约定固化：`tests/unit/sink_test.cc`（4 例）锁“队列满立即失败并关闭”“关闭后 pop 不阻塞”“溢出只计队列满、关闭后的写入不算”。计时断言一律用 `std::chrono::milliseconds` 比较，不用 `steady_clock::count()` 直接比常量——MSVC 的 period 是 100ns，`50'000'000` 实际是 5 秒，测不出 2 秒阻塞回归。

阶段 4 的指标/准入/资源层补四组用例：

- `tests/unit/metrics_test.cc`（9 例）：`MetricId` 表与 KPI 冻结名、8 线程分片累加不丢、gauge 覆盖、标签白名单、sum/count、每个 family 唯一 TYPE/HELP、snapshot、reset 语义。
- `tests/contract/admission_test.cc`（2 例）：预算 `max_concurrency + queue_capacity` 的突发拒绝只计一次；预留兑现后 `accepting()` 恢复且不重复拒绝。
- `tests/unit/resource_test.cc`（5 例，4-G0-3）：无 GPU 依赖也能拿到正 RSS；20 次采样峰值只增不减；VRAM known/unknown 时 `vram_mib`/`vram_peak_mib`/`vram_probe_ok` 三者自洽（unknown 必须 NaN + 0）；`start`/`stop` 幂等；未显式 stop 析构也安全。`tests/tools/test_peak_sampler.py` 用真实子进程验证 `scripts/bench/peak_sampler.py` 的 CSV 五列与未知 pid 失败码。
- `tests/integration/test_server_metrics.py` / `test_server_admission.py`：进程级校验 TYPE/HELP 与 KPI 计数一致（含 K-8 RSS>0、峰值不低于当前值、VRAM unknown 语义）；8 并发打满预算 3 时回 3×200 + 5×429，429 带 `Retry-After: 1`，退避后重试 200。

`tests/unit/runner_raii_test.cc`（4-G0-7）只在 `LLMOPT_ENABLE_LLAMA=ON` 时登记：反复构造/析构、非法模型两次 load 后 release、以及 `LLMOPT_TEST_MODEL` 提供真模型时的 load→tokenize→release→reload；在 WSL ASan 构建跑 `ctest -R runner_raii`，用于证明 C 句柄无泄漏。

阶段 4 矩阵工具链的用例：

- `tests/tools/test_matrix.py`（9 例）：JSON 配置必填/覆盖、§1.6 十二列、两轮偏差 ok/high、全错档 `failed`、不可行档保留行、n<30 标 `insufficient`、压测窗口 K-4/K-5 峰值采样、比率列保留 6 位小数（防 K-4 被舍成 0）。
- `tests/tools/test_report.py`（4 例）：仅凭 JSONL + manifest 离线重建报告、四张 PNG 签名、`/metrics` 计数漂移检测、偏差 `high` 返回非零。
- `tests/tools/test_peak_sampler.py`、`tests/tools/test_bench_*`：采样与 SSE 解析口径。
- `tests/integration/test_server_matrix.py`：真 stub 服务跑 2 并发 × 2 档 × 1 轮；校验 CSV 列、warmup 丢弃、manifest 哈希、RSS 峰值、`.prom` 快照、`step_summary.csv`，并回调 `aggregate.py --check-metrics --check-deviation`。
- `tests/integration/test_server_mixed.py`（4-G6）：长短 1:1 同轮，CSV 分组行各 3 条、长请求全部完成。
- `tests/integration/test_server_slow_client.py`（4-G0-2）：慢读客户端 + 快请求并发，`backpressure.csv` 记录快侧 ITL p99 与溢出计数。

CI 工具自测 `ci/test_phase4_scanners.py`：依赖清单无纸面依赖、规模 4,500–25,000 行、范围无分布式/CUDA/训练/PD、测试全登记；`scripts/ci/check_code.py` 每次代码闸门自动调用三者。

阶段 5 的判据复算与预留接口各有一组用例，全部 `fast`/`hermetic`、不启服务：

- `tests/tools/test_spec5.py`（28 例）：线性插值分位数、trace 的 share 代理（长间隔含 prefill）、截断/坏字段 JSONL 容错、缺列/样本不足/非 ok 档三态、PD 三条件（SLO/占比/chunk 改善）与候选态、迁移不等式与 `Δstall` 缺失、投机 batch_decay/触发/ITL 容差、`--chunk-csv` 同档校验、CLI 落盘与退出码、官方 14 行 CSV 的黄金判定（0 触发/2 候选）。数据在 `tests/data/spec5/`（<2 MB）。
- `tests/unit/p5_reserved_test.cc`（3 例，`unit_p5_reserved`）：C-P5-01…03 默认值等价 v0、`BlockRef` 与 v0 同名类型区分、快照/轨迹值语义。`test_spec5.py` 另扫描 `src/` 与其余头文件，锁定不引用 `p5_reserved.hpp`（R-02）。

阶段 5 不改 `src/`，因此 v0 指标对比沿用阶段 4 官方 14 行（同一 CSV + 同一 trace 复算）；真实判据表在 `docs/evidence/phase5/`，结论“本期数据不支持翻案”。

阶段 6 的兼容面与指标收口（7-2）：
- `tests/unit/protocol_test.cc` 追加 6 例：chat `messages` 纯文本解析与文本分片合并、`tools`/图片分片/`role=tool` 归 `unsupported_field`、`stop` 最多 4 条与长度边界、`n` 1–4 及 `stream+n>1` 拒绝、`StopFilter` 跨帧命中与尾部 flush、chat 帧/非流式体/models 列表形状。
- `tests/integration/test_compat_openai.py`（登记名 `compat_openai_test`）：真 stub 服务跑 `/v1/models`、chat 非流式/流式（帧数 ≥2、以 `[DONE]` 结束、流式拼接与一次性文本一致）、旧 `/completion` 回归、`n=2` 双候选、seed 0/7 文本不同、`stop` 命中后 `finish_reason=stop` 且截断、`tools` 返回 400 `unsupported_field`；同时校验 `llmopt_http_requests_total` 与实际请求数一致，请求 id 出现在服务日志行。帧与指标证据落 `build/evidence/compat_frames.txt`、`metrics_format.txt`。

阶段 6 的服务状态与排空（7-3）：

- `tests/unit/service_test.cc`（`unit_service`，3 例）：`Starting→Ready→Draining→Stopped` 合法迁移表、同态/回退迁移拒绝、`accepting()` 仅 Ready 为真、状态名可打印。
- `tests/integration/test_drain.py`（登记名 `service_state_test`）：8 路在飞 + `LLMOPT_TEST_STUB_DELAY_MS=40`，SIGTERM 后 100 ms 内 `/readyz` 必须 503，新请求回 503 `SERVICE_UNAVAILABLE`，在飞 SSE 全部以 `[DONE]` 收尾，进程退出码 0，日志含 `draining`。Windows 无 SIGTERM 语义，本机跳过、CI Linux 真跑。

阶段 6 的限流与背压（7-4）：

- `tests/unit/rate_limit_test.cc`（`unit_rate_limit`，3 例）：`rps<=0` 恒放行；burst 耗尽后拒绝、按 1/rps 回填；低速率回填不超过 burst（`steady_clock` 统一换算 ns，避免 MSVC 100 ns period 把速率算慢 10 倍）。
- `tests/integration/test_rate_limit.py`（登记名 `backpressure_test`）：`--rate-limit-rps 2 --rate-limit-burst 2` 下连续三次请求为 200/200/429，429 带 `Retry-After: 1`、code `RATE_LIMITED`、`retryable=true`，等待 0.6 s 后恢复 200；`llmopt_rate_limited_total` 精确等于 1。
- 慢客户端隔离沿用 `integration_server_slow_client`（`scripts/bench/slow_client.py`）的 `fast_itl_p99_ms`/溢出列；本机为 stub 读数，真实 1 帧/s 慢读与 ≤10% 退化判定留待 WSL/真后端复测。

阶段 6 的部署与健壮性（7-5）：

- `scripts/release/docker_smoke.sh`（登记名 `deploy_smoke`）：Dockerfile 起服务 → `/readyz` 200 → chat 200 → `docker stop` 后退出码 0。Windows 本机无 Docker，只在 Linux/CI 跑。
- `scripts/release/soak.py`（登记名 `soak_30m`）：持续流式请求 + 每 30 s 采 `llmopt_rss_mib`，断言非 200 为 0、RSS 相对基线增长 <5%，报告落 `build/evidence/soak_report.json`。本机 30 s 冒烟已验证（1566 请求 / 0 错误 / +0.95%）。
- `.github/workflows/nightly.yml`（登记名 `sanitizer_nightly`）：TSan 10 轮、ASan/UBSan 一轮、30 min soak；不进 PR 阻塞链路。本机 Windows 不能跑 sanitizer，口径为“脚本与 workflow 已固化，结论以 CI 运行为准”。
- systemd 模板 `scripts/release/llmopt.service`；部署说明见 `scripts/release/README.md`。

D6-2 的可选 Bearer（用户确认实现，默认关）：

- `tests/unit/auth_test.cc`（`unit_auth`，3 例）：空 token 放行；`Bearer` 前缀、大小写、长度、尾随空格严格匹配；`secure_equals` 长度不同即失败。
- `tests/integration/test_auth.py`（`auth_test`）：配置 `--api-key secret-token` 后无 header/错误 token 均 401 `UNAUTHORIZED` + `WWW-Authenticate: Bearer`，正确 token 的 chat 200；`/healthz`、`/readyz`、`/metrics` 仍 200，`/v1/models` 无 token 401。





准入/背压用例需要稳定复现“突发并发”，stub 后端支持环境变量 `LLMOPT_TEST_STUB_DELAY_MS`（毫秒，默认 0，仅 `--backend stub` 生效），在 `decode_step` 前 sleep；真实后端不读该变量。它只用于测试，不进入 CLI 契约。

自动执行靠 `.githooks/pre-commit`：每次提交先跑 `run_ci.py --mode fast`，再增量构建并跑 `ctest -L fast`，失败即拒绝提交。覆盖率不在钩子里跑（要插桩重建，约 1 分钟），由 CI 的 `coverage` 作业与 `scripts/ci/check_coverage.py` 负责。

## 8 维护

- 测试删除或改期望值，必须同时更新本表，并在 PR 中写 `BEHAVIOR-CHANGE:` 说明。
- 每季度清理：删掉从未失败过且不锁定任何行为的测试，它们只消耗预算。
- 表是给人看的，也是给脚本读的；格式改动需 `ci-rule-change` 标签。
