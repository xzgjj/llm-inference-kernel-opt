# SPEC-DEBUG1 缺陷修复、算力入口封堵与阶段 12 收口

> 版本：v1.1（对应阶段 12）｜状态：approved（2026-11-21 裁决完成，S0 开工）
> 契约基线：C-01…C-13、C-P2-13、C-15、D-01…D-09｜更新：2026-11-21
> 上游输入：2026-11-21 架构与漂移审查、三组代码级专项审查（内存/参数、错误/RAII/生命周期/智能指针、并发/版本）

## 0 一句话目标

在不动现有所有权模型的前提下，先堵住"缺陷面 + 算力放大入口"，再用 30 min soak 与全驻留性能对照把项目叙事补齐，最后把打包、key 生命周期与文档计数收口到可交付状态。

## 1 输入与基线

- 代码基线：分支 `10` @ `965dc38`（feat(phase11)），工作区干净，`origin/10` 已同步。
- 测试基线：`ctest -N` 192 注册；`-L fast` 191（128 unit / 47 contract / 16 integration）；`-L e2e` 1。
- 构建基线：Ninja + GCC（Windows 本机），stub 后端可跑全量；真后端仅 Linux/WSL。
- 审查结论：**无正常输入可触发的 P0；确认 3 项 P1；P2 若干**，另有 9 项文档漂移。

### 1.1 缺陷清单（本 SPEC 的修复对象）

| 编号 | 级别 | 位置 | 问题一句话 |
|---|---|---|---|
| DEBUG-01 | P1 | `http_server.cpp:537-540` → `scheduler.cpp:1573-1592` → `kv.cpp:342-350` | `/metrics` 在 HTTP 线程无锁遍历 KV 块表，与 Core 改 `ref_cnt`/`tenant` 构成数据竞争（UB，非仅指标不准） |
| DEBUG-02 | P1 | `scheduler.cpp:277-290,411-421,1573-1587` | 租户计数器 `make_shared` 抛异常后留空 slot：本次准入名额泄漏，后续 `/metrics` 空解引用 |
| DEBUG-03 | P1 | `config.cpp:243-246,435-463`、`main.cpp:162-172`、`runner_llama.cpp:96-99` | `n_gpu_layers` 负值不校验：门控按 0 层算预算，后端按 -1 全量卸载，预算与行为分裂 |
| DEBUG-04 | P2 | `main.cpp:261-266,288-298` | `core_thread` 启动后异常展开 → `std::thread` 析构 terminate，退出码契约失效 |
| DEBUG-05 | P2 | `runner_llama.cpp:69-80,191-196` | `llama_sampler_chain_init` 空指针未判空即 `chain_add`/`accept` |
| DEBUG-06 | P2 | `config.cpp:878-886` | 配置文件一次性无上限读入，超大文件先 `bad_alloc` 而非配置错误 |
| DEBUG-07 | P2 | `config.cpp:243-244`、`runner_llama.cpp:141-145` | `threads` 无范围校验：负值静默改自动、极大值直传后端 |
| DEBUG-08 | P2 | `gguf.cpp:290-298,316-346` | profile 整数窄化为 `int32`、KV 估算乘法溢出无保护 |
| DEBUG-09 | P2 | `config.cpp:33-109,249-252,891-895` | `donor-slots` / `kv-cache-type` 只有 CLI/env 可用，JSON 入口按未知键拒绝 |
| DEBUG-10 | P2 | `runner_stub.cpp:89,107` | stub 后端对缺失序列隐式插入状态，把生命周期错误伪装成功 |
| DEBUG-11 | P2 | `http_server.cpp:353-360` vs `628-636` | `/v1/models` 走单 key 校验：只配 key 文件时该端点不鉴权 |
| DEBUG-12 | P2 | `scheduler.cpp:194-205`、`scheduler.hpp:46-47` | 后端永久阻塞时 `stop()` 无条件 join，关停不收敛（当前为显式限制） |
| DEBUG-13 | P3 | `http_server.cpp:45,672,731`；`241-251`；`CMakeLists.txt:27` | `g_server` 死全局；`forced_id` 重复赋值；版本宏无人消费 |

