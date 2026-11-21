# 阶段 8 改动基线取证报告（只取证与定位，未改源码）

- 取证时间：2026-11-21 15:19–15:25 (UTC+8)
- 仓库：`D:\c++_myproject\llm-inference-kernel-opt`（WSL 内 `/mnt/d/c++_myproject/llm-inference-kernel-opt`），git 分支 `8`
- 构建目录：WSL Ubuntu-26.04 内 `/root/llmopt-build`（Release / Ninja / `LLMOPT_ENABLE_LLAMA=ON`）
- 取证口径：所有结论来自实际执行命令，每条命令均有退出码与原始日志（见 `evidence/raw/`）；未执行 `git add/commit/push/checkout/stash/reset`，未修改任何源码、测试、脚本。
- 工作区改动（`git status --porcelain` 实测）：41 个已修改 + 22 个未跟踪；其中 C/HPP/CC 修改 12 个，无新增 C++ 源文件（未跟踪项中仅 `tests/tools/test_capacity_check.py`、`tests/tools/test_watch_matrix.py` 两个 Python 测试）。

---

## ① 环境与版本

| 项 | 值 | 来源 |
| --- | --- | --- |
| 操作系统 | WSL2 Ubuntu-26.04，kernel 6.18.58.1-microsoft-standard-WSL2，x86_64 | `evidence/raw/env_versions.txt` |
| 编译器 | gcc/g++ 15.2.0-16ubuntu1（`/usr/bin/c++`） | 同上 |
| CMake | 4.2.3 | 同上 |
| Ninja | 1.13.2 | 同上 |
| Python | 3.14.4 | 同上 |
| clang-format | Ubuntu clang-format 21.1.8 (6ubuntu1) | 同上 / `clang_format.log` |
| 构建类型 | `CMAKE_BUILD_TYPE=Release`，生成器 Ninja | `CMakeCache.txt` |
| LLAMA 后端 | `LLMOPT_ENABLE_LLAMA=BOOL=ON` | `CMakeCache.txt` |
| 编译告警旗标 | GNU/Clang：`-Wall -Wextra -Werror -Wpedantic -Wshadow`（`llmopt_warnings` 接口库） | `CMakeLists.txt:19-21` |
| 预设环境变量 | 无 `LLMOPT_*` / `LLAMA_*`；`models/` 为空，`/root` 无 GGUF | 取证时 `env` / 目录列举 |

`.clang-format` 关键键：`BasedOnStyle: LLVM`、`Standard: c++20`、`ColumnLimit: 100`、`IndentWidth: 4`、`ContinuationIndentWidth: 8`，均为 clang-format 21 支持的标准键，版本与配置匹配，解析无告警。

---

## ② 命令表

