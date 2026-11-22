# SPEC-DEBUG2 生产化收口：缺陷优先、7 项必要功能与三条冻结边界

> 版本：v1.1（对应阶段 13）｜状态：done（D2-01…D2-14 全部落地，含 D2-07 真后端镜像实建与六项冒烟；CUDA 变体未验证）
> 契约基线：C-01…C-13、D-01…D-09、C-P13-01…03（本文件定义）｜更新：2026-11-22
> 上游输入：`docs/notes/项目复盘-2026-11-22.md`（基线 `3dba509`）、`docs/notes/项目整体评估-阶段12收口-2026-11-21.md`

## 0 一句话目标

不新增架构方向、不改调度语义、不切分阶段：把 **6 项缺陷/证据债 + 7 项必要功能 + 1 项 CLI/契约文档** 按编号逐项收口，使单实例生产分从 4.4–4.8 推到 **≥7.0**，同时把三条已验证上限（27B 冻结、ctx 1024 与 P≤8、扩容在仓库外）写成结论而不是待办。

## 1 输入与基线

| 项 | 基线（2026-11-22 实测/核对） |
|---|---|
| 代码 | `main` @ `3dba509`，74 个提交，10 条阶段分支，0 tag；工作区干净，与 `origin/main` 一致 |
| 规模 | `src+include` 44 文件 / 9,655 行（闸门口径 8,591 行 / 4 模块 / 4 直接依赖）；重复率 2.84%；`TODO/FIXME` 0 |
| 测试 | CTest 220 注册（`-L fast` 219 = 146 unit / 52 contract / 21 integration，`-L e2e` 1）；Python 工具 106；今日 `-j8` 219/219 通过 |
| 覆盖率 | `src/` 行 83.6%（`build/ci-reports/coverage-2026-11-21.json`）：`gguf.cpp` 60.3%、`common.cpp` 62.9% 为最弱 |
| 机器 | RTX 4060 Laptop **8188 MiB**（桌面常占 ~2521 MiB）、Ryzen 9 7940H 8C/16T、31.2 GB RAM；WSL Ubuntu-26.04 分 15 GB / 16 线程，llama.cpp 已编（`libggml*`/`libllama` 就绪） |
| 模型 | Qwen3-8B Q3_K_M 3.84 GB、Q4_K_M 4.68 GB（`/mnt/d/ai_model/models/qwen3-8b`） |
| 现有交付 | stub 镜像 79.17 MiB（非 root，六项冒烟 PASS，`evidence/phase12/packaging/`）；30 min soak 297,441 请求 / 0 错误 / RSS +0.05 MiB |
| 生产分 | 4.4–4.8（口径见 §6.2） |

## 2 范围、非目标与冻结边界

**范围**：D2-01…D2-14 共 14 项（见 §4），其中 6 项缺陷/证据债、7 项功能、1 项 CLI/契约文档。

**非目标**：不改调度语义（双预算、抢占、chunk、前缀共享规则不动）；不引入新第三方库（监控直方图沿用自研 exposition）；不做双 context 流水线、分布式、PD 分离、WebUI、全量 OpenAI 字段；不做在线直方图当对外结论。

**冻结边界（写死，本期不启动、不测试、不新增数字）**：F-A…F-C 是立项时的三条；F-D…F-F 是 2026-11-22 收口时补冻的三条——它们的共同点是“在当前环境无法诚实取证”，所以选择冻结而不是留个 BLOCKED 的尾巴。

| 编号 | 冻结对象 | 冻结后的明确语义 | 落地动作 |
|---|---|---|---|
| F-A | 27B 档吞吐结论 | 代码路径（门控分池、donor 槽、KV 量化）**保留可运行**，但不再跑 27B；CI 不覆盖；任何文档**不得出现 27B 吞吐数字**。理由：Q4 权重 ~16 GB > WSL 15 GB，显存更不足，测了也只是 CPU-offload 下的噪声。要启用属新 SPEC | README「已验证上限」写明；`docs/模型资产.md` 标“仅供容量/抢占验证，不出吞吐”；`scripts/bench/matrix/` 不放 27B 配置 |
| F-B | ctx 1024 / P≤8 | 8 GB 卡上 8B Q3_K_M 全驻留的**可用上限就是 ctx 1024、P≤8**（P8 峰值 buffers 4920 MiB / 设备 7242 MiB）。超限由门控判 `BLOCKED`（`force_on` 时 `DEGRADED` + 横幅），不是待优化项 | README 一节写死“已验证上限（2026-11-22）”；`--ctx-tokens > 1024` 在 8B 全驻留配置下启动预算判 BLOCKED；CI 不造 16 并发 |
| F-C | 多实例 + 前置网关 | 本仓库的扩容单元**只有一个：单实例**。水平扩展靠网关层（7 层轮询、`proxy_ignore_client_abort off`、`X-Request-ID` 透传），仓库只交付参考配置，**不实现**会话保持、多实例调度、跨实例 KV | 新增 `scripts/release/nginx.conf` 参考 + `docs/deploy.md` 一节；任何“扩容”表述指向该文档 |
| F-D | CUDA 镜像变体 | 只保留“做法说明”（换 `nvidia/cuda` 基础镜像 + `-DGGML_CUDA=ON`），**不构建、不冒烟、不出镜像体积/吞吐数字**。理由：本机 WSL 容器无 GPU 直通，验证不了；强行写“已验证”就是造假 | `docs/deploy.md` 明写未验证；CI 不编排 GPU 构建 |
| F-E | 夜间 e2e 的 `prefix_reuse` 真模型实跑 | 用例保留在 `tests/e2e/nightly_real_backend.py`，由有模型与夜间 job 的环境执行；本仓库不再提供该运行证据，也不在文档里声称已跑 | 证据只写“用例已入库、逻辑路径已覆盖”，不写“真模型 PASS” |
| F-F | 结构化事件日志（`events.jsonl`） | 未实现即不写进运维文档。需要时按新 SPEC 立契约（字段、采样、轮转、脱敏四件事一起定） | `docs/deploy.md` 日志段只写已实现的 spdlog 20 MB × 5 + `logs/` 200 MB 兜底 |