### 1.2 文档漂移清单（S6 的修复对象）

| 编号 | 位置 | 漂移内容 |
|---|---|---|
| D1 | `README.en.md:221-227` | 英文版停在阶段 6 / 153 用例 / 0.5B 旧矩阵，与中文版（阶段 11 / 191）冲突 |
| D2 | `README.md:46` vs `ci.yml:263`、`deps.cmake:29-49` | llama.cpp 双锚点 `b94959a` / `b12557`，构建侧不固化 revision |
| D3 | `CLI.md:390-391` | §10 仍写"`--config` 推迟到阶段 2""chat 恒 404"，与 §2/§3 矛盾 |
| D4 | `docs/SPEC总览.md:5-13,75-79` | 缺 SPEC-9 索引；状态全 draft；非目标写 0.5B–7B；"多租户不做"与阶段 11 冲突 |
| D5 | `implementation_plan.md:87,91` | POC 通过线仍是"并发 4 吞吐 ≥1.5×"，与实测负结果冲突且未重基线 |
| D6 | `implementation_plan.md:23,25,40`、`SPEC总览.md:44-45` | 声明 moodycamel/Google Benchmark 已复用，实际 `deps.cmake` 未引入，Mailbox 用 deque+mutex |
| D7 | `阶段11-实现记录.md:5,63`、`implementation_plan.md:145` | 写"未提交、未推送"，实际已提交并同步 |
| D8 | `README.md:183` | 结构段列 `tokenizer/sampler/seq` 等不存在的源码目标 |
| D9 | `docs/testing.md:261` vs `.github/workflows/*` | 旧计数 177/175；`real-backend` 叙述归到 nightly，实际在 `ci.yml` |

## 2 范围与非目标

范围：上述 13 条缺陷 + 9 条漂移 + 用户指定的四项新增交付（断连取消、30 min soak、性能对照重做、key 生命周期与打包运维）。

非目标：不改调度语义（双预算、抢占、chunk、前缀共享规则不动）；不引入新第三方库（断连若必须换框架，只出方案不落地）；不做分布式、多实例、PD 分离；不做 token 日配额（用户已定后置）；不改单机单进程定位。

## 3 设计原则

1. **读侧一律快照**：Core 之外不得直接读 Core/KV 的可变状态；`/metrics` 只读原子与不可变快照（DEBUG-01 的根因是规则没写死，不是漏加锁）。
2. **异常路径与正常路径同一收尾**：准入、租户、KV 的每一项预留都必须有 RAII 回滚守卫（DEBUG-02）。
3. **一处语义一处解释**：同一个参数从校验、预算、后端传参必须共用同一个解释函数（DEBUG-03、DEBUG-07）。
4. **先可复现再修**：每条缺陷先落一个"红灯用例"，修复后转绿；用例登记进 `docs/testing.md`，并成为回归资产。
5. **证据先于结论**：soak / perf / 打包都必须产出 `evidence/phase12/` 下的机器可读文件，正文只引用证据。

## 4 阶段总览

| 阶段 | 目的 | 出口（一句话） | 证据 | 预估 |
|---|---|---|---|---|
| S0 缺陷面修复 | 3 P1 + 9 P2 逐条落地 | 全部 DEBUG 项有红灯→绿灯记录，fast 全绿 | `evidence/phase12/s0/`、`docs/notes/阶段12-S0-review.md` | 2–3 天 |
| S1 断连取消 | 先堵算力放大入口 | 探测结论 + 应用层或网关层方案落地并验证 | `disconnect_probe.json`、取消时序日志 | 1–2 天 |
| S2 soak | 30 min 三指标 | RSS / kv_used / p99 三条曲线达标 | `evidence/phase12/soak/` | 1 天（含运行） |
| S3 性能对照 | 证明或证伪 batching 收益 | 全驻留矩阵 + 结论，允许负结果 | `evidence/phase12/perf/` | 2–3 天 |
| S4 key/配额生命周期 | 同 id 多摘要 + SIGHUP | 轮换与热加载可用、失败保留旧表 | `evidence/phase12/s4/` | 2 天 |
| S5 打包与运维 | 容器、systemd、日志、冒烟 | 容器内真后端 5 项 PASS，SIGTERM 收敛 | `evidence/phase12/packaging/` | 2 天 |
| S6 文档与版本 | 计数自动化 + 漂移清零 | 计数由脚本生成，D1–D9 全部关闭 | `scripts/ci/gen_test_counts.py` 报告 | 1–2 天 |
| S7 收口 | 三轮 review + 交付检查 | Problems panel 0、文档一致、可打 tag | 三份 review、一致性报告 | 1 天 |

