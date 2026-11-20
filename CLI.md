# CLI 与接口契约（阶段 1 冻结版 v1.1 / 阶段 3 chunk-size 补充）

> 状态：阶段 1 参数已实现并冻结；阶段 3 新增 `--chunk-size`，2026-11-20 经用户确认纳入冻结契约，其余不变。对应 SPEC-1（C-01…C-13）与 SPEC-3 F-04。
> 与 SPEC 冲突处已记录在 `implementation-notes.md`。适用进程：`llmopt_server`。`tools/dev.py` 参数命名独立，见第 8 节。



## 1 入口模式

| 模式 | 命令 | 用途 | 后端要求 |
| --- | --- | --- | --- |
| 服务进程 | `llmopt_server` | 常驻 HTTP/SSE 推理服务 | `--backend llama` 或 `stub` |
| 一次性自检 | `llmopt_server --model <gguf> --port 0` 之外的任意端口 + `/healthz` | 部署前探活 | 同上 |
| 开发工具 | `python tools/dev.py <子命令>` | 构建、测试、诊断 | 不涉及推理 |

本阶段不提供 `serve/bench/replay` 子命令；服务只有一个入口，避免两套参数。



## 2 启动参数

```bash
llmopt_server --model <gguf> [--host 127.0.0.1] [--port 8080] \
              [--ctx-tokens 2048] [--max-concurrency 8] \
              [--max-num-batched-tokens 2048] [--queue-capacity 32] \
              [--block-size 16] [--watermark-ratio 0.05] [--disable-prefix-cache] \
              [--chunk-size 128] \
              [--mode off|auto|force_on] [--safety-ratio 0.80] [--allow-force-on] \
              [--threads 0] [--n-gpu-layers 0] [--backend llama|stub] \
              [--log-level info|debug|warn]
```



| 参数 | 环境变量 | 默认 | 取值/校验 | 说明 |
| --- | --- | --- | --- | --- |
| `--model` | `LLMOPT_MODEL` | 无（必填） | 存在的 GGUF 文件 | 只读元数据做预算，不在此处加载权重 |
| `--host` | `LLMOPT_HOST` | `127.0.0.1` | 任意可绑定地址 | 默认只本机 |
| `--port` | `LLMOPT_PORT` | `8080` | 1–65535 | 占用时退出 3 |
| `--ctx-tokens` | `LLMOPT_CTX_TOKENS` | `2048` | 128–131072 | 同时决定 KV 估算与上下文拒绝阈值 |
| `--max-concurrency` | `LLMOPT_MAX_CONCURRENCY` | `8` | 1–64 | 连续批调度的运行序列上限，超出的请求进 waiting 队列 |
| `--max-num-batched-tokens` | `LLMOPT_MAX_NUM_BATCHED_TOKENS` | `2048` | 1–8192，且 ≥ `max-concurrency` | 单步调度 token 预算；chunked prefill 后允许小于 `ctx-tokens`，长 prompt 按 chunk 推进 |
| `--queue-capacity` | `LLMOPT_QUEUE_CAPACITY` | `32` | 1–1024 | waiting 队列上限，满即 429 |
| `--block-size` | `LLMOPT_BLOCK_SIZE` | `16` | 4–128 | KV 逻辑块 token 数，同时决定前缀命中粒度 |
| `--watermark-ratio` | `LLMOPT_WATERMARK_RATIO` | `0.05` | 0–0.2 | 准入预留的空闲块比例，按块池总量折算 |
| `--chunk-size` | `LLMOPT_CHUNK_SIZE` | `128` | 仅 `32`/`64`/`128`/`256` | chunked prefill 的单步宽度；末块才采样，中间块只推进 KV |
| `--disable-prefix-cache` | `LLMOPT_DISABLE_PREFIX_CACHE` | `false` | 布尔开关，不带值 | 关闭满块哈希与前缀复用；后端不支持零拷贝共享时也会自动关闭 |
| `--mode` | `LLMOPT_MODE` | `off` | `off`/`auto`/`force_on` | 门控三档，见第 5 节 |
| `--safety-ratio` | `LLMOPT_SAFETY_RATIO` | `0.80` | 0.80–0.85 | 软预算系数 |
| `--allow-force-on` | `LLMOPT_ALLOW_FORCE_ON` | `false` | 布尔开关，不带值 | `force_on` 的二次确认 |
| `--threads` | `LLMOPT_THREADS` | `0` | ≥0 | 0 交给 llama.cpp 决定 |
| `--n-gpu-layers` | `LLMOPT_N_GPU_LAYERS` | `0` | ≥0 | 0 为纯 CPU |
| `--backend` | `LLMOPT_BACKEND` | `llama` | `llama`/`stub` | `stub` 需 `LLMOPT_ALLOW_STUB=1` |
| `--log-level` | `LLMOPT_LOG_LEVEL` | `info` | spdlog 级别 | 跑基准建议 `warn` |

