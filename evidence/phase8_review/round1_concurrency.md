# Phase 8 第 1 轮独立审查：并发、生命周期与锁（只读静态）

- 轮次：R1（三轮独立审查之第 1 轮）
- 仓库：`D:\c++_myproject\llm-inference-kernel-opt`（WSL `/mnt/d/c++_myproject/llm-inference-kernel-opt`），分支 `8`
- 审查口径：未提交改动（`git diff` + 未跟踪文件）及其紧邻的并发上下文
- 方法：`git diff` / `git show` 口径核对 + 源码通读 + Grep 定位；**未执行** cmake/ctest/启动服务/起端口，未改任何源码、脚本、测试
- 时间基准：2026-11-21（UTC+8）
- 约束遵守：未 `git add/commit/push/checkout/stash/reset`；未修改 `tools/bench/matrix.py`、`scripts/bench/matrix/*.json`、`docs/**`、`bench/results/**`

## 1. 结论概览

| 严重度 | 数量 | 说明 |
| --- | --- | --- |
| P0（确认致命/数据竞争/use-after-free） | 0 | 未发现确认缺陷 |
| P1（确认竞态/泄漏/挂死） | 0 | 未发现确认缺陷 |
| P2（待验证的潜在缺陷/兜底缺口） | 3 | R1-01、R1-02、R1-03 |
| P3（观察项/口径提示） | 2 | R1-04、R1-05 |

**总体判断**：阶段 8 改动本身（`kv_used_peak_` 峰值指标、`capacity-only` 早退路径、`read_model_profile` JSON 解析、`kv_cache_type` 估算分支）均运行在启动单线程路径或 Core 线程私有上下文，**未新增跨线程共享可变状态，也未引入新的锁/生命周期缺陷**。既有并发架构（单线程 Core 状态机 + 单线程 Executor 前向回投 + 有界 Sink + 分片原子指标）的锁序、CV 谓词、线程 join 与析构顺序总体自洽。下列 P2/P3 均为既有代码的潜在缺口或观察项，本轮只读，未修复。

## 2. 发现项明细

### R1-01 [P2] 完成事件无超时看门狗：单步 Completion 丢失会让 Core 在关停时永久等待

- **文件:行**：`src/scheduler.cpp:418`（退出条件）、`src/scheduler.cpp:1176-1177`（`pending_remaining_` 递减）、`src/scheduler.cpp:248-251`（Executor 回投失败仅记日志）
- **代码原文**：
  ```cpp
  // src/scheduler.cpp:418
  if (shutting_down_ && !pending_step_ && running_.empty()) {
      break;
  }
  // src/scheduler.cpp:1176-1177
  pending_serials_.erase(it);
  pending_remaining_ -= 1;
  ```
  ```cpp
  // src/scheduler.cpp:248-251
  if (!mailbox_.push_critical(std::move(event))) {
      logger()->error("completion 回投失败 req={} serial={}", ...);
  }
  ```
- **触发场景（并发交错）**：某一批次中某个 step 的 `Completion` 未被 `apply_completion` 处理（未来回投通道变为有界、或 runner 异常路径漏投、或事件被误判 stale 丢弃）。`pending_step_` 恒为 `true`，`pending_remaining_` 不归零；即使 `running_` 已清空，`run()` 的退出条件 `shutting_down_ && !pending_step_ && running_.empty()` 仍不成立，`main` 中 `core_thread.join()`（`src/main.cpp:264`）挂死，进程无法退出。
- **判定依据（违反的不变式）**：在飞批次必须有逐步配对的完成事件；当前正确性只依赖"关键通道不丢"这一前提，缺少超时兜底。`push_critical` 失败路径只 `logger()->error`，没有把对应序列降级收尾。
- **确认状态**：**待验证**。静态推断；当前 `push_critical` 为无界通道，不会因容量失败，Executor 的 catch 路径仍逐 step 回投错误事件，故现有代码路径下不可直接复现。验证方法：在 harness 中屏蔽/丢弃一条 Completion（或直调 `apply_completion` 缺失分支），驱动 `run_once` 直到 pending，再触发 `shutdown()`，观察 Core 是否在有限轮次内退出。
- **最小修复建议**：Core 记录在飞批次最早未完成时刻；`pump()` 内检测到 pending 超时后，对 pending 序列按 `Error/Timeout` 收尾，清空 `pending_serials_`、`pending_remaining_`、`pending_step_`，使退出条件可达成。
- **最小回归用例**：contract 用例「单步完成丢失后看门狗收尾不挂死」——harness 提交 1 条请求，`run_once` 推进到 pending；注入一条 Completion 丢失；断言：有限轮次内 `running_` 清空、请求落 `Error` 终态、`pending_step_` 复位、`shutdown()` 后 `run()` 返回（不挂死）。可挂在现有 `PreemptHarness` + `contract_continuous_batch` 套件上。