依赖：S0 → S1 → S2 → S3 串行（S2/S3 都需要 S0 的指标快照才能信任读数）；S4、S5 可与 S2/S3 并行；S6 收尾统一改文档；S7 最后。

## 5 分阶段规格与验收标准

### 5.1 S0 缺陷面修复

| 项 | 改动点 | 验收标准 | 测试（含错误码路径） |
|---|---|---|---|
| S0-1 DEBUG-01 | 新增 `KVUsage` 的 per-tenant 快照：Core 在 `publish_kv_usage()` 内生成 `tenant_kv_used` 并原子发布；`tenant_stats()` 只读快照，删除锁外 `kv_.used_blocks_for()` 调用 | 高频 `/metrics`（≥50 rps）× 高压请求（P=8）连续 60 s，TSan 0 race；快照值与 Core 内计数在静默点一致 | 新增 `contract/metrics_snapshot_test.cc`：快照一致性 + 并发抓取；TSan 作业纳入 `-L fast` 复跑 |
| S0-2 DEBUG-02 | admission 与 tenant 预留合并为 RAII 守卫；先构造 `TenantCounter` 再插入；`tenant_stats`/释放路径判空 | 注入 `bad_alloc`（重载 `operator new` 或故障开关）后：`inflight_admissions_` 归零、请求返回 `ResourceInsufficient`、后续抓取不崩溃 | 新增 `unit/tenant_rollback_test.cc` 覆盖 3 条异常路径 + 正常路径；错误码断言 `ResourceInsufficient` |
| S0-3 DEBUG-03 | 在 `validate_backend_knobs()` 强制 `n_gpu_layers ∈ [0, 层数]`；预算、日志、后端传参共用 `resolve_gpu_layers()` | CLI/env/JSON 三入口传 `-1` 均配置错误退出（码 2）；`0` 正常按 CPU 预算 | 三入口参数化用例；断言 `config source` 与退出码 |
| S0-4 DEBUG-04 | `core_thread` 改 RAII 守卫：异常路径先 `core.shutdown()` 再 join | 注入"监听失败"异常，进程以退出码 1 结束且不 terminate | `integration/server_config_sources_test.py` 增一条启动失败用例 |
| S0-5 DEBUG-05 | 采样链与各节点判空，失败返回 `ResourceInsufficient`；`add_sequence` 失败回滚 slot | 故障开关令 `chain_init` 返回空：请求 507 + `Detail` 落日志 | `unit/sampler_chain_guard_test.cc`（无 llama 依赖用假链） |
| S0-6 DEBUG-06 | 读取前 `file_size` 上限（建议 16 MiB，与请求体 4 MB 区分），超限返回配置错误 | 20 MiB 文件退出 2 且 message 含上限值 | `unit/config_file_size_test.cc` |
| S0-7 DEBUG-07 | `threads ∈ [0, 256]`（上限待裁决），负值拒绝 | 负值/超界退出 2；`0=自动` 语义保留 | `unit/config_threads_test.cc` |
| S0-8 DEBUG-08 | profile 字段按 `int64` 读并校验 `1..INT32_MAX`；KV 估算用 checked multiply，溢出饱和为"超预算" | 声明 `layers=2^31` 的 profile 返回配置错误；极大 ctx 判 `BLOCKED` 而非静默 | `unit/gguf_profile_bounds_test.cc` |
| S0-9 DEBUG-09 | 两键补入 `kFields/kConfigKeys`，JSON 等价生效 | 同一配置经 CLI/env/JSON 三入口产生一致 `print_config` 行 | `unit/config_sources_test.cc` 扩展 |
| S0-10 DEBUG-10 | stub 改 `find()`，未命中返回 `ExecutionFailed` | 陈旧 `backend_id` 用例断言 500 + 日志 | `unit/runner_stub_state_test.cc` |
| S0-11 DEBUG-11 | `/v1/models` 统一走 `resolve_bearer`；只配 key 文件时同样要求合法 key | 401 用例 × 两种鉴权配置；有效 key 200 | `integration/tenant_keys_test.py` 增 `/v1/models` 两例 |
| S0-12 DEBUG-12 | 不做强制取消（契约不变）；新增 `executor_stuck_total` 指标 + 关停超时告警，文档写死限制 | 人为阻塞 runner 时关停 ≤ 5 s 返回并以 1 退出，指标 +1 | `contract/watchdog_test.cc` 扩展（已存在 SlowRunner） |
| S0-13 DEBUG-13 | 删 `g_server`、去重 `forced_id`、`LLMOPT_VERSION` 交给 S6 统一消费 | 无新增告警；`clang-tidy` 干净 | `check_code.py` 通过 |

