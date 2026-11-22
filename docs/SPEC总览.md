# SPEC 总览

索引。只列关系与状态，规格正文在各 SPEC 文件内。契约（C-01…C-13）只在 SPEC-1 定义，其余 SPEC 只引用不重定义。

| SPEC | 目标一句话 | 前置 | 状态 | 出口数据物 |
|---|---|---|---|---|
| [SPEC-0 基线测量](SPEC-0_基线测量.md) | 用可复现的压测脚本量出 llama.cpp 在本机的 TTFT/ITL/吞吐基线，作为后续所有对比的参照 | 规划/03 阶段 0 | done | `baseline.csv`、`capability_snapshot.json`、`run_manifest.json` |
| [SPEC-1 串行闭环引擎](SPEC-1_串行闭环引擎.md) | 一个进程内跑通"HTTP 进 → 逐 token 出"的单请求闭环，固定全部接口契约 | SPEC-0 | done | `demo_server`（单请求闭环）、`sse_frames.log`、`consistency_test` 报告 |
| [SPEC-2 连续批处理与KV复用](SPEC-2_连续批处理与KV复用.md) | 多请求并发下按 token 级组批、按定长块管 KV，块可共享复用 | SPEC-1（C-01…C-10） | done | `batch_trace.csv`、`kv_util.json`、P=1/2/4/8 吞吐与 ITL 对比表 |
| [SPEC-3 ChunkedPrefill与抢占](SPEC-3_ChunkedPrefill与抢占.md) | 长 prompt 切块混排进 decode 批次，显存不足时按优先级抢占并恢复 | SPEC-2（组批与块表） | done | `chunk_trace.csv`、`preempt_events.json`、长 prompt 下 ITL p99 曲线 |
| [SPEC-4 指标与实验矩阵](SPEC-4_指标与实验矩阵.md) | 统一指标口径与打点，跑满实验矩阵产出可对比的 CSV | SPEC-1…SPEC-3 | done | `metrics_dump.txt`、`experiment_matrix.csv`、`report.md` |
| [SPEC-5 可选预研](SPEC-5_可选预研.md) | 只写判据与评估口径，不写实现；PD 分离与投机解码达到阈值才立项 | SPEC-2、SPEC-4 | done（无代码） | 判据计算表、`phase5_预研评估.md` |
| [SPEC-6 软件与服务完成度](SPEC-6_软件与服务完成度.md) | 把实验床补成单机可用小服务：最小兼容面、配置分层、drain/ready、限流背压、指标格式、部署与 sanitizer；不改调度语义 | SPEC-4（指标口径必须先冻结） | done | `compat_frames.txt`、`drain_report.json`、`metrics_format.txt`、`deploy_smoke.log` |
| [SPEC-9 安全隔离与可运维性](SPEC-9_安全隔离与可运维性.md) | 补 TLS、控制面隔离、Completion watchdog、静态 key 文件与租户配额 | SPEC-6 | done | TLS 握手日志、控制面端口证据、租户配额用例 |
| [SPEC-DEBUG1 缺陷修复与算力入口封堵](SPEC-DEBUG1.md) | 以项目现状为准收口 13 项缺陷与 9 项文档漂移，并完成断连取消、soak、batching 重基线、key 生命周期、打包运维、计数自动化 | SPEC-9 | done（S0–S6 已落地；S7 收口并入 SPEC-DEBUG2） | `evidence/phase12/**` |
| [SPEC-DEBUG2 生产化收口](SPEC-DEBUG2.md) | 缺陷优先 + 7 项必要功能，把单实例生产分从 4.4–4.8 推到 ≥7.0；同时冻结 27B、写死 ctx 1024 / P≤8、把扩容移出仓库 | SPEC-DEBUG1 | approved（2026-11-22，待开工，不切分阶段） | `evidence/phase13/**`、`docs/容量与SLO` |



## 版本与状态

- 每份 SPEC 标题下的引用块带 `> 版本：vX.Y（对应阶段 N）｜状态：…｜契约基线：C-xx…｜更新：日期`。
- **v0.x = 未开工草案**（验收表未跑）；**v1.0 = 该阶段 1.7 验收表全过**，同时提交完成记录。
- 阶段内重大改动升 v0.x 的次位（v0.1 → v0.2），并在该 SPEC 末尾"变更"区保留一行：`v0.N · 日期 · 改了什么`。只记改动与日期，不写理由。
- 契约基线：SPEC-1 写 `C-01…C-13（本文件定义）`；其余写所引用的 C-xx 与自有 C-Px 扩展号。



