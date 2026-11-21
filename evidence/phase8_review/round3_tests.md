# Phase 8 第 3 轮独立审查：测试有效性、回归覆盖与新增脚本可运行性（只读静态）

- 轮次：R3（三轮独立审查之第 3 轮）
- 仓库：`D:\c++_myproject\llm-inference-kernel-opt`（WSL `/mnt/d/c++_myproject/llm-inference-kernel-opt`），分支 `8`
- 审查口径：未提交改动（`git diff` + 未跟踪文件）的测试与脚本；**只读**，未修任何源码/测试/脚本/文档/证据
- 方法：`git diff` 逐文件断言核对 + 源码通读 + Grep 定位 + 字节级换行/模式检查；允许的执行仅限纯 Python 校验（`py_compile`、两个新 hermetic 模块与 `test_matrix.py` 的 `unittest`），**未**跑 cmake/ctest、未起服务、未占端口、未碰 GPU
- 时间基准：2026-11-21（UTC+8）
- 约束遵守：未 `git add/commit/push/checkout/stash/reset`；未改 `tools/bench/matrix.py`、`scripts/bench/matrix/*.json`、`docs/**`、`bench/results/**`；唯一写文件为本报告

## 1. 结论概览

| 严重度 | 数量 | 说明 |
| --- | --- | --- |
| P0（确认致命：恒真断言/吞掉失败/回归裸奔） | 0 | 未发现 |
| P1（确认缺口：新对外行为在 ctest 中无任何可失败用例） | 1 | R3-01 |
| P2（待验证或局部缺陷：断言偏弱/隔离泄漏/脚本兜底缺失） | 7 | R3-02 … R3-08 |
| P3（观察项/口径提示） | 3 | R3-09 … R3-11 |

**总体判断**：本轮改动的既有测试**没有**出现"删断言、放宽阈值、加 skip、确定值换范围、try/except 吞错"的痕迹；相反，`test_server_matrix.py` 的显存口径、`test_real_service_contract.py` 的健康判定、`test_matrix.py` 的 token 口径与显存解析都是**加强**。新增 6 个 hermetic Python 用例与 `test_matrix.py` 16 例在本机 WSL 实跑全部通过（0.002s / 0.225s），语法编译全绿。主要风险集中在：**capacity-only 新启动路径只有单元级与 mock 级覆盖，没有进程级回归**；两个新指标的"行为语义"只做了名解析/采样解析，未在服务端口径上对账；以及少量测试隔离与脚本兜底缺口。

### 1.1 实跑记录（纯 hermetic，未起服务）

| 命令 | 结果 |
| --- | --- |
| `python3 -m py_compile` 7 个改动/新增 .py | `COMPILE_OK` |
| `python3 -m unittest tests.tools.test_watch_matrix tests.tools.test_capacity_check -v` | 6/6 ok（watch 2 + capacity 4） |
| `python3 -m unittest tests.tools.test_matrix -v` | 16/16 ok |
| `bash -n run_long_matrix.sh` / `run_real_contract.sh` | 语法 OK |
| `capacity_check.py --help` / `watch_matrix.py --help` | argparse OK |

## 2. 发现项明细

### R3-01 [P1] `--capacity-only` / `--model-profile` 主启动路径无进程级回归，早退与"不监听"行为无人守

- **文件:行**：`src/main.cpp:136-147`（profile 分支）、`src/main.cpp:223-232`（capacity-only 早退）；对照 `tests/unit/config_test.cc:103-119`、`tests/unit/gguf_test.cc:120-152`、`tests/tools/test_capacity_check.py:46-58`
- **证据原文**：
  ```cpp
  // src/main.cpp:136-147
  GgufMeta meta;
  if (cfg.model_profile_path.empty()) { meta = read_gguf_meta(cfg.model_path); }
  else {
      meta = read_model_profile(cfg.model_profile_path);
      if (meta.ok) { std::cout << "capacity-profile: path=..." ...; }
  }
  // src/main.cpp:223-232
  if (gate_rc != kExitOk) {
      if (cfg.capacity_only) { std::cout << "capacity-only: allow_start=0\n"; }
      return gate_rc;
  }
  if (cfg.capacity_only) {
      std::cout << "capacity-only: allow_start=1\n";
      return kExitOk;   // 不加载权重、不监听端口
  }
  ```
  现有覆盖中，`test_capacity_check.py:49` 与 `:69` 均 `mock.patch.object(capacity_check.subprocess, "run", ...)`，**子进程被替身**；`test_server_smoke.py:158-169` 的 `probe_gate_profile` 只构造 `--model` 命令，从未传 `--model-profile`/`--capacity-only`。