阶段 3 起支持 chunked prefill，`ctx-tokens` 不再受单步预算限制；`chunk-size` 取四档，其他值启动即拒绝。调度顺序固定为：在跑 decode 保位 → 在跑 prefill 的下一 chunk → 预算内补新请求，合计 token 不超过 `max-num-batched-tokens`。

优先级固定：**CLI > `LLMOPT_*` > 内置默认**。启动第一行打印生效配置，第三行打印预算与门控结论，便于复核。

被拒绝的参数：

- `--config`：阶段 2 才引入，当前按未知参数处理（退出 2）。
- `--seed` / `--max-tokens`：属于请求级字段，不做全局默认，避免“服务默认值”与“请求默认值”两套事实。
- 重复参数：后者覆盖前者，不报错。



## 3 HTTP 契约

| 方法 | 路径 | 行为 | 备注 |
| --- | --- | --- | --- |
| POST | `/v1/completions` | 文本补全；`stream=true` 走 SSE | 主端点 |
| POST | `/completion` | 与上者同一解析路径 | 兼容 llama.cpp 客户端 |
| POST | `/v1/chat/completions` | 恒 404 `NOT_SUPPORTED` | 阶段 1 不做 chat 模板 |
| GET | `/healthz` | 启动中/就绪 200，停止中 503 | 返回 `{"status":"ready"}` |
| GET | `/metrics` | Prometheus 文本，仅 counter/gauge | 启动即预置 0 值序列 |

请求体（`application/json`，上限 4 MB）：

| 字段 | 类型 | 默认 | 约束 |
| --- | --- | --- | --- |
| `prompt` | string | 必填 | 非空；分词后不能为空 |
| `id` | string | 自动 `req-<hex>` | 回显在同一响应的 `id` 中 |
| `stream` | bool | `false` | 非布尔即 400 |
| `model` | string | `""` | 仅回显，不校验 |
| `max_tokens` / `n_predict` | int | `128` | 1–4096，两者同名同义 |
| `top_k` | int | `40` | 0–256，0 表示关闭 |
| `temperature` | number | `0.7` | 0.0–2.0 |
| `top_p` | number | `0.95` | 0.0–1.0 |
| `min_p` | number | `0.05` | 0.0–1.0 |
| `repeat_penalty` | number | `1.0` | 0.5–2.0 |
| `seed` | int | `-1` | -1 为随机；否则 0–1e9 |

采样链固定为：**惩罚 → top-k → top-p → min-p → 温度 → dist**。顺序写死在 `runner_llama.cpp`，请求不能改顺序。

上下文判定：`tokenize(prompt).size() + max_tokens > ctx_tokens` 即 400 `CONTEXT_OVERFLOW`。

流式帧：

```text
data: {"choices":[{"finish_reason":null,"index":0,"text":"A","token_id":323}],"id":"req-1"}

data: [DONE]

```