## 变更记录

- v0.7 · 2026-11-22 · 新增 SPEC-DEBUG2（阶段 13，生产化收口，不切分阶段）；DEBUG1 状态由 doing 改为 done（S0–S6 落地，S7 收口并入 DEBUG2）。
- v0.6 · 2026-11-21 · 索引补 SPEC-9 / SPEC-DEBUG1，状态由 draft 改为实际（0–6、9 done，DEBUG1 doing）；依赖表按实际实现修正（moodycamel / Google Benchmark 均未引入）；非目标改 0.5B–27B 并更新多租户口径。
- v0.5 · 2026-11-20 · 新增 SPEC-9 安全隔离与可运维性。
- v0.4 · 2026-11-20 · 新增 SPEC-6 软件与服务完成度（七份索引）；关系补 4→6 顺序与 01 §2.3 例外待裁决。
- v0.3 · 2026-11-20 · SPEC-0 新增 §1.9 开发工具链（`tools/dev.py` 九命令契约 + 可扩展性 + 三条验收），阶段 0 交付 `build/env.json`。
- v0.2 · 2026-11-20 · KV 改"块表唯一真相 + 协作/自主两档"（C-06 重定义、ADR-14）；新增 C-11 `SequenceStep`/Batch、C-12 `SchedulerMailbox`/`SchedulerCore`、C-13 `ResponseSink`（ADR-15/16）；KPI 拆分（TTFT 四段、K-5 四项、最大连续需求降为后端约束）；指标改在线/离线两套。
- v0.1 · 2026-11-20 · 六份 SPEC 按八节格式重写并迁至 `docs` 根；补版本块、1.3 复用件落点、决策落点核对表。



## 决策 → SPEC 落点核对表

基准是 01/02/03/04/05 已定稿结论。每份 SPEC 的 1.3 末尾"复用件落点"表给出本阶段的库与落点，本表只记"决策有没有真落下去"。