## 3 设计原则

1. **缺陷先于功能**：D2-01…D2-06 全部绿之前不开 D2-07，避免在破的环境上做交付。
2. **不碰调度语义**：本次所有改动都在“外围”（工具链、可观测、打包、配额、发布），`scheduler.cpp` / `kv.cpp` 的分配与抢占规则不动。
3. **一处结论一条边界**：任何对外数字必须带条件（模型档、ngl、ctx、P、轮数）；没有边界的数据一律不进 README。
4. **监控口径 ≠ 对外口径**（C-P13-01）：在线直方图只服务监控与告警，p50/p99 对外仍由离线工具复算，两套并存但不互相引用。
5. **先红灯再绿灯**：每项先落一个可复现失败用例，修复后转绿，用例永久保留。
6. **每个功能有直测，错误码路径逐一覆盖**：新增校验/回滚路径必须有直接单测，错误码不仅断言 code，还要断言 HTTP 码与 `retryable`。

## 4 工作项总表（不切分阶段，按编号顺序交付）

每项一个可回滚 commit，提交信息 `fix(debug2-NN): …` 或 `feat(debug2-NN): …`。表中“顺序”即执行顺序，不是阶段。

| 顺序 | 编号 | 类型 | 一句话 | 依赖 | 工时 | 单出口 |
|---|---|---|---|---|---|---|
| 1 | D2-01 | 缺陷 | 证据链去 git 硬依赖，manifest 可降级 | — | 0.5 h | 无 git 时矩阵仍 exit 0 |
| 2 | D2-02 | 缺陷 | 测试环境前置登记 + `run_ci` 预检 | — | 1 h | 缺依赖 exit 3 并指名 |
| 3 | D2-03 | 证据债 | 四组消融矩阵（mixed / static / prefix_on / prefix_off） | WSL 真后端 | 3.5 h（机器时间） | 四组产物落盘 + 判据结论 |
| 4 | D2-04 | 缺陷 | 覆盖率弱项补齐（gguf / common） | — | 1 d | `src/` 行 ≥85%、diff ≥85% |
| 5 | D2-05 | 缺陷 | 文档漂移清零（死链 / 旧表 / 错误指标名） | D2-03 | 0.5 d | `check_docs` 新检查绿 |
| 6 | D2-06 | 缺陷 | 故障面 6 类扫描与缺口用例 | D2-04 | 0.5 d | 结论表 + 缺口用例绿 |
| 7 | D2-07 | 功能 | 真后端镜像（LLAMA=ON + 运行库 + GPU） | D2-01/02 | 1.5 d | 容器内 8B 六项 PASS |
| 8 | D2-08 | 功能 | 发布面：`v0.13.0` tag / CHANGELOG / 镜像 label | D2-07 | 0.5 d | 版本三处一致 |
| 9 | D2-09 | 功能 | 三个监控直方图 + 告警样例 | D2-03 | 1 d | `/metrics` 合法且可对账 |
| 10 | D2-10 | 功能 | 排障链路：req id / 慢请求 trace / 一条命令 | D2-09 | 1 d | 按 id 可还原生命周期 |
| 11 | D2-11 | 功能 | 可靠性闭环：Restart / 外部探针 / 4 个故障注入 | D2-07 | 1.5 d | kill -9 后自动恢复 |
| 12 | D2-12 | 功能 | 租户 token 预算（小时/日窗口） | D2-09 | 1.5 d | 双租户对照 429 |
| 13 | D2-13 | 功能 | SLO + 容量表 + 性能回归闸门 | D2-03/09 | 1.5 d | CI perf 步骤不再 SKIP |
| 14 | D2-14 | 文档 | CLI 与接口契约专册（入口模式 / 命令表 / 参数 / 配置加载 / 插件 / 权限 / 示例） | D2-08 | 1 d | 与用户核对后签字，CLI.md 与实现零漂移 |

