# 日志与运行产物管理规则

版本：v1.0（2026-11-20）　适用：llm-inference-kernel-opt　状态：生效

本文件只回答四件事：**什么写到哪里、每个字段长什么样、保留多久、哪些事不许做**。CI 闸门判定见 [ci-spec.md](ci-spec.md)，测试登记见 [../testing.md](../testing.md)，各阶段出口数据物见 `docs/SPEC-N_*.md`。三者冲突时以本文件为准并立即修正另一份。

---

## 1 三类产物，三种寿命

| 类别 | 内容 | 落点 | 保留 | 可自动删 | 入库 |
| --- | --- | --- | --- | --- | --- |
| 诊断日志 | 人读的运行期日志（spdlog rotating sink） | `logs/server/` | 20 MB × 5 个 | 是（删最旧） | 否 |
| 崩溃现场 | core / minidump / 最后一次 stderr | `logs/crash/` | 最新 2 份 | 是（删最旧） | 否 |
| 证据流 | 批次、抢占、SSE 帧、KV 快照、`/metrics` 抓取 | `bench/results/phase<N>/runs/<run_id>/` | 不轮转，只增 | **否** | 单个 ≤2 MB 可入，超限只留盘 |

判据：**能事后复算验收结论的，是证据流；只为当时看懂发生了什么的，是诊断日志。** 前者进 `bench/results/`，后者进 `logs/`。把证据流写进 `logs/` 是本文件最主要的禁止项——那里有轮转上限。

---

## 2 目录与命名

```text
llm-inference-kernel-opt/
├── logs/                         # 永不入库
│   ├── server/  server.log（当前）+ server.1.log … server.4.log（数字越大越旧）
│   └── crash/   crash-<run_id>.dmp, last-stderr-<run_id>.log
└── bench/results/
    ├── phase0|1|2|3|4/            # 与 03 §1.3 阶段产物同名对齐
    │   ├── <阶段汇总 CSV>          # baseline.csv、cb_curve.csv、chunk_tradeoff.csv …
    │   └── runs/<run_id>/
    │       ├── run_manifest.json
    │       ├── batch-<run_id>.jsonl
    │       ├── preempt-<run_id>.jsonl
    │       ├── sse-<run_id>.jsonl
    │       ├── kv-<run_id>.jsonl
    │       └── metrics_dump.txt
    └── archive/                   # 超大 run 的 zip，保留 manifest 与校验和
```

- `run_id` 格式：`YYYYMMDD-HHMMSS`，本地时区，一次进程启动一个，全链路透传；同一 run 内所有事件文件共用该前缀。
- `run_manifest.json` 必填：`run_id`、`phase`、`config_hash`、`config_snapshot`（生效配置全文）、`model_path` + `model_sha256`、`capability_snapshot`（L1 快照）、`seed`、`git_commit`、`build_type`、`argv`、`host`（CPU/GPU/RAM）、`started_at`、`finished_at`。缺项即该 run 不计入阶段出口证据（对应 N-02、A-9）。
- 日志目录不写进代码常量：配置项 `log.dir` 默认 `<repo>/logs`，CLI 可覆盖。

---

## 3 事件流字段（最小证据集）

格式：**JSONL**，UTF-8，一行一个事件，时间戳 `ts` 为 ISO 8601 带毫秒，另有单调递增 `tick` 供回放对齐。字段名固定，不得按模块各自改名。

| 事件 | 文件 | 必填字段 |
| --- | --- | --- |
| 批次构成 | `batch-<run_id>.jsonl` | `ts, run_id, tick, req_ids[], phases[]`（每个序列的 `PREFILL_CHUNK/PREFILL_LAST/DECODE`）、`step_tokens[]`、`total_tokens`、`block_ids[]`、`running_count`、`waiting_count` |
| 抢占 | `preempt-<run_id>.jsonl` | `ts, victim_req_id, freed_blocks, freed_block_ids[], recomputed_tokens, kv_active_before, reason` |
| SSE 帧 | `sse-<run_id>.jsonl` | `ts, req_id, token_index, token_id, is_first` |
| 生命周期 | 并入 `batch` 文件 | `ts, req_id, from_state, to_state, reason` |
| KV 快照 | `kv-<run_id>.jsonl` | `ts, tick, active_blocks, cached_blocks, free_blocks, internal_frag_ratio, prefix_hit_blocks, leak_check` |
| 指标导出 | `metrics_dump.txt` | 基准结束前抓一次 `/metrics` 原文，不做二次加工 |

