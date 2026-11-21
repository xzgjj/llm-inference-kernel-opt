# CI 规范（完整版）

版本：v1.0（2026-11-20）　适用：llm-inference-kernel-opt　负责人：@xzgjj

执行入口是 [../ci.md](../ci.md)（≤3000 字，给人也给 agent）；测试登记表是 [../testing.md](../testing.md)（唯一事实来源）。本文件是细则：每条规则的判定方式、阈值、失败消息、豁免路径。三者冲突时，以本文件的规则目录为准，并立即修正另两份文档——文档之间的不一致本身就是 CI 缺陷。

---



## 1 目的

这个仓库要同时容纳三种产出：C++ 推理服务（Continuous Batching 调度）、ggml 后端与算子、低比特量化评估。它们都容易出现"代码能跑但没人看得懂"的状态，而且大量工作由 agent 完成。agent 的偏差和人的偏差不是同一种：

- **模型偏保守地"周全"**：把每个细节写进需求文档，文档迅速膨胀到没人读；为每个新需求造一层抽象，抽象成本没人算。
- **模型偏局部地"过关"**：测试失败时先改测试；小任务顺手重构三个文件；发现规则碍事就把规则关掉。
- **人偏向省略与遗忘**：不写失败原因，不更新文档，把"先这样"留成永久状态。

CI 的职责是把**可判定**的部分变成机器红灯，把**不可判定**的部分固定成人的决策点，并且让决策留下记录。CI 不负责让作者感觉良好。

---



## 2 设计原则

| 编号 | 原则 | 落地方式 |
| --- | --- | --- |
| P1 | 反馈越早越贵 | L0 在提交前 10 秒内完成；L1 三分钟内出结论 |
| P2 | 只自动化可判定的 | 每条规则标注"自动/人工"，人工项进 PR 模板，不写成脚本 |
| P3 | 单一事实来源 | 测试只认 testing.md 的表；规则只认本文件的规则目录；命令只认 run_ci.py |
| P4 | 预算制而非禁令 | 文档有字数、Diff 有行数、复杂度有上限，超限走批准，不是一律禁止 |
| P5 | 例外显式、有主、会过期 | waivers.json 强制 owner/approver/expires，默认 90 天 |
| P6 | 批准者与开发者分离 | CODEOWNERS + 规则改动独立 PR，同一个人不能自己批自己 |
| P7 | 跳过必须留痕 | 环境缺失打印原因并以非零退出，或显式 skip 并计入报告，不静默通过 |
| P8 | 重试有上限 | 单 job 最多 2 次；第 3 次必须定位或隔离，不允许"再跑一次看看" |
| P9 | 可重复 | 测试自带数据与随机种子；禁止依赖本机状态、时间和网络 |
| P10 | 规则可本地复现 | 本地一条命令跑出与 CI 相同的判定，差异只允许 matrix 与 sanitizer |

---



## 3 闸门与预算

| 层 | 触发 | 预算 | 必须通过的内容 | 未通过的含义 |
| --- | --- | --- | --- | --- |
| L0 | pre-commit / 手动 | 10 s | 文档字数、重复段落、测试注册表、空白与行尾 | 本次提交不合规，不应推送 |
| L1 | 每次 push 与 PR | 3 min | L0 全部 + 规则自洽 + Diff 预算 + 范围白名单 + 代码质量快检 | PR 不可合并 |
| L2 | PR | 15 min | Debug/Release 构建（警告即错误）+ 注册表 fast 测试 | 编译失败或行为回归 |
| L3 | PR 且存在 CMake 工程 | 30 min | ASan+UBSan、TSan、受保护回归测试 | 内存/并发缺陷，优先级高于功能开发 |
| L4 | nightly / 手动 | 2 h | slow 测试、性能基线、长稳、GPU 队列 | 不阻塞个人 PR，阻塞发版 |

闸门实施细节：