### R1-02 [P2] `abort_all()` 裸清零 `inflight_admissions_`，与并发 HTTP 线程的 add/sub 非配对

- **文件:行**：`src/scheduler.cpp:1424`
- **代码原文**：
  ```cpp
  // 仍在 mailbox 里的提交不会再被处理，预留名额必须一并归零。
  inflight_admissions_.store(0, std::memory_order_relaxed);
  ```
- **触发场景（并发交错）**：调度线程异常进入 `abort_all()` 兜底的同一时刻，某个 HTTP handler 已在 `try_reserve_admission()` 完成 `fetch_add(1)`、尚未把事件兑现/回滚。`abort_all` 把计数 `store(0)` 后，该 handler 后续 `release_admission()` 的 `fetch_sub(1)` 会把计数打到负值；`accepting()`（`src/scheduler.cpp:348-353`）读到偏小的在飞占用，存在短时超发窗口。
- **判定依据（违反的不变式）**：在飞预留数必须 `add/sub` 配对计数；异常兜底路径不应以裸 `store` 与并发的 `fetch_sub` 竞争复位。
- **确认状态**：**待验证**。需 TSan 或并发压测复现；`abort_all` 本身是异常兜底，服务随即关停，实际窗口很小，当前未见可复现的超发现场。
- **最小修复建议**：`abort_all` 后由服务状态拒绝新准入（`accepting()` 叠加 `ServiceState::Ready` 判断，排空期一律拒绝），使残余 `fetch_sub` 不再影响准入判定；或把"兜底清零"与"封锁 submit"做成同一道闸门，避免计数与并发 sub 直接竞争。
- **最小回归用例**：admission/并发用例「异常兜底后准入立即关闭，不出现负在飞/超发」——构造触发 `abort_all` 的路径，断言此后 `accepting()==false`，指标在飞数不为负，无新请求进入队列。

### R1-03 [P2] LlamaRunner 部分配置字段跨线程读取未加锁，依赖启动 happens-before

- **文件:行**：`src/runner_llama.cpp:348`（`chat_template_`/`model_`）、`src/runner_llama.cpp:373-375`（`n_ctx_`）、`src/runner_llama.cpp:402-404`（`n_gpu_layers_`）、`src/runner_llama.cpp:285-295`（`release()` 重置）
- **代码原文**：
  ```cpp
  // render_chat：未持 mutex_/vocab_mutex_ 读取
  if (chat_template_.empty() || !model_) { ... }
  // n_ctx()：无锁返回
  std::int32_t n_ctx_ = 2048;
  // release()：持 mutex_ → vocab_mutex_ 后清空 states_/ctx_/model_
  ```
- **触发场景（并发交错）**：当前生命周期为 `load_model → serve → core_thread join → release()` 串行，HTTP 线程在读模板/后端名/`n_ctx` 时这些字段已冻结，不构成竞争。**若**未来支持服务期重载模型，HTTP 线程的无锁读取与 `release()` 的重置会发生数据竞争；`release()` 内嵌套获取 `mutex_ → vocab_mutex_`，当前无反向获取路径，未形成锁序环。
- **判定依据（违反的不变式）**：跨线程共享可变字段应有明确同步归属；当前注释未固化"服务期不可重载"这一前提。
- **确认状态**：**观察项（当前生命周期下不可达，非本轮可复现缺陷）**。验证方法：TSan 下模拟 serve 期 `release()` + 并发 `render_chat/n_ctx`。
- **最小修复建议**：二选一——(a) 将这些只读字段纳入 `mutex_` 保护；(b) 在 `runner.hpp`/实现注释中固化"服务期不可重载、`release()` 前必须无 HTTP 在读"的不变式，并在 `release()` 前置屏障/注释说明。
- **最小回归用例**：观察项暂不补 C++ 用例；若走 (a)，补 TSan 重载场景用例。