- **影响（失去的回归保护）**：以下任一回归都不会被 ctest 捕获——① capacity-only 仍走到 `listen`/加载权重；② profile 元数据未参与预算（仍按空/缺省估算）；③ profile 解析失败未按启动失败退出；④ `allow_start=1/0` 行缺失或退出码与门控结论不一致；⑤ env 键 `LLMOPT_MODEL_PROFILE`/`LLMOPT_CAPACITY_ONLY` 在进程级未接上。配置校验（unit）与档位 JSON 解析（unit）已覆盖，但 `main()` 装配层是空白。
- **确认状态**：确认缺口（静态核对：`tests/` 全树 grep `capacity-only|model-profile` 仅命中 unit/config、unit/gguf 与被 mock 的 tools 测试；无 integration/e2e 用例）。
- **最小修复建议**：在 `test_server_smoke.py`（或新 integration 用例）复用 `probe_gate_profile` 模式，增加一条 profile 变体：写临时档位 JSON → 以 `--model-profile <json> --capacity-only --backend stub` 起一次性子进程 → 断言早退码、stdout 含 `capacity-profile:` 与 `capacity-only: allow_start=`、且未绑定端口（轮询 `/healthz` 应拒连）。
- **最小回归用例**：`integration_capacity_only_profile`（建议名）
  - 断言点：退出码 ∈ {0, 3, 4}；stdout 含 `decision=` 与 `capacity-only: allow_start=`；`allow_start=1` 时端口仍拒连（证明未 listen）；profile 缺字段时退出码为启动失败且 stderr 含档位原因。

### R3-02 [P2] `llmopt_kv_used_blocks_peak` 只验名解析，峰值"非递减/排空后保留"语义无断言

- **文件:行**：`src/scheduler.cpp:688-690`；`tests/unit/metrics_test.cc:54`；对照 `tests/integration/test_server_metrics.py:73-78`
- **证据原文**：
  ```cpp
  // src/scheduler.cpp:689-690
  kv_used_peak_ = std::max(kv_used_peak_, usage.used_blocks);
  metrics_.set(MetricId::KvUsedPeak, kv_used_peak_);
  ```
  ```cpp
  // metrics_test.cc:54 —— 仅序列名可解析
  CHECK(llmopt::find_metric("llmopt_kv_used_blocks_peak") == llmopt::MetricId::KvUsedPeak);
  ```
  集成测试只对 running_peak 有"收尾后运行数为 0、峰值 ≥1"的对账（`test_server_metrics.py:73-75`），没有对 KV 峰值的同类断言。
- **影响**：若 `kv_used_peak_` 被写成覆盖写（丢失 `max` 累积）、或排空后峰值被刷成当前 0，现有用例不会失败；峰值水位（watermark 判定依据）失去回归保护。
- **确认状态**：确认缺口。
- **最小修复建议**：在 `test_server_metrics.py` 收尾快照处补一条：`kv_used_blocks_peak ≥ kv_used_blocks`，且峰值 ≥ 压测期间观测到的任意 used 快照；或在 contract 层用两段不同占用量喂 `publish_kv_usage` 后核对峰值。
- **最小回归用例**：`integration_server_metrics` 增补断言 `samples["llmopt_kv_used_blocks_peak"] >= samples["llmopt_kv_used_blocks"]`；contract 用例「KV 峰值随占用取大、排空不清零」。

### R3-03 [P2] 确认缺陷：`clear_env()` 未清理两个新 env 键，用例间进程环境污染