- 增量帧每帧一个 token，`text` 是该 token 的 detokenize 结果。
- `id` 取请求里的 `req_id`；调用方必须保证同一进程内 `req_id` 唯一，重试请换新 id，否则取消与收尾可能命中错误请求。
- 末帧 `finish_reason` 取 `stop` / `length` / `abort` / `error`，随后补 `data: [DONE]`。
- `data: [DONE]\n\n` 实测 14 字节；SPEC-1 C-02 写 15 字节是笔误，以本文与实现为准。

非流式响应：

```json
{"choices":[{"finish_reason":"length","index":0,"text":"..."}],"id":"req-1",
 "object":"text_completion",
 "usage":{"completion_tokens":3,"prompt_tokens":5,"total_tokens":8}}
```

错误响应统一为 `{"error":{"code":"QUEUE_FULL","message":"...","retryable":true}}`，HTTP 映射见第 4 节。



## 4 错误码与退出码

| ErrCode | HTTP | code | 可重试 |
| --- | --- | --- | --- |
| `InvalidJson` | 400 | `INVALID_JSON` | 否 |
| `ModelNotLoaded` | 503 | `MODEL_NOT_LOADED` | 是 |
| `PromptTooLong` | 400 | `PROMPT_TOO_LONG` | 否 |
| `ContextOverflow` | 400 | `CONTEXT_OVERFLOW` | 否 |
| `KvInsufficient` | 429 | `KV_INSUFFICIENT` | 是 |
| `Cancelled` | 499 | `REQUEST_CANCELLED` | 否 |
| `ExecutionFailed` | 500 | `EXECUTION_FAILED` | 否 |
| `QueueFull` | 429 | `QUEUE_FULL` | 是 |
| `NotSupported` | 404 | `NOT_SUPPORTED` | 否 |
| `ResourceInsufficient` | 507 | `RESOURCE_INSUFFICIENT` | 否 |

进程退出码：

| 码 | 含义 | 触发 |
| --- | --- | --- |
| 0 | 正常退出 | SIGINT/SIGTERM 后 HTTP 停止、调度排空 |
| 1 | 启动失败 | 模型文件不可读/元数据缺失、取值非法、后端未编译 |
| 2 | 用法错误 | 缺 `--model`、未知参数、缺少取值 |
| 3 | 资源不足 | `required > free`，或端口被占用 |
| 4 | 门控拒绝 | `auto` 软超预算、`force_on` 未带开关、快照字段缺失 |



## 5 门控与预算

1. L1 探测：RAM 总量/可用、GPU 型号与空闲显存、逻辑核数、是否 WSL。
2. L2 估算：`required = GGUF 文件字节 + KV 估算`；KV = `block_count × ctx × head_count_kv × (key_len + value_len) × 2`。
3. L3 判据：有 GPU 时用显存空闲值，否则用 RAM 可用值。
   - `required ≤ free × safety_ratio`：`ELIGIBLE`。
   - `free × safety_ratio < required ≤ free`：`DEGRADED`。`off` 放行；`auto` 拒绝（退出 4）；`force_on` 需 `--allow-force-on`，放行时并发减半并打印横幅。
   - `required > free`：`BLOCKED`，任何模式都退出 3。
   - 快照/元数据缺字段：`BLOCKED`，退出 4。



## 6 配置加载机制（Profile）

阶段 1 **不读配置文件**，没有 profile 概念；配置只有 CLI 与环境变量两个来源。阶段 2 引入 `--config` 时的约定：

- JSON 顶层为平铺键，与参数名同名去 `--` 前缀，例如 `{"port": 8080, "ctx_tokens": 2048}`。
- 优先级插入在 CLI 与环境变量之间：CLI > profile > env > 默认。
- 生效来源在启动摘要中逐项标注，禁止静默回退。



## 7 插件与依赖

- 推理后端不是运行期插件，而是编译期开关：`-DLLMOPT_ENABLE_LLAMA=ON` 编入真实 llama.cpp，`OFF` 只留 stub。
- 第三方依赖由 CMake FetchContent 拉到 `third_party/_deps`（不入库），版本见 `cmake/deps.cmake`。
- 无动态加载、无插件目录扫描；避免运行期 ABI 风险。