- **L0** 由 `.githooks/pre-commit` 调用 `run_ci.py --mode fast`。钩子需手动启用一次（`git config core.hooksPath .githooks`），未启用的仓库在 L1 兜底。
- 表中预算指检测本身的目标时长；workflow 的 `timeout-minutes` 取整并含 runner 启动开销，超出即判失败。该超时是兜底，不是放宽。
- **L1** 是唯一对所有改动生效的闸门，任何"只改文档"的 PR 也要过 L1，因为文档改动同样会破坏规则自洽。
- **L2/L3** 在没有 `CMakeLists.txt` 时整体跳过，跳过原因写进 job summary 与 artifacts；这一阶段的绿灯只代表"结构合规"，不代表"功能正确"。
- **L4** 允许失败但不允许长期红：连续 3 天夜间失败必须在 issue 里给出结论（修复、隔离或删除该基线）。

---



## 4 规则目录

判定列：`自动` 由脚本或 workflow 执行；`人工` 由 PR 模板的结构化问答执行，不写脚本。
实现列：`script:文件名:符号` 指向 `scripts/ci/` 下的实现；`workflow:job` 指向 `.github/workflows/ci.yml`；`manual:*` 指向人工清单。脚本内的规则用 `rule: <ID>` 标记，检查器据此做双向一致性校验。



### 4.1 闸门自身

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-GAT-001 | 分层闸门时限 | 自动 | L0 10s / L1 180s / L2 900s / L3 1800s | `workflow:gates` | 无 |
| CI-GAT-002 | 跳过必须留痕 | 自动 | 每个跳过任务必须输出原因 | `script:ci_common.py:skip_reason` | 无 |
| CI-GAT-003 | 重试上限 | 自动 | 单 job ≤2 次 | `script:check_diff.py:retry_budget` | 无 |
| CI-GAT-004 | 并发取消 | 自动 | 同 ref 新推送取消旧运行 | `workflow:gates` | 无 |
| CI-GAT-005 | 本地可复现 | 自动 | 本地入口与 CI 判定一致 | `script:run_ci.py:main` | 无 |

### 4.2 文档

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-DOC-001 | 要求类文档字数预算 | 自动 | 每份 ≤3000 字 | `script:check_docs.py:doc_budget` | 无 |
| CI-DOC-002 | 规范体积 | 自动 | 18–26 KB | `script:check_docs.py:spec_size` | 无 |
| CI-DOC-003 | 重复段落 | 自动 | 归一化后 ≥40 字的段落不得跨文档重复 | `script:check_docs.py:duplicate_paragraphs` | 无 |
| CI-DOC-004 | 必需要素 | 自动 | 每份文档含规定小节与表格表头 | `script:check_docs.py:required_sections` | 无 |
| CI-DOC-005 | 相对链接可解析 | 自动 | 链接目标必须存在 | `script:check_docs.py:link_integrity` | 无 |
| CI-DOC-006 | 规则与实现双向一致 | 自动 | 文档声明的脚本规则 ↔ 代码 `rule:` 标记完全相等 | `script:check_docs.py:rule_implementation_map` | 无 |
| CI-DOC-007 | 架构文档模块归属 | 人工 | 新增模块必须在文档中标注用途与边界 | `manual:pr-review` | 无 |

字数口径：中文字符逐字计 1，ASCII 单词（连续字母数字/路径）计 1，Markdown 结构符号不计。记录与分析类文档（`docs/testing.md`、报告、ADR）不设上限，但仍受 CI-DOC-003 约束。

### 4.3 测试

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-TST-001 | 注册表结构 | 自动 | 固定五列；Level 与 Metadata 取值在词表内 | `script:check_tests.py:registry_table` | 无 |
| CI-TST-002 | 注册表非空 | 自动 | 存在测试文件时至少 1 条有效登记 | `script:check_tests.py:not_empty` | 项目初始化期（无测试）自动放行 |
| CI-TST-003 | 遗漏登记 | 自动 | `tests/**` 与 `ci/**` 下每个测试文件必须被某条 Scope 覆盖 | `script:check_tests.py:unregistered` | `ignore` 小节 + 理由 |
| CI-TST-004 | 失效登记 | 自动 | Scope 内至少 1 个测试文件，否则视为过期 | `script:check_tests.py:stale_entry` | 删除该行 |
| CI-TST-005 | 环境标签与执行面一致 | 自动 | `needs-gpu`/`needs-vm` 不得进入 PR fast 计划 | `script:check_tests.py:env_partition` | 无 |
| CI-TST-006 | 受保护测试 | 自动 | 删除或 skip 受保护测试必须带行为变更标签 | `script:check_diff.py:protected_tests` | 行为变更批准 |
| CI-TST-007 | flaky 隔离期 | 自动 | 隔离 ≤14 天，必须有 owner 与 issue | `script:check_tests.py:quarantine_ttl` | 无 |