- **文件:行**：`tests/unit/config_test.cc:35-61`（清理清单）、`:113-114`（设置新键）、`:118`（清理调用）
- **证据原文**：
  ```cpp
  // :35-61 clear_env 的键清单（节选）——没有 LLMOPT_MODEL_PROFILE / LLMOPT_CAPACITY_ONLY
  for (const char* key : {"LLMOPT_MODEL", "LLMOPT_CONFIG", ... "LLMOPT_STEP_TRACE_RATE"}) {
      set_env(key, "");
  }
  // :113-114 新用例设置了清单外的两个键
  set_env("LLMOPT_MODEL_PROFILE", "env.json");
  set_env("LLMOPT_CAPACITY_ONLY", "1");
  // :118 结束时 clear_env()，但上述两键仍残留在进程环境
  ```
- **影响**：doctest 同进程串行跑例，后续用例继承 `LLMOPT_MODEL_PROFILE=env.json` 与 `LLMOPT_CAPACITY_ONLY=1`。当前后续用例都带 `--model m.gguf`，校验仍放行，故本轮全绿；但任何后续新增"不带 model"的反向用例都会被残留的 capacity-only 错误放行，形成隐性假绿。
- **确认状态**：确认缺陷（清单比对实锤；当前为潜伏触发）。
- **最小修复建议**：把 `LLMOPT_MODEL_PROFILE`、`LLMOPT_CAPACITY_ONLY` 加入 `clear_env()` 键数组。
- **最小回归用例**：现有「容量档位可免 model…」用例末尾补 `CHECK(cfg.model_profile_path.empty()); CHECK_FALSE(cfg.capacity_only);` 风格的隔离自检；或在其后紧接一个无 model 用例确认仍 `ConfigInvalid`。

### R3-04 [P2] e2e 新口径 `normalize_base` 无 hermetic 单测；非 200 体的 `json.loads`/`int` 裸调用可直接崩用例

- **文件:行**：`tests/e2e/test_real_service_contract.py:17-24`（新校验）、`:53`、`:75`、`:86`、`:91-93`
- **证据原文**：
  ```python
  # :17-24 新增的根地址校验，逻辑只在真服务（CI 中 SKIP 77）上才会走到
  def normalize_base(raw):
      parsed = urllib.parse.urlparse(raw)
      if parsed.scheme not in ("http", "https") or not parsed.hostname: raise ValueError(...)
      if parsed.path not in ("", "/"): raise ValueError(...)
      return parsed.hostname, parsed.port or (443 if ... else 80)
  ```
  ```python
  # :75 / :86 / :93 响应体未判 JSON 就解析
  payload = json.loads(body)                       # 非 200/非 JSON → 抛异常，记不进 failures
  message = json.loads(body)["error"]["message"]
  check(status == 400 and json.loads(body)["error"]["code"] == "CONTEXT_OVERFLOW", ...)
  # :91 环境变量非整数直接 ValueError
  overflow_words = int(os.environ.get("LLMOPT_E2E_OVERFLOW_TOKENS", "1600"))
  ```
- **影响**：① 带路径 base、非法 scheme、https 默认端口等新分支没有可在 fast 闸门跑到的单测，只能等 needs-gpu 的 e2e 夜间跑；② 服务返回 HTML/纯文本错误时用例以 `JSONDecodeError`/`ValueError` 硬崩，而不是把该条记入 `failures` 继续报契约偏差，失败定位变差；③ `LLMOPT_E2E_OVERFLOW_TOKENS` 配成非数字会崩在前置解析。
- **确认状态**：确认缺陷（健壮性）；分支覆盖缺口待验证（验证方法：把 `normalize_base` 拆成可 import 的纯函数后补 hermetic 用例）。
- **最小修复建议**：`json.loads` 包一层 `try/except` 取空 dict 再喂 `check`；`overflow_words` 解析失败回退默认并告警；把 `normalize_base` 放到可单测位置补路径/非法 scheme/默认端口三例。
- **最小回归用例**：`test_real_contract_base_parse`（hermetic）——`http://h:8080/v1` 抛错、`https://h` 端口取 443、`http://h:8080` 正常；契约用例内：错误响应体为非 JSON 时记 failure 而非崩溃。