## 8 环境变量、权限与资源上限

| 变量 | 作用 | 默认 |
| --- | --- | --- |
| `LLMOPT_*` | 见第 2 节 | 见第 2 节 |
| `LLMOPT_ALLOW_STUB=1` | 允许 stub 后端，供测试与 CI | 未设置即拒绝 |
| `LLMOPT_LLAMA_ROOT` | CMake 查 llama.cpp 头/库的根 | `/root/llm-src/llama.cpp` |

- 默认只绑 `127.0.0.1`，无鉴权、无 TLS；对外暴露必须由反向代理承担认证。
- 请求体上限 4 MB；读超时 5 s；写超时 120 s；单请求非流式排空上限 120 s。
- 队列容量 32，满即 429；等待中的请求只占分词后的 token 数组，不占 KV。



## 9 可直接执行的示例

安装/构建（Windows，stub 档）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

真实后端（WSL/Linux，需先编好 llama.cpp）：

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release \
  -DLLMOPT_ENABLE_LLAMA=ON -DLLMOPT_LLAMA_ROOT=/root/llm-src/llama.cpp
cmake --build build-wsl --parallel
```

启动（真实模型，CPU）：

```bash
LLMOPT_LLAMA_ROOT=/root/llm-src/llama.cpp \
./build-wsl/llmopt_server --model /root/llm-src/models/qwen2.5-0.5b-q4.gguf \
  --port 8080 --ctx-tokens 2048 --max-num-batched-tokens 512 --chunk-size 64 \
  --mode auto --threads 8 --log-level warn
```

启动（stub 自检，无模型权重也能跑通路由）：

```powershell
python tests\fixtures\models\make_fake_gguf.py tests\fixtures\models\fake.gguf
$env:LLMOPT_ALLOW_STUB="1"
.\build\llmopt_server.exe --model tests\fixtures\models\fake.gguf --backend stub `
  --port 18080 --ctx-tokens 512 --mode off --log-level warn
```

流式调用：

```bash
curl -N -X POST http://127.0.0.1:8080/completion \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"用一句话解释 KV cache","stream":true,"max_tokens":64}'
```

非流式 + 脚本化取文本：

```bash
curl -s -X POST http://127.0.0.1:8080/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"hello","max_tokens":8}' | jq -r '.choices[0].text'
```

探活与指标：

```bash
curl -s http://127.0.0.1:8080/healthz
curl -s http://127.0.0.1:8080/metrics | grep llmopt_requests_total
```

脚本化压测（重用阶段 0 采集器，指向本服务时只需改端口与路径）：

```bash
python -m tools.bench.collect --config tools/bench/config/phase0_smoke.json \
  --out tools/bench/results/phase1/smoke
```



## 10 已与用户核对并冻结的契约点

1. 阶段 0+1 共用整数分支 `1`，下一阶段从 `2` 开始。
2. 采样链顺序见第 3 节。
3. 阶段 1 只做 CLI，`--config` 推迟到阶段 2。
4. `/completion` 保留，`/v1/chat/completions` 恒 404。
5. 双档后端：`LLMOPT_ENABLE_LLAMA=ON` 才编真实后端；Windows/CI 用 stub。
6. 退出码 3/4 语义见第 4 节。
7. 阶段 2 调度参数默认值：`max-num-batched-tokens=2048`、`queue-capacity=32`、`block-size=16`、`watermark-ratio=0.05`、前缀缓存默认开。
8. 阶段 3 新增 `--chunk-size`，默认 128，仅 32/64/128/256；`max-num-batched-tokens` 可小于 `ctx-tokens`（chunked prefill 承接）。
9. 分支约定：阶段 2 用 `2`，阶段 3 用 `3`；每阶段结束需用户 review 后才允许推送。
