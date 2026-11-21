# 阶段 8 三轮审查发现-修复-复跑验证报告

- 验证时间：2026-11-21（UTC+8）
- 仓库：`D:\c++_myproject\llm-inference-kernel-opt`（WSL `/mnt/d/c++_myproject/llm-inference-kernel-opt`），git 分支 `8`
- 构建目录：WSL Ubuntu-26.04 `/root/llmopt-build`（Release / Ninja / `LLMOPT_ENABLE_LLAMA=ON`，gcc 15.2.0，cmake 4.2.3，ninja 1.13.2，python3 3.14.4，clang-format 21.1.8）
- 审查输入：`evidence/phase8_review/round1_concurrency.md`（R1，5 项）、`round2_http_sse.md`（R2，9 项）、`round3_tests.md`（R3，11 项）
- 基线取证：`evidence/phase8_baseline.md`（基线 165 注册 / 164 通过 / 0 失败 / 1 跳过；tools 75 例 1 error；check_code RC=1）
- 约束遵守：未执行 `git add/commit/push/checkout/stash/reset`；未改 `src/config.cpp`、`tools/bench/**`、`docs/testing.md`；`tests/unit/gguf_test.cc` 仅在 MainAgent 格式化版本上追加负样本，未回退其格式；临时探针仅落 `/tmp`，交付前已清理（`NO_TMP_PROBE`）。

---

## 1. 最终闸门总览（本轮复跑，命令与退出码见 §5）

| 闸门 | 命令（工作目录） | 退出码 | 结果 | 原始日志 |
| --- | --- | --- | --- | --- |
| Release 增量构建 | `cmake --build /root/llmopt-build -j$(nproc)` | **0** | 0 warning / 0 error（`grep -cE "warning\|error"` = 0） | `raw/phase8_build.log` |
| fast 标签全量 | `ctest -L fast --output-on-failure`（`/root/llmopt-build`） | **0** | **171 / 171 通过，0 失败** | `raw/phase8_ctest_fast.log` |
| 全量 ctest | `ctest --output-on-failure` | **0** | **172 注册：171 通过 / 0 失败 / 1 跳过**（e2e，RC=77） | `raw/phase8_ctest_full.log` |
| 代码静态闸门 | `python3 scripts/ci/check_code.py`（仓库根） | **0** | code PASS；deps/size/scope PASS；仅 CI-COD-004 重复率 3.58% warning（阈值 3% 警告 / 5% 失败，不阻塞）；**CI-COD-003 已消失** | `raw/phase8_check_code.log` |
| 阶段 4 扫描器自测 | `python3 ci/test_phase4_scanners.py` | **0** | 5 OK / 0 失败 | `raw/phase8_scanners.log` |
| 测试注册表自洽 | `python3 scripts/ci/check_tests.py` | **0** | tests PASS；49 个测试文件、6 条登记 | `raw/phase8_check_tests.log` |
| Python tools 单测 | `python3 -m unittest discover -s tests/tools -t .` | **0** | **96 OK** / 0 失败 | `raw/phase8_tools_unittest.log` |
| ci 脚本单测 | `python3 -m unittest discover -s ci -t .` | **0** | **21 OK** / 0 失败 | `raw/phase8_ci_unittest.log` |
| PR 模式全闸门 | `python3 scripts/ci/run_ci.py --mode pr` | **0** | docs / tests / code / diff 全 PASS | `raw/phase8_ci_pr.log` |
| 改动文件格式 | `clang-format --dry-run -Werror`（14 个改动 C/HPP/CC 文件） | **0** | 0 违规（日志 0 行） | `raw/phase8_clang_format.log` |

### 1.1 基线 → 最终对比

| 口径 | 基线（phase8_baseline.md） | 最终 | 说明 |
| --- | --- | --- | --- |
| ctest 注册 | 165 | **172** | 本轮新增 C++ 用例：admission 1、protocol 3、contract scheduler 2；其余为既有套件内扩断言 |
| fast 通过 / 失败 | 164 / 0 | **171 / 0** | |
| 全量通过 / 失败 / 跳过 | 164 / 0 / 1 | **171 / 0 / 1** | 跳过仍为同一条 e2e 真服务契约用例 |
| tools unittest | 75 例，74 OK + 1 error | **96 OK / 0 失败** | 基线 `test_report` TypeError 已由 MainAgent 侧对齐；本轮新增 watch/capacity/real_contract_base 共 19 例 |
| ci unittest | 21 OK | **21 OK** | 无回归 |
| check_code | RC=1（COD-003 复杂度 + COD-001 gguf 格式） | **RC=0** | COD-003 由 MainAgent 拆分 `validate_model_entry/validate_runtime_limits/validate_backend_knobs` 消除；COD-001 经 clang-format 清零；COD-004 3.58% 为非阻塞 warning |
| run_ci `--mode pr` | RC=1（code FAIL） | **RC=0** | 四步全 PASS |

