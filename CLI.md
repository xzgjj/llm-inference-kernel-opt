# CLI 与接口契约（阶段 1 冻结版 v1.1 / 阶段 3 chunk-size / 阶段 5 预研工具）

> 状态：阶段 1 参数已实现并冻结；阶段 3 新增 `--chunk-size`，2026-11-20 经用户确认纳入冻结契约，其余不变。阶段 5 只新增只读判据工具，不改服务 CLI。对应 SPEC-1（C-01…C-13）、SPEC-3 F-04、SPEC-5 §1.7。
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
llmopt_server [--config <json>] --model <gguf> [--host 127.0.0.1] [--port 8080] \
              [--ctx-tokens 2048] [--max-concurrency 8] \
              [--max-num-batched-tokens 2048] [--queue-capacity 32] \
              [--block-size 16] [--watermark-ratio 0.05] [--disable-prefix-cache] \
              [--chunk-size 128] \
              [--mode off|auto|force_on] [--safety-ratio 0.80] [--allow-force-on] \
              [--threads 0] [--n-gpu-layers 0] [--backend llama|stub] \
              [--rate-limit-rps 0] [--rate-limit-burst 32] \
              [--drain-deadline-seconds 30] [--api-key ""] [--step-trace-rate 0] \
              [--log-level info|debug|warn]
```



| 参数 | 环境变量 | 默认 | 取值/校验 | 说明 |
| --- | --- | --- | --- | --- |
| `--config` | `LLMOPT_CONFIG` | 空 | 可读 JSON 文件 | 文件层参数；未知键/越界/坏 JSON 启动即退出 2 |
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
| `--fairness-aging` | `LLMOPT_FAIRNESS_AGING` | `false` | 布尔开关，不带值 | 开启等待老化豁免：无进展超阈值的请求不再被选为 victim |
| `--aging-threshold-ms` | `LLMOPT_AGING_THRESHOLD_MS` | `30000` | 1–3600000 | 老化阈值（毫秒），仅 `--fairness-aging` 开启后生效 |
| `--max-consecutive-preemptions` | `LLMOPT_MAX_CONSECUTIVE_PREEMPTIONS` | `3` | 1–32 | 同一请求连续被抢占上限；达到即转 `Error`，防饿死 |
| `--preempt-trace` | `LLMOPT_PREEMPT_TRACE` | 空 | 可写路径 | 每次抢占写一行 CSV：victim/块数/重算 token/路径/代价 |
| `--step-trace` | `LLMOPT_STEP_TRACE` | 空 | 可写路径 | 每个调度 step 写一行 JSONL（ts/steps/tokens/prefill/decode/ns）；步级 p99 只离线复算，不写回 `/metrics` |
| `--disable-prefix-cache` | `LLMOPT_DISABLE_PREFIX_CACHE` | `false` | 布尔开关，不带值 | 关闭满块哈希与前缀复用；后端不支持零拷贝共享时也会自动关闭 |
| `--mode` | `LLMOPT_MODE` | `off` | `off`/`auto`/`force_on` | 门控三档，见第 5 节 |
| `--safety-ratio` | `LLMOPT_SAFETY_RATIO` | `0.80` | 0.80–0.85 | 软预算系数 |
| `--allow-force-on` | `LLMOPT_ALLOW_FORCE_ON` | `false` | 布尔开关，不带值 | `force_on` 的二次确认 |
| `--threads` | `LLMOPT_THREADS` | `0` | ≥0 | 0 交给 llama.cpp 决定 |
| `--n-gpu-layers` | `LLMOPT_N_GPU_LAYERS` | `0` | ≥0 | 0 为纯 CPU |
| `--backend` | `LLMOPT_BACKEND` | `llama` | `llama`/`stub` | `stub` 需 `LLMOPT_ALLOW_STUB=1` |
| `--log-level` | `LLMOPT_LOG_LEVEL` | `info` | spdlog 级别 | 跑基准建议 `warn` |
| `--rate-limit-rps` | `LLMOPT_RATE_LIMIT_RPS` | `0` | 0–1e6 | 全局令牌桶速率；0=关，>0 时超额回 429（行为已接入） |
| `--rate-limit-burst` | `LLMOPT_RATE_LIMIT_BURST` | `32` | 1–1024 | 桶容量；突发额度，回填不超过该值 |
| `--drain-deadline-seconds` | `LLMOPT_DRAIN_DEADLINE_SECONDS` | `30` | 1–300 | SIGTERM 排空上限（行为在 7-3 接入） |
| `--api-key` | `LLMOPT_API_KEY` | 空 | ≤512 字符 | 可选单 token Bearer；空=不鉴权，不打印值 |
| `--step-trace-rate` | `LLMOPT_STEP_TRACE_RATE` | `0` | 0–64 | 每步 trace 1/N 采样；0=关（行为在 7-5 接入） |

阶段 3 起支持 chunked prefill，`ctx-tokens` 不再受单步预算限制；`chunk-size` 取四档，其他值启动即拒绝。调度顺序固定为：在跑 decode 保位 → 在跑 prefill 的下一 chunk → 预算内补新请求，合计 token 不超过 `max-num-batched-tokens`。

优先级固定：**CLI > `LLMOPT_*` > `--config` 文件 > 内置默认**。启动第一行打印生效配置，随后逐行打印 `config source: <键>=cli|env|file|default`，第三行打印预算与门控结论，便于复核。

配置文件键与 CLI 同名，允许 kebab-case 或 snake_case（`ctx-tokens` / `ctx_tokens` 等价）；未知键、类型不符、越界与坏 JSON 一律退出 2，不静默取默认。`api-key` 可来自文件或 env，但启动摘要只打印 `set/unset`。

被拒绝的参数：

- `--seed` / `--max-tokens`：属于请求级字段，不做全局默认，避免“服务默认值”与“请求默认值”两套事实。
- 重复参数：后者覆盖前者，不报错。



## 3 HTTP 契约

| 方法 | 路径 | 行为 | 备注 |
| --- | --- | --- | --- |
| POST | `/v1/completions` | 文本补全；`stream=true` 走 SSE | 主端点 |
| POST | `/completion` | 与上者同一解析路径 | 兼容 llama.cpp 客户端 |
| POST | `/v1/chat/completions` | `messages` → 模型 chat template → 同一条调度路径 | 仅纯文本；流式 `delta.content` |
| GET | `/v1/models` | 返回 `{"object":"list","data":[{"id":...}]}` | id 取模型文件名去扩展名 |
| GET | `/healthz` | 存活探针，始终 200，body 带当前状态 | `{"status":"ready"}` |
| GET | `/readyz` | 就绪探针，仅 `ready` 返回 200；启动中/排空/停止一律 503 | 6-G3 |
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
| `stop` | string / string[] | 无 | 最多 4 条，每条 1–32 字符；命中即截断并按 `stop` 收尾 |
| `n` | int | `1` | 1–4；仅非流式，`stream=true` 且 `n>1` 返回 400 |

chat 端点额外约束：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `messages` | array | 非空；`role` 仅 `system`/`user`/`assistant` |
| `messages[].content` | string / array | 数组仅接受 `{"type":"text","text":...}` 分片 |

`tools` / `tool_choice` / `functions` / `logprobs` / `logit_bias` / `response_format` / 图片分片等兼容面之外的字段返回 400 `unsupported_field`（D6-1），不静默忽略。模板读不到时回退 `role: content\n` 拼接并记 warn。

采样链固定为：**惩罚 → top-k → top-p → min-p → 温度 → dist**。顺序写死在 `runner_llama.cpp`，请求不能改顺序。

上下文判定：`tokenize(prompt).size() + max_tokens > ctx_tokens` 即 400 `CONTEXT_OVERFLOW`。

流式帧：

```text
data: {"choices":[{"finish_reason":null,"index":0,"text":"A","token_id":323}],"id":"req-1","object":"text_completion"}