登记表分两张：2.1 产品测试（`tests/`）与 2.2 CI 工具自测（`ci/`），同一五列、同一文件，分开统计。混表会让覆盖率把"规则自测"算成产品能力，因此 Scope 的首层目录即归属，不允许跨目录借用。

登记表的每一行同时是执行单元的声明：Runner 与 Args 必须能直接执行，CI 通过 `run_registry.py` 读表运行，不允许在 workflow 里另写一套测试命令。新增测试不登记，L1 直接失败并打印缺失路径。

### 4.4 代码质量

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-COD-001 | 格式 | 自动 | `clang-format --dry-run -Werror` 零差异 | `script:check_code.py:format` | 生成代码目录白名单 |
| CI-COD-002 | Lint 与类型 | 自动 | `-Wall -Wextra -Werror` + 项目 `.clang-tidy` 无 error 级告警 | `workflow:build` | 无 |
| CI-COD-003 | 复杂度 | 自动 | 函数认知复杂度 ≤25，函数体 ≤60 行 | `script:check_code.py:complexity` | 单函数条目 + 理由 |
| CI-COD-004 | 重复代码率 | 自动 | >3% 警告，>5% 失败 | `script:check_code.py:duplication` | 无 |
| CI-COD-005 | 未使用代码 | 自动 | 未引用符号、未使用参数/变量告警为 0 | `workflow:build` | 接口占位需 `[[maybe_unused]]` + 注释 |
| CI-COD-006 | 悬空 TODO | 自动 | 只允许 `TODO(owner):` 形式，无主 TODO 计入失败 | `script:check_code.py:todos` | 无 |
| CI-LLM-001 | 真实后端可编译 | 自动 | `runner_llama.cpp` 在固定版本 llama.cpp 头文件下零警告编译通过 | `workflow:llama-compile` | 找不到固定版本时失败并留痕，不静默跳过 |
| CI-TLS-001 | TLS 可选构建与握手 | 自动 | `-DLLMOPT_ENABLE_TLS=ON` 零警告构建；`integration_server_tls`、`integration_server_control_isolation`、`integration_server_tls_config_error` 通过 | `workflow:tls` | 无 |
| CI-REAL-001 | 真后端模型固定 | 自动 | nightly 下载官方 Qwen2.5-0.5B Q4_K_M，sha256 必须匹配 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`（491,400,032 bytes） | `script:tests/e2e/nightly_real_backend.py --expect-sha256` + `workflow:real-backend` | 无 |
| CI-REAL-002 | 真后端五项运行时 | 自动 | `healthz/readyz`、非流式、SSE、断连取消、`/metrics` 全部通过；失败不跳过 | `script:tests/e2e/nightly_real_backend.py` + `workflow:real-backend` | 无 |

编译器版本：`build-clang` 用 runner 预装的 clang（当前 21.1.8），不锁旧版本。此前 `spdlog 1.14.1` 内嵌的 fmt 10.2 在 clang ≥19 会以 `call to consteval function ... is not a constant expression` 硬报错，导致全量构建失败；根因在依赖侧而不是编译器侧，故把 spdlog 升到 1.17.0（内嵌 fmt 11），冻结表同步更新，编译器不再需要降级。第三方头噪声单独豁免、不外扩：doctest 2.4.11 在 clang 下无条件 `#include <ciso646>`，libstdc++ 15 对该头抛 `#warning`，故 clang 作业加 `-Wno-error=#warnings`（仅降级该类，其余仍 error）。两处工具侧豁免同样单独记录、不外扩：`modernize-use-scoped-lock` 在 clang-tidy 21.1.6 上段错误（上游 `diagOnSingleLock` 崩溃，与本项目代码无关），已从 `.clang-tidy` 关闭并留 TODO；sanitizer 作业在 RelWithDebInfo 下对 libstdc++ `<regex>` 的 `maybe-uninitialized` 假阳性只降级为告警。

