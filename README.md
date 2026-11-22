# llm-inference-kernel-opt

**C++20 模型推理服务实验**

[简体中文](README.md) ｜ [English](README.en.md)



## 项目简介

本项目和 llama 相关，主要围绕 C++ 大模型推理服务：目前 C++ 大模型主要有以下方向，我结合能力和设备情况选择尝试服务调度。

> - 偏算子 / 后端：ggml 后端插件化：自定义 Backend 与算子加速
> - **偏服务调度：C++ 大模型推理服务：Continuous Batching 与请求调度**
>- 偏算法加速：LLM 投机解码落地与端到端加速评估
> - 偏低比特：低比特量化下的推理精度 — 吞吐权衡（INT4/FP8）



项目主要完成 ：模型加载和计算直接交给 llama.cpp / ggml，只在做“交通调度”：谁先算、几个一起算、显存不够时让谁等、KV 缓存怎么分块和复用。仓库名里的 `kernel` 指**服务内核**（调度与 KV 管理），不是 GPU kernel；项目不做分布式与训练，规模限定在单机、单进程、0.5B–27B 量化模型。：**请求拼批、切块、复用 KV，究竟能快多少**； 不训练模型，也不写 CUDA 算子。

以 Qwen3-8B 为主力档（S3 用 `Q3_K_M` 全驻留，早期 L1 证据用 `Q4_K_M`）；Windows 侧主要跑 stub 后端做调度与接口测试。它既可以当学习 continuous batching 的可运行参考，也可以当 llama.cpp 之上的最小 OpenAI 兼容服务骨架。注册数见下方统计块。



## 版本

- 当前版本 **0.13.0**（`CMakeLists.txt` 里 `project(llmopt VERSION)` 是唯一来源，`--version`、启动日志、`/metrics` 的 `llmopt_build_info` 同源）。变更记录见


- 回滚三步：


1. 代码：`git revert <坏提交>` 或 `git checkout <上一个 tag>`，重新 `cmake --build build`；
2. 部署：把镜像/二进制换回上一版本，确认 `/readyz` 回 200 且 `llmopt_build_info` 的 version 已变；
3. 数据面：无持久状态，KV 与队列都在内存里，回滚只需重启进程；慢请求 dump 与证据目录可直接保留。



## 核心功能



- 目前本机与实际测试位置：暂时先实现小团队内部使用

1. **Continuous Batching（请求拼批）**——解决“一次只能服务一个请求、GPU 总在等人”的问题。把多个请求按 token 拼进同一批，每步有 token 预算上限（`max-num-batched-tokens`），跑不下的进等待队列，队列满直接回 429。实测（8B 全驻留、等长短输出、单执行线程）：1 路 77.1 tok/s，8 路 80.7 tok/s，**只快 5%**；但单请求尾延迟 ITL p99 从 23.3 ms 涨到 171.3 ms。所以”拼批一定提吞吐”在这台机器上、这个负载形状下不成立，瓶颈是单执行线程串行 + 显存带宽封顶，控制面本身只占 0.01–0.02% CPU。等长负载抹掉了「短请求先完、槽位立刻复用」这一项；混长/静态批/前缀开关四组对照已于 D2-03 跑完（见第 8 节与 `evidence/phase13/README.md`）：混长并发在 TTFT 上收益巨大（P8 6970→307 ms），但吞吐只从 57.1 提到 62.4 tok/s（+9.3%），未达 1.3× 翻案线。

2. **块表 KV 管理（给显存做分页）**——解决“KV 缓存越长越占显存、释放不干净”的问题。把 KV 切成固定 16 token 的块，用引用计数标记谁在用，写时复制（CoW）避免互相踩；块表是分配的唯一决策者，后端只按块表给的地址算，不反向改块表。实测 30 分钟压测收尾后 `used=cached=0`，块表对账差异为 0。

3. **前缀复用（相同开头不重复算）**——解决“同一段系统提示、同一段上下文反复 prefill”的浪费。相同前缀的满块共享逻辑块与引用计数，可用 `--disable-prefix-cache` 关掉。注意：共享的是逻辑块，物理 KV 仍由 llama.cpp 的 `seq_cp` 复制或预留目标槽，**不宣称物理零拷贝**。单测里同前缀第二次请求命中 8 token，只需再算剩余 2 token。