### R3-05 [P2] `watch_matrix` 主循环（STALL、`--until-report`、停滞年龄）零覆盖；snapshot 对非数值字段无容错

- **文件:行**：`scripts/bench/watch_matrix.py:40-42`（字段取值）、`:78-90`（循环/STALL/退出）；`tests/tools/test_watch_matrix.py:12-34`（仅 2 个 snapshot 例）
- **证据原文**：
  ```python
  # :40-42 缺字段/非数值时 int(None) 或 int("x") 直接 TypeError
  key = (int(record.get("concurrency", 0)), str(record.get("tier", "")),
         int(record.get("round", 0)))
  # :82-89 STALL 标注与 report 出现后退出，均无测试
  if age is not None and age >= args.stall_after: line += f" STALL(...)"
  if args.until_report and state["report"]: return 0
  ```
- **影响**：`last_event_age_s` 停滞判定、报告出现即退出这两条脚本核心承诺没有可失败用例；脏 JSONL（`concurrency` 为 null/字符串）会让监控进程崩掉，而设计意图是"坏行跳过不崩"（模块 docstring 与现有 snapshot 例都按此口径）。
- **确认状态**：确认缺口 + 确认容错不一致。
- **最小修复建议**：取值包 `try/except (TypeError, ValueError)` 归 0/空串；主循环抽 `decide(state, now)` 纯函数以便单测 STALL 与退出条件；补一例 concurrency 为 null 的脏行。
- **最小回归用例**：`watch_matrix_stall_label`（年龄 ≥ stall-after 出 STALL 行）、`watch_matrix_until_report_exit`（report 存在返回 0）、`watch_matrix_dirty_row_tolerant`（null 字段不抛）。

### R3-06 [P2] `capacity_check.py` 子进程超时/非零无兜底，整轮在写 CSV 前崩掉；解析负样本偏薄

- **文件:行**：`scripts/bench/capacity_check.py:100`（`subprocess.run` 无 try）、`:135-147`（逐档收集后一次性写 CSV）
- **证据原文**：
  ```python
  # :100 TimeoutExpired/CalledProcessError 直接向上抛，run_one 不兜底
  result = subprocess.run(command, capture_output=True, text=True, timeout=timeout_s, env=env)
  # :135-147 任一档抛异常 → rows 不落盘，已跑档位证据全丢
  for ngl in [...]:
      row = run_one(...); rows.append(row)
  with out.open("w", ...) as handle: writer.writerows(rows)
  ```
  解析侧只有 `test_missing_lines_stay_blank`（空行留空）一例；没有"半行/字段非数字/decision 小写"的负样本。
- **影响**：长扫档中一档挂起超时会让整份容量 CSV 缺失，无法对账其余档位；正则对脏输出的容错行为未锁定。
- **确认状态**：确认缺陷（兜底）；解析容错缺口待验证。
- **最小修复建议**：`run_one` 捕获 `subprocess.TimeoutExpired`/`OSError`，落一行 `decision=""`、`exit_code=124/1` 的占位行继续扫；先写表头再逐行 append，避免崩溃丢全部行。
- **最小回归用例**：`capacity_check_timeout_row_kept`（mock `run` 抛 `TimeoutExpired` → 主流程仍出 CSV 且该行超时标记）；`parse_output_partial_garbage`（半行/非数字 → 留空不抛）。

### R3-07 [P2] `llmopt_vram_device_mib` 服务端发射未在集成测试核对，仅有离线采样解析

- **文件:行**：`src/metrics.cpp:159-161`（指标登记）、`src/resource.cpp:233-237`（写入）；`tests/tools/test_matrix.py:224-225`（采样解析）；`tests/integration/test_server_metrics.py:49-53`（泛化非负检查）
- **证据原文**：
  ```python
  # test_matrix.py:224-225 只喂一段假 prom 文本给采样器
  "# TYPE llmopt_vram_device_mib gauge\n"
  "llmopt_vram_device_mib 6554.0000\n"
  ```
  集成测试没有任何一处断言 `/metrics` 里存在该序列或其 NaN/0 口径。