| 决策 / ADR | 结论一句话 | 落在哪份 SPEC 的哪节 | 是否已对齐 |
|---|---|---|---|
| ADR-02 | httplib 0.18.3 直接复用，做 HTTP/SSE 层 | SPEC-1 §1.3 落点表、SPEC-2 §1.3、SPEC-3 §1.3 | 已对齐 |
| ADR-03 | nlohmann 3.11.3 直接复用，做 JSON 解析与帧渲染 | SPEC-1 C-01/C-02、SPEC-2/3/4 §1.3 | 已对齐 |
| 日志 | spdlog 1.17.0 header-only，内嵌 fmt，不引独立 fmt | SPEC-1/2/4 §1.3 落点表 | 已对齐 |
| 请求队列 | 自研 `SchedulerMailbox`（`std::deque` + `mutex` + `condition_variable`）；moodycamel **未引入** | SPEC-1 C-12、`include/llmopt/scheduler.hpp` | 已修正（原写 moodycamel 直接复用） |
| 测试/基准 | doctest 2.4.11 + CTest；**未引入** Google Benchmark（基准走 `tools/bench` 自研采集 + 离线复算） | 各 SPEC §1.7、`tools/bench` | 已修正（原写 Google Benchmark 1.8.3） |
| ADR-08 | **自研**文本 exposition（100–150 行）+ `IMetricsSink`；prometheus-cpp 备选**未采用** | SPEC-4 §1.3、§1.8 | 已修正（原按 histogram 写） |
| ADR-01 | ggml/llama.cpp 复用+调整，落在 C-07 内部，不改对外契约 | SPEC-1 C-07、SPEC-2 ubatch、SPEC-3 混排 | 已对齐 |
| C-06 | 块表是唯一分配决策者，只做逻辑分页/前缀索引/引用计数，不分配物理内存；协作模式（阶段 2，对账）与自主模式（阶段 3，显式 cell，需 patch）两档 | SPEC-1 C-06、SPEC-2 §1.3/2-G8、SPEC-3 3-G7 | 已修正（原"块表管物理块"） |
| ADR-14 | KV 唯一真相 + 两档模式；cell 只作下游映射，禁止反向回写；两边各自记账判 bug | SPEC-1 C-06、SPEC-2 §1.3/2-G8、SPEC-3 3-G7 | 已修正（原"块表管物理块"） |
| ADR-15 | `SchedulerCore` 单线程独占零锁、状态只由 Core 改；`Mailbox` 只入队与背压；取消走 Mailbox | SPEC-1 C-12、R-09、SPEC-2 §1.2 | 已修正 |
| ADR-16 | 每请求一个 `ResponseSink`，HTTP 层独占；执行/调度线程只 push 有界队列，socket 写只在持连接线程 | SPEC-1 C-13、§1.2 图、SPEC-2 §1.2 | 已修正 |
| ADR-06 | HTTP 池 → 队列 → 单调度 → 单执行；HTTP 线程不碰 KV/context | SPEC-1 §1.2/R-01、SPEC-2 §1.2、SPEC-3 R-07、SPEC-4 §1.2 | 已修正（SPEC-4 补"只记入队/出队时刻"） |
| K-8 | 峰值 ≤4.5 GiB；启动前估算超预算即报错退出（N-06/A-8） | SPEC-1 C-09、1-G6、§1.6 配置表；SPEC-3 §1.4/3-G5；SPEC-4 K-8 行 | 已修正（SPEC-3 补预算约束） |
| ADR-11/12 | M11 四层 L0–L3、三态、默认 `off`、`force_on` 需横幅+降级 | SPEC-0 0-G5、SPEC-1 1-G7、SPEC-3 3-G5、SPEC-4 K-9 | 已修正（SPEC-3 补默认态） |
| 容量判据 | 不写死路数，走 `required_bytes vs free_bytes×0.80` 三态 | SPEC-2 2-G3、SPEC-3 3-G5、SPEC-4 §1.8 | 已修正（四处"8GB 跑几路"改判据） |
| 指标口径 | 两套：`/metrics` 只 counter+gauge（sum/count）；分位数与 p50/p95/p99 由基准工具离线算 | SPEC-4 §1.3（"在线/离线"列）、§1.8 D4-6 | 已修正（原标 histogram 与自研后端矛盾） |
| C-11 | 批次按序列描述：`SequenceStep`（phase、token 区间、KV 落点、是否采样）；Prefill 不顺手采样 | SPEC-1 C-11、R-11、SPEC-2 §1.4、SPEC-3 §1.4 | 已修正 |
| KPI 拆段 | TTFT 分 `TTFT_e2e`/`TTFT_in` + tokenize/调度等待/prefill 三段；K-5 拆 active/cached/free/碎片；最大连续需求降为后端约束 | 01 §3.1（K 表）、SPEC-4 K 映射 | 已修正 |
| 无 GPU 可测 | A-2/N-07：单测不依赖 GPU | SPEC-2/3/4 §1.7 | 已对齐 |
| 阶段 5 门槛 | 阶段 5 不挂验收门槛（03 §7） | SPEC-5 引用块、§1.7 | 已修正（原引不存在的 `5-Gx`） |
| 被否方案 | prometheus-cpp / Drogon / libuv / simdjson / RapidJSON / Catch2 若出现只能是"备选，未采用" | 六份全文 | 已对齐（Drogon 等零残留） |
| 开发工具链 | 单入口 `tools/dev.py` 九命令，零第三方依赖，退出码 0/1/2/3/4，只服务开发闭环 | SPEC-0 §1.9、01 F-14、03 0-G6 | 已对齐（脚本 v0.1 已落） |



## 关系

- 0 → 1 → 2 → 3 串行；4 在 1 之后即可并行起步，矩阵最后跑。
- 6 必须在 4 之后（6-G5 依赖指标口径冻结）；5 与 6 无依赖，可并行。
- 5 无代码、无前置实现，只吃 2/4 的数据。
- 每份 SPEC 的 1.7 验收表全过 → 状态改 done → 同时提交 `阶段完成记录_<阶段>.md`（模板见 [阶段完成记录_模板](阶段完成记录_模板.md)）。无记录视为未完成。



## 非目标（各 SPEC 共同约束）

单机、单进程，量化模型覆盖 0.5B–8B 出吞吐结论；**27B 档自 2026-11-22 起冻结**（保留容量/抢占验证代码，不启用、不测试、不出任何吞吐数字，见 SPEC-DEBUG2 F-A）。不做多机 PD 分离、不做张量并行、不引 CUDA 自研 kernel、不做跨实例 KV 传输；**水平扩展靠多实例 + 前置网关，不在本仓库实现**（SPEC-DEBUG2 F-C）。理由与判据见 [规划/03](规划/03_实施路线图.md) 与 SPEC-5。

> 多租户与配额的口径已随阶段 11 变化：静态 key 文件 → 租户 + 并发/队列/限流/KV 配额已实现（SPEC-9）；token 日配额、key 热加载与管理 API 属阶段 12（SPEC-DEBUG1 S4）。WebUI 与全量 OpenAI 兼容面仍不做。