4. **Chunked Prefill（长提示切段）**——解决“一个超长 prompt 把整批卡住、别的请求干等”的问题。长 prompt 按 `--chunk-size`（32/64/128/256）切段，和正在生成的 token 混着算；只有最后一段采样输出，中间段只推进 KV。单测覆盖 320 token 切成 128/128/64，切块与整段 prefill 的输出逐 token 一致。

5. **抢占与恢复（显存不够时让位）**——解决“资源不够就整批失败或 OOM”的问题。按代价模型挑一个“最划算”的请求暂停、释放它的 KV，等资源够了再重算恢复；等太久的请求有老化豁免，同一请求连续被抢有上限，每次抢占可写 CSV 回溯。原则是**宁可慢一点，也不 OOM、不 5xx**。

6. **最小 OpenAI 兼容面（现成客户端能直接连）**——解决“不想自己写协议适配”的问题。提供 `/v1/completions`、`/completion`、`/v1/chat/completions`、`/v1/models`；流式走 SSE，逐 token 下发，以 `data: [DONE]` 收尾。只做常用字段，不追求完整 OpenAI 字段集。

7. **容量门控（启动前先算账）**——解决“模型一加载就 OOM”的问题。启动前探测内存和显存，按“需求 ≤ 空闲 × 0.8”判断，给出 `ELIGIBLE / DEGRADED / BLOCKED` 三态，`off / auto / force_on` 三档，默认 `off`；层没全放 GPU 时，显存和内存分开算。8B 在 8 GB 卡上就是靠“部分层放 CPU”才起得来；全驻留时权重占 3672 MiB，KV 随上下文涨到约 918 MiB。

8. **可运维性（能上线、能排障）**——解决“出了事不知道、停服很粗暴”的问题。配置按 CLI > 环境变量 > JSON > 默认四层覆盖；有 `/healthz`（活着没）和 `/readyz`（能接活没）；SIGTERM 后先拒新请求、把在飞请求收尾再退出；令牌桶限流；静态 key 文件鉴权 + 每租户并发/队列/限流/KV 配额（阶段 11）；Prometheus 文本指标输出 counter/gauge，并带三支 TTFT/ITL/时延直方图（D2-09）与 `llmopt_tenant_*` 租户标签。实测：容器 `docker stop` 退出码 0；30 分钟压测 297,441 个请求、0 错误、RSS 只涨 0.05 MiB。

9. **实验与证据链（结论可复查）**——解决“性能数字口口相传、回头没法复现”的问题。`tools/bench` 负责发请求、收 SSE、算 TTFT/ITL/吞吐、画图，结果按 `phaseN/runs/<run_id>/` 归档，CSV / JSONL / 日志都能重跑。阶段 12 的 30 分钟长稳和 batching 矩阵就是这么出的数，原始文件在 `evidence/phase12/`。



## 技术栈



| 类别 | 选型 |
| --- | --- |
| 语言标准 | C++20：`std::span` / `std::optional` / `std::atomic` / RAII / `enum class`；热路径不用 modules、协程、ranges |
| 构建 | CMake ≥ 3.20 + FetchContent（依赖只拉到 `third_party/_deps`，不入库）；可选 Ninja |
| 推理后端 | ggml + llama.cpp（tag `b12557`，可用 `LLMOPT_LLAMA_EXPECTED_REVISION` 严格校验），编译期开关 `LLMOPT_ENABLE_LLAMA`；默认 stub 后端供 CI 与路由自检 |
| HTTP / SSE | cpp-httplib 0.18.5（单头，关闭 OpenSSL / zlib / brotli；0.18.5 起提供 `is_connection_closed`） |
| JSON | nlohmann/json 3.11.3 |
| 日志 | spdlog 1.17.0（header-only，内嵌 fmt） |
| 调度并发模型 | HTTP 线程池 → 有界 `SchedulerMailbox`（`deque` + `mutex` + `condition_variable`）→ `SchedulerCore` 单线程零锁改状态 → 执行线程串行写 KV |
| 测试 | doctest 2.4.11 + CTest；ASan / UBSan / TSan；Python 集成与 e2e 脚本 |
| 指标 | 自研 Prometheus 文本 exposition（counter / gauge），分位数由离线工具复算 |
| 脚本 | Python 3.10+（仅标准库，用于 CI 闸门、基准与容量探测） |
| 平台基线 | Windows 11：g++ 16.2 / clang 22.1.8 / CMake 3.30.3（stub 档）；WSL Ubuntu 26.04：真实 llama.cpp 后端 |