### 4.4.1 覆盖率

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-COV-001 | 总体行覆盖 | 自动 | `src/` 行覆盖率 ≥70%（gcovr 统计，排除 tests/third_party） | `script:check_coverage.py:line` | Windows/MSVC 无 gcov 时 SKIP 并留痕，由 Linux runner 补跑 |
| CI-COV-002 | 增量行覆盖 | 自动 | 本分支改动行 ≥80% | `script:check_coverage.py:diff` | 基线不可用时 SKIP 并留痕，不允许静默通过 |

覆盖率在 PR 的 `coverage` 作业执行（`run_ci.py --mode nightly` 也带同一步，前提是先有插桩构建）；两者都要求 `-DLLMOPT_COVERAGE=ON` 的独立构建目录（`build-cov` 或带插桩的 `build`）与 gcovr；Windows 本机无 gcov 时该步 SKIP，WSL/Linux runner 必须实跑。SKIP 与“通过”在报告里分开记录，不允许把 SKIP 当绿灯。

### 4.5 改动范围

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-DIF-001 | 范围白名单 | 自动 | 顶层目录只能是 `src/ include/ tests/ docs/ scripts/ cmake/ .github/ bench/ evidence/` 与根级配置（含 `README.md`、`CLI.md`） | `script:check_diff.py:scope_whitelist` | 新顶层目录需批准 |
| CI-DIF-002 | Diff 预算 | 自动 | 核心代码 ≤400 行警告，>800 行失败（不含测试与生成物） | `script:check_diff.py:diff_budget` | 重构例外需批准 |
| CI-DIF-003 | 规则文件改动 | 自动 | 触碰 `.github/**`、`.githooks/**`、`scripts/ci/**`、`ci/**`、`docs/ci/**`、`docs/testing.md` 需 `ci-rule-change` 标签 | `script:check_diff.py:rule_files` | 无 |
| CI-DIF-004 | 受保护测试改动 | 自动 | 删除/禁用受保护测试需 `behavior-change` 标签与说明块 | `script:check_diff.py:protected_tests` | 无 |
| CI-DIF-005 | 敏感与大文件 | 自动 | 禁止私钥/凭据；单文件 ≤2 MB | `script:check_diff.py:sensitive_files` | 无 |
| CI-DIF-006 | 生成物混入 | 自动 | `build/`、`third_party/`、`*.o` 等不得进 Diff | `script:check_diff.py:generated_paths` | 无 |
| CI-DIF-007 | 例外文件有效性 | 自动 | 字段齐全、approver≠owner、未过期 | `script:check_diff.py:load_waivers` | 无 |

范围与预算的判定对象是**本次 PR 的全部改动**，不是单个文件。把一个大重构拆成七个 PR 同样触发 CI-DIF-002，因为人的审查带宽是同一个。

### 4.6 内存与并发

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-MEM-001 | ASan + UBSan | 自动 | 全部注册 fast 测试在 `-fsanitize=address,undefined` 下零报告 | `workflow:sanitizers` | `needs-gpu` 测试延后到 GPU 队列 |
| CI-MEM-002 | TSan | 自动 | 调度、线程池、KV cache 相关测试零竞态报告 | `workflow:sanitizers` | 无 |
| CI-MEM-003 | 抑制文件变更 | 自动 | `*/sanitizer-suppressions.txt` 变更需 `memory-review` 标签 | `script:check_diff.py:rule_files` | 无 |

