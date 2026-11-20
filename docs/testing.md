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

C++ 用例由 CTest 登记，`Args` 使用仓库根的 `build/`（CI 与本机约定一致）；本机另建目录时手动执行 `ctest --test-dir <dir>`，不改本表。

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



## 6 隔离与 flaky

| 测试 | 隔离原因 | 到期日 | Owner | Issue |
| --- | --- | --- | --- | --- |
| （空） | | | | |

隔离期内该测试不参与闸门判定，但每天在 nightly 重跑并记录。到期未修复自动回到闸门并失败——不允许靠延期维持绿灯。



## 7 待补齐（不属于注册表）

按项目方向列出的预期产品测试，**未实现前不得写成注册表行**。实现后移入 2.1 并删除此处条目。

- [ ] unit：连续批处理的批次合并/拆分决策（`tests/unit/scheduler`）
- [ ] contract：调度器与后端之间的 token 预算与 slot 分配不变量（`tests/contract/scheduler_backend`）
- [ ] contract：KV cache 的分配/回收在并发请求下不泄漏、不重叠（`tests/contract/kv_cache`）
- [ ] integration：长短请求混跑时，短请求不被长请求饿死（`tests/integration/batching`）
- [ ] unit：INT4/FP8 量化路径与参考实现的最大相对误差在阈值内（`tests/unit/quant`）
- [ ] e2e：单进程起服务，固定请求集，输出与 golden 对齐（`tests/e2e/smoke`，阶段 1 只有进程级冒烟）
- [ ] perf：固定输入长度下吞吐基线，连续三次中位数（`tests/perf/throughput`、`tests/perf/latency`）
- [ ] sanitizer：以上 unit/contract 用例在 ASan+UBSan 与 TSan 下重跑（复用用例，不单建目录）



### 7.1 缺陷回归的存放约定

修复缺陷必须把触发场景固化成用例，跟着修复一起提交，不允许“改完即算”。用例放在对应层级的目录（逻辑判定进 `unit`/`contract`，进程行为进 `integration`），名称或注释要写明修复前的症状与触发条件，便于后来者判断它锁的是什么行为。

已落地的例子：真实 llama 纯 CPU 后端没有统一 KV stream，`llama_memory_seq_cp` 会跨 stream 整段复制并 reset 目标 cells，与批内写入冲突，实测 4 条请求 `ExecutionFailed`。回归用例在 `tests/unit/config_test.cc` 的「后端不支持共享时前缀复用降级为关闭」，锁定 `effective_prefix_cache(true, false) == false`；真实后端用 `scripts/bench/verify_prefix.sh` 配 `NGL=0 EXPECT_SHARE=0` 复核“零命中、零崩溃、零对账差异”。

另一个真实后端缺陷：`n_ctx_total = ctx × concurrency`，但 `n_seq_max = concurrency + donor_slots`，llama.cpp 会把总数均分给所有槽，8 并发 + 4 donor 时单序列只剩 `8/12` 上下文，约 840 token 的 prompt 直接 `failed to find a memory slot`。修复为 `n_ctx_total = ctx × (concurrency + 实际需要的 donor 槽)`，且 CPU（前缀共享已关闭）不再预留 donor。回归证据是 2-G2 的 8 路约 920-token prompt：修复前 `decode` 报 batch of size 840 失败，修复后 24/24 成功、利用率 85.39%、`reconcile=0`（`docs/阶段2-证据.md`）。

阶段 3 的切块行为由三层用例锁定：`tests/unit/scheduler/chunk_planner_test.cc` 锁 128/128/64、预算截断与批次构成快照（5 例）；`tests/contract/continuous_batch_test.cc` 锁“切块输出与整段 prefill 逐 token 一致”“decode 保位混批”“chunk 宽大于预算跨 tick”“中途取消 Aborted”（14 例中含 4 例切块）；`tests/integration/test_server_chunked.py` 锁进程级 SSE 一致、`budget < ctx` 可推进与 7 个指标键真实存在。`unit_config` 另覆盖四档/非法值/CLI 覆盖 env、预算等于并发与小于 ctx 两组边界。真实后端另用 `scripts/bench/verify_chunk.py` 在 CPU/GPU 两档复核同一 token 序列，结果见 `docs/阶段3-1-实现记录.md`。

自动执行靠 `.githooks/pre-commit`：每次提交先跑 `run_ci.py --mode fast`，再增量构建并跑 `ctest -L fast`，失败即拒绝提交。

## 8 维护

- 测试删除或改期望值，必须同时更新本表，并在 PR 中写 `BEHAVIOR-CHANGE:` 说明。
- 每季度清理：删掉从未失败过且不锁定任何行为的测试，它们只消耗预算。
- 表是给人看的，也是给脚本读的；格式改动需 `ci-rule-change` 标签。