S0 出口：`ctest -L fast` 全绿（预计 191 + ≥12）；`run_ci.py --mode pr` PASS；ASan/TSan 各一轮 0 报错；三条 P1 的"红灯记录"写入 review 文件。

### 5.2 S1 非流式断连取消（先探测，再决定落点）

**S1-a 探测实验（必须先做，不得先改协议）**
在 `tests/e2e/` 下加一次性探针脚本，逐项给出证据：

1. `httplib::Request` 暴露字段枚举结果（是否只有 `remote_addr/port`、`local_addr/port`）。
2. handler 等待期间能否拿到 `DataSink` / `is_writable()`（预期：只在 chunked provider 内）。
3. `getsockopt(SO_ERROR)` / `send` 探针在无 fd 时是否可行；若 `remote_port` 可反查 `/proc/net/tcp`，测五元组匹配率与冲突率。
4. Windows 侧同等手段是否成立（`GetExtendedTcpTable` 成本）。
5. 升级到 httplib 最新版后 API 是否新增能力（只读源码结论，不落地升级）。

出口：`evidence/phase12/disconnect_probe.json`（每项 `feasible: true/false` + 证据），并给出"应用层可探测 / 不可探测"的判定。判定为不可行时，S1-b 直接走网关方案。

**S1-b 实现（二选一）**

- 路径 A（探测成立）：在非流式等待循环里以 ≤1 s 粒度检测 `HUP`，命中后 `core.cancel(req.req_id)`，响应按 `Cancelled` 收尾；不动 chunked 语义与状态码契约。
- 路径 B（探测不成立）：交付 `scripts/release/nginx-disconnect.conf`（`proxy_ignore_client_abort off` + upstream 取消）与契约说明：取消发生后客户端可能收到截断响应；服务端仍按 request deadline 收尾。

**验收标准**
- 客户端在 5 s 断开后，≤2 s 内 `cancel()` 被调用（日志 + `llmopt_requests_total{status="cancelled"}` +1），`kv_used` 在 10 s 内回落。
- 断开请求的平均占用时长从 120 s 降到 ≤10 s（同一 P=4 场景对比）。
- 正常请求、慢客户端、`stop` 触发三条既有契约用例不回归。

**测试**：`integration/server_disconnect_test.py`（断连→取消→KV 释放）；路径 B 只做配置静态断言 + README 说明，不伪造运行证据。

### 5.3 S2 30 min soak（只盯三件事）