- **影响**：若服务端不再发射该设备级显存序列（或探针失败时未按约定写 NaN/0），矩阵侧 `vram_device_peak_mib` 会静默取 0，离线单测仍绿。
- **确认状态**：确认缺口。
- **最小修复建议**：`test_server_metrics.py` 收尾快照核对 `vram_device_mib` 存在且为非负或 NaN，未知探针口径与 `vram_mib` 对齐。
- **最小回归用例**：`integration_server_metrics` 增补 `samples` 含 `llmopt_vram_device_mib`，值 ∈ 非负实数 ∪ NaN。

### R3-08 [P2] 两个新增 `.sh` 可执行位尚未入 git 索引；调用方式未在 CLI 文档固化

- **文件:行**：`scripts/bench/run_long_matrix.sh:1-7`、`scripts/bench/run_real_contract.sh:1-57`；`git config core.filemode=false`；对照 `git ls-files -s scripts/bench/` 现存 `.sh` 均为 `100644`
- **证据原文**：
  - WSL `stat -c %a` 两个新脚本均为 `777`（drvfs ACL 映射），但文件未跟踪；`core.filemode=false`，提交入索引时默认记 `100644`（与既有 `bench_chunk.sh`/`phase3_manifest.sh` 等同款）。
  - 字节检查：`tr -cd '\r' | wc -c` 两脚本 CR=0，LF 换行，`file` 识别为 Bash 可执行，`bash -n` 通过——**无 CRLF 执行失败风险**。
  - 调用约定：既有脚本用法均为 `bash scripts/bench/xxx.sh`；`run_long_matrix.sh` 在 `CLI.md` 与 `docs/testing.md` 均无用法行（`run_real_contract.sh` 仅在 testing.md 证据段提及）。
- **影响**：若后来者按 shebang 直接 `./run_long_matrix.sh` 调用，提交后 100644 会在 Linux 上 `Permission denied`；脚本自身 `cd "$(dirname "$0")/../.."` 的相对路径假设正确（已核对：位于 `scripts/bench/`，两级回到仓库根）。
- **确认状态**：确认待办（模式以提交时索引为准）；验证方法：提交后 `git ls-files -s scripts/bench/*.sh` 核对 mode。
- **最小修复建议**：文档统一写 `bash scripts/bench/run_long_matrix.sh`；若坚持直接执行，入库前 `chmod +x` 并确认索引记 `100755`。
- **最小回归用例**：无自动用例；CI 脚本闸门可校验 `scripts/**/*.sh` 要么索引 100755、要么文档/调用方均经 `bash` 显式解释。

### R3-09 [P3] 采样器用例依赖 `time.sleep(0.22)` 等待线程 tick，属于定时脆弱点（既有模式，本轮扩展到新字段）

- **文件:行**：`tests/tools/test_matrix.py:229-237`
- **证据原文**：
  ```python
  sampler = matrix._LiveMetricSampler(cfg, 1, interval_s=0.05)
  with mock.patch.object(matrix, "fetch_metrics", return_value=text):
      sampler.start(); time.sleep(0.22); sampler.stop()
  self.assertAlmostEqual(sampler.device_vram, 6554.0)
  self.assertEqual(sampler.kv_used, 37)
  ```
- **影响**：繁忙 CI 上线程未在 0.22s 内完成一轮抓取时，新字段断言取到 0 而偶发失败；`kv/sched` 旧字段同此模式。
- **确认状态**：待验证（本机 16/16 通过）。
- **最小修复建议**：暴露同步屏障（一轮抓取完成事件）替代固定 sleep，或断言"停止后至少一轮结果落地"。
- **最小回归用例**：同例改为等待 `sampler` 首轮回调计数 ≥1 再 stop。

### R3-10 [P3] 档位 JSON 缺"类型不符/非正整数"负样本；`recommended` 块无消费者也无校验