### 1.2 全量标签分布（`phase8_ctest_full.log`）

| 标签 | 用例数 | 标签 | 用例数 |
| --- | --- | --- | --- |
| unit | 115 | integration | 13 |
| contract | 43 | e2e | 1 |
| fast | 171 | hermetic | 163 |
| asan | 3 | needs-gpu / slow | 1 / 1 |

---

## 2. R1 并发批次（round1_concurrency.md）

| 编号 | 结论 | 处置 | 回归用例 / 证据 |
| --- | --- | --- | --- |
| R1-01 [P2] 完成事件无超时看门狗，单步 Completion 丢失会让 Core 关停永久等待 | **本轮不改（结构性改动，冻结契约下不硬改）** | 未动 pump 状态机。证据：当前 `mailbox_.push_critical` 为无界通道，容量失败不会发生；Executor catch 路径仍逐 step 回投错误完成事件，丢失 Completion 在现有代码路径不可直接复现。看门狗需在 Core 记录在飞批次最早未完成时刻，`pump()` 检测 pending 超时后对 pending 序列按 Error/Timeout 收尾并清空 `pending_serials_/pending_remaining_/pending_step_`，牵动收尾与取消路径，属结构性改造，留待后续阶段。 | 不可复现说明见本节；建议设计：超时阈值可配置，收尾后退出条件 `shutting_down_ && !pending_step_ && running_.empty()` 可达成。 |
| R1-02 [P2] `abort_all()` 裸 `store(0)` 清 `inflight_admissions_`，与并发 HTTP 线程 `fetch_sub` 非配对，可能下溢/短时超发 | **已修（低风险闸门方案）** | `include/llmopt/scheduler.hpp`：`shutting_down_` 由裸 `bool` 改为 `std::atomic<bool>`（Core 线程写、HTTP 线程在 `accepting()` 只读，原子化消除跨线程数据竞争）。`src/scheduler.cpp`：`accepting()` 与 `try_reserve_admission()` 首行叠加关停闸门——`shutting_down_` 置位即返回拒绝，无论深度镜像如何；`abort_all` 内裸 `store(0)` 保留用于镜像复位，但闸门关闭后残余 `fetch_sub` 无法重新打开准入，计数负值不再影响准入判定。未把分片原子换成全局 mutex，热路径无加锁。 | `tests/contract/admission_test.cc` 新增用例 **「关停闸门置位后 accepting 关闭，Core 边界拒绝新准入」**：提交 1 条 → `run_once` 受理 → `core.shutdown()` → 处理 Shutdown 事件后断言 `accepting()==false`，直达 Core 的两条新 `submit` 均返回 `QueueFull`，末态 `pump` 关闭 sink。定向 `ctest -R admission` 3/3 通过（含本新例）。 |
| R1-03 [P2] LlamaRunner 配置字段跨线程裸读，依赖启动 happens-before | **观察项（当前生命周期不可达）；补不变式注释** | `src/runner_llama.cpp` `release()` 上方固化生命周期不变式：服务期不支持重载模型；`release()` 仅在 Core 线程 join、且无 HTTP 线程在读模板/后端名/`n_ctx_` 之后调用；`chat_template_/model_/n_ctx_/n_gpu_layers_` 在 `load_model` 后冻结，`render_chat/n_ctx()` 无锁读取依赖该前提；若未来引入服务期重载，这些字段必须先纳入 `mutex_` 保护。未加热路径锁。 | 无 C++ 用例（观察项）；若走加锁方案再补 TSan 重载场景。 |
| R1-04 [P3] ResourceSampler 峰值成员非原子裸变量，访问归属未固化 | **观察项；补归属注释** | `include/llmopt/resource.hpp`：`rss_peak_mib_/vram_peak_mib_` 旁注释固化——仅 `start()` 同步首报与采样线程 `loop` 写入，`std::thread` 构造提供 happens-before，`stop()` join 后才析构；新增第三处写入口前必须先改原子或纳入 `mutex_`。未改原子（当前无竞争）。 | 无需新增用例；峰值单调不降由既有 sampler 单测覆盖。 |
| R1-05 [P3] `kv_used_peak_` 峰值仅随 `publish_kv_usage()` 刷新，极端收尾路径可能偏低 | **观察项；补口径注释** | `include/llmopt/scheduler.hpp`：`kv_used_peak_` 旁注释明确 Core 线程私有、仅随 `publish_kv_usage()` 快照刷新；新增收尾出口必须走该路径。字段无线程竞争。 | 非递减/排空保留语义由 R3-02 用例守护（见 §4 R3-02）。 |