依赖约束：直接第三方库 ≤ 5（doctest / cpp-httplib / nlohmann / spdlog / llama.cpp），运行期不依赖 Python，无数据库。



## 快速开始

前置条件：C++20 编译器（GCC 13+ 或 Clang 16+ 实测可用）、CMake ≥ 3.20、Python 3.10+（仅工具与 CI）。GPU 可选，CPU 路径必须能跑完单测与闭环。



### 构建与测试（stub 后端，无需模型权重）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build -L fast --output-on-failure
```

产物为 `build/llmopt_server`（Windows 下为 `build/llmopt_server.exe`）。



###  stub 自检（跑通 HTTP → 调度 → SSE）

```powershell
python tests\fixtures\models\make_fake_gguf.py tests\fixtures\models\fake.gguf
$env:LLMOPT_ALLOW_STUB = "1"
.\build\llmopt_server.exe --model tests\fixtures\models\fake.gguf --backend stub `
  --port 18080 --ctx-tokens 512 --mode off --log-level warn
```



```bash
curl -s http://127.0.0.1:18080/healthz
curl -N -X POST http://127.0.0.1:18080/completion \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"hello","stream":true,"max_tokens":16}'
```



### 后端



- Linux / WSL，需先编好 llama.cpp

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLMOPT_ENABLE_LLAMA=ON -DLLMOPT_LLAMA_ROOT=/root/llm-src/llama.cpp
cmake --build build --parallel
./build/llmopt_server --model /mnt/d/ai_model/models/qwen3-8b/Qwen3-8B-Q4_K_M.gguf \
  --port 8080 --ctx-tokens 1024 --max-num-batched-tokens 1024 \
  --chunk-size 64 --max-concurrency 4 --mode off --threads 12 \
  --n-gpu-layers 30 --kv-cache-type q8_0 --donor-slots 0 --disable-prefix-cache