字段口径直接决定验收能不能过：2-G7 靠 `batch` 的 `req_ids + phases + step_tokens` 回放；3-G3 靠 `preempt` 的 `victim + freed_blocks + recomputed_tokens`；1-G2 靠 `sse` 的 `req_id + token_index + token_id` 两侧对账；3-G6 靠 `kv` 的 `leak_check`。

---

## 4 级别与性能纪律

日志会直接污染本项目唯一要测的东西（K-4 调度 CPU ≤5%、K-2 ITL p99 ≤3×p50），因此纪律先于信息量：

1. 默认 `info`；**跑基准时锁 `warn`**；`trace/debug` 只按 `--log-req-id` 白名单临时开启，且只对白名单请求生效。
2. 热路径禁止逐 token 同步 flush：用异步 sink，队列上限 8192 条，满了丢弃并累加 `dropped_logs` 计数（进 `/metrics`，不写日志）。
3. 每个 tick 只允许写**一条** `batch` 事件，禁止按序列逐条刷；单次前向的日志行数是 O(1) 而不是 O(并发)。
4. HTTP、调度、执行三个线程各持 logger，调度线程侧只写内存队列；不让日志锁与 socket 写进入调度线程。
5. 指标只走 `/metrics`（在线 counter/gauge）与离线分位数，**日志不参与 KPI 口径计算**；不得用日志反推吞吐。

---

## 5 保留与上限（默认值）

| 项 | 默认 | 超限动作 | 执行者 |
| --- | --- | --- | --- |
| `logs/server/` 单文件 | 20 MB，保留 5 个（合计 ≤100 MB） | 旋转时删最旧 | spdlog `rotating_file_sink_mt` |
| `logs/crash/` | 保留最新 2 份 | 删最旧 | 启动自检 + 脚本 |
| `logs/` 总量 | ≤200 MB | 打印告警并删最旧的 `server.log.*` | 脚本 |
| `bench/results/` | 不自动删 | 单个 run >1 GB 时打包进 `archive/`，留 `run_manifest.json` 与校验和 | 人工/脚本，`--prune-runs` 显式指定才执行 |

`scripts/log_hygiene.py` 是唯一执行器：默认 dry-run 只报告，`--apply` 才删除，且只作用于 `logs/server/` 与 `logs/crash/`；**不加 `--prune-runs N` 就永不触碰 `bench/results/`**。启动自检只打印超限告警，不在启动路径上删文件。

---

## 6 禁止项

1. 不把证据流写成 `.log` 后缀——`*.log` 被 `.gitignore` 全排，会连解析和归档一起丢掉；证据流用 `.jsonl` / `.csv` / `.txt`。
2. 不 `git add -f logs/`。顶层 `logs/` 不在 CI-DIF-001 的 `ALLOWED_TOP` 白名单里，任何入库文件都会判失败；它本就是"永不入库"目录。
3. 单个数据文件 >2 MB 不入库（CI-DIF-005），只留盘并在 manifest 里登记校验和。
4. 不用日志替代 `/metrics`，不在日志里打权重、完整 prompt 或 token 文本（体积与隐私双重理由），需要时打 `prompt_sha256` 与长度。
5. 不自动清理 `bench/results/`，不因磁盘告警删除任何带 `run_manifest.json` 的目录。

---

## 7 与阶段验收的对应

| 验收项 | 证据物 |
| --- | --- |
| 2-G7 批次可回放 | `phase2/runs/<run_id>/batch-*.jsonl` |
| 3-G1 / 3-G3 切块与抢占可回溯 | `phase3/runs/<run_id>/batch-*.jsonl` + `preempt-*.jsonl` |
| 1-G2 SSE 两侧逐 token 对账 | `phase1/runs/<run_id>/sse-*.jsonl` |
| 3-G6 无泄漏块、回收有日志 | `phase3/runs/<run_id>/kv-*.jsonl` |
| K-4 调度单步耗时分布 | `phase2/sched_cpu.csv`（离线统计，非日志） |
| A-9 两轮可复现对账 | `run_manifest.json` ×2 + 阶段汇总 CSV |

---

## 8 变更

- v1.0 · 2026-11-20 · 首版：三类产物分寿命、事件字段最小集、保留上限与禁止项；执行器 `scripts/log_hygiene.py`。
