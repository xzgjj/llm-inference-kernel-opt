# llm-inference-kernel-opt

**单机、单进程的 C++20 大模型推理服务实验床**

[简体中文](README.md) ｜ [English](README.en.md)



## 项目简介

本项目和 llama 相关，主要围绕 C++ 大模型推理服务：目前 C++ 大模型主要有以下方向，我结合能力和设备情况选择尝试服务调度。

> - 偏算子 / 后端：ggml 后端插件化：自定义 Backend 与算子加速
> - **偏服务调度：C++ 大模型推理服务：Continuous Batching 与请求调度**
>- 偏算法加速：LLM 投机解码落地与端到端加速评估
> - 偏低比特：低比特量化下的推理精度 — 吞吐权衡（INT4/FP8）

计算面不自己写 GEMM：模型加载、图构建与 kernel 执行直接复用 ggml / llama.cpp；自研部分全部落在控制面——准入、组批、KV 块表、chunked prefill、抢占与恢复、容量门控、指标打点。要回答的问题很具体：在这台机器上，continuous batching 与 chunked prefill 到底带来多少收益，控制面的开销在哪里、边界在哪。

仓库名里的 `kernel` 指**服务内核**（调度与 KV 管理），不是 CUDA kernel；本项目不做自定义算子，也不做分布式与训练。规模限定在单机、单进程、0.5B–27B 量化模型，本机实测档为 Qwen3-8B Q4_K_M。

当前阶段 0–6 与阶段质量整改 A 已落地：162 条用例全绿（Windows stub 档 + WSL 真后端各一轮），Linux/WSL 侧真实 llama.cpp 后端以 Qwen3-8B Q4_K_M（L1 证据档）跑通非流式与流式推理，Qwen2.5-0.5B 退居 CI 冒烟档。它可以作为学习 continuous batching 的可运行参考实现，也可以当作 llama.cpp 之上的最小 OpenAI 兼容服务骨架。



## 核心功能

1. **Continuous Batching**：按 token 级组批，运行序列与等待队列分离；每步受 `max-num-batched-tokens` 预算约束，达上限的请求进 waiting 队列，队列满返回 429。
2. **块表 KV 管理**：定长块（默认 16 token）+ 引用计数 + 满块哈希 + CoW。块表是 KV 分配的唯一决策者，后端 cell 只作下游映射，不做反向回写。
3. **前缀复用**：相同前缀的满块共享逻辑块与引用计数，可关闭（`--disable-prefix-cache`）；物理 KV 仍由后端 `seq_cp` 复制或目标槽预留，不宣称物理零拷贝。
4. **Chunked Prefill**：长 prompt 按 `--chunk-size`（32/64/128/256）切块，与 decode 批次混排；只有末块采样，中间块只推进 KV。
5. **抢占与恢复**：显存/块不足时按代价模型选 victim，重算恢复；支持等待老化豁免、同请求连续抢占上限，抢占事件可写 CSV 回溯。
6. **最小 OpenAI 兼容面**：`/v1/completions`、`/completion`、`/v1/chat/completions`、`/v1/models`；流式走 SSE，逐 token 下发，以 `data: [DONE]` 收尾。
7. **容量门控**：启动前探测 RAM / GPU 空闲显存，按 `required ≤ free × safety_ratio` 给出 `ELIGIBLE / DEGRADED / BLOCKED` 三态，`off / auto / force_on` 三档决策，默认 `off`；`n_gpu_layers` 小于层数时按**显存/内存双池**分别判据（8B 在 8GB 卡上靠它才起得来）。
8. **可运维性**：CLI > env > JSON 配置 > 默认 四层优先级；`/healthz`、`/readyz`；SIGTERM 排空；令牌桶限流；可选单 token Bearer 鉴权；Prometheus 文本指标（仅 counter/gauge）。
9. **实验与证据链**：`tools/bench` 负责请求生成、SSE 采集、聚合与绘图，结果按 `phaseN/runs/<run_id>/` 归档，CSV / JSONL / 日志可复现。



## 技术栈

| 类别 | 选型 |
| --- | --- |
| 语言标准 | C++20：`std::span` / `std::optional` / `std::atomic` / RAII / `enum class`；热路径不用 modules、协程、ranges |
| 构建 | CMake ≥ 3.20 + FetchContent（依赖只拉到 `third_party/_deps`，不入库）；可选 Ninja |
| 推理后端 | ggml + llama.cpp（commit `b94959a`），编译期开关 `LLMOPT_ENABLE_LLAMA`；默认 stub 后端供 CI 与路由自检 |
| HTTP / SSE | cpp-httplib 0.18.3（单头，关闭 OpenSSL / zlib / brotli） |
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