| 指标 | 判据 | 采样 |
|---|---|---|
| RSS 增长 | 末 10 min 均值 − 首 10 min 均值 ≤ 30 MiB，且无阶梯式跳变 | 10 s 一次 |
| `kv_used` 单调性 | 稳态期不出现"无新请求仍持续上升"；drain 后 `used=0`、`cached=0`、`invariants=ok` | 每 500 ms |
| p99 漂移 | 末 10 min 的 ITL p99 ≤ 首 10 min 的 1.2 倍 | 逐请求样本离线算 |

固定参数：P=4、`ctx=1024`、`max-tokens=32`、prompt 固定集（复用 `tools/bench/prompts.py` 的固定种子）；30 min 持续压。允许负结果，但必须给出归因与是否可复现。

**产物**：`evidence/phase12/soak/soak_report.json` + `rss.csv` / `kv_used.csv` / `itl_samples.jsonl` + 服务端日志。登记为 `-L slow` 用例（不进 PR 闸门）；报告生成脚本进 `scripts/release/soak.py` 扩展，不新建目录。

### 5.4 S3 性能对照重做（项目叙事的根）

**配置（待裁决，建议 A）**
A. 全驻留：Qwen3-8B Q3_K_M，`ngl=37`、`q8_0 KV`，P=1/2/4/8，每档 30 请求 × 32 token × 5 轮。
B. 小模型全驻留：0.5B / 1.5B 全上卡，P=1/2/4/8/16，固定输出长度。
C. A+B 都跑（时间约 +1 天，结论最强）。

**判据（二选一，都算通过）**
- 证明收益：P≥2 的稳态总吞吐 ≥ 1.3×P=1，且 ITL p99 ≤ 3×P=1。
- 证伪收益：给出瓶颈归因（`sched_cpu_ratio`、步间空闲占比、kernel/内存带宽、KV 水位），并说明该配置下 CB 的上限位置。

**必须新增的计量**：本次要显式排除/量化 A2 级结构因素——`pending_step_` 造成的"调度与执行零重叠"。至少记录 `sched_step_ns` 占比与每轮步间空闲时间，否则"CB 无收益"的结论说不清是硬件还是串行点。

**产物**：`evidence/phase12/perf/matrix_report.csv`（沿用十二列 + `tok_accounting`）、`repro_manifest.json`、每档图与中位数/四分位、GPU 温度与频率。旧矩阵标注"作废原因"，README 只引用新表。

### 5.5 S4 key / 配额生命周期

| 项 | 契约 | 验收 | 测试 |
|---|---|---|---|
| 同 id 多摘要 | 同 `id` 允许 ≥1 条 `sha256`；同 id 的 `quota` 必须一致；重复 `sha256` 仍拒绝；共用租户计数与指标 | 两把 key 同一租户：401/200/429 均按同一配额生效 | `unit/config_key_rotation_test.cc` + `integration/tenant_keys_test.py` |
| SIGHUP 热加载 | 收到 SIGHUP 重读文件：校验通过原子替换；失败保留旧表 + 告警 + 指标 `api_keys_reload_failed_total` | 坏文件（缺 version/重复摘要/权限 0644）三种都保留旧表；好文件切换后旧 key 立即失效 | `integration/server_keys_reload_test.py`（Linux）；Windows 用 `--api-keys-reload-seconds`（默认 0） |
| 日配额 | 后置，不在本 SPEC 实现 | — | — |

权限：POSIX 0600 校验沿用现有逻辑；Windows 不做权限校验但启动日志须打印 `perm_check=skipped`。

### 5.6 S5 打包、运维与崩溃回溯

