# CI 规则（执行版）

> 本文件 ≤3000 字，只回答"怎么做"。完整判定、阈值依据与失败消息见 [ci/ci-spec.md](ci/ci-spec.md)；测试如何登记见 [testing.md](testing.md)。
> 分工：本文件 = 入口与决策表；ci-spec.md = 细则（按需查阅）；testing.md = 测试唯一事实来源。三者冲突时以细则的规则目录为准，并立即修正另两份。



## 1 定位

项目尚无代码，CI 先行。目标不是"跑绿"，而是让可判定的错误在改动后几分钟内变成红灯，把不可判定的问题固定交给人。agent 容易把细节堆进需求文档、为小需求造抽象、用改测试的方式消除失败；CI 拦住可判定的部分，人保留判断权。



## 2 分层闸门

| 层 | 触发 | 预算 | 内容 | 失败含义 |
| --- | --- | --- | --- | --- |
| L0 | pre-commit | 10 s | 文档字数、重复段落、测试注册表、格式 | 本次提交不合规，不推送 |
| L1 | 每次 push / PR | 3 min | L0 + 规则自洽 + Diff 预算 + 范围白名单 | PR 不可合并 |
| L2 | PR | 15 min | Debug/Release 构建（警告即错误）+ fast 测试 + 覆盖率（`src/` 行 ≥70%、改动行 ≥80%） | 编译失败、行为回归或覆盖不足 |
| L3 | PR（有 CMake 工程） | 30 min | ASan+UBSan、TSan、受保护回归 | 内存/并发缺陷，最高优先级 |
| L4 | nightly | 2 h | slow 测试、性能基线、GPU 队列 | 阻塞发版，不阻塞个人 PR |

未产生源码或 GPU 时 L2/L3/L4 自动跳过，必须打印原因；skip 不算 pass。



## 3 失控模式与拦截

| 失控模式 | 自动检查（可判定） | 人工判断（不可判定） |
| --- | --- | --- |
| 细节堆砌、文档膨胀 | 字数预算、重复段落、必需要素 | 内容是否值得保留 |
| 造多余抽象 | 重复代码率、未使用代码 | 抽象是否必要 |
| 小任务变大重构 | 范围白名单、Diff 行数预算 | 超范围是否批准 |
| 代码能跑但读不懂 | clang-format、clang-tidy、复杂度上限 | 命名与设计审查 |
| 改测试让失败消失 | 受保护测试、删除告警、注册表一致 | 行为变更是否合理 |
| 顺手关闭 CI 规则 | 规则文件改动需指定人批准 + 标签 | 批准者与开发者分离 |
| 内存与并发缺陷 | ASan/UBSan/TSan 作业 | 竞态是否属设计之内 |



## 4 阈值速查

| 项 | 阈值 | 说明 |
| --- | --- | --- |
| 要求类文档 | 3000 字 | CLAUDE.md、SPEC.md、implementation_plan、AGENT_EXECUTION_PROTOCOL、本文件 |
| 规范细则 | 18–26 KB | `docs/ci/ci-spec.md`，按需查阅 |
| 段落重复 | ≥40 字 | 归一化后跨文档重复即失败 |
| 核心代码 Diff | 400 警告 / 800 失败 | 测试、文档不计 |
| 函数复杂度 | 25 / 60 行 | 与 `.clang-tidy` 一致 |
| 重复代码率 | 3% 警告 / 5% 失败 | 6 行窗口 |
| 例外有效期 | 90 天 | 到期失效，不续期 |



## 5 命令

```bash
python scripts/ci/run_ci.py --mode fast          # L0+L1，等价于 CI
python scripts/ci/run_ci.py --mode pr            # 加 Diff 预算与代码质量
python scripts/ci/check_docs.py                  # 只跑文档规则
python scripts/ci/check_tests.py                 # 注册表结构与覆盖
python scripts/ci/run_registry.py --speed fast   # 按注册表执行测试
python scripts/ci/check_coverage.py --build-dir build-cov --base origin/main   # 覆盖率闸门
```

本地与 CI 的判定必须一致，唯一允许的差别是 matrix 与 sanitizer。



## 6 例外

例外写进 [ci/waivers.json](ci/waivers.json)，每条必须有 owner、approver（非同一人）、理由、到期日（≤90 天）。过期即失效，不续期。PR 评论里的"以后修"不构成例外。



## 7 红线

1. 不得删除测试来消除红灯；行为变了就改期望，并附 `BEHAVIOR-CHANGE:` 说明。
2. 不得在 PR 里降低阈值或跳过检查；改规则需独立 PR + 规则负责人批准。
3. 不得无限重试：单 job 最多 2 次，第 3 次必须定位原因或标记 flaky 隔离。
4. 不得把"环境跑不了"当通过：缺失必须失败，或显式 skip 并记录原因。



## 8 落地

| 位置 | 作用 |
| --- | --- |
| `.github/workflows/ci.yml` | L1–L4 作业；无源码时构建与 sanitizer 自动跳过 |
| `.github/CODEOWNERS` | 规则文件与闸门的批准人 |
| `.githooks/pre-commit` | L0；用 `git config core.hooksPath .githooks` 启用 |
| `scripts/ci/*.py` | 闸门实现，仅标准库，无三方依赖 |
| `ci/` | 闸门脚本自测，与 `tests/` 的产品测试分属两套，不计入覆盖率 |
| `docs/testing.md` | 测试注册表（产品测试 + CI 自测两张表），CI 与本地共用 |
| `docs/ci/log-policy.md` | 日志与运行产物的落点、字段与保留上限；`logs/` 永不入库 |

首次启用三步：启用钩子 → 跑一次 `--mode pr` → 提交时打上 `ci-rule-change` 标签。钩子入库要保留可执行位（`git update-index --chmod=+x .githooks/pre-commit`）。新增规则按细则第 11 节流程走，先写负样本再写脚本。