```

`LLMOPT_ENABLE_LLAMA=OFF` 时只编 stub 后端；Windows 本机没有 CUDA 工具链与 SIGTERM 语义，真后端与部署用例以 Linux/CI 为准。



### 开发工具单入口

| 命令 | 作用 |
| --- | --- |
| `python tools/dev.py env` | 打印工具链、内存、GPU 与模型路径快照 |
| `python tools/dev.py build` | 统一构建（Debug / Release） |
| `python tools/dev.py test` | 按注册表跑 fast / slow 用例 |
| `python tools/dev.py run` | 用配置启动服务 |
| `python tools/dev.py diagnose` | 收集构建、测试与运行诊断信息 |
| `python scripts/ci/run_ci.py --mode fast` | 本地跑与 CI 等价的门禁（文档 + 注册表 + 代码） |
| `python scripts/ci/run_ci.py --mode pr` | 追加 Diff 预算与代码质量检查 |
| `python scripts/ci/check_coverage.py --build-dir build --base origin/main` | 覆盖率门禁（`src/` 行 ≥ 70%，改动行 ≥ 80%） |

`dev.py` 退出码：0 成功、1 失败、2 用法错误、3 环境缺失、4 跳过。



## 环境变量

所有服务参数都有同名环境变量，优先级为 **CLI > `LLMOPT_*` > `--config` 文件 > 内置默认**。下表只列常用项，完整清单见 [`CLI.md`](CLI.md)。

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `LLMOPT_MODEL` | 无（必填） | GGUF 模型路径；只读元数据做预算，不在此处加载权重 |
| `LLMOPT_CONFIG` | 空 | 平铺 JSON 配置文件；未知键 / 类型不符 / 越界即退出 2 |
| `LLMOPT_HOST` / `LLMOPT_PORT` | `127.0.0.1` / `8080` | 监听地址与端口，默认只绑本机 |
| `LLMOPT_CTX_TOKENS` | `2048` | 上下文上限，同时决定 KV 估算与溢出判定 |
| `LLMOPT_MAX_CONCURRENCY` | `8` | 同时在跑的序列上限，超出进 waiting 队列 |
| `LLMOPT_MAX_NUM_BATCHED_TOKENS` | `2048` | 单步 token 预算；chunked prefill 后允许小于 `ctx-tokens` |
| `LLMOPT_QUEUE_CAPACITY` | `32` | waiting 队列上限，满即 429 `QUEUE_FULL`；数据面线程预算要求 `max-concurrency + queue-capacity + 3 ≤ 256` |
| `LLMOPT_BATCH_POLICY` | `continuous` | `continuous`=完成即补位；`static`=整批跑空才准入，仅供对照实验 |
| `LLMOPT_BLOCK_SIZE` | `16` | KV 逻辑块 token 数，也决定前缀命中粒度 |
| `LLMOPT_CHUNK_SIZE` | `128` | chunked prefill 单步宽度，仅 32 / 64 / 128 / 256 |
| `LLMOPT_MODE` / `LLMOPT_SAFETY_RATIO` | `off` / `0.80` | 容量门控模式与软预算系数 |
| `LLMOPT_BACKEND` | `llama` | `llama` 或 `stub`；stub 需 `LLMOPT_ALLOW_STUB=1` |
| `LLMOPT_API_KEY` | 空 | 单 token Bearer 鉴权，空表示不鉴权，日志与启动摘要只打印 set/unset；与 `LLMOPT_API_KEYS_FILE` 互斥 |
| `LLMOPT_API_KEYS_FILE` | 空 | 阶段 11 静态 key 文件路径；JSON 只存 SHA-256 与租户配额，POSIX 权限必须 0600 |
| `LLMOPT_RATE_LIMIT_RPS` | `0` | 全局令牌桶速率，0 关闭；超额 429 `RATE_LIMITED` + `Retry-After: 1` |
| `LLMOPT_DRAIN_DEADLINE_SECONDS` | `30` | SIGTERM 后在飞请求的排空上限 |
| `LLMOPT_REQUEST_DEADLINE_SECONDS` | `120` | 请求级总时限，从入队起算；流式/非流式共享 |
| `LLMOPT_ALLOW_STUB` | 未设置 | 设为 `1` 才允许 stub 后端，供测试与 CI 使用 |
| `LLMOPT_LLAMA_ROOT` | `/root/llm-src/llama.cpp` | CMake 查找 llama.cpp 头文件与库的根目录 |

无 `DATABASE_URL`：本项目不引入数据库与持久化。



## 项目结构

```text
llm-inference-kernel-opt/
├─ src/                  # 生产代码：HTTP、协议、调度、抢占、KV、门控、指标、后端适配
│  ├─ http_server.cpp    # 路由、SSE 回写；handler 只构造请求对象入队
│  ├─ protocol.cpp       # 请求 JSON 解析、SSE 帧序列化
│  ├─ scheduler.cpp      # 准入、组批、token 预算、chunk 规划
│  ├─ preempt.cpp        # 抢占代价模型、victim 选择、重算恢复
│  ├─ kv.cpp             # 块表、引用计数、前缀哈希、CoW
│  ├─ runner_stub.cpp    # 假后端：无模型也能跑通控制面
│  ├─ runner_llama.cpp   # llama.cpp 适配：batch → ubatch → compute → 采样
│  ├─ gate.cpp           # 容量探测与三态判据
│  ├─ metrics.cpp        # Prometheus 文本指标
│  └─ main.cpp           # 参数解析、启动预算、信号与排空
├─ include/llmopt/       # 对外接口与数据结构（scheduler / sequence / kv / sink / runner ...）
├─ tests/                # unit / contract / integration / e2e / perf + Python 服务夹具
├─ tools/                # dev.py 单入口；bench 采集、聚合、绘图、矩阵编排
├─ scripts/              # ci 闸门、bench 复现脚本、release（Dockerfile / systemd / soak）
├─ docs/                 # 规划、SPEC-0…9 + SPEC-DEBUG1、CLI 契约、测试注册表、调研与性能证据
├─ cmake/                # FetchContent 依赖版本与 llama.cpp 接入
├─ ci/                   # 闸门脚本自测（不计入产品覆盖率）
└─ .github/workflows/    # ci.yml（L0–L4 闸门 + 定时 real-backend）/ nightly.yml（TSan、ASan/UBSan、30 min soak）
```

目录约束：顶层只保留源码、测试、脚本、文档与根级配置；深度 ≤ 3，不建 `application/domain/infrastructure` 之类抽象层。CMake 目标为 `llmopt_core`（common、protocol、sha256、config、kv、preempt、scheduler、sink、metrics、resource、text、gate、gguf、runner_stub、log）与 `llmopt_server`（http_server、main，真后端开启时追加 runner_llama）。



## 使用说明



### 请求与流式响应

```bash
# 流式补全
curl -N -X POST http://127.0.0.1:8080/completion \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"用一句话解释 KV cache","stream":true,"max_tokens":64,"seed":42}'