### 2.1 R1-02 复现性说明

`abort_all()` 是调度线程异常兜底路径，服务随即关停；裸 `store(0)` 与 handler 线程残余 `fetch_sub` 的交错窗口很小，本轮未构造出稳定下溢现场（需 TSan 或并发压测）。本次修复按审查建议的「把兜底清零与封锁 submit 做成同一道闸门」落地：关停标志原子化 + 准入双侧（入口查询 `accepting()`、Core 预留 `try_reserve_admission()`）拒绝，不依赖计数镜像的绝对非负。

---

## 3. R2 HTTP/SSE 契约批次（round2_http_sse.md）

| 编号 | 结论 | 处置 | 回归用例 / 断言点 |
| --- | --- | --- | --- |
| R2-01 [P2] >4MB body 走 httplib 默认 413 空体，绕过统一错误 JSON | **已修** | `src/http_server.cpp` 错误处理器内：`res.status == 413` → 统一 `send_error`，领域码新增 `ErrCode::PayloadTooLarge`（HTTP 413 / 名称 `PAYLOAD_TOO_LARGE` / 不可重试）；`detail` 带 `payload_max=4194304`。新码四处齐备：`include/llmopt/common.hpp` 枚举、`src/common.cpp` 三处 switch（状态码 / 名称 / retryable 默认 false）、`CLI.md` §4 表、`docs/错误面规范.md` §3 表。 | `tests/integration/test_server_smoke.py` `raw_request` 发 5MB body → 断言 413、`error.code == "PAYLOAD_TOO_LARGE"`、body 可 `json.loads` 且含 `retryable` 字段。 |
| R2-02 [P2] 未知路径 / 错误方法与正常路由混落 404 空体 | **已修** | `run_server` 内建 `route_methods` 表（每条 `register_route` 登记 method→path）。错误处理器：已回包的 4xx（body 非空）返回 Unhandled 不覆盖；404 下查表——命中路由则置 405、`Allow` 头列允许方法、`NotSupported` 文案「该端点不支持 \<method\> 方法（允许: ...）」；未命中则 404 `NotSupported`「未登记的端点: \<path\>」。`Allow` 头按 httplib 语义在 405 响应上给出。 | smoke：`POST /nope` → 404 + `NOT_SUPPORTED`；`GET /v1/completions` → 405 + `Allow` 含 `POST` + `NOT_SUPPORTED`；两条 body 均为可解析统一错误 JSON。 |
| R2-03 [P2] 非流式断连不触发取消，handler 线程占到生成结束或 120s deadline | **本轮不改（结构性改动），附静态证据** | 证据见 §6.1。未在本轮把非流式改成 provider/回调驱动；兜底仍是 `request_deadline_seconds`（默认 120s）限制线程占用。 | 无新增用例；建议后续补 integration「非流式中途断连快速取消」。 |
| R2-04 [P2] 响应不回显请求 `model`，与 CLI.md「仅回显」不符 | **已修** | `src/protocol.cpp`：`render_completion_body` 与 `render_frame` 均序列化 `body["model"] = req.model_echo`（此前 `model_echo` 仅赋值、无读取）。 | `tests/unit/protocol_test.cc` 新增 **「成功响应与流式帧回显请求的 model 字段」**：`model="echo-x"` 请求，断言非流式体与流式帧均含 `"model":"echo-x"`。不起服务，doctest 直断言序列化输出。 |
| R2-05 [P2] nlohmann 异常 `what()` 进 HTTP `message` 外发 | **已修（行为变更，见 §7）** | `parse_body`/`parse_chat_body` 签名加可选尾参 `std::string* detail = nullptr`（`include/llmopt/protocol.hpp` 同步）；两处 catch 的 `message` 改为通用文案「请求体 JSON 非法，请检查字段类型与结构」，`exc.what()` 只写 `detail` 出参；`src/http_server.cpp` `submit_request` 捕 `parse_detail` 透传进 `make_error`。 | protocol_test 新增 **「解析异常 message 不含第三方库名，原始 what() 进 detail」**：断言 `message` 不含 `json.exception`/`type_error`/`parse_error`，`detail` 非空。已全仓核对 `tests/` 无旧文本（「解析异常」/库名）依赖断言。 |
| R2-06 [P2] chat 分片 `text` 类型非法兜底成通用「解析异常」 | **已修** | `parse_message_content` 取值前判 `part["text"].is_string()`，否则 message=`messages[].content[].text 必须是字符串`，返回 `InvalidJson`，不再抛到外层通用 catch。 | protocol_test 新增 **「chat 分片 text 非字符串按字段级错误拒绝」**：数字 `123` 与对象 `{"a":1}` 两个负样本，断言 `InvalidJson`、message 含 `text` 与「字符串」、不含库名。 |
| R2-07 [P2] 线程池 = 并发+3，慢连接可能饿死控制端点 | **本轮不改（牵动调度语义），附风险与建议参数** | 证据见 §6.2。未改池容量与路由隔离。 | 无新增用例；建议后续补「慢连接占满池时探活不饿死」integration。 |
| R2-08 [P3] Content-Type 缺 `charset=utf-8` | **已修** | 所有 JSON 响应（`send_error`、`/v1/models`、`/healthz`、`/readyz`、非流式成功体）改 `application/json; charset=utf-8`；流式 provider 改 `text/event-stream; charset=utf-8`。既有测试仅 metrics 断言子串 `text/plain`，无精确匹配，未打破。 | 无独立新用例；随 smoke/admission/metrics 集成用例回归。 |
| R2-09 [P3] QueueFull / ContextOverflow 的 message 缺数值 | **已修** | ContextOverflow message 带 `prompt=<n> + max_tokens=<n> 超过 ctx-tokens=<n>`；两处 QueueFull message 带 `等待队列已满（<waiting>/<cap>），请退避后重试`；原始 cap/req_id 仍留 `detail`。 | `tests/integration/test_server_admission.py` rejected 循环补断言 `"/3" in message`（max_concurrency 1 + queue_capacity 2 = 3）。 |