| # | 命令（工作目录） | 退出码 | 通过 / 失败 / 跳过 | 耗时 | 备注 |
| --- | --- | --- | --- | --- | --- |
| 1 | `cmake --build /root/llmopt-build -j$(nproc)`（`/root/llmopt-build`） | 0 | — | 0 s | 首次增量：`ninja: no work to do`；对象文件新于全部源码，构建树与当前源码同步。日志 `build_incremental_nop.log` |
| 2 | 定向重编：删除 8 个改动 TU 的 `.o` 后 `cmake --build /root/llmopt-build -j$(nproc)` | 0 | 33 步（8 编译 + 25 链接） | 9 s | 为在本次取证中直接采集当前源码的编译器 warning；0 warning / 0 error。日志 `build.log` |
| 3 | `ctest -N`（`/root/llmopt-build`） | 0 | 注册 165 条用例 | — | 日志 `ctest_list.log` |
| 4 | `ctest --print-labels` | 0 | 9 个标签 | — | 标签：`asan, contract, e2e, fast, hermetic, integration, needs-gpu, slow, unit`。日志 `ctest_labels.log` |
| 5 | `ctest -N -L fast` | 0 | fast 子集 164 条 | — | 日志 `ctest_fast.log` |
| 6 | `ctest --output-on-failure`（无 `-L` 过滤，全量） | 0 | **164 通过 / 0 失败 / 1 跳过** | 23.25 s（ctest real time） | 日志 `ctest_full.log`；跳过用例见 §⑤ |
| 7 | `python3 scripts/ci/run_ci.py --mode pr`（仓库根） | **1** | docs PASS / tests PASS / **code FAIL** / diff PASS | 6 s | base=`origin/8`。日志 `ci_pr.log` |
| 8 | `python3 scripts/ci/check_code.py`（仓库根） | **1** | 1 error + 1 warning；deps/size/scope PASS | 3 s | 日志 `check_code.log` |
| 9 | `python3 ci/test_phase4_scanners.py -v`（仓库根） | 0 | 5 ok / 0 失败 | 1 s（0.326 s 测试内） | 日志 `scanners.log` |
| 10 | `clang-format --dry-run -Werror` × 12 个改动 C/HPP/CC 文件 | **1** | 11 文件干净；1 文件 2 处违规 | — | 日志 `clang_format.log`、`clang_format_gguf_test.diff` |
| 11 | （补充，非 ctest）`python3 -m unittest discover -s tests/tools -t . -v` | **1** | 74 ok / **1 error** / 0 失败 | 1.763 s | 注册表 `tests/tools` 套件；日志 `tools_unittest.log`、复跑 `tools_report_rerun.log` |
| 12 | （补充，非 ctest）`python3 -m unittest discover -s ci -t . -v` | 0 | 21 ok / 0 失败 | 3.796 s | CI 工具自测全量；日志 `ci_unittest.log` |

### ctest 标签分布（来自 `ctest_full.log` Label Time Summary）

| 标签 | 用例数 | 标签 | 用例数 |
| --- | --- | --- | --- |
| unit | 110 | integration | 13 |
| contract | 41 | e2e | 1 |
| fast | 164 | hermetic | 156 |
| asan | 3 | needs-gpu | 1 |
| slow | 1 | | |

---

## ③ 构建 warning 清单

**清单为空：0 条编译器 warning，0 条 error。**

- 首次增量构建无活可干（对象新于源码）；定向重编 8 个改动 TU（`src/config.cpp`、`src/gguf.cpp`、`src/metrics.cpp`、`src/scheduler.cpp`、`src/main.cpp`、`tests/unit/config_test.cc`、`tests/unit/gguf_test.cc`、`tests/unit/metrics_test.cc`）并重链 `libllmopt_core.a`、`llmopt_server` 与全部测试可执行文件，`build.log` 中 `grep -E "warning|error|Werror"` 无任何命中。
- 构建带 `-Werror`，本次重编在该旗标下通过；未观察到警告即错误类问题。

---

## ④ 失败用例清单

### 4.1 ctest 全量套件

**无失败用例。** 165 条中 164 条 Passed、0 条 Failed、1 条 Skipped；无端口占用 / 超时类疑似 flaky 现象，未触发重试。

### 4.2 补充 Python 套件（`tests/tools`，不在 ctest 注册口径内）

| 用例名 | 标签（注册表） | 失败摘要 | 是否稳定复现 | 定位线索 |
| --- | --- | --- | --- | --- |
| `tests.tools.test_report.ReportTest.test_offline_rebuild_from_jsonl` | `tests/tools`，`fast`, `hermetic` | `ERROR`：`TypeError: MatrixConfig.__init__() missing 6 required positional arguments: 'n_gpu_layers', 'threads', 'kv_cache_type', 'donor_slots', 'disable_prefix_cache', 'chunk_size'` | **稳定复现，非 flaky**：首轮 discover 失败；随后单跑重跑 2 次 + 落盘复跑 1 次，共 3 次均同一 `TypeError`（`tools_report_rerun.log`，`EXIT_CODE=1`） | 调用链 `tests/tools/test_report.py:45` → `tools/bench/report.py:62 rebuild` → `report.py:38 config_from_manifest`。`report.py:38-57` 仍按旧 16 参构造 `matrix.MatrixConfig`；`tools/bench/matrix.py:102-107` 已把 `n_gpu_layers / threads / kv_cache_type / donor_slots / disable_prefix_cache / chunk_size` 列为必填字段；测试 fixture `tests/tools/test_report.py:13-25` 的 manifest 同样不含这 6 个字段。`tools/bench/matrix.py` 属并行维护禁改侧，对齐点在 `report.py` 构造时补这 6 个字段（从 manifest 读取或给默认值），或由 `MatrixConfig` 提供默认参数。 |