# 非流式 + 取文本
curl -s -X POST http://127.0.0.1:8080/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"hello","max_tokens":8}' | jq -r '.choices[0].text'
```

请求体上限 4 MB，主要字段：`prompt`（必填）、`id`、`stream`、`max_tokens` / `n_predict`（默认 128）、`top_k`、`top_p`、`min_p`、`temperature`、`repeat_penalty`、`seed`、`stop`（最多 4 条）、`n`（1–4，仅非流式）。采样链顺序固定为 **惩罚 → top-k → top-p → min-p → 温度 → dist**，请求不能改顺序。

Chat 端点只支持纯文本 `system / user / assistant`；`tools`、`tool_choice`、`logprobs`、`logit_bias`、`response_format`、图片分片等一律返回 400 `UNSUPPORTED_FIELD`，不静默忽略。



### 错误

错误统一为 `{"error":{"code":"QUEUE_FULL","message":"...","retryable":true}}`。

| 场景 | HTTP | code |
| --- | --- | --- |
| 队列满 | 429 | `QUEUE_FULL` |
| 连接排队满（传输层过载） | 503 | `SERVICE_UNAVAILABLE` |
| 限流 | 429 | `RATE_LIMITED` |
| 租户配额满 | 429 | `QUOTA_EXCEEDED` |
| KV 低于水位 | 429 | `KV_BELOW_WATERMARK` |
| 上下文溢出 | 400 | `CONTEXT_OVERFLOW` |
| 鉴权失败 | 401 | `UNAUTHORIZED` |
| KV 或资源不足 | 507 | `KV_INSUFFICIENT` / `RESOURCE_INSUFFICIENT` |
| 执行失败 | 500 | `EXECUTION_FAILED` |



```bash
curl -s http://127.0.0.1:8080/healthz   # 存活，始终 200，body 带 starting|ready|draining|stopped
curl -s http://127.0.0.1:8080/readyz    # 就绪，仅 ready 返回 200，其余 503
curl -s http://127.0.0.1:8080/metrics | grep llmopt_requests_total
curl -s http://127.0.0.1:8080/metrics | grep 'llmopt_tenant_.*tenant="tenant-a"'
```

进程退出码：0 正常、1 启动失败、2 用法错误、3 资源不足、4 门控拒绝。



### 基准与复现

```bash
python -m tools.bench.collect --config tools/bench/config/phase0_smoke.json \
  --out tools/bench/results/phase0/smoke
python -m tools.bench.aggregate --input tools/bench/results/phase0/smoke
python scripts/spec5/recompute_spec5.py \
  --csv docs/evidence/phase4-official/matrix_report.csv \
  --traces docs/evidence/phase4-official --out docs/evidence/phase5