---

## 4. R3 测试有效性批次（round3_tests.md）

| 编号 | 结论 | 处置 | 回归用例 / 断言点 |
| --- | --- | --- | --- |
| R3-01 [P1] capacity-only / model-profile 主启动路径无进程级回归 | **已补** | `tests/integration/test_server_smoke.py` 新增 `write_profile`（写声明字段档位 JSON，不需真实权重）与 `probe_capacity_only`（一次性进程，早退后探测所选端口是否仍监听）。`main()` 内：小档位断言 rc=0、`capacity-profile:` 行、profile 名进摘要、`allow_start=1`、`decision=ELIGIBLE`、未监听；超大档位断言 rc ∈ {3,4}、`allow_start=0`、未监听。 | 随 `integration_server_smoke` 进程级起子进程验证。 |
| R3-02 [P2] KV 峰值只验名解析，非递减 / 排空保留语义无断言 | **已补** | `tests/contract/scheduler_test.cc` 新增 **「KV 峰值随占用取大，排空后保留不清零」**；`tests/integration/test_server_metrics.py` 补 peak 序列存在、`peak ≥ used`、排空后 `peak ≥ 1` 断言。 | 变异记录：`std::max` 改为覆盖写后该 contract 断言变红（本轮前已验证）。 |
| R3-03 [P2] `clear_env()` 未清两个新 env 键，用例间环境污染 | **已修** | `tests/unit/config_test.cc` `clear_env` 补清 `LLMOPT_MODEL_PROFILE`/`LLMOPT_CAPACITY_ONLY`；新增隔离自检用例 **「容量档位环境变量经 clear_env 后不残留到后续用例」**。 | 变异：不清两键时隔离用例变红（本轮前已验证）。 |
| R3-04 [P2] `normalize_base` 无 hermetic 单测；非 200 体 `json.loads`/`int` 裸调用可崩用例 | **已补** | `tests/e2e/test_real_service_contract.py` 抽 `parse_json_body` / `error_object` 容错 helper，替换四处裸 `json.loads`；`LLMOPT_E2E_OVERFLOW_TOKENS` 非整数回退 1600 并打印 WARN。新建 `tests/tools/test_real_contract_base.py` hermetic 单测 10 例：`normalize_base` 6 分支（host:port / http 默认 80 / https 默认 443 / 拒路径 / 拒坏 scheme / 拒缺 host）+ 容错解析 4 例（非 JSON 空体 / JSON 数组非对象 / error 对象缺失或类型错 / error 对象提取）。 | tools discover 全量绿；新文件经 `check_tests.py` 目录级 scope 覆盖（49 文件登记 PASS）。 |
| R3-05 [P2] `watch_matrix` 主循环零覆盖；snapshot 对非数值字段无容错 | **已修（脚本 + 测试）** | `scripts/bench/watch_matrix.py` 加 `_safe_int`、非对象行跳过；抽出纯函数 `decide(state, stall_after, until_report)`。`tests/tools/test_watch_matrix.py` 补 DirtyRowTest（脏字段不崩）+ DecideTest（超阈值 stalled / 阈值下不 stalled / 年龄未知不 stalled / 仅出 report 才退出）共 5 例；文件 8 例全绿。 | tools discover 96 OK。 |
| R3-06 [P2] `capacity_check.py` 子进程超时/非零无兜底，整轮写 CSV 前崩掉；负样本偏薄 | **已修（脚本 + 测试）** | `run_one` 捕 `subprocess.TimeoutExpired`（exit 124，保留 partial 输出解析；注意 `TimeoutExpired` 构造参数名是 `output`，部分取 `exc.output`）/ `OSError`（exit 1，启动失败占位行）；`main` 先写表头再逐行 `writerow`+`flush`，失败行不丢早期行。`tests/tools/test_capacity_check.py` 补超时行保留、OSError 占位、半行/垃圾解析负样本、失败行不丢早期行共 4 例；文件 8 例全绿。 | tools discover 96 OK。 |
| R3-07 [P2] `llmopt_vram_device_mib` 服务端发射未在集成测试核对 | **已补** | `test_server_metrics.py` 补 `llmopt_vram_device_mib` 序列存在 + 非负或 NaN 断言。 | 随 `integration_server_metrics` 回归。 |
| R3-08 [P2] 两个新增 `.sh` 可执行位未入 git 索引；调用方式未固化 | **只记录证据，不改码** | 实测：`scripts/bench/run_long_matrix.sh`、`run_real_contract.sh` 当前 untracked，磁盘 0777；`core.filemode=false`，入库后 git 记 100644；两脚本均 `cd "$(dirname "$0")/../.."` 自定位。**调用必须 `bash scripts/bench/<name>.sh`，不可直接 `./`**（入库后直接执行会因 644 无执行位失败）。 | 证据见 §8.2。 |
| R3-09 [P3] 采样器用例依赖 `time.sleep(0.22)` 定时等待，脆弱点 | **已改测试侧** | `tests/tools/test_matrix.py` 采样器固定 sleep 改为轮询 `sampler.kv_used == 0` 直到首轮回调写入、2s 上限兜底；`tools/bench/matrix.py` 未动。 | 17 例全绿（tools discover 内）。 |
| R3-10 [P3] 档位 JSON 缺类型不符 / 非正整数负样本 | **已补** | `tests/unit/gguf_test.cc` 用例 **「容量档位拒绝浮点/字符串/非正整数字段」** 补三个拒绝子例：浮点 `1.5`、字符串 `"64"`、`block_count=0`。该文件以 MainAgent 格式化版本为准，本轮未再动。 | 随 `unit_gguf` 回归。 |
| R3-11 [P3] 短/长证据矩阵未设 `kv_cache_type`，落默认 f16 | **观察项，不改证据配置** | `scripts/bench/matrix/phase8b_short.json`、`phase8b_long.json` 实测无 `kv_cache_type` 键；容量档 q8_0 建议口径与 f16 默认存在偏差。属禁改证据配置，仅列出供后续矩阵校准。 | 无。 |