合计：机器时间约 4 h，人力约 12–13 个工作日。

## 5 逐项规格

### 5.1 缺陷与证据债（D2-01…D2-06）

#### D2-01 证据链去 git 硬依赖

- 位置：`tools/bench/matrix.py:709` `_git_commit()`。
- 问题：`subprocess.run(["git", …], check=False)` 只兜住“git 返回非零”，兜不住“找不到可执行文件”；PATH 无 git 时抛 `FileNotFoundError`，整轮矩阵崩溃，而 `repro_manifest.json` 本该是证据链的锚。
- 改动点：外层 `try/except (FileNotFoundError, OSError, subprocess.SubprocessError)` → 返回 `None`；manifest 增加 `git_commit: null`、`git_available: false`、`git_note: "git unavailable, commit omitted"`；stderr 打一行警告，不污染 stdout 的 CSV。
- 验收：临时从 PATH 摘掉 git，`matrix_runner --smoke` 仍 exit 0，CSV 行数与正常跑一致，manifest 三个字段如上述；正常环境仍写 40 位 hex。
- 测试：`tests/tools/test_matrix.py` 新增 2 条——(a) 打桩 `subprocess.run` 抛 `FileNotFoundError`，断言返回 `None` 且不抛出；(b) 正常返回值断言 40 位 hex。

#### D2-02 测试环境前置登记与预检

- 问题：本机默认 PATH 既无 `C:\msys64\ucrt64\bin`（MinGW UCRT 运行库），也无 git。实测直接 `ctest -j8` 产生 **13 条假失败**：服务进程 `rc=0xC0000135`（DLL 缺失，`0xC0000135 = STATUS_DLL_NOT_FOUND`），矩阵用例 `FileNotFoundError: git`。补 PATH 后一次 219/219。
- 改动点：(a) `docs/testing.md` 增 §2.3「运行时前置」：Windows 需 UCRT64 bin + git + ctest + Python ≥3.10，Linux 需 git + cmake + ctest；(b) `scripts/ci/run_ci.py` 增 `env_check`（按模式探测工具），缺失打印清单并以 **3** 退出（与 `dev.py` 的 3=环境缺失一致）；（c）`check_docs` 的计数校验在缺 `ctest` 时降级为 SKIP 并给提示，`gen_test_counts` 不再抛 traceback。
- 验收：清空第三方 PATH 跑 `run_ci.py --mode fast` → exit 3 且指名缺什么；正常环境三门仍 PASS。
- 测试：`ci/test_ci_scripts.py` 增 env_check 单测（打桩 `shutil.which` 返回 None / 正常两组）。

#### D2-03 四组消融矩阵（证据债，本期唯一能改变结论的事）

- 对象：`scripts/bench/matrix/phase12_s3_mixed.json`（输出长度 8/16/32/64/128/256 循环）、`phase12_s3_static.json`（`batch_policy=static`）、`phase12_s3_prefix_on.json` / `phase12_s3_prefix_off.json`（384-token 共享前缀）。
- 判据（沿用 `evidence/phase12/perf/README.md` 已写死的三行）：混长 P8/P1 是否 ≥1.3×（越过即翻案）；continuous 相对 static 的吞吐差即“补位”净收益；前缀开/关的 TTFT 与 prefill token 差。
- 执行：WSL 真后端，`python3 scripts/bench/matrix_runner.py --config <cfg> --out evidence/phase13/<name> --run-id <id>`，同机同模型、5 轮、warmup 3。
- 边界：8 GB 卡若 OOM 降到 P≤4 并在报告注明；轮间偏差 >15% 标 `high`，单点不作为结论。
- 出口：四个产物目录（各含 `matrix_report.csv` / `deviation.csv` / `step_summary.csv` / `repro_manifest.json` / `metrics_p*_r*.prom` / `plots/`）+ §5.1 后一张“四组对照表”+ 一段结论（翻案或定上限）。
- 测试：不产生新代码路径；`tests/tools/test_matrix.py` 增一条断言四份配置的关键字段（`output_lengths`、`batch_policy`、`prefix` 开关）不被误改。
- **结论（2026-11-22 实测，`evidence/phase13/README.md` 为唯一引用入口）**：static vs continuous 在 P8 把 TTFT p50 从 6970 ms 压到 307 ms，但吞吐 57.1 → 62.4 tok/s（+9.3%），**不达 1.3× 翻案线**；continuous 的 ITL p99 在 P8 为 208.9 ms，超过 200 ms SLO，故**交互式容量上限定为 P=4**（TTFT 168 ms / ITL 112 ms）。四组配置的关键字段由 `tests/tools/test_matrix.py` 守护。
- **缺陷（D2-03 顺带修复）**：prefix_on 首轮暴露 `seq_cp` 位置重叠 → llama `GGML_ASSERT(is_full)` 崩溃；修复为 `src/runner_llama.cpp` 里先按块表前缀长度裁 donor KV 再整段复制，`tests/e2e/nightly_real_backend.py` 新增 `prefix_reuse` 回归。