C++ 推理服务的两类高危缺陷（use-after-free、数据竞争）不会自己显形，只能靠常驻的 sanitizer 作业。因此 L3 不是可选加项，而是"存在 `src/` 就必须有"的闸门。ASan 与 TSan 互斥，分两个 job 跑；UBSan 跟 ASan 合并以省预算。

### 4.7 性能

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-PRF-001 | 吞吐回归 | 自动 | 三组基准中位数回退 >5% 失败 | `workflow:nightly` | 基线更新需批准 |
| CI-PRF-002 | 基准稳定性 | 自动 | 同机重复 3 次离散度 >8% 则该数据作废 | `workflow:nightly` | 无 |

性能数字默认不可信，直到它在同一台机器上稳定复现三次。GPU 队列用 self-hosted runner，PR 不跑，避免排队把反馈时延拉长到小时级。

### 4.8 规则治理

| ID | 规则 | 判定 | 阈值 | 实现 | 豁免 |
| --- | --- | --- | --- | --- | --- |
| CI-RUL-001 | 规则负责人 | 自动 | CODEOWNERS 覆盖全部规则与闸门文件 | `script:check_diff.py:rule_owners` | 无 |
| CI-RUL-002 | 例外不得续期 | 自动 | 到期即失效，同一规则同一范围最多两次 | `script:check_diff.py:load_waivers` | 无 |
| CI-RUL-003 | 季度复审 | 人工 | 每季度删掉至少一条无效规则 | `manual:quarterly-review` | 无 |
| CI-RUL-004 | Action 固定版本 | 人工 | v1.0 前允许版本标签，之后固定 commit SHA | `manual:pr-review` | 无 |

规则只能通过独立 PR 修改，且必须说明三件事：旧规则误报了什么、新规则漏掉了什么、阈值依据是什么。删规则比加规则更难，这是刻意的。

---



## 5 本项目实现

### 5.1 目录与职责

| 路径 | 职责 | 备注 |
| --- | --- | --- |
| `src/` | 服务端：调度器、请求生命周期、KV cache、后端适配 | 未来存放 Continuous Batching 主体 |
| `include/` | 对外头文件 | 公共 API 面，改动触发 L3 |
| `tests/` | 产品测试，按层级分子目录 | 与 testing.md 2.1 的 Scope 一一对应 |
| `ci/` | CI 工具自测（验闸门脚本本身） | 不计入产品覆盖率，带 `infra` 标签 |
| `docs/` | 规则、测试策略、ADR、论文 | 只有 `ci.md`、`testing.md`、`ci/` 入库 |
| `scripts/ci/` | 闸门脚本（仅标准库，无三方依赖） | 保证任何机器直接可跑 |
| `CLI.md`（根级） | 服务 CLI 与预研工具契约 | 与 `README.md` 同属根级合同文档，已登记进 `ALLOWED_ROOT_FILES` |
| `.github/` | workflow、CODEOWNERS、PR 模板 | 规则文件，改动需批准 |

### 5.2 无源码阶段的策略

当前仓库只有 README，CI 必须"能跑且不撒谎"：

1. L0/L1 的文档与注册表检查始终生效，这是本阶段唯一的真实信号。
2. 构建、测试执行、sanitizer 作业通过 probe job 探测 `CMakeLists.txt`，不存在则 skip 并输出原因。
3. `tests/ci/` 下保留一个工具自测，用来验证注册表解析、字数口径、Diff 判定本身正确——规则也是代码，也需要测试。
4. 任何"占位通过"都会被 CI-GAT-002 判为违规：skip 不是 pass。

### 5.3 技术栈约束

- C++20 + CMake ≥3.20，预设走 `CMakePresets.json`，CI 只调用 preset 名称，不复制参数。
- CUDA 可选：`GGML_CUDA=ON` 的构建只在 GPU runner 上执行；CPU 路径必须能在无 GPU 环境完整跑通，否则 PR 闸门会被人为绕过。
- 依赖固定版本并校验哈希；`third_party/` 的任何手改都会触发 CI-DIF-006。
- 编译器矩阵：GCC 与 Clang 各一套 Debug；Release 只跑一套以控预算，矩阵扩张需要数据支撑（近 30 天无特有失败才允许删减）。