---

## 5. 最终复跑命令台账（含退出码）

> 工作目录除注明外均为仓库根 `/mnt/d/c++_myproject/llm-inference-kernel-opt`；ctest 在 `/root/llmopt-build`。

| # | 命令 | 退出码 | 结果摘要 |
| --- | --- | --- | --- |
| 1 | `cmake --build /root/llmopt-build -j$(nproc)` | 0 | 增量构建成功；`grep -cE "warning\|error"` = 0 |
| 2 | `ctest -R "protocol\|admission\|smoke\|metrics" --output-on-failure` | 0 | 36/36 通过（定向复跑受影响套件） |
| 3 | `ctest -L integration --output-on-failure` | 0 | 13/13 通过 |
| 4 | `ctest -L fast --output-on-failure` | 0 | 171/171 通过，0 失败 |
| 5 | `ctest --output-on-failure` | 0 | 172 注册：171 通过 / 0 失败 / 1 跳过（#172 e2e） |
| 6 | `python3 scripts/ci/check_code.py` | 0 | code PASS；COD-004 3.58% warning（非阻塞） |
| 7 | `python3 ci/test_phase4_scanners.py` | 0 | Ran 5 tests，OK |
| 8 | `python3 scripts/ci/check_tests.py` | 0 | tests PASS，49 文件 / 6 登记 |
| 9 | `python3 -m unittest discover -s tests/tools -t .` | 0 | Ran 96 tests，OK |
| 10 | `python3 -m unittest discover -s ci -t .` | 0 | Ran 21 tests，OK |
| 11 | `python3 scripts/ci/run_ci.py --mode pr` | 0 | docs/tests/code/diff 全 PASS |
| 12 | `clang-format --dry-run -Werror`（14 个改动 C/HPP/CC） | 0 | 0 违规，日志 0 行 |
| 13 | `env -u LLMOPT_REAL_BASE python3 tests/e2e/test_real_service_contract.py` | 77 | `SKIP: 未设置 LLMOPT_REAL_BASE，真实服务契约用例不执行` |