### 1. 构建与测试（stub 后端，无需模型权重）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build -L fast --output-on-failure
```

产物为 `build/llmopt_server`（Windows 下为 `build/llmopt_server.exe`）。



### 2. stub 自检（跑通 HTTP → 调度 → SSE）

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



### 3. 真实后端（Linux / WSL，需先编好 llama.cpp）

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



### 4. 开发工具单入口

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
| `LLMOPT_QUEUE_CAPACITY` | `32` | waiting 队列上限，满即 429 `QUEUE_FULL` |
| `LLMOPT_BLOCK_SIZE` | `16` | KV 逻辑块 token 数，也决定前缀命中粒度 |
| `LLMOPT_CHUNK_SIZE` | `128` | chunked prefill 单步宽度，仅 32 / 64 / 128 / 256 |
| `LLMOPT_MODE` / `LLMOPT_SAFETY_RATIO` | `off` / `0.80` | 容量门控模式与软预算系数 |
| `LLMOPT_BACKEND` | `llama` | `llama` 或 `stub`；stub 需 `LLMOPT_ALLOW_STUB=1` |
| `LLMOPT_API_KEY` | 空 | 单 token Bearer 鉴权，空表示不鉴权，日志与启动摘要只打印 set/unset |
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
├─ docs/                 # 规划、SPEC-0…6、CLI 契约、测试注册表、论文调研、性能证据
├─ cmake/                # FetchContent 依赖版本与 llama.cpp 接入
├─ ci/                   # 闸门脚本自测（不计入产品覆盖率）
└─ .github/workflows/    # ci.yml（L0–L4 闸门）/ nightly.yml（TSan 10 轮、ASan/UBSan、30 min soak）
```

目录约束：顶层只保留源码、测试、脚本、文档与根级配置；深度 ≤ 3，不建 `application/domain/infrastructure` 之类抽象层。源码目标固定为 `http protocol tokenizer gate runner sampler sched seq kv metrics main`。



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



### 错误与探针

错误统一为 `{"error":{"code":"QUEUE_FULL","message":"...","retryable":true}}`。

| 场景 | HTTP | code |
| --- | --- | --- |
| 队列满 | 429 | `QUEUE_FULL` |
| 限流 | 429 | `RATE_LIMITED` |
| KV 低于水位 | 429 | `KV_BELOW_WATERMARK` |
| 上下文溢出 | 400 | `CONTEXT_OVERFLOW` |
| 鉴权失败 | 401 | `UNAUTHORIZED` |
| KV 或资源不足 | 507 | `KV_INSUFFICIENT` / `RESOURCE_INSUFFICIENT` |
| 执行失败 | 500 | `EXECUTION_FAILED` |