data: [DONE]

```

chat 流式帧的结构是 `object=chat.completion.chunk`，文本放在 `choices[].delta.content`；两条路径都以 `data: [DONE]` 结束。

- 增量帧每帧一个 token，`text` 是该 token 的 detokenize 结果。
- `id` 取请求里的 `req_id`；调用方必须保证同一进程内 `req_id` 唯一，重试请换新 id，否则取消与收尾可能命中错误请求。
- `X-Request-Id` 头优先于请求体 `id`，并写入该请求的日志行。
- 末帧 `finish_reason` 取 `stop` / `length` / `abort` / `error`，随后补 `data: [DONE]`。
- `stop` 跨帧命中时先下发截断前的文本，再补一帧 `finish_reason=stop` 并取消后续生成。
- `data: [DONE]\n\n` 实测 14 字节；SPEC-1 C-02 写 15 字节是笔误，以本文与实现为准。

非流式响应（`n>1` 时 `choices` 按 `index` 排列，`usage` 汇总全部候选）：

```json
{"choices":[{"finish_reason":"length","index":0,"text":"..."}],"id":"req-1",
 "object":"text_completion",
 "usage":{"completion_tokens":3,"prompt_tokens":5,"total_tokens":8}}
```

chat 非流式用 `object=chat.completion` 与 `choices[].message={"role":"assistant","content":...}`；`usage` 口径与文本端点一致。

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
| `UnsupportedField` | 400 | `unsupported_field` | 否 |
| `Unavailable` | 503 | `SERVICE_UNAVAILABLE` | 是 |
| `RateLimited` | 429 | `RATE_LIMITED` | 是 |

限流语义（6-G4）：`--rate-limit-rps > 0` 时先过令牌桶再解析；超额回 429 `RATE_LIMITED` + `Retry-After: 1`，不进队列、不占调度预算，并累加 `llmopt_rate_limited_total`。

排空语义（6-G3）：SIGTERM/SIGINT 后状态转 `draining`，`/readyz` 与新请求立即 503；在飞请求在 `--drain-deadline-seconds` 内自然收尾，超时记 `llmopt_drain_timeout_total` 并强制停止，进程退出码仍为 0。`/healthz` 始终 200，body 的 `status` 反映 `starting|ready|draining|stopped`。

进程退出码：

| 码 | 含义 | 触发 |
| --- | --- | --- |
| 0 | 正常退出 | SIGINT/SIGTERM 后 HTTP 停止、调度排空 |
| 1 | 启动失败 | 模型文件不可读/元数据缺失、取值非法、后端未编译 |
| 2 | 用法错误 | 缺 `--model`、未知参数、缺少取值、`--config` 未知键/类型不符/越界/坏 JSON |
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

阶段 6 起支持 `--config`，没有命名 profile/合并多个文件的概念；一次只读一个平铺 JSON。

- 优先级固定：**CLI > `LLMOPT_*` > file > 默认**（R-07）。文件层先于 env 应用；CLI 最后覆盖。
- JSON 顶层为平铺键，与参数名同名去 `--` 前缀，kebab 与 snake 均可：`{"port": 8080, "ctx_tokens": 2048}`。
- 启动摘要逐项打印来源：`config source: port=cli` 等；未出现的字段为 `default`。
- 未知键、类型不符、越界、坏 JSON 一律退出 2（R-08）；即使后续 CLI 能覆盖也不静默放行。
- `--config` 的路径本身可来自 CLI 或 `LLMOPT_CONFIG`；两者同时存在时 CLI 决定读哪个文件。
- `api-key` 只打印 `set/unset`，不打印值；日志同样不落明文。



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

- 默认只绑 `127.0.0.1`；`--api-key` 只做单 token Bearer 预留（默认关，校验行为待用户确认边界后接入），不做多租户/配额。对外暴露仍建议反向代理承担 TLS 与更强认证。
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



## 11 预研判据工具（阶段 5，非服务 CLI）

`scripts/spec5/recompute_spec5.py` 只读 SPEC-4 产物，不启服务、不写 KV、不执行 PD/投机路径。它没有环境变量、没有 profile、没有插件；权限与普通脚本相同（读输入目录、写 `--out`）。

| 子命令 | 语法 | 作用 |
| --- | --- | --- |
| （无子命令） | `python scripts/spec5/recompute_spec5.py --csv <file> [options]` | 复算判据表，输出 `criteria.csv` / `criteria.md`，可选评估报告 |

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--csv` | 必填 | `matrix_report.csv`，SPEC-4 十二列；缺列即退出 1 |
| `--traces` | CSV 同目录 | `steps-p<N>.jsonl` 所在目录；缺档记 `trace_missing` |
| `--chunk-csv` | 无 | 同 P 同档的调参后十二列报告；缺档时 PD 停在候选 |
| `--spec-csv` | 无 | 草稿模型实测行（`baseline_tok_s`/`draft_tok_s`/`alpha`/`k`/`cost_ratio`/`itl_p99_delta_pct`） |
| `--out` | `<csv 目录>/spec5` | 输出目录 |
| `--report` | 仅触发时写 | 评估报告路径；未触发也可显式写一页“不支持翻案” |
| `--slo-itl-ms` | `50` | D5-1 SLO |
| `--share-threshold` | `0.50` | prefill 占比阈值 |
| `--chunk-gain-threshold` | `0.10` | chunk 调参最小改善 |
| `--spec-gain-threshold` | `0.10` | 投机最小增益 |
| `--spec-itl-tolerance` | `0.07` | 投机 ITL p99 允许恶化 |
| `--min-samples` | `30` | 少于此记 `数据不足` |
| `--kv-kib-per-token` | `12` | B_kv 估算系数 |
| `--ctx-tokens` | `4096` | 迁移估算上下文 |
| `--link-gibps` | `0` | 链路实测带宽；0 表示未测，不给 PD 结论 |
| `--delta-stall-ms` | 无 | 混批−隔离 ITL 差；缺省不给 PD 结论 |
| `--route-ms` / `--overlap-ms` | `0` | 路由与重叠开销 |
| `--quiet` | 关 | 不打印摘要 |

退出码：`0` 计算完成（含数据不足/不触发）、`1` 输入不可用（路径/缺列）、`2` 参数用法错误。判定值固定为 `pd_triggered` / `candidate_pending_chunk` / `candidate_pending_migration` / `negative_gain` / `batch_decay` / `not_triggered` / `data_insufficient`。

**典型用法**

```bash
# 仓库自带的官方 14 行样例（hermetic）
python scripts/spec5/recompute_spec5.py \
  --csv tests/data/spec5/matrix_report.csv --traces tests/data/spec5 \
  --out /tmp/spec5

# 真机矩阵复算，并落一页评估报告
python scripts/spec5/recompute_spec5.py \
  --csv docs/evidence/phase4-official/matrix_report.csv \
  --traces docs/evidence/phase4-official \
  --out docs/evidence/phase5 --report docs/notes/phase5_预研评估.md

# 有调参后矩阵时，才可能从候选变成触发
python scripts/spec5/recompute_spec5.py --csv base.csv --chunk-csv tuned.csv \
  --link-gibps 10 --delta-stall-ms 8 --out out/
```

脚本化调用只看退出码与 `criteria.csv` 的 `row_verdict` 列，不要解析人读的 `criteria.md`。