### 5.1 定向集成用例清单（ctest 名）

`integration_server_smoke`、`integration_server_admission`、`integration_server_metrics`、`compat_openai_test`、`service_state_test`、`auth_test`、`backpressure_test`、`integration_server_matrix`、`integration_server_mixed`、`integration_server_slow_client`、`integration_server_concurrency`、`integration_server_chunked`、`integration_server_config_sources`。

---

## 6. 结构性不改项的静态证据

### 6.1 R2-03 非流式断连取消

- httplib v0.18.3（`third_party/_deps/httplib-src/httplib.h`）：`Request` 公开字段仅 method/path/params/headers/body/remote_addr/port 等（612–639 行），`Response`（666 行起）同样不暴露 socket 句柄或连接存活探测；`virtual socket_t socket() const = 0`（735 行）属于内部 `Conn` 连接对象，路由 handler 拿不到。
- 现有断连通知钩子只有流式 chunked provider 的写失败回调（`src/http_server.cpp:169-185`：`write_frame` 失败 → `core->cancel(req_id)` → `done()`）。非流式 `drain_sink`（110–148 行）只能观察 sink 帧 / `is_closed()` / deadline，无连接不可写事件。
- 复用流式取消钩子给非流式，需要把非流式也改成 provider 驱动的响应生命周期，牵动 n>1 多候选装配、错误/超时/取消收尾路径，属结构性改动；本轮冻结 HTTP/SSE 契约下不硬改。兜底：`request_deadline_seconds`（默认 120s）限制线程占用上限。
- 待真服务复测项：stub 服务并发 1 条非流式请求，首字节后 RST，观测在飞序列清零时刻与 handler 释放时刻。

### 6.2 R2-07 控制端点线程预留

- `src/http_server.cpp:472-473`：`handler_threads = concurrency_limit() + 3`，`server.new_task_queue` 挂单个 `httplib::ThreadPool(handler_threads)`。
- `httplib.h:752-776`：ThreadPool 为单一共享 job 队列 + N 个 worker，无按路由预留、无独立队列、无信号量门控；注释假设的「3 条留给控制端点」在线程被慢/断连接占满后无法兑现，`/healthz`、`/readyz`、`/metrics` 只能排队等空闲。
- 隔离方案（后续阶段）：控制端点独立小线程池 / 独立 listener，或 provider 入口检测连接不可写即快速失败释放线程；池容量与慢连接占用策略写入 CLI.md。建议参数方向：控制池 2–3 线程独立于推理池；推理池仍按 `max-concurrency + 余量` 配置。本轮未改，避免牵动调度与接入语义。