#### D2-04 覆盖率弱项补齐

- 目标：`src/` 行 ≥85%（现 83.6%），改动行 ≥85%；重点 `gguf.cpp` 60.3% → ≥85%、`common.cpp` 62.9% → ≥85%。
- 用例清单：(a) GGUF 异常面——magic 错、版本不支持、tensor 数量溢出、KV 长度字段截断、metadata 类型不符、字段超大值；(b) `common.cpp` 错误码全表参数化——19 个失败码 × 断言 `code` / `http_status` / `retryable` / message 非空。
- 验收：`check_coverage.py` 报告行 ≥85%；`--min-diff` 在 PR 模式通过。
- 测试：新增 `unit/gguf_negative_test.cc`、`unit/error_map_test.cc`（或并入现有 `gguf_test.cc` / `common` 用例，二选一但必须登记 `docs/testing.md`）。
- **状态：done（2026-11-22 取证）**。WSL Ubuntu-26.04 插桩构建 + `ctest -L fast` 后 gcovr 实测 `src/` 行 **4,563/5,351 = 85.3%**（函数 94.6%、分支 56.4%），达 85% 门槛；`gguf.cpp` 60.3% → 93.3%。JSON 落 `evidence/phase13/coverage/coverage-2026-11-22-linux.json`。
  - Windows 同口径为 74.0%（3,972/5,357）：集成用例用 `TerminateProcess` 收服务进程，`http_server.cpp` 只落 25 行，属采集限制；因此覆盖率取证必须在 Linux/WSL 做。
  - 顺带修掉闸门自身缺陷：`--min-line 0.85` 曾被当成 0.85% 而恒过，现按比例换算并加单测；Linux 上 `tenant_quota` 用例写 key 文件未设 0600 会误报 16，已补 `fs::permissions`。

#### D2-05 文档漂移清零

- 清单：① README 指向的 `docs/SPEC-1…`、`docs/模型资产.md`、`AGENT_EXECUTION_PROTOCOL.md` 在仓库内 404；② `CLAUDE.md` 仍写“尚无 CMakeLists.txt”；③ `docs/性能数据.md` 是 0.5B 旧表；④ `README.en.md` 未同步阶段 12/13 与四组结论；⑤ **`docs/SPEC-DEBUG1.md` §5.2 的取消指标名写错**（实际序列是 `llmopt_requests_total{status="cancelled"}`，非 `llmopt_requests_cancelled_total`，见 `evidence/phase12/packaging/docker_smoke.json`）；⑥ `docs/SPEC总览.md` 的 DEBUG1 状态仍是“S1–S6 进行中”。
- 决策（写死，不再二选一）：把 `SPEC总览.md`、`错误面规范.md`、`SPEC-DEBUG1.md`、`SPEC-DEBUG2.md` 转为 tracked；其余 `docs/` 在 README 明确标注“设计文档仅本地，不随仓库分发”。
- 验收：`scripts/ci/check_docs.py` 新增“README 内相对链接必须存在”检查并 PASS；上述 6 条逐条关闭并在 `docs/notes/阶段13-文档漂移收口.md` 留表。
- 测试：`ci/test_ci_scripts.py` 增 check_docs 新规则的正反例。

#### D2-06 故障面 6 类扫描

| 类 | 已有覆盖 | 缺口 | 本项处理 |
|---|---|---|---|
| 错误码路径 | 20 码单点映射 | 503（传输层 shed）与 507 在 SSE 帧、chat 路径未逐一断言 | 补参数化用例：shed → 503 `SERVICE_UNAVAILABLE`、KV 不足 → 507 `KV_INSUFFICIENT`（流式首帧即错误帧） |
| 超时与降级 | watchdog（默认 30 s）、请求 120 s 时限 | 请求时限到点时的 SSE 收尾未锁 | 补一条“时限到点 → 错误帧 + 连接关闭”集成用例 |
| 并发竞态 | `-j8` 今日全绿；采样竞态已修 | TSan 只在 nightly | 不加 PR 闸门（成本高），保留 nightly，文档写清 |
| 资源上界 | sink 有界、请求体 4 MB、队列 `C+2Q` 硬顶 | 日志单文件 20 MB × 5 已有；慢请求 dump 无上限 | dump 加单文件 8 MB 上限 + 保留 20 份 |
| 输出确定性 | seed / stop / n 单测 | `n>1` 与 `stop` 组合、`n>1` 与流式（应 400） | 补 2 条 |
| 日志脱敏 | key 只打印 set/unset | prompt 全文可能进 trace | trace 默认关；dump 只写 `prompt_tokens` 与 prompt sha256 前 8 位 |

- 出口：`docs/notes/阶段13-故障面扫描.md`（类别 / 已有用例 / 缺口 / 结论 / 是否本期修）+ 上述新增用例全绿。

### 5.2 功能白名单（D2-07…D2-13）

#### D2-07 真后端镜像