- **文件:行**：`src/gguf.cpp:296-318`（`read_int` 校验）；`tests/unit/gguf_test.cc:139-152`；`scripts/bench/capacity/qwen3-27b-dense-q4.json:12-20`
- **证据原文**：现有负样本只覆盖"缺字段"与"坏 JSON"；`"file_bytes": 1.5`、`"block_count": 0`、字段给字符串等分支未演练。profile JSON 内 `recommended` 整块（ngl/ctx/threads/kv…）解析端不读、也无用例核对它与服务默认值不冲突。
- **影响**：类型/边界分支的拒绝口径未锁定；`recommended` 与 CLI 实际默认漂移时无提醒。
- **确认状态**：观察项。
- **最小修复建议**：补 2 例：浮点/字符串字段 → `ok=false`；`block_count=0` → `ok=false`。
- **最小回归用例**：`profile_rejects_non_integer_and_non_positive`。

### R3-11 [P3] 观察：短/长证据矩阵未设 `kv_cache_type`，落默认 f16，与容量档 q8_0 建议口径不一致

- **文件:行**：`scripts/bench/matrix/phase8b_short.json`、`phase8b_long.json`（无 `kv_cache_type` 键）；`tools/bench/matrix.py:159`（默认 `"f16"`）；对照 `capacity/qwen3-27b-dense-q4.json:17` 推荐 `q8_0`
- **证据原文**：实跑 `matrix.load_config` 四档：short/long `kv=f16`，`kv_pressure`/`q3_full` `kv=q8_0`。
- **影响**：证据曲线若按 q8_0 口径解读会偏差；属证据配置口径，本轮按约束不改 JSON，仅记录。
- **确认状态**：待业务确认口径。

## 3. 已覆盖但未发现问题的子项

- **既有 6 个改动测试的断言方向**：逐 `git diff` 核对，无删断言、无阈值放宽、无 `skip`/`xfail`/`#if 0`、无裸 `except: pass` 吞错；e2e 健康判定由"`b"ready" in body`"收紧为 JSON `status=="ready"`；matrix 报告列改为从 `matrix_tool.REPORT_FIELDS` 单一真源导入，并新增 stub 显存 unknown/0 口径与显存分项键断言。
- **`tests/unit/config_test.cc` 新例**：CLI 与 env 两条路径都覆盖；`--model-profile` 单独出现被拒、与 `--capacity-only` 同现放行，断言可失败（非恒真）。
- **`tests/unit/gguf_test.cc` 新例**：q8_0（272 B/token）、f32（2×f16）、unknown 回落 f16 三分支均有确定值断言；档位 OK 例与缺字段/坏 JSON 两个负样本齐备；估算公式 `层×KV头×(k+v)×元素字节` 与实现一致（64×8×256×2 = 262144 B/token 复核通过）。
- **`tests/unit/metrics_test.cc`**：新序列名解析断言存在；表完整性用例（`kMetricCount` 遍历、系列唯一、family 前缀）自动覆盖新登记的 `KvUsedPeak`/`VramDeviceMib`。
- **`tests/tools/test_matrix.py`**：16 例实跑全绿；token 口径（prefill+prefix_hit 覆盖字符估算、缺指标回落并打 `char_estimate` 标签）有正反两例；显存三态（`buffers`/`buffers_partial`/`unknown`）与缺日志 unknown 均有用例；GPU/KV 参数进 argv 有逐值断言。
- **`tests/tools/test_capacity_check.py`**：4 例实跑全绿；解析不补算、缺行留空、argv 含关键开关、CSV 行数与 ngl 对齐；临时目录用 `TemporaryDirectory`，无临时文件泄漏；子进程被 mock，不占端口。
- **`tests/tools/test_watch_matrix.py`**：坏 JSON 行跳过、缺目录 `rows=0`/`age=None` 不崩，断言可失败。
- **脚本可运行性（字节级）**：
  - 换行：新增 2 `.sh` + 2 `.py` + profile JSON 实测 CR=0，纯 LF，WSL 下不会因 CRLF 报 `bad interpreter`。
  - shebang：`.sh` 为 `#!/usr/bin/env bash`，`.py` 为 `#!/usr/bin/env python3`；`bash -n` 与 `--help` 均通过。
  - 路径假设：两个 `.sh` 都 `cd "$(dirname "$0")/../.."`，从任意 cwd 调用都回到仓库根；`capacity_check.py`/`watch_matrix.py` 内部路径由参数传入，无写死仓库相对路径。
  - 端口/进程：`watch_matrix.py` 只读文件不起服务；`run_real_contract.sh` 有 `trap 'kill "$PID"' EXIT`、显式 `kill`+`wait`，退出回收；默认端口 18080 可由 `PORT` 环境变量覆盖。