---

## 7. 行为变更记录（BEHAVIOR-CHANGE）

### BEHAVIOR-CHANGE: R2-05 解析异常 message 不再外露 nlohmann 库文本

- **变更前**：畸形 body 触发 nlohmann 类型/解析异常时，HTTP `error.message` 形如 `解析异常: [json.exception.type_error.302] type must be string, but is number`，第三方库异常码与内部定位直接外发；`Error.detail` 未设置。
- **变更后**：`message` 为通用可行动文案「请求体 JSON 非法，请检查字段类型与结构」；`exc.what()` 只进 `detail`（日志侧结构化 cause，不进 HTTP body）。错误码仍为 `InvalidJson`（400），对外契约形状（`error.code/message/retryable`）不变。
- **既有测试依赖核对**：全仓 `tests/` 检索 `解析异常` / `json.exception` / `type_error` / `parse_error`，仅本轮新增的负向断言（要求 message **不含** 这些串）命中；无既有用例断言旧文本。e2e 仅断言 `INVALID_JSON` 码，不依赖 message 文本。
- 同批 R2-06 的字段级 message 同理替代旧通用兜底文案。

---

## 8. 跳过 / 观察 / 未执行项标注

### 8.1 跳过用例

| 用例 | 标签 | 跳过原因 |
| --- | --- | --- |
| `#172 e2e_real_service_contract` | e2e / slow / needs-gpu | 未设置 `LLMOPT_REAL_BASE`；脚本打印 `SKIP: 未设置 LLMOPT_REAL_BASE，真实服务契约用例不执行`，直跑 RC=77（`raw/phase8_e2e_skip.log`）。跳过不计为通过；需真模型服务环境复测。 |
| `runner_raii_test.真模型 load->tokenize->release->reload` | unit / fast / asan | ctest 计 Passed，但 `LLMOPT_TEST_MODEL` 未设置时用例早退，真模型路径本次未执行；非 ctest 跳过，单列标注。 |

补充：R2 HTTP 修复落地后，MainAgent 用 8B Q4 真服务重跑 `bash scripts/bench/run_real_contract.sh`，结果 **`PASS: 真实服务契约 10 项`**，关停 `kv_used=0 cached=0 free=256 total=256 shared=0 invariants=ok`；证据 `evidence/e2e_8b_contract.txt`（2026-11-21 16:30 刷新）。

### 8.2 观察项留痕

- **R3-08**：两个 `.sh` untracked、磁盘 0777、`core.filemode=false`；入库后记 100644，调用方式固定为 `bash scripts/bench/<name>.sh`。
- **R3-11**：`phase8b_short.json` / `phase8b_long.json` 未设 `kv_cache_type`，落默认 f16；证据配置本轮未动。
- **CI-COD-004**：重复率 3.58%（阈值 3% 警告 / 5% 失败），warning 级不阻塞，需观察是否继续上升。
- **diff 闸门口径**：`run_ci.py --mode pr` 以 `origin/8` 为 base，三点比较不含工作区未提交修改与未跟踪文件；CI-DIF-001/002/003/004 在本地未提交状态下未被有效评估，需提交 / PR 环境以正确基线复核。
- **未执行闸门**：覆盖率 CI-COV-001/002（需 `LLMOPT_COVERAGE=ON` 插桩构建）；ASan/UBSan/TSan 构建（CI-MEM-001/002）；真 8B 服务契约与真后端 runner 路径；`deploy_smoke`（Docker）、`soak_30m`。

### 8.3 变异红→绿记录（回归用例可失败性验证）