### R1-04 [P3] ResourceSampler 峰值成员为非原子裸变量，happens-before 依赖未固化

- **文件:行**：`include/llmopt/resource.hpp:55-56`、`src/resource.cpp:217`、`src/resource.cpp:222`
- **代码原文**：
  ```cpp
  double rss_peak_mib_ = 0.0;
  double vram_peak_mib_ = -1.0;
  ```
- **触发场景**：`start()` 在调用线程同步 `publish(read_now(false))` 后才创建采样线程（`src/resource.cpp:163-173`），`std::thread` 构造提供 happens-before，当前无并发写。但峰值字段为裸 `double`，未用原子，也无注释固化"仅 start 主线程一次 + sampler 线程"的访问归属；后续若有人在别处调用 `publish/sample_once` 会直接踩数据竞争。
- **判定依据**：共享可变状态的访问归属应显式可见。
- **确认状态**：**观察项，当前无竞争**。
- **最小修复建议**：在字段旁注释固化访问线程归属；或把两个峰值改为 `std::atomic<double>`（CAS 取峰）。
- **最小回归用例**：无需新增；若改原子，随 sampler 单测覆盖峰值单调不降即可。

### R1-05 [P3] `kv_used_peak_` 峰值口径：仅在 `publish_kv_usage()` 路径更新，极端收尾路径可能偏低

- **文件:行**：`include/llmopt/scheduler.hpp:365`、`src/scheduler.cpp:689-690`、`src/scheduler.cpp:1324`、`src/scheduler.cpp:1429`
- **代码原文**：
  ```cpp
  kv_used_peak_ = std::max(kv_used_peak_, usage.used_blocks);
  metrics_.set(MetricId::KvUsedPeak, kv_used_peak_);
  ```
- **触发场景**：峰值只在 `publish_kv_usage()` 被调用时刷新；`finish()`、`apply_preemption()`、`abort_all()` 均会走到该路径，常规路径覆盖完整。仅当某条收尾路径绕过 `publish_kv_usage()`（未来改动引入）时，峰值可能低于真实占用。该字段为 Core 线程私有，**不存在数据竞争**。
- **判定依据**：观测口径提示，非并发缺陷。
- **确认状态**：**观察项**。
- **最小修复建议**：注释明确"峰值随 KV 快照发布刷新"；后续若新增收尾出口，确保调用 `publish_kv_usage()`。
- **最小回归用例**：无需新增；随 KV 指标对账用例顺带断言峰值 ≥ 任意时刻 used。

## 3. 已覆盖但未发现问题的子项（核对记录）

### 3.1 锁与临界区
- `active_ids_mutex_`（`src/scheduler.cpp:382-393`）只保护 `active_ids_` 集合，不与任何其他锁嵌套；`reserve_request_id`/`release_request_id` 对称加解锁。
- Mailbox（`src/scheduler.cpp:123-132`）、Executor 队列（`src/scheduler.cpp:182-206`）、Sink（`src/sink.cpp:11-58`）、StubRunner `states_`（`src/runner_stub.cpp:60-130`）、RateLimiter（`include/llmopt/rate_limit.hpp:21-37`）各自单锁，锁内只做算术/内存操作，未在锁内做阻塞 IO 或回调。
- LlamaRunner 两把锁（`mutex_`/`vocab_mutex_`）：`release()` 顺序为 `mutex_ → vocab_mutex_`；其余路径只持其中一把，未发现反向获取，当前不构成锁序环。
- 状态字段归属：`waiting_`/`running_`/`kv_`/`block_table` 只在 Core 线程读写；Executor 线程只接触 `runner_`、`mailbox_`、`metrics_`；HTTP 线程只接触 `sink`、原子镜像与 `metrics_`。未发现同一可变状态被两线程无锁共享。
- `seq.cancel_requested` 为 `std::atomic<bool>`（`include/llmopt/sequence.hpp:33`）；深度镜像 `waiting_depth_`/`running_depth_`/`inflight_admissions_` 为原子；服务状态 `ServiceController` 为 CAS 原子（`include/llmopt/service.hpp:31-46`）。未发现 atomic 与非 atomic 混用读写同一字段。