### 5.4 从文档到执行的链路

```
docs/testing.md（登记表）
    → check_tests.py（结构 + 覆盖 + 环境分区）
    → run_registry.py（按 Scope/Runner/Args 执行）
    → 报告写入 build/ci-reports/report.json
    → workflow 汇总到 job summary
```

`run_ci.py` 的 mode 与步骤一一对应，不在 workflow 里另写命令：

| mode | 步骤 | 用途 |
| --- | --- | --- |
| `fast` | docs → tests → code | 提交前/pre-commit，≤30 s 级 |
| `pr` | docs → tests → code → diff | PR 闸门 |
| `nightly` | docs → tests → code → diff → coverage | 夜间全量 + 覆盖率 |

基线规则：未传 `--base` 时取当前分支 upstream，回落 `origin/main`；分支开发必须显式传 `--base origin/<父分支>`，否则 diff 预算会被整条分支持平历史撑爆（CI-DIF-002 误报）。

任何一环出现第二份清单（例如 workflow 里硬编码 `ctest -R xxx`），都视为破坏单一事实来源，按 CI-DOC-006 处理。

---



## 6 阈值

| 阈值 | 默认值 | 依据 | 调整方式 |
| --- | --- | --- | --- |
| 要求文档字数 | 3000 | 一屏能读完，agent 每次都能完整加载 | 只能下调；上调需说明新增内容为何不可拆分 |
| 规范体积 | 18–26 KB | 足够完整，又不至于让按需查阅变成通读 | 季度复审时调整 |
| 段落重复 | ≥40 字 | 低于此长度的重复多为术语与固定句式 | 不调整 |
| Diff 预算 | 400 / 800 行 | 超过 400 行人的审查质量明显下降 | 按实测误报率调整 |
| 复杂度 | 25 / 60 行 | 与 clang-tidy 默认量级一致 | 可按模块收紧 |
| 重复代码率 | 3% / 5% | 低于 3% 多为不可避免的样板 | 仅下调 |
| flaky 隔离 | 14 天 | 足够修一次，又不至于让问题沉淀 | 不延长，只能关闭 issue 后重新登记 |
| 性能回退 | 5% | 小于此值在共享机器上不可判定 | 依据噪声带调整 |
| 例外有效期 | 90 天 | 一个季度内必然复审 | 不续期 |

阈值是项目约定，不是普适真理。调整阈值必须走规则 PR，并在 PR 中贴出触发调整的真实失败样本。

---



## 7 例外与批准

### 7.1 例外文件

`docs/ci/waivers.json`，结构：

```json
{
  "version": 1,
  "waivers": [
    {
      "rule": "CI-DIF-002",
      "scope": "src/scheduler/**",
      "owner": "xzgjj",
      "approver": "reviewer-id",
      "reason": "调度器状态机拆分，一次性重构 900 行",
      "created": "2026-11-20",
      "expires": "2027-02-18"
    }
  ]
}
```

校验要求：`rule` 必须存在于规则目录；`scope` 为仓库相对 glob；`approver` 与 `owner` 不同；`expires - created ≤ 90 天`；过期条目保留但不再生效，同时产生一条"待清理"提示。空数组是合法状态，表示当前没有例外。

### 7.2 审批矩阵

| 改动 | 需要的批准 | 不允许的批准者 |
| --- | --- | --- |
| 阈值、规则目录、workflow | 规则负责人（CODEOWNERS） | 本次 PR 作者 |
| 例外申请 | 规则负责人 | 例外受益人 |
| 受保护测试的期望值变更 | 行为负责人 + 规则负责人 | 仅测试作者 |
| 新增顶层目录 | 维护者 | agent 自动批准 |

批准只认 CODEOWNERS 的 review 与 PR 标签，不认聊天记录、口头同意、代码注释里的"已沟通"。

### 7.3 为什么例外要写进仓库