| 变异 | 临时改动（已还原，`grep MUTATION` 0 命中） | 观察到的红态 | 还原后 |
| --- | --- | --- | --- |
| A（R2-04） | 注释掉 `render_frame` / `render_body` 的 `body["model"] = req.model_echo` | `unit_protocol.成功响应与流式帧回显请求的 model 字段` Failed（21 例中 1 失败） | 重建后 protocol 全绿 |
| B（R2-05） | `parse_body` catch 回退 `message = "解析异常: " + exc.what()` | `unit_protocol.解析异常 message 不含第三方库名，原始 what() 进 detail` Failed（21 例中 1 失败） | 重建后 protocol 全绿 |
| C（R2-01/R2-02） | 错误处理器首行直接 `return Unhandled`，交回库默认空体 | `integration_server_smoke` Failed：`JSONDecodeError: Expecting value: line 1 column 1 (char 0)`（413/404 空体不可解析） | 全量重建后 smoke 及全量 ctest 绿 |

变异后均单独重建目标 TU/二进制跑定向用例；还原后全量重建（0 warning）并复跑 §5 全部闸门。

---

## 9. 本轮改动文件清单（工作区未提交）

**源码 / 头文件**
- `include/llmopt/common.hpp`、`src/common.cpp`：新增 `PayloadTooLarge`（413 / `PAYLOAD_TOO_LARGE` / 不可重试）。
- `include/llmopt/protocol.hpp`、`src/protocol.cpp`：model 回显进体/帧；`detail` 出参承接 `what()`；chat `text` 类型守卫。
- `include/llmopt/scheduler.hpp`、`src/scheduler.cpp`：`shutting_down_` 原子化 + 准入双侧关停闸门；峰值字段口径注释。
- `include/llmopt/resource.hpp`：峰值字段访问归属注释。
- `src/runner_llama.cpp`：服务期不可重载不变式注释。
- `src/http_server.cpp`：`route_methods` 表 + 统一错误处理器（413/404/405/Allow）；charset=utf-8；QueueFull/ContextOverflow message 带数值；`parse_detail` 透传。

**测试**
- `tests/unit/protocol_test.cc`：3 新例（回显 / 异常 message 纯度 + detail / chat text 非字符串）。
- `tests/contract/admission_test.cc`：1 新例（关停闸门准入拒绝）。
- `tests/contract/scheduler_test.cc`：KV 峰值非递减 / 排空保留断言。
- `tests/unit/config_test.cc`：clear_env 补两键 + 隔离用例。
- `tests/unit/gguf_test.cc`：类型不符 / 非正整数负样本（MainAgent 格式化版本）。
- `tests/integration/test_server_smoke.py`：`raw_request` / `write_profile` / `probe_capacity_only` helper；404/405/413 与 capacity-only 进程级断言。
- `tests/integration/test_server_admission.py`：QueueFull message 容量断言。
- `tests/integration/test_server_metrics.py`：KV peak 与 `vram_device_mib` 对账断言。
- `tests/e2e/test_real_service_contract.py`：容错 JSON helper + 溢出 tokens 回退。
- `tests/tools/test_real_contract_base.py`（新建，10 例）、`test_watch_matrix.py`（+5）、`test_capacity_check.py`（+4）、`test_matrix.py`（轮询等待改造）。

**文档对齐**
- `CLI.md` §4、`docs/错误面规范.md` §3：补 `PayloadTooLarge` 413 行。

**MainAgent 侧（继承，未回退）**：`src/config.cpp` 校验函数拆分（COD-003 消除）、`tools/bench/**` 工具链修复、`tests/unit/gguf_test.cc` 格式化。

---

## 10. 原始日志清单（`evidence/raw/`）

| 文件 | 内容 |
| --- | --- |
| `phase8_build.log` | 还原变异后全量增量构建输出，0 warning / 0 error |
| `phase8_ctest_fast.log` | `ctest -L fast` 全量输出，171/171 |
| `phase8_ctest_full.log` | 全量 `ctest --output-on-failure`，171 通过 / 1 跳过 / 0 失败 + 标签分布 |
| `phase8_check_code.log` | check_code 输出，RC=0，COD-004 warning |
| `phase8_scanners.log` | phase4 扫描器自测，5 OK |
| `phase8_check_tests.log` | 测试注册表自洽，49 文件 / 6 登记 |
| `phase8_tools_unittest.log` | tests/tools discover，96 OK |
| `phase8_ci_unittest.log` | ci/ discover，21 OK |
| `phase8_ci_pr.log` | `run_ci.py --mode pr` 四步全 PASS |
| `phase8_clang_format.log` | 14 个改动文件 dry-run，0 违规（空文件） |
| `phase8_e2e_skip.log` | e2e 直跑 SKIP 文本与 RC=77 |