```

`/metrics` 输出 counter / gauge，并按 D2-09 补了三支在线直方图（`llmopt_latency_queue_wait_ms`、`llmopt_latency_ttft_ms`、`llmopt_latency_itl_ms`，前缀刻意用 `latency_`，避免与既有 `llmopt_ttft_ms_sum/_count` 撞成重复时间序列）；离线基准工具仍可复算 p50 / p95 / p99，口径一致。



## 当前水平与局限



- 已完成的验证面：阶段 0 基线采集、阶段 1 串行闭环、阶段 2 连续批处理与 KV 复用、阶段 3 chunked prefill 与抢占、阶段 4 指标与实验矩阵、阶段 6 服务完成度、阶段 8 8B 真服务证据、阶段 9 TLS/控制面隔离与 Completion watchdog、阶段 10 真后端 nightly 可靠性收口、阶段 11 静态 key 与租户配额、阶段 12 缺陷面修复（S0，3 项 P1 + 9 项 P2）；阶段 5 只做预研判据，不落实现。真服务 e2e 10 项另行通过。


<!-- test-counts:start -->
测试基线由脚本从 CTest 注册表生成，见 [docs/testing.md](docs/testing.md) §2.1（勿手改）。
<!-- test-counts:end -->



- 官方 8B 矩阵（阶段 12 S3 重基线，WSL Ubuntu 26.04，RTX 4060 Laptop 8 GB，Qwen3-8B Q3_K_M，`ngl 37` 全驻留 + `q8_0 KV`，每档 30 请求 × 32 token × **5 轮**，真实 token 口径）：

| 并发 P | TTFT p50 (ms) | ITL p99 (ms) | 总 token 吞吐 (tok/s) | 相对 P1 | 调度 CPU 占比 |
| --- | --- | --- | --- | --- | --- |
| 1 | 28.5 | 23.3 | 77.1 | 1.00× | 0.0002 |
| 2 | 82.6 | 67.7 | 62.0 | 0.80× | 0.0001 |
| 4 | 207.1 | 108.1 | 78.2 | 1.01× | 0.0001 |
| 8 | 320.6 | 171.3 | 80.7 | 1.05× | 0.0001 |

**结论：全驻留档下 continuous batching 的吞吐收益被证伪**（P8/P1 仅 1.05×，判据要求 ≥1.3×），代价是 ITL p99 放大 7.4×。归因是"单执行线程串行 + 显存带宽封顶"：调度步 p50 仅 4–8 µs、`sched_cpu_ratio` 0.01–0.02%，控制面不是瓶颈；77–81 tok/s 已接近该卡有效带宽上限。吞吐在 P=4 到平台，再往上只增加排队与尾延迟。旧表（`ngl 24`、13/37 层留 CPU 时的 1.61× 与 12.8 tok/s 单路）是 CPU-offload 天花板下的相对改善，**已作废**，只作历史对照。证据、分档显存分项与复现命令见 `evidence/phase12/perf/README.md`。

**这条结论的边界**：负载是等长 32-token 短解码、prompt 单档、P=1/2/4/8，且没有"静态批"对照。它证明的是"把并发从 1 拉到 8 不再涨吞吐、只涨尾延迟"，不能推出"continuous batching 本身没有收益"——等长输出恰好抹掉了槽位复用，而没有静态批基线就无法把这 1.05× 精确归因到"补位"这一机制。混长输出、static/continuous 对照、前缀缓存开/关三组配置已落在 `scripts/bench/matrix/phase12_s3_*.json`，跑出来之前不要引用更宽的结论。



- **局限：**只能做到初步的生产级别部署 ，暂时存在SLA级别性能和冗余差距 ：

无法任何 99.9% 及以上的可用性——没有第二份算力；

并发 > 4 的交互式延迟承诺——P8 的 ITL p99 实测已越线；

目前上下文 > 1024 / 8B 以上——容量判据会直接判 BLOCKED；

“长 prompt 低延迟”——8B 全驻留时显存峰值已到 7.5 GB / 8.19 GB。



### 容量与 SLO



- SLO 目标（写死，只对单实例、单卡、8B Q3_K_M、`ngl 37`、ctx 1024、P≤8 成立）：

| 指标 | 目标 | 判定口径 |
| --- | --- | --- |
| TTFT p50 | ≤ 350 ms | 连续批、短 prompt 档，P≤8 |
| ITL p99 | ≤ 200 ms | 同上；超过即该并发档不可用于交互式场景 |
| 可用性 | ≥ 99.5% / 30 天 | systemd `Restart=on-failure` + 外部探针（D2-11） |
| 服务端错误率 | ≤ 0.1% | 只算 5xx，4xx 客户端错误不计 |

容量表由 `python scripts/bench/capacity_table.py --evidence evidence/phase13` 从 D2-03 的 `matrix_report.csv` 生成，不手写数字：

| 组 | P | TTFT p50 (ms) | ITL p99 (ms) | 总 token 吞吐 (tok/s) | VRAM 峰值 (MiB) | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| perf-static | 1 | 29.4 | 23.2 | 62.0 | 4384 |  |
| perf-static | 2 | 744.5 | 23.4 | 62.2 | 4461 |  |
| perf-static | 4 | 2160.3 | 70.2 | 56.0 | 4614 |  |
| perf-static | 8 | 6970.0 | 142.1 | 57.1 | 4920 | high† |
| perf-mixed | 1 | 29.8 | 23.3 | 61.0 | 4384 |  |
| perf-mixed | 2 | 70.9 | 51.7 | 51.5 | 4461 | high† |
| perf-mixed | 4 | 168.3 | 112.0 | 61.2 | 4614 | high† |
| perf-mixed | 8 | 307.2 | 208.9 | 62.4 | 4920 | high† |
| perf-prefix-on | 1 | 34.4 | 22.7 | 265.5 | 4384 | insufficient† |
| perf-prefix-on | 4 | 349.1 | 71.8 | 281.0 | 4614 | insufficient† |
| perf-prefix-on | 8 | 654.5 | 134.1 | 332.6 | 4920 | insufficient† |
| perf-prefix-off | 1 | 102.1 | 23.2 | 242.4 | 4078 |  |
| perf-prefix-off | 4 | 411.1 | 73.3 | 290.4 | 4308 | high† |
| perf-prefix-off | 8 | 792.3 | 119.1 | 317.1 | 4614 | high† |



判读：

- **交互式容量上限 P=4**：P=8 的 ITL p99 208.9 ms 已越过 200 ms 目标，TTFT 仍达标；P=4 的 ITL p99 112 ms、TTFT 168 ms 都在目标内，是当前单卡可承诺的档位。
- **static 批的 P2/P4 TTFT 爆炸**（744 ms / 2160 ms）是"整批跑空才补位"的必然代价；continuous 在 P=8 把 TTFT 压到 307 ms，补位净收益成立，但吞吐只从 57.1 涨到 62.4 tok/s（+9%），**没有 1.3×**，因此不翻案吞吐结论。
- **前缀缓存的收益在 TTFT**：384-token 共享前缀下 P1 从 102 ms 降到 34 ms（3×）、P8 从 792 ms 降到 654 ms；吞吐 P1 +9.5%（242.4 → 265.5 tok/s），P4 基本持平，P8 +5%。代价是 P8 的 ITL p99 从 119 ms 升到 134 ms（仍在 200 ms 内）。首轮 prefix_on 曾因 `seq_cp` 位置重叠崩溃，已修复并保留回归用例，详见 `evidence/phase13/README.md`。
- `deviation_flag=high` 表示该项目按固定阈值判为偏差（不是失败），用于提示"该点与相邻档差异过大，引用时要看原始 `deviation.csv`"；`insufficient` 表示该组按当前阈值样本不足，只作趋势参考。
- 27B 吞吐、ctx>1024、P>8、多实例 + 网关一律不在本期结论内（见文末边界）。







## 演进规划



- 功能演进与能力保证为主

1. **chunk 调参验证**：SPEC-5 判据把当前数据判为 `candidate_pending_chunk`，需要在同一硬件上补调参后矩阵，才可能从候选转为触发。
2. **基线稳定性**：重测阶段 0 基线，缩小轮间漂移；GPU 路径目前只透传 `n-gpu-layers`，未做显存水位下的调度策略。
3. **预研立项门槛**：PD 分离与投机解码官方 14 行复算结果为 0 触发 / 12 候选 / 2 不可判，结论是“本期数据不支持翻案”；要立项必须先有新矩阵与链路实测。
4. **部署面补齐**：S5 已完成容器镜像、六项冒烟、logrotate 与 systemd 单元校验；真后端镜像（llama.cpp 运行库）与慢读退化复测仍待排期。



## 部署说明

最小部署面是「一个二进制 + 一个模型文件 + 一份 JSON 配置」。



### 容器

```bash
docker build -f scripts/release/Dockerfile -t llmopt:s5 .
docker run --rm -p 8080:8080 -e LLMOPT_ALLOW_STUB=1 \
  -v "$PWD/models:/app/models:ro" llmopt:s5 \
  --model /app/models/qwen3-8b.gguf --host 0.0.0.0 --port 8080 \
  --control-port 8081 --backend stub --mode off
```

镜像默认 `LLMOPT_ENABLE_LLAMA=OFF`（只带 stub）；真后端需构建时打开开关并准备 llama.cpp。模型不进镜像，运行时挂载。多阶段构建、runtime 非 root（uid 10001）、实测 83 MB；`bash scripts/release/docker_smoke.sh` 六项全 PASS（healthz / readyz / 非流式 / SSE / 断连取消 / `docker stop` 退出码 0），证据在 `evidence/phase12/packaging/`。



### systemd

`scripts/release/llmopt.service` 是模板：`ExecStart` 读 `/etc/llmopt/config.json`，`WorkingDirectory=/var/lib/llmopt` 固定崩溃 dump 与相对日志锚点，`KillSignal=SIGTERM`，`TimeoutStopSec=45`，比默认 30 s drain 多留余量；日志按 20 MB × 5 轮转，部署层再用 `scripts/release/logrotate/llmopt` 兜底。`systemd-analyze verify` 已通过。









## License

[Apache License 2.0](LICENSE)