例外写在 PR 描述里会随合并消失；写进文件才能被下一轮 CI 读到，也才能回答"这条规则到底被绕过多少次"。绕过次数是规则质量的唯一硬指标：某条规则长期靠例外通过，说明阈值错了，应当修规则而不是继续批例外。

---



## 8 人工审查清单

自动检查拦不住的部分，固定在 PR 模板里，用结构化问题代替自由发挥：

1. 本次改动**最小可行范围**是什么？超出部分为什么不另开 PR？
2. 新增的抽象解决了哪个具体问题？如果删掉它，哪一行会变复杂？
3. 新文档里的信息，有哪些是代码或测试已经表达的？重复的部分为什么不删？
4. 测试改动是"行为变了"还是"让红灯消失"？前者请贴 `BEHAVIOR-CHANGE:` 说明。
5. 有没有引入只在特定机器/特定数据下成立的假设？
6. 这份改动让下一个读代码的人多花了多少时间？

六问不要求长篇回答，要求具体。回答"已优化""符合规范"视为未回答。

---

## 9 失败、重试与 flaky

- **失败消息格式**：`[规则ID] 文件:行 期望 vs 实际 → 下一步动作`。缺任一要素的消息视为规则缺陷，允许提 issue 修脚本。
- **重试**：仅允许针对基础设施型失败（拉取依赖超时、runner 掉线）。断言失败不得重试。第 3 次重试需要人工批准并记录在 PR。
- **flaky 治理**：连续 3 次在不同提交上出现"同样的代码一次红一次绿"，即判定为 flaky。处置顺序是隔离 → 定位 → 修复 → 回归。隔离期内该测试不计入闸门，但必须在 testing.md 的隔离表登记 owner 与 issue。
- **禁止**：`continue-on-error: true` 用于掩盖闸门；用 `|| true` 吞掉退出码；把失败改写成 warning。

---

## 10 指标与复审

| 指标 | 采集方式 | 目标 |
| --- | --- | --- |
| L1 时长 p90 | workflow 运行时长 | ≤3 min |
| 首次通过率 | PR 首次 L1 结果 | ≥80% |
| flaky 率 | 隔离表 + 重跑记录 | ≤2% |
| 例外数量 | waivers.json | ≤3 条常驻 |
| 规则误报 | 例外理由归类 | 每季度下降 |

复审节奏：每季度一次，只做减法——删掉从未触发过的规则、合并重复规则、收紧长期被例外绕过的规则。CI 变慢和规则变多的趋势必须被主动打断。

---

## 11 附录

### A. 新增一条规则的流程

1. 在规则目录加一行，写清判定方式、阈值、实现、豁免。
2. 脚本里加 `rule: <ID>` 标记，附一个会失败的用例。
3. 本地 `run_ci.py --mode pr` 验证误报率为零后再提 PR。
4. PR 必须附一段真实 Diff 作为正样本、一段违规 Diff 作为负样本。
5. 合并后观察两周，误报超过 3 次即考虑降级为警告或删除。

### B. 脚本契约

- 只依赖 Python 3.10+ 标准库，禁止 `pip install`。
- 统一入口 `main(argv)`，返回 0/1/2（通过/违规/环境缺失）。
- 所有输出为 UTF-8，Windows 与 Linux 行为一致，路径一律 `pathlib`。
- 支持 `--json` 输出，供 workflow 汇总；人读格式默认简洁，细节写文件。
- 不修改仓库内任何文件，只读 + 写 `build/ci-reports/`。

### C. 失败消息示例

```
[CI-DOC-001] docs/ci.md: 字数 3210 > 3000 → 删掉重复的背景说明，细则移入 ci/ci-spec.md
[CI-TST-003] tests/scheduler/test_batch.cc: 未被任何 Scope 覆盖 → 在 testing.md 增加一行 src 对应登记
[CI-DIF-003] .github/workflows/ci.yml: 规则文件改动缺少标签 ci-rule-change → 申请规则负责人 review 并打标签
```

消息必须能让人在不打开代码的情况下知道下一步做什么。做不到，就是脚本写得不够好，不是使用者的问题。