```bash
curl -s http://127.0.0.1:8080/healthz   # 存活，始终 200，body 带 starting|ready|draining|stopped
curl -s http://127.0.0.1:8080/readyz    # 就绪，仅 ready 返回 200，其余 503
curl -s http://127.0.0.1:8080/metrics | grep llmopt_requests_total
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

`/metrics` 只输出 counter / gauge；p50 / p95 / p99 由基准工具离线复算，避免在线直方图与自研后端口径冲突。



## 当前水平与局限

已完成的验证面：阶段 0 基线采集、阶段 1 串行闭环、阶段 2 连续批处理与 KV 复用、阶段 3 chunked prefill 与抢占、阶段 4 指标与实验矩阵、阶段 6 服务完成度；阶段 5 只做预研判据，不落实现。Windows 本机 `ctest -L fast` 162 条（107 unit / 42 contract / 13 integration）全绿；Linux 侧 ASan / UBSan 无报告，TSan 10 轮 0 race，30 分钟 soak 343,736 请求 / 0 非 200 / RSS +0.25 MiB。

官方 8B 矩阵（WSL Ubuntu 26.04，RTX 4060 Laptop 8 GB，Qwen3-8B Q4_K_M，每档 30 请求 × 32 token × 2 轮，`ngl 24 / threads 8`）：

| 并发 P | TTFT p50 (ms) | ITL p99 (ms) | 总吞吐 (tok/s) | 相对 P=1 | 两轮偏差 |
| --- | --- | --- | --- | --- | --- |
| 1 | 229.5 | 245.6 | 40.96 | 1.00× | ≤7% |
| 2 | 434.4 | 291.0 | 50.28 | 1.23× | ≤7% |
| 4 | 711.7 | 407.1 | 65.94 | 1.61× | ≤5% |

同为 P=4 的调优对照：`ngl 28` 把 TTFT 压到 347.8 ms；`threads 12` 再降到 339 ms、ITL p99 从 393 ms 砍到 230 ms；`ngl 30 + q8_0 KV + donor 0`（前缀关闭）拿到当前最优 **31.07 tok/s 纯 decode、TTFT 339 ms、ITL p99 210 ms**。

怎么读这组数：P=4 吞吐 1.61×（含 prefill token）过 1.5× 门槛，纯 decode 23.3→31.1 tok/s；两轮偏差 ≤7%，复现性达标。ITL p99 随并发放大（245→407 ms）是 13/37 层留在 CPU 的必然结果——8B Q4 在这张 8GB 卡上无法全量驻留（`-ngl 99` 需 5370 MiB，桌面常驻后仅剩约 4996 MiB，被判 BLOCKED）。0.5B 的旧矩阵保留在 `docs/evidence/` 作历史对照。

局限，直说：

- 单机单进程，实测覆盖 0.5B–8B 量化模型；27B 档只做容量与抢占验证，不出吞吐结论。
- 真实 llama.cpp 后端只在 Linux / WSL 编译与验证；Windows 侧以 stub 后端与路由测试为主，本机没有 Docker 与 SIGTERM 语义。
- llama.cpp 的 `llama_batch` 没有 slot 字段，cell 由内部 `find_slot` 选择且无公开 getter，块表与 cell 的对账依赖内部头文件，属于**源码耦合点**，升级 llama.cpp 必须复核。
- 兼容面是最小集：不做多租户、配额、WebUI、OpenAI 全量字段；鉴权只有一个 Bearer token。
- `/metrics` 不含直方图，p99 需离线复算；慢读退化与真后端容器冒烟仍缺真机复测。
- 基准噪声偏大：同一档两轮的 TTFT / 吞吐漂移经常越过 10%，暂不适合做精细调参依据。



## 后续演进规划

按现有 SPEC 与证据推进，不新开架构方向：

1. **质量整改 B / C**：阶段 A 已收掉请求身份、流式 stop、token 预算、drain 镜像、抢占预判与错误面；B/C 项（长稳、边界语义、文档与契约对齐）待排期。
2. **chunk 调参验证**：SPEC-5 判据把当前数据判为 `candidate_pending_chunk`，需要在同一硬件上补调参后矩阵，才可能从候选转为触发。
3. **基线稳定性**：重测阶段 0 基线，缩小轮间漂移；GPU 路径目前只透传 `n-gpu-layers`，未做显存水位下的调度策略。
4. **预研立项门槛**：PD 分离与投机解码官方 14 行复算结果为 0 触发 / 12 候选 / 2 不可判，结论是“本期数据不支持翻案”；要立项必须先有新矩阵与链路实测。
5. **部署面补齐**：`docker_smoke.sh`、真后端慢读退化、systemd 排空需在具备 Docker 与 GGUF 模型的机器上复测，不接受“环境跑不了”当通过。

## 部署说明

最小部署面是「一个二进制 + 一个模型文件 + 一份 JSON 配置」。

### 容器

```bash
docker build -f scripts/release/Dockerfile -t llmopt:dev .
docker run --rm -p 8080:8080 -e LLMOPT_ALLOW_STUB=1 \
  -v "$PWD/models:/app/models:ro" llmopt:dev \
  --model /app/models/qwen2.5-0.5b-q4.gguf --host 0.0.0.0 --port 8080 \
  --backend stub --mode off
```

镜像默认 `LLMOPT_ENABLE_LLAMA=OFF`（只带 stub）；真后端需构建时打开开关并准备 llama.cpp。模型不进镜像，运行时挂载。

### systemd

`scripts/release/llmopt.service` 是模板：`ExecStart` 读 `/etc/llmopt/config.json`，`KillSignal=SIGTERM`，`TimeoutStopSec=45`，比默认 30 s drain 多留余量；日志按 20 MB × 5 轮转。

### 健康检查与排空

- 就绪探针用 `/readyz`，存活探针用 `/healthz`；
- SIGTERM / SIGINT 后状态转 `draining`，`/readyz` 与新请求立即 503，在飞请求在 `--drain-deadline-seconds` 内收尾，超时计数后强制停止，退出码仍为 0。

### 夜间作业

`.github/workflows/nightly.yml` 定时跑：TSan fast 套件 10 轮（0 race 才通过）、ASan/UBSan 一轮、`scripts/release/soak.py` 30 分钟持续请求（无非 200、RSS 增长受限）。CI 闸门分 L0 预提交（10 s）、L1 每次 push（3 min）、L2 PR 构建与覆盖率（15 min）、L3 sanitizer（30 min）、L4 nightly（2 h）。

## License

[Apache License 2.0](LICENSE)