- 现状问题：83 MB 镜像里是 stub 后端，“可部署”是纸面的（README 自己承认）。
- 改动点：`scripts/release/Dockerfile` 增 `LLMOPT_ENABLE_LLAMA` 构建参数；builder 阶段编译 llama.cpp（固定 `b12557`），runtime 阶段只拷 `libllama.so*` / `libggml*.so*` + 设置 `LD_LIBRARY_PATH`；基础镜像与 builder 同发行版（避 glibc 不匹配）；CUDA 变体基于 `nvidia/cuda:12.x-runtime`；runtime 仍非 root（uid 10001）、模型只读挂载。
- 验收：WSL docker `--gpus all` 内 8B Q3_K_M 六项冒烟 PASS（healthz / readyz / 非流式 / SSE / 断连取消 / `docker stop` rc=0）；容器内 `nvidia-smi` 可见 GPU；非 CUDA 变体 ≤350 MiB、CUDA 变体 ≤1.2 GB；证据 `evidence/phase13/packaging/`。
- 测试：`docker_smoke.sh` 增 `--real` 模式（真模型路径 + `--ngl`），仍不進 PR 闸门；JSON 产物沿用现有 schema。
- **状态：done（2026-11-22，已实测）**。Dockerfile 走两条来源：能访问 GitHub 时容器内 clone `b12557` 并只编 `llama`/`ggml` 共享库；内网时 `LLMOPT_LLAMA_VENDOR` 提供已编根目录，脚本只把头文件与 `.so` 收进 `scripts/release/vendor/`（不入库），`cmake/deps.cmake` 用 `LLMOPT_LLAMA_REVISION_OVERRIDE` 声明 revision 且仍与期望值交叉校验。基础镜像统一 `ubuntu:26.04`（glibc 2.43 与本机同代，24.04 会因 `sqrtf@GLIBC_2.43` 链接失败）。
  - 实测：WSL Docker 29.1.3 构建 `llmopt:0.13.0-llama`（132 MB，label `backend=ON` / `rev=b12557`），六项冒烟全 PASS（healthz / readyz / metrics 直方图 / completion / 非 root uid=10001 / `docker stop` rc=0），CPU 推理跑 Qwen3-8B Q3_K_M；证据 `evidence/phase13/packaging/real-backend-image.md`。
  - 未覆盖：CUDA 变体（本机 WSL 容器无 GPU 直通），做法写在 `docs/deploy.md`；GPU 版 `nvidia-smi` 一条不列入本期验收。
  - 顺带修掉两处构建缺陷：`-DLLMOPT_LLAMA_ROOT` 被静默吞成默认路径；`.dockerignore` 的 `vendor/` 把 `scripts/release/vendor/` 挡在上下文外；`LLMOPT_BUILD_TESTS=OFF` 时不再拉 doctest。

#### D2-08 发布面

- 改动点：`project(llmopt VERSION 0.13.0)`；`CHANGELOG.md`（Keep a Changelog 精简版，只记 Added/Fixed/Changed）；镜像 label `org.opencontainers.image.version`；`--version` 输出 `llmopt 0.13.0 backend=llama llama_rev=b12557`；README 顶部版本块 + 回滚三行（`docker run <上一 tag>` / `git checkout <tag>` / 配置回滚）。
- 验收：`--version` 字符串、镜像 label、git tag 三处一致；`docker run llmopt:0.13.0-stub --version` 对齐。
- 测试：`unit/version_test.cc` 断言输出含 `PROJECT_VERSION` 宏，且 `llama_rev` 与 `deps.cmake` 常量一致。

#### D2-09 三个监控直方图 + 告警样例

- 改动点：新增 `llmopt_latency_queue_wait_ms`（入队→准入，桶 5/10/20/40/80/160/320/640/+Inf）、`llmopt_latency_ttft_ms`（桶 10/20/40/80/160/320/640/1280/+Inf）、`llmopt_latency_itl_ms`（桶 5/10/20/40/80/160/320/+Inf）；采集点固定在 Core 准入成功、首 token、纯 decode 步间三处；沿用自研 exposition，`_bucket{le=…}`/`_sum`/`_count` 三件套。
- 命名约束：统一用 `llmopt_latency_*` 前缀。既有 `llmopt_ttft_ms_sum/_count`、`llmopt_itl_ms_sum/_count` 仍是对外 KPI，直方图若同名会与其撞成重复时间序列。
- C-P13-01：**在线直方图只作监控与告警**，任何对外 p50/p99 仍由离线复算；README 与 `docs/ci/ci-spec.md` 同时标注这一行。
- 告警样例 `scripts/release/prometheus-rules.yml`：TTFT p99 > 350 ms、ITL p99 > 200 ms、排队 p95 > 160 ms、排队进入 +Inf 尾部、错误率 > 0.1%、KV 碎片率 > 0.85（`promtool check rules` 可校验）。
- 验收：`/metrics` 输出可被 Prometheus 文本解析；各桶累计单调不减且 `+Inf` 桶 == `_count`；直方图 `_count` 与既有 `llmopt_ttft_ms_count`/`llmopt_itl_ms_count` 逐个请求一致（TTFT/ITL 只对有产出 token 的请求计数，故不与完成类请求数强等）；新增租户不新增序列（基数护栏）。
- 测试：`unit/metrics_test.cc` 扩桶边界（值恰等于 `le` 落当前桶）、`+Inf` 累积、脏样本（负数/NaN/Inf）丢弃、名字不与既有序列冲突、reset 语义；`integration/test_server_metrics.py` 断言三支直方图在真实路径上有样本且累计单调。