### 3.2 条件变量
- Mailbox `wait_pop(_for)` 谓词 `!queue_.empty()`（`src/scheduler.cpp:145-149`）；Executor 谓词 `stopping_ || !queue_.empty()`（`src/scheduler.cpp:213`）；Sink 谓词 `closed_ || !queue_.empty()`（`src/sink.cpp:36-37`）；Sampler 谓词 `stopping_`（`src/resource.cpp:194`）；ControlledRunner 谓词 `permits_ > 0`（`tests/support/preempt_harness.hpp:36`）。均带谓词，可防虚假唤醒。
- 唤醒通知：Mailbox/Sink 在解锁后 `notify_one/all`，Executor/Sampler 在持锁置标志后 `notify_all`；未发现持锁状态下丢信号的路径。
- 关停阻塞：Shutdown 事件为关键通道无界入队；Core 排空在飞后退出循环（见 R1-01 的兜底缺口）。

### 3.3 线程生命周期
- `main`：`core_thread` 先 `core.shutdown()` 再 `join()`（`src/main.cpp:263-264`），join 后才析构 `core`/`kv`/`runner`；`ResourceSampler` 析构 `stop()` 自 join（`src/resource.cpp:159-161,175-188`）。
- 成员析构顺序：`executor_` 声明在 `mailbox_` 之后，析构逆序先销毁 `executor_`（`stop()` join 执行线程），再销毁 `mailbox_`，执行线程不会在 mailbox 析构后回投。
- Executor `stop()`：置 `stopping_` → `notify_all` → `joinable().join()`，幂等（重复调用直接返回）；`loop()` 在 `stopping_ && queue_.empty()` 时退出，关停前排空队列批次。
- RAII 句柄：`runner_raii_test` 覆盖未加载析构、加载失败早退、重复 `release`、重载四条路径；未发现半初始化句柄析构泄漏。
- `SchedulerCore::~SchedulerCore()` 调 `shutdown()` 兜底；正常路径线程已 join，析构仅投一条 Shutdown 事件到仍存活的 mailbox。

### 3.4 KV 块账本配对
- 准入失败回滚：runner 登记失败 → `kv_.free(seq)`（`src/scheduler.cpp:844`）；切片预算不足 → `kv_.free` + `release_sequence`（`src/scheduler.cpp:854-855`）；前缀挂载后 acquire 失败 → `kv_.free` 回滚共享引用（`src/scheduler.cpp:785-789`）。
- 抢占：`try_preempt` 内 `kv.free(victim->seq)`（`src/preempt.cpp:171`），`apply_preemption` 释放后端槽并回队首（`src/scheduler.cpp:885-897`）；连续抢占超限走 `finish(Error)`，块与槽均已归还。
- 取消/断连：等待中取消摘队直接 `finish`（未占块）；运行中取消置标志，pending 步完成后在 `advance_chunk`/`deliver` 点收尾归还块；Sink 溢出关闭 → `DeliverResult::Failed` → `finish(Aborted)` 归还块并释放槽。
- 达到 max_tokens：`deliver` 判 `LengthCapped` → `finish` 留置或释放块。
- `finish()` 幂等：`seq.status == Finished` 早退（`src/scheduler.cpp:1278-1280`），不会二次归还块/槽；共享块 `ref_cnt>1` 时只减引用不误释放（`src/kv.cpp:237-241`）。
- 不变式自检：`kv.check_invariants()` 校验 ref 总数、空闲链、哈希索引、donor 归属自洽（`src/kv.cpp:372-421`）；`releasable_blocks` 不计共享块。
- 未发现计数下溢、重复归还、共享 donor 被误释放的路径。

