# tests/ 目录约定

产品测试的唯一落点。CI 工具自测在 `ci/`，两者不共用目录、不共用统计口径。登记表见 [../docs/testing.md](../docs/testing.md) 2.1。

## 1 结构

```
tests/
├── unit/                     # 单函数/类，无 IO，无时钟依赖
│   ├── scheduler/            # 批次合并/拆分、请求状态机
│   ├── cache/                # KV cache 分配/回收
│   ├── sampler/              # 采样参数与固定种子
│   └── quant/                # INT4/FP8 误差界
├── contract/                 # 模块边界不变量
│   ├── scheduler_backend/    # token 预算、slot 分配
│   └── kv_cache/             # 并发下的不泄漏/不重叠
├── integration/              # 多模块协同
│   ├── batching/             # 长短请求混跑、饥饿
│   └── lifecycle/            # 取消/超时后的资源释放
├── e2e/                      # 进程级
│   └── smoke/                # 起服务、发请求、比对输出
├── perf/                     # 基准
│   ├── throughput/
│   └── latency/
├── fixtures/                 # 输入、模型桩、固定种子
│   └── models/               # 生成小模型的脚本，模型文件本身不入库
├── data/                     # 期望输出
│   └── golden/
└── tools/                    # 测试辅助：fake backend、请求生成器、断言
```

空目录用 `.gitkeep` 占位，不建空测试文件。

## 2 命名

- C++ 用例：`*_test.cc`，一个文件对应一组同源行为。
- 脚本用例（请求生成、结果比对）：`test_*.py`。
- 辅助代码不进 `tests/unit` 等层级目录，放 `tests/tools`，避免被当成用例统计。
- 目录与源码同构优先：`src/scheduler/batch.cpp` 的用例放 `tests/unit/scheduler/`。

## 3 层级与闸门

| 目录 | Level | 闸门 | Metadata |
| --- | --- | --- | --- |
| `tests/unit/*` | unit | L1/L2 | `fast`, `hermetic` |
| `tests/contract/*` | contract | L2 | `fast`, `hermetic` |
| `tests/integration/*` | integration | L2 | `fast`, `hermetic` |
| `tests/e2e/smoke` | e2e | L4 | `slow`, `needs-vm` |
| `tests/perf/*` | perf | L4 | `slow`, `perf-baseline` |
| 复用以上用例 | sanitizer | L3 | 由 CI 以 `-fsanitize` 重跑，不单建目录 |

`needs-gpu` 的用例必须同时能在 CPU 上跑降级版本，否则 PR 闸门会被绕过。

## 4 登记

新增用例后，在 `docs/testing.md` 2.1 加一行：

| Scope | Level | Runner | Args | Metadata |
| --- | --- | --- | --- | --- |
| `tests/unit/scheduler` | `unit` | `ctest` | `-R unit.scheduler --output-on-failure` | `fast`, `hermetic` |

Scope 写目录，不写单个文件——目录内新增文件自动被覆盖，漏登记会由 `check_tests.py` 报出具体路径。

## 5 构建与执行

- 每个用例通过 CTest 注册，`ctest` 的 label 与 Metadata 同名（`fast` / `slow` / `needs-gpu` / `protected`），CI 用 label 选择，不再写第二份命令。
- 首个用例落地时再补 `tests/CMakeLists.txt` 与根 `add_subdirectory(tests)`。现在不写，避免留下没有目标的 CMake 代码。
- 断言库默认沿用零依赖风格（`assert` + 明确失败信息），引入第三方框架需在规则 PR 里说明理由。

## 6 数据

- 小样本（<2 MB）可入库，放 `data/`、`fixtures/`；模型权重、大词表一律由脚本生成或下载，不入库。
- 期望输出用文本/JSON，带版本与生成命令，避免"某次跑出来的数"无法复现。
- 固定随机种子与输入长度写在用例内，不依赖环境变量。