| 项 | 内容 | 验收 |
|---|---|---|
| 镜像 | 多阶段 Dockerfile：builder（C++20 + CMake）+ runtime（非 root、只暴露 8080/8081、模型挂载只读） | `docker build` 成功，镜像 ≤ 250 MiB（stub 档）；`docker run` 后 `/healthz` 200 |
| systemd | 沿用 `scripts/release/llmopt.service`，补 `Restart=on-failure`、`TimeoutStopSec=45` 与日志轮转 | 真机 SIGTERM：`readyz` 立刻 503，在飞收尾，进程 ≤ drain+5 s 退出码 0 |
| 日志轮转 | 20 MB × 5，实测滚动一次 | 压测写满一轮后文件数与大小符合 |
| 容器内真后端冒烟 | `docker_smoke.sh`：healthz/readyz、非流式、SSE、断连取消、`/metrics` 五项 | 五项 PASS，证据 JSON 入 `evidence/phase12/packaging/` |
| 崩溃回溯（可选，需裁决） | `SIGSEGV/SIGABRT` 处理器：写 `logs/crash/` 的调用栈 + 最近 100 行日志 + 关键状态，保留 2 份；CI 用注入崩溃的 fixture 验证 | 注入崩溃后产出 dump 且不吞原始退出码；Windows 用 `DbgHelp` 路径单独验证 |

### 5.7 S6 文档计数自动化、版本收口与漂移清零

1. **计数单一事实来源**：新增 `scripts/ci/gen_test_counts.py`，从 `ctest -N/-L` 与 `check_tests.py` 产出 `{total, fast, unit, contract, integration, e2e}`；README（中/英）、`docs/testing.md`、`CLAUDE.md` 只允许写脚本生成的 `<!-- test-counts:start -->` 块；`check_docs.py` 增一条"块内容与实测不一致即失败"。
2. **版本单一来源**：`project(llmopt VERSION 0.12.0)` → 生成 `version.hpp`；`--version`、启动摘要、`/metrics` 的 `llmopt_build_info{version,backend,llama_revision}` 共用；`LLMOPT_VERSION` 删除或改为引用生成值。
3. **依赖 revision 固化**：`deps.cmake` 增加 `LLMOPT_LLAMA_EXPECTED_REVISION`，构建时读取 `LLAMA_ROOT` 的 git rev 并比对，不一致 `FATAL_ERROR`（无 git 目录时只告警）。
4. **漂移清零**：D1 英文 README 按中文重写；D2 统一为同一 revision；D3 §10 标注"历史冻结"；D4 补 SPEC-9 与真实状态；D5 用 S3 结果正式改写 POC 通过线；D6 决策表改为"实际实现 + 兜底原因"；D7 更新提交状态；D8 删不存在的源码目标；D9 修计数与 nightly 归属。

验收：`check_docs.py` 与新增 `check_counts.py` 在 CI 中 PASS；人为改一处计数→CI 变红（保留该反例记录）。

### 5.8 S7 收口

- 三轮 review（正确性/并发安全、契约与测试有效性、工程与文档），每轮结论入 `docs/notes/阶段12-SN-review.md`。
- VS Code Problems panel 0 报错：需要 `compile_commands.json`（`CMAKE_EXPORT_COMPILE_COMMANDS=ON` 已有）+ `.clangd` 指向 `build/`；以 `clangd --check` 与 `clang-tidy` 双证据替代"肉眼干净"。
- 文档一致性复核：SPEC 状态、`docs/testing.md` 注册表、CLI 参数表、README 数字、`implementation_plan.md` 阶段 12 段落全部对齐。
- 交付检查：`ctest` 全量、`run_ci.py --mode pr`、sanitizer 一轮、soak 报告、perf 报告齐备；建议 tag `v0.12.0`（推送与打 tag 均需用户批准）。

## 6 全局验收门槛（每阶段都适用）

- G1 每阶段独立 commit，提交前 `python scripts/ci/run_ci.py --mode pr` PASS；`ctest -L fast` 全绿。
- G2 新增/修改代码过 `clang-format`、`clang-tidy -p build`；函数 ≤60 行、认知复杂度 ≤25。
- G3 覆盖率：`src/` 行 ≥70%、改动行 ≥80%（现有 `check_coverage.py`），分支比例不下降。
- G4 每个新增校验/回滚路径都有直测，错误码逐一断言（不只测 happy path）。
- G5 代码与文档同批：`docs/testing.md` 注册表、`CLI.md`、README、SPEC 状态、`implementation_plan.md` 同步更新。
- G6 bug 修复必须留下可复现场景，作为回归用例永久保留。
- G7 每阶段结束先更新重要文档，再由用户 review；**git push 与打 tag 一律等你明确同意**。