### 3.5 取消与断连
- 运行中取消：`cancel_requested` 置位，`pending_step_` 时延后到完成回调收尾；非 pending 直接 `finish(Aborted)`（`src/scheduler.cpp:471-476`）。
- 断连回调（SSE provider 写失败 / httplib 完成回调）经 `core->cancel(req_id)` 投关键事件到 mailbox（`src/http_server.cpp:157,164,168,179,282`），mailbox 线程安全。
- 非流式 `drain_sink` 超时/stop-hit 调 `core.cancel` 并落终态（`src/http_server.cpp:118,126-131`）。

### 3.6 数据竞争与指标对齐
- `MetricsRegistry`：计数器 8 分片原子 `fetch_add`，gauge 原子 store，浮点 counter CAS 循环（`src/metrics.cpp:219-236`）；渲染/快照为冷路径读原子。
- 指标表对齐：枚举 75 项与 `kMetricDefs` 75 条按索引逐一核对，结果 `ALIGNED`（脚本解析 `metrics.hpp` 枚举名与 `metrics.cpp` series 名）；本轮新增 `KvUsedPeak`、`VramDeviceMib` 位置与定义一致。
- 全局序号线程安全：`g_seq_id`（`src/scheduler.cpp:17`）、`g_request_seq`（`src/protocol.cpp:18`）均为原子。
- HTTP 线程与 Core 之间不共享 `SequenceGroup` 可变字段；仅共享 `BoundedSink`（互斥）与原子深度镜像。

### 3.7 既有并发/生命周期用例覆盖（只读核对，未运行）
- 契约层：断连后终止并归还全部块、排队取消不占块、超上下文不留队、max_tokens 收尾归还块、压力抢占后关停无悬挂序列/无泄漏块（`lifecycle_test`）；抢占预判零副作用、共享前缀不计可释放量、连续抢占 cap 终止块全归还（`preempt_test`）；重复 ID 拒绝与复用、批内单条结束不打断他者、chunked prefill 中途取消 Aborted、关停零泄漏（`continuous_batch_test`）；突发提交按 running+waiting+在飞总额拒绝且只计一次 queue_full、预留兑现后名额释放（`admission_test`）。
- 集成层：4 路线程并发 + 同前缀复用 + 对账零差异（`test_server_concurrency.py`）；慢读客户端不阻塞快请求 + 溢出计数（`test_server_slow_client.py`）。

## 4. 不在本轮范围 / 未检视清单

- 按约束未检视缺陷（仅记录存在改动）：`tools/bench/matrix.py`、`scripts/bench/matrix/phase8b_long.json`、`scripts/bench/matrix/phase8b_short.json`、`docs/CLI.md`、`docs/testing.md`、`bench/results/**`；这些由 MainAgent 并行维护，若有问题另行上报。
- 未跟踪 Python 工具与测试：`scripts/bench/watch_matrix.py`、`scripts/bench/capacity_check.py`、`tests/tools/test_watch_matrix.py`、`tests/tools/test_capacity_check.py` 及其矩阵脚本，属 R3（测试有效性/回归覆盖）范围，本轮 C++ 并发视角未深入。
- 真服务 e2e（`tests/e2e/test_real_service_contract.py`）需外部真模型服务，本轮未运行、未静态下结论。

## 5. 附：本轮通读文件

- `include/llmopt/scheduler.hpp`、`src/scheduler.cpp`
- `include/llmopt/metrics.hpp`、`src/metrics.cpp`
- `include/llmopt/gguf.hpp`、`src/gguf.cpp`
- `src/main.cpp`、`src/http_server.cpp`
- `include/llmopt/kv.hpp`、`src/kv.cpp`、`src/preempt.cpp`
- `include/llmopt/sink.hpp`、`src/sink.cpp`
- `include/llmopt/resource.hpp`、`src/resource.cpp`
- `include/llmopt/runner.hpp`、`src/runner_stub.cpp`、`src/runner_llama.cpp`
- `include/llmopt/service.hpp`、`include/llmopt/rate_limit.hpp`、`include/llmopt/sequence.hpp`
- 用例：`lifecycle_test.cc`、`preempt_test.cc`、`state_test.cc`、`admission_test.cc`、`continuous_batch_test.cc`、`runner_raii_test.cc`、`service_test.cc`、`preempt_harness.hpp`、`test_server_concurrency.py`、`test_server_slow_client.py`