> 说明：该失败属于 Python 工具链层（离线报告重建路径），不影响 Release 二进制与 ctest 套件；按任务约束未修改 `tools/bench/matrix.py`、`scripts/bench/matrix/*.json`、`docs/**`、`bench/results/**`。

---

## ⑤ 跳过用例清单

| 用例名 | 标签 | 跳过原因 |
| --- | --- | --- |
| `#165 e2e_real_service_contract` | `e2e`, `slow`, `needs-gpu` | 未设置 `LLMOPT_REAL_BASE`；脚本 `tests/e2e/test_real_service_contract.py:41-44` 打印 `SKIP: 未设置 LLMOPT_REAL_BASE，真实服务契约用例不执行`，以 `SKIP_RETURN_CODE=77` 退出（实测 RC=77，见 `e2e_skip.log`）。按任务约束未启动 8B 真模型服务（GPU 正被长 prompt 矩阵占用）。**跳过不计为通过。** |
| `#151 runner_raii_test.真模型 load->tokenize->release->reload` | `unit`, `fast`, `asan` | ctest 计为 Passed，但 `LLMOPT_TEST_MODEL` 未设置；`tests/unit/runner_raii_test.cc:51-54` 在环境变量缺失时直接 `return` 早退，**真模型 load → tokenize → release → reload 路径本次未被执行**。非 ctest 跳过，属"真模型覆盖缺失"，单独标注以免误判为已验证。 |

其余 13 条 integration 用例均以 stub 后端起子进程通过，未依赖真模型 / 真服务 / GPU。

---

## ⑥ CI 闸门违规清单

| 规则编号 | 文件:行 | 原文 | 初步归因 |
| --- | --- | --- | --- |
| **CI-COD-003**（error，阻塞） | `src/config.cpp:365` | `validate_config 复杂度 27/25、行数 56/60` | `validate_config` 内串联 12 段以上范围 / 枚举校验 `if`，决策点累计 27 超过复杂度上限 25（函数体 56/60 行未超限）。文件内已有 `validate_sched_config`、`validate_service_limits` 子函数先例，可将剩余端口 / 上下文 / 并发 / safety-ratio / donor / kv-cache-type / backend 等校验继续抽取为子函数以降复杂度。 |
| **CI-COD-004**（warning，不阻塞） | `src`（全量重复率） | `重复率 3.21% > 3%（警告）` | 6 行窗口重复代码率 3.21%，略超 3% 警告线，未达 5% 失败线；warning 级，需观察是否继续上升。 |
| **CI-COD-001**（本地手动改动文件扫描；CI 脚本口径外） | `tests/unit/gguf_test.cc:62:51`；`tests/unit/gguf_test.cc:140:47` | `error: code should be clang-formatted [-Wclang-format-violations]`（2 处）。期望改动见 `clang_format_gguf_test.diff`：62 行默认参数折行 `write_profile(const std::string& text,\n                          const std::string& name = ...)`；140-141 行调用合并为单行 `write_profile(R"(...)")` | `check_code.py` 的 `format_check` 只扫描 `SOURCE_DIRS = ("src", "include", "benchmarks")`，`tests/` 不在格式闸门范围内，因此 CI-COD-001 未在 `check_code.py` 输出中拦截该测试文件；本次按任务要求对改动文件单独跑 `clang-format --dry-run -Werror` 暴露。改动侧需对该文件执行 `clang-format -i`。 |
| 观察项：diff 闸门未覆盖未提交工作区 | `run_ci.py --mode pr` diff 步 | `改动文件 0 个`、`核心代码改动 0 行`、`生效例外 0 条`；`base: origin/8` | 三点比较 `origin/8...` 不含工作区未提交修改与未跟踪文件，因此本次未提交批次的 CI-DIF-001（范围白名单）、CI-DIF-002（Diff 预算）、CI-DIF-003（规则文件标签）、CI-DIF-004（受保护测试）等规则在本地本次运行**未被有效评估**；需在提交 / PR 环境以正确基线复核。注意：工作区存在未跟踪顶层目录 `bench/` 与根级 `.pulse.json`，按白名单口径在 PR diff 中可能触发 CI-DIF-001，此处仅提示，未改动。 |