## 7 测试与证据规范

- 新用例一律进 `tests/{unit,contract,integration,e2e}` 并在 `docs/testing.md` 2.1 登记（Scope 写目录）；Python 侧写 `test_*.py`，C++ 侧写 `*_test.cc`。
- 速度标签：单条 ≤30 s 记 `fast`；soak / 真模型 / 容器冒烟记 `slow`（或 `needs-gpu`、`needs-vm`）。
- 证据目录固定 `evidence/phase12/<stage>/`，文件名带时间戳与命令摘要；正文引用 `path#anchor`，不粘贴大段日志。
- 一次性探针脚本允许进 `tests/e2e/` 但必须带 `--dry-run`/退出码语义，且不进入 PR 闸门。

## 8 流程与审批

1. 开工前：本文件 §10 裁决项确认 → 转 v1.1；再读 `CLAUDE.md`、`AGENT_EXECUTION_PROTOCOL.md`。
2. 每阶段：红灯用例 → 最小实现 → 绿灯 → 集成/e2e（符合条件时）→ 三轮 review → 文档 → 用户 review → commit。
3. 不新建分支，阶段 12 全程在 `10` 上；每阶段一个可回退 commit，提交信息 `feat(phase12-sN): …`。
4. 破坏性操作（改契约、改默认行为、删文件）必须先说明影响面并获批准。
5. 与用户的固定核对点：CLI/接口契约（S4、S6 前各一次）、性能矩阵配置（S3 前）、soak 阈值（S2 前）、推送与 tag（S7 后）。

## 9 风险与回滚

| 风险 | 触发 | 应对 |
|---|---|---|
| 断连探测在 Windows 不可用 | S1-a | 直接走网关方案，不维护 fork |
| 全驻留配置本机跑不起来（8 GB 卡） | S3 | 降级到 0.5B/1.5B 全驻留；仍须给出"小模型正收益 + 8B 负结果"的组合结论 |
| soak 中 RSS 阶梯上涨 | S2 | 先取 `kv_used`/块表对账快照定位，不先改代码；确认是 allocator 碎片则不视为缺陷，写进局限 |
| 计数脚本与 CTest 并行冲突 | S6 | 脚本只读 `ctest -N` 输出，不触发构建 |
| 文档大改影响既有引用 | S6 | 先改生成块与索引，再改正文；`check_docs` 每步跑 |

回滚：每阶段 commit 独立；任一门禁失败即 `git checkout -- <files>` 回到上一阶段，并在 `implementation-notes.md` 记一条偏差。

## 10 待裁决（确认后本文件转 v1.1）

| # | 问题 | 裁决（2026-11-21） |
|---|---|---|
| Q1 | 断连取消落点 | 由独立 Agent 做**隔离探测**，按其结论走应用层或网关；S0 与其余阶段不等待探测结果 |
| Q2 | S3 性能矩阵档位 | **A：Qwen3-8B Q3_K_M `ngl=37` 全驻留**，P=1/2/4/8，每档 30×32×5 轮 |
| Q3 | soak 阈值 | RSS ≤30 MiB、时长 30 min、**p99 ≤1.3×**（报告中注明放宽原因） |
| Q4 | `threads` / 配置文件上限 | `threads ≤256`、配置文件 ≤16 MiB（S0 实现） |
| Q5 | 崩溃回溯 | **纳入 S5**：`logs/crash/` 调用栈 + 最近 100 行日志，注入 fixture 验证 |
| Q6 | 版本号与 tag | `v0.12.0`，与阶段号一致 |

## 11 变更记录

- v1.0 · 2026-11-21 · 首版：13 条缺陷、9 条漂移、8 个阶段与全局门禁；§10 待裁决。
- v1.1 · 2026-11-21 · 裁决落地：S1 交独立 Agent 隔离探测、S3 定 A 档、soak p99 放宽至 1.3×、S5 纳入崩溃回溯；状态转 approved。