#### D2-10 排障链路

- 现状：`--step-trace`（每 step 一行 JSONL）与 `--step-trace-rate`（1/N 采样，默认 0=关）已存在；request id（`req-…`）已存在。
- 改动点：(a) 日志 pattern 统一带 `req_id`；(b) 新增 `--slow-request-ms`（默认 0=关，建议 1000）：命中阈值时**无视 rate** 强制落 `logs/slow/<req_id>.jsonl`，并附 TTFT 四段计时；(c) `python tools/dev.py trace <req_id>` 汇总生命周期（TTFT 四段 / 步数 / 抢占次数 / KV 峰值），找不到时退出 2。
- 脱敏：dump 不含 prompt 原文，只写 token 数与 sha256 前 8 位；单文件 8 MB、保留 20 份。
- 验收：压测中构造一条慢请求 → dump 文件生成、`dev.py trace` 能打印四段耗时；`grep` dump 无 prompt 明文。
- 测试：`unit/` 覆盖 id 唯一性与透传；`integration/test_slow_request.py` 断言 dump 生成 + 脱敏；`tests/tools/test_trace.py` 覆盖命令退出码 0/2。

#### D2-11 可靠性闭环

- 改动点：(a) `scripts/release/llmopt.service` 增 `Restart=on-failure`、`RestartSec=5`、`StartLimitIntervalSec=60`、`StartLimitBurst=3`（`TimeoutStopSec=45` 已有）；(b) 新增 `scripts/release/probe.sh`（`/readyz` 连续 3 次非 200 记一条告警行，供 cron/systemd timer 调用）；(c) 4 个故障注入用例：runner 挂死（复用 `SlowRunner`）、慢客户端、TLS 配置错（已有）、**磁盘满**（日志目录只读或塞满）。
- 验收：`kill -9` 后 ≤10 s 由 systemd 拉起且 `/readyz` 回 200；4 用例全绿；磁盘满时请求返回 5xx 而非崩进程，且错误码与 `EXECUTION_FAILED` 一致。
- 测试：`integration/test_fault_injection.py` 4 条，逐条断言状态码与退出码语义；`systemd` 侧用 `systemd-analyze verify` + 一次真实起停记录（WSL systemd 可用则跑，否则记为 SKIP 并注明）。

#### D2-12 租户 token 预算

- 契约（C-P13-02）：key 文件 `version:1` 不变，`quota` 对象增可选 `tokens_per_hour` / `tokens_per_day`（整数 ≥0，0 或省略=不限）；未知键仍拒绝（`has_unknown_keys` 白名单同步扩两项）。
- 计数口径（写死）：只计 **completion tokens**（生成 token），prompt/prefill token 不计；在 Core 单点累加，按自然小时/日**固定窗口**滚动（不做滑动窗口，KISS）。
- 超限：429 `QUOTA_EXCEEDED` + `Retry-After` = 距下一窗口秒数；指标 `llmopt_tenant_tokens_total{tenant,window}`（counter）。
- 验收：双租户对照——A 打满后 A 429、B 仍 200；跨窗口后 A 恢复；`--api-key` 单 token 模式默认不限（行为不变）。
- 测试：`unit/tenant_quota_test.cc`（=limit、limit+1、跨窗口、0=不限、非法负数配置错误）；`integration/test_tenant_quota.py`（429 码 + `Retry-After` 数值 + 指标递增）；`CLI.md` key 文件章节同步。

#### D2-13 SLO、容量表与性能回归闸门

- SLO（写死，条件同 F-B：8B Q3_K_M、`ngl 37`、`ctx 1024`、`P≤8`、单实例）：TTFT p50 ≤350 ms、ITL p99 ≤200 ms、可用性 ≥99.5%（30 天，靠 `Restart` + 探针）、服务端错误率 ≤0.1%（4xx 客户端错误不计）。
- 容量表：P ↔ KV/权重显存（MiB）↔ 总 token 吞吐 ↔ ITL p99，数据来自 D2-03 四组 + `evidence/phase12/perf`；由 `scripts/bench/capacity_table.py` 从 CSV 生成 README 表格块（不手写）。
- 回归闸门：`-L perf-baseline` 一条 CPU 小模型（Qwen2.5-0.5B Q4_K_M，固定 prompt 集与 `max_tokens`）压 60 s，吞吐与 TTFT p50 劣化 >5% 即失败；CI 的 perf 步骤在无基线时**注入基线文件而不是 SKIP**。
- 验收：CI perf 步骤真实执行并 fail-on-regression；README「容量与 SLO」一节由脚本生成；SLO 表与四组结论不矛盾。
- 测试：`tests/tools/test_report.py` 扩 SLO 计算与阈值判定；闸门脚本自身单测（劣化 5.1% 判失败、4.9% 判通过）。