### 闸门通过项（留痕）

- **docs PASS**：`docs/ci.md` 1133/3000 字、`CLAUDE.md` 2738/3000、`implementation_plan.md` 2260/3000、`AGENT_EXECUTION_PROTOCOL.md` 1106/3000、`docs/ci/ci-spec.md` 24533 bytes（18–26 KB 区间内）。
- **tests PASS**：48 个测试文件、6 条登记，注册表结构与覆盖检查通过。
- **code 子项**：deps PASS（直接第三方 `doctest / httplib / nlohmann_json / spdlog`，无纸面依赖）；size PASS（6768 行 / 4 模块 / 4 直接依赖）；scope PASS（37 个源文件、38 个测试文件均登记）。
- **阶段 4 扫描器自测**：5/5 OK（依赖清零、规模口径、范围与测试登记）。
- **ci/ 自测全量**：21/21 OK。

### 本次未执行的闸门 / 用例（边界说明）

- 覆盖率闸门 CI-COV-001/002：需 `LLMOPT_COVERAGE=ON` 插桩构建，不在本次 Release 基线范围。
- ASan/UBSan、TSan 构建（CI-MEM-001/002，L3）：未在本次 Release 构建上重跑。
- 真 8B 服务契约、真后端 runner 路径：按任务约束未启动；对应覆盖缺口见 §⑤。
- `deploy_smoke`（Docker）、`soak_30m`：本机 / 本次基线未执行。

---

## 附：证据文件清单（绝对路径）

报告：
- `D:\c++_myproject\llm-inference-kernel-opt\evidence\phase8_baseline.md`

原始日志（`D:\c++_myproject\llm-inference-kernel-opt\evidence\raw\`，WSL 路径 `/mnt/d/c++_myproject/llm-inference-kernel-opt/evidence/raw/`）：

| 文件 | 内容 |
| --- | --- |
| `env_versions.txt` | 编译器 / cmake / ninja / python3 / clang-format 版本与构建缓存关键项 |
| `build_incremental_nop.log` | 首次增量构建（no work to do，rc=0） |
| `build.log` | 定向重编 33 步完整输出、退出码与耗时、warning 扫描依据 |
| `ctest_list.log` | `ctest -N` 全量 165 条清单 |
| `ctest_labels.log` | `ctest --print-labels` 标签列表 |
| `ctest_fast.log` | `ctest -N -L fast` fast 子集 164 条清单 |
| `ctest_full.log` | 全量 `ctest --output-on-failure` 完整输出与汇总 |
| `e2e_skip.log` | e2e 跳过脚本直跑输出（RC=77）与环境变量引用梳理 |
| `ci_pr.log` | `run_ci.py --mode pr` 全闸门输出 |
| `check_code.log` | `check_code.py` 单独输出 |
| `scanners.log` | `test_phase4_scanners.py -v` 输出 |
| `clang_format.log` | 12 个改动文件逐个 `clang-format --dry-run -Werror` 结果 |
| `clang_format_gguf_test.diff` | `gguf_test.cc` 期望格式化 diff |
| `tools_unittest.log` | 补充：`tests/tools` unittest discover 全量输出（75 例，1 error） |
| `tools_report_rerun.log` | 补充：失败用例稳定复现记录（rc=1） |
| `ci_unittest.log` | 补充：`ci/` unittest discover 全量输出（21 例 OK） |