- **注册表一致性**：`docs/testing.md` 目录级 scope（`tests/tools` / `tests/unit` / `tests/contract` / `tests/integration` / `tests/e2e`）覆盖全部新增测试文件；`run_registry.py` 按表执行 `python -m unittest discover -s tests/tools -t . -v`，两个新模块会被 discover 实跑；C++ 新例由 `doctest_discover_tests` 自动枚举进 ctest，`unit_config`/`unit_gguf`/`unit_metrics` 目标均已在 `tests/CMakeLists.txt:33,36,40` 登记。未发现"文件存在但注册表/CTest 未注册"的用例。
- **既有行为回归面**（本轮未改、抽查仍在）：chunked prefill（`test_server_chunked.py` + `continuous_batch_test.cc` + `chunk_planner_test.cc`）、抢占与重算（`preempt_test.cc` 6 例 + `state_test.cc`）、取消/断连收尾（`lifecycle_test.cc`、`scheduler_test.cc`、Aborted 收尾例）、admission 预留（`admission_test.cc` + `test_server_admission.py`）、配置四层来源（`test_server_config_sources.py`）、KV 账本零泄漏与对账差异（`test_server_concurrency.py`、lifecycle 关停例）。

## 4. 缺失覆盖矩阵（阶段 8 新行为 × 用例 × 缺口）

| 新/改行为 | 有无用例 | 对应用例（位置） | 缺口与对应发现 |
| --- | --- | --- | --- |
| chunked prefill（切块预算/跨 tick/混批） | 有 | `test_server_chunked.py`；`continuous_batch_test.cc:191,208,358,382,403`；`chunk_planner_test.cc` | 无 |
| 抢占与重算（预判零副作用/共享前缀不计可释放/trace/连续抢占上限） | 有 | `preempt_test.cc:48,83,129,158,167,202`；`state_test.cc:56,78` | 无 |
| 取消 / 断连收尾（单路取消、中途 Aborted、断连归还块） | 有 | `lifecycle_test.cc:27,42`；`scheduler_test.cc:191,212`；`continuous_batch_test.cc:268,429` | 无 |
| admission 预留（running+waiting+在飞 总额、handler 兑现释放） | 有 | `admission_test.cc:30,62`；`test_server_admission.py:58-96` | 无 |
| 配置来源（CLI>env>file>default 旧键） | 有 | `test_server_config_sources.py:54-59`；`config_test.cc:121-135,343+` | 无 |
| 新配置键 `--model-profile` / `--capacity-only`（CLI+env 解析） | 部分 | `config_test.cc:103-119`（unit 校验 + env 绑定） | 进程级来源打印/装配未对账；env 清理泄漏（R3-03） |
| capacity-only 启动早退（不 listen、不载权重、`allow_start` 行、profile 参与预算） | **缺** | 无（`test_capacity_check.py` 子进程被 mock） | 主路径无进程级回归（R3-01） |
| 档位 JSON 解析（声明字段/缺字段/坏 JSON） | 有 | `gguf_test.cc:120-152` | 类型不符/非正整数负样本缺（R3-10） |
| KV 估算按 dtype 折算（q8_0/f32/unknown 回落） | 有 | `gguf_test.cc:101-113` | 无 |
| 指标 `llmopt_kv_used_blocks_peak` 登记与名解析 | 部分 | `metrics_test.cc:54`；表完整性遍历 | 峰值非递减/排空保留语义无断言（R3-02） |
| 指标 `llmopt_vram_device_mib` 登记/探针写入 | 部分 | `test_matrix.py:224-225`（离线采样解析） | 服务端发射与 NaN/0 口径未在集成测试核对（R3-07） |
| 矩阵 token 口径（prefill+prefix_hit，回落字符估算并打标） | 有 | `test_matrix.py` `test_prefill_and_prefix_hit_tokens_...`、`test_missing_prefill_metric_...` | 无 |
| 显存缓冲区解析（CUDA0 三项/Host/partial/unknown/缺日志） | 有 | `test_matrix.py:176-209` 四例；`test_server_matrix.py:53-55,67` | 无 |
| GPU/KV 配置进服务 argv（ngl/threads/kv/donor/chunk-size/disable-prefix） | 有 | `test_matrix.py:61-77` | 无 |
| 进度监控 snapshot（行/格/失败计数、坏行、缺目录） | 有 | `test_watch_matrix.py:13-34` | STALL/until-report/停滞年龄与脏字段容错未覆盖（R3-05） |
| 容量扫描脚本（解析/argv/CSV 落盘） | 部分 | `test_capacity_check.py:25-79` | 超时/异常兜底与解析负样本缺（R3-06） |
| e2e 根地址口径（禁带路径、healthz+readyz 双探活） | 部分 | `test_real_service_contract.py:17-60`（needs-gpu，CI 中 SKIP 77） | 新解析分支无 hermetic 单测；错误体解析可硬崩（R3-04） |
| KV 账本（收尾块表、零泄漏、reconcile 零差异） | 有 | `lifecycle_test.cc:83`；`test_server_concurrency.py:147`；`test_server_chunked.py:95,109` | 无 |