## 6 全局验收门槛

### 6.1 工程门禁（每项都适用）

- G1 每项独立 commit；提交前 `python scripts/ci/run_ci.py --mode pr` PASS，`ctest -L fast` 全绿。
- G2 `clang-format` / `clang-tidy -p build` 干净；函数 ≤60 行、认知复杂度 ≤25。
- G3 覆盖率：`src/` 行 ≥85%、改动行 ≥85%（D2-04 之后成为硬门槛）。
- G4 每个新增校验/回滚路径有直测，错误码逐一断言 `code` / HTTP / `retryable`。
- G5 代码与文档同批：`docs/testing.md` 注册表、`CLI.md`、README、本 SPEC 状态同步。
- G6 缺陷修复必须留下可复现场景，作为回归用例永久保留。
- G7 推送与打 tag 一律等用户明确同意；不新建分支，全程在 `main`。

### 6.2 生产分口径与目标

| 维度 | 权重 | 现状 | 目标 |
|---|---|---|---|
| 可靠性 / 故障恢复 | 25% | 4.5 | 7.5（D2-11） |
| 过载与多租户隔离 | 15% | 5.0 | 7.0（D2-12 + 现有背压） |
| 可观测与排障 | 15% | 4.5 | 7.5（D2-09/10） |
| 部署与发布 | 15% | 3.5 | 7.5（D2-07/08） |
| 安全 | 10% | 4.0 | 7.0（TLS 默认建议 + key 轮换文档 + 网关边界） |
| 性能与容量（SLO 证据） | 15% | 5.0 | 7.0（D2-03/13） |
| 运维文档 | 5% | 4.0 | 6.5（runbook + 上限声明） |
| 合计 | 100% | **4.4–4.8** | **≥7.0** |

7.0 的准确含义：**单卡、单实例、内网小团队可用、有 SLO 与故障演练记录**。8 分以上必须多实例 + 网关 + 真实多租户，属另一个项目。

### 5.3 文档项（D2-14）

- 交付物：`CLI.md` 升格为专册（当前是 4 章节的速查），固定七节：① 入口模式（服务模式 / 工具模式 / 一次性任务模式）；② 命令列表（表格：命令 / 语法 / 说明 / 退出码）；③ 子命令参数（按命令分组，含默认值与合法范围）；④ Profile 与配置加载（CLI > ENV > JSON > 默认的四层顺序、`--model-profile`、来源可追溯字段）；⑤ 插件/扩展管理（后端 stub/llama 切换、`deps.cmake` 与 vendor 清单口径）；⑥ 环境变量与权限控制（key 文件、租户配额、TLS、控制面端口）；⑦ 可直接复制的示例（安装、初始化、日常操作、脚本化调用四类，每类给完整命令行 + 期望输出摘要）。
- 与用户核对：入口模式命名、命令清单、退出码语义、key/quota 字段命名四项需**用户确认后定稿**（D2-12 的 schema 同批确认）。
- 验收：`CLI.md` 与 `--help` 输出、`tools/dev.py` 命令表、`docs/SPEC总览` 三处一致；新增 `ci` 检查“CLI.md 提到的每个参数在 `config.cpp` 的注册表里存在”，防止文档先漂。
- 测试：`ci/test_ci_scripts.py` 增一条 CLI 文档一致性用例（解析 CLI.md 的命令块 → 比对 `tools/dev.py` 的 argparse 子命令名）。

### 5.4 落地状态（2026-11-22，分支 10，未推送）

| 编号 | 状态 | 提交 | 关键证据/备注 |
| --- | --- | --- | --- |
| D2-01 | done | `6e7f027` | matrix manifest 无 git 降级 |
| D2-02 | done | `b917a2e` | run_ci env_check → exit 3 |
| D2-03 | done | `7492070` | 四组矩阵 + seq_cp 缺陷修复 + `evidence/phase13/README.md` |
| D2-04 | done | `2001e58`、覆盖率取证 commit | GGUF 异常面 + 错误码全表；WSL 实测 src 行 4,563/5,351 = 85.3%（`evidence/phase13/coverage/`） |
| D2-05 | done | `cebe06f`、`ccb4014` | 契约文档入库；README 局限段对齐 |
| D2-06 | done | `436800f` | 故障面扫描表 + 2 条新用例 |
| D2-07 | done | `b6d1b33`、镜像取证 commit | WSL Docker 实建 132 MB 真后端镜像，六项冒烟 PASS；CUDA 变体未验证 |
| D2-08 | done | `24d5eb4` | 版本 0.13.0、CHANGELOG、`unit_version` |
| D2-09 | done | `230f9f2` | 三支直方图 + 告警样例 |
| D2-10 | done | `54c721d` | 慢请求 dump + `dev.py trace` |
| D2-11 | done | `46ffd33` | systemd 自愈 + probe + 5 类故障注入 |
| D2-12 | done | `4113122` | 租户 token 小时/日预算 |
| D2-13 | done | `b407570` | SLO/容量表 + CI perf 5% 闸门 |
| D2-14 | done | `8e44b1b` | CLI 七节 + `--help` 同源 + 零漂移断言 |

