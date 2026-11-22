# CHANGELOG

版本号唯一来源是 `CMakeLists.txt` 的 `project(llmopt VERSION …)`，
`--version`、启动日志、`/metrics` 的 `llmopt_build_info` 都从这里派生。

## 0.13.0 — 2026-11-22

目标：把单实例生产分从 4.4–4.8 推到 ≥7.0（SPEC-DEBUG2 收口），并把三条边界写死。

### 缺陷与证据债

- D2-01 证据链不再硬依赖 git：`tools/bench/matrix.py` 取不到 commit 时降级写 `git_available=false`，manifest 仍可复现。
- D2-02 测试前置显式登记：`run_ci.py` 缺 `clang-format`/`git`/`cmake` 直接 exit 3，Windows 缺 UCRT 只告警；`check_docs` 无 ctest 时 SKIP 而非 FAIL。
- D2-03 Phase 13 四组对照矩阵（static / mixed / prefix-on / prefix-off）产出与判据见 `evidence/phase13/`；结论：补位净收益 +9%（不达 1.3×，不翻案吞吐），交互式容量上限 P=4。顺带修复前缀复用的 KV 位置重叠缺陷（`runner_llama.cpp` 的 donor 裁剪）。
- D2-04 覆盖率弱项补齐：GGUF 类型分支与 20 个错误码全表。
- D2-05 文档漂移修正：取消指标名、构建前置、历史性能数据作废、llama.cpp 版本号对齐。
- D2-06 故障面扫描结论表（`docs/notes/阶段13-故障面扫描.md`）。

### 功能

- D2-07 真后端镜像：固定 llama.cpp revision、非 root 运行、CUDA 变体；本机无 Docker 时构建记 BLOCKED。
- D2-08 版本与发布面：0.13.0、本 CHANGELOG、镜像 label、回滚段落。
- D2-09 监控直方图：`llmopt_latency_queue_wait_ms` / `llmopt_latency_ttft_ms` / `llmopt_latency_itl_ms`＋`scripts/release/prometheus-rules.yml`。对外分位数仍只来自离线复算（C-P13-01）。
- D2-10 排障链路：`--slow-request-ms` 慢请求 dump（四段计时、脱敏、保留 20 份）＋`tools/dev.py trace <req_id>`。
- D2-11 可靠性：systemd `Restart=on-failure`、外部探针 `probe.sh`、四类故障注入用例。
- D2-12 租户 token 预算：key 文件 `quota` 新增 `tokens_per_hour` / `tokens_per_day`，超限 429 + `Retry-After`。
- D2-13 SLO 与容量表：TTFT p50 ≤350 ms、ITL p99 ≤200 ms、可用性 ≥99.5%/30d、错误率 ≤0.1%；容量表由 `scripts/bench/capacity_table.py` 生成；`run_ci --mode fast` 新增 perf 步骤（`check_perf_baseline.py`，劣化 >5% 即失败）。
- D2-14 CLI 与接口契约专册：`CLI.md` 七节，与 `--help`、`tools/dev.py`、`docs/SPEC总览.md` 三处一致。

### 冻结边界（结论，不是待办）

- 27B 吞吐：代码与文档保留，不启用、不测试、不出数字；启用需新 SPEC。
- 单实例上限：8 GB 卡 + 8B 全驻留 = ctx 1024、并发 P≤8；超限由容量门控 BLOCKED。
- 扩容方式：多实例 + 前置网关，不在本仓库实现。

## 0.12.0 — 2026-11-20

阶段 12 收口：断连取消、30 分钟长稳、batching 重基线、key 热加载、打包与运维模板。