## 5. 新增脚本"能跑但意义/覆盖受限"清单

| 脚本 | 可运行性 | 自动测试覆盖 | 结论 |
| --- | --- | --- | --- |
| `scripts/bench/capacity_check.py` | `--help`/语法 OK；LF；`python3` 一致 | 4 例全部 mock `subprocess.run`，真实服务子进程路径（estimate/estimate-split/gate 行对账、退出码 0/3/4）在 CI 中**不跑**，证据 CSV 为手工产物 | **能跑，但核心集成路径无自动回归**；超时即丢整份 CSV（R3-06） |
| `scripts/bench/watch_matrix.py` | `--help`/语法 OK；LF | 仅 snapshot 纯函数 2 例；STALL 标注、`--until-report` 退出、停滞年龄主循环未测 | **能跑，但调度承诺未被验证**；脏字段可崩（R3-05） |
| `scripts/bench/run_long_matrix.sh` | `bash -n` OK；LF；相对路径自定位正确 | 无测试；未在 CLI 文档固化调用方式；配置内 server/model 为绝对 WSL 路径 | 薄封装，仅人工证据流程；直接 `./` 执行依赖入库 mode（R3-08） |
| `scripts/bench/run_real_contract.sh` | `bash -n` OK；LF；trap 回收 PID；端口可 env 覆盖 | 无测试；归类 needs-gpu，CTest 中对应 Python 用例未设置 `LLMOPT_REAL_BASE` 时 SKIP(77) | 真模型 e2e 包装器，CI 闸门不跑；脚本自身起服/等待/落证据链路无自动对账 |
| `scripts/bench/capacity/qwen3-27b-dense-q4.json` | JSON 可被 `read_model_profile` 解析；字段齐备 | unit 用例用的是临时 JSON，未直接校验此档位文件本身 | 数据文件；`recommended` 块无消费者（R3-10） |

## 6. 备注与边界

- 本轮未执行 cmake/ctest，未核对 165 条用例的实跑结果；C++ 侧结论基于静态阅读与注册表/CMake 一致性比对。
- `tools/bench/matrix.py`、`scripts/bench/matrix/*.json`、`docs/**`、`bench/results/**` 按约束只读，其中观察到的口径问题（R3-11 等）仅记录，未修改。
- 另一个 Agent 正在跑全量用例，本轮未与其并行触发任何服务/端口/GPU 操作。