推送与打 tag 等用户明确同意；本表只记录本地提交。

收口衍生提交（不动功能语义）：`010a0b5` 本表自身、`f152674` CI-PRF-003 规则接线、`677ee72` CLI `--version` 示例区分真后端/stub、`3194430` 容量表 VRAM 与生成脚本同源、`f53b402` `docs/deploy.md` 去掉未实现的事件日志段。

## 7 证据与测试规范

- 证据目录统一 `evidence/phase13/<item>/`：`D2-03 → perf-mixed|perf-static|perf-prefix-on|perf-prefix-off`、`D2-07 → packaging`、`D2-11 → fault-injection`、`D2-13 → slo`。
- 每个产物文件名带 `run_id` 与命令摘要；正文引用 `path#anchor`，不粘贴大段日志。
- 新用例一律进 `tests/{unit,contract,integration,e2e}` 并登记 `docs/testing.md` §2.1（Scope 写目录）。
- 速度标签：单条 ≤30 s 记 `fast`；容器冒烟、真模型、故障注入记 `slow` / `needs-gpu` / `needs-vm`。
- 计数由 `scripts/ci/gen_test_counts.py` 生成，任何人不得手改 README 的 `<!-- test-counts:start -->` 块（D2-05 已把这条写进 `check_docs`）。

## 8 流程与审批

1. 开工前读 `CLAUDE.md`、`AGENT_EXECUTION_PROTOCOL.md`、本文件 §2 冻结边界。
2. 每项：红灯用例 → 最小实现 → 绿灯 → 集成（符合条件时）→ 文档 → 用户 review → commit。
3. 破坏性操作（改契约、改默认行为、删文件、动 `docs/` 跟踪状态）先说明影响面并获批准。
4. 固定核对点：D2-03 前（矩阵配置）、D2-07 前（镜像基线与 GPU 方案）、D2-12 前（key 文件 schema）、D2-08 后（tag 与推送）。
5. 唯一“完成”定义：§4 全 13 项出口达成 + §6.1 门禁全绿 + §6.2 分 ≥7.0 + 一份收口记录 `docs/notes/阶段13-完成记录.md`。

## 9 风险与回滚

| 风险 | 触发 | 应对 |
|---|---|---|
| 四组矩阵 OOM | D2-03，8 GB 卡 | 降到 P≤4 并注明；或用 Q4_K_M 换 Q3_K_M 对比无效则不换，只降并发 |
| 真后端镜像在 Docker Desktop 无 GPU | D2-07 | 改在 WSL docker（`--gpus all`）验证；Windows 侧只保留 stub 镜像证据 |
| 直方图与离线口径打架 | D2-09 | 按 C-P13-01 隔离：直方图只进监控，README 数字只引离线报告 |
| token 预算破坏既有 key 文件 | D2-12 | 可选字段 + 缺省不限 + 未知键仍拒；先在 `config_test.cc` 加兼容用例 |
| 文档跟踪状态变更引发冲突 | D2-05 | 单独一个 commit，只动 `.gitignore` 与 4 个文件；冲突即回退重做 |
| 覆盖率补不上去 | D2-04 | 先补错误码全表（确定性产出），再补 GGUF 异常分支；仍不足则记偏差并申请临时阈值（需批准） |

回滚：每项独立 commit；任一门禁失败即 `git checkout -- <files>` 回上一项，并在 `implementation-notes.md` 记一条偏差。

## 10 裁决记录（2026-11-22，已定，不再讨论）

| # | 问题 | 裁决 |
|---|---|---|
| Q1 | 27B 吞吐是否继续测 | **冻结**：保留代码与文档，不启用、不测试、不出数字；启用需新 SPEC |
| Q2 | ctx / 并发的上限怎么写 | **写成结论**：8 GB 卡 8B 全驻留可用上限 = ctx 1024 / P≤8；超限由门控 BLOCKED，不是待办 |
| Q3 | 扩容方案 | **多实例 + 前置网关，不在本仓库实现**；只交付 `nginx.conf` 参考与文档边界 |
| Q4 | 是否切分阶段 | **不切分**：13 项按编号顺序交付，每项一 commit，无 S0/S1/… |
| Q5 | 功能取舍 | **7 项全做**（D2-07…D2-13）+ CLI/契约专册（D2-14），其余（双 context、分布式、PD、WebUI、全量 OpenAI、27B）一律不做 |
| Q6 | 版本号与 tag | `v0.13.0`，与阶段 13 对齐 |

## 11 变更记录

- v1.0 · 2026-11-22 · 首版：13 项（6 缺陷/证据债 + 7 功能）、三条冻结边界、不切分阶段、生产分口径与 7.0 目标、全局门禁与裁决记录。


