# CLI 与接口契约专册（v0.13.0）

> 状态：**D2-14 专册，已按用户确认的四项契约（入口模式命名、命令清单与退出码语义、key 文件 `tokens_per_hour`/`tokens_per_day`、`--slow-request-ms`）定稿**。
> 唯一事实来源：`src/config.cpp` 的键注册表（`--help` 与本文由同一份表渲染，`unit_config` 有断言守住两者一致）。改参数必须同时改注册表、本文与 `docs/SPEC-DEBUG2.md`。
> 适用进程：`llmopt_server`（服务本体）与 `tools/dev.py`（开发工具链）。两者参数命名各自独立，不要混用。

## 1 入口模式

| 模式 | 命令 | 用途 | 退出方式 |
| --- | --- | --- | --- |
| 服务模式 | `llmopt_server --model <gguf> ...` | 常驻 HTTP/SSE 推理服务 | SIGTERM/SIGINT 排空后 0 |
| 一次性容量模式 | `llmopt_server --model-profile <json> --capacity-only` | 权重未落盘时只算预算，不监听端口 | 0 可启动 / 3 硬阻塞 / 4 软超预算被拒 |
| 信息模式 | `llmopt_server --version`、`llmopt_server --help` | 版本与全部参数清单（由键注册表渲染） | 0 |
| 工具模式 | `python tools/dev.py <子命令>` | 构建、测试、诊断、按 req_id 回放 | 见第 2 节 |

没有 `serve` / `bench` / `replay` 子命令：服务只有一个二进制入口，避免两套参数各自演化。

## 2 命令列表

| 命令 | 语法 | 说明 | 退出码 |
| --- | --- | --- | --- |
| 启动服务 | `llmopt_server --model <gguf> [参数]` | 加载模型并监听数据面端口 | 0 正常 / 1 启动失败 / 2 用法错误 / 3 资源不足 / 4 门控拒绝 |
| 打印版本 | `llmopt_server --version` | 输出 `llmopt <ver> (llama revision <rev>)` | 0 |
| 打印用法 | `llmopt_server --help` | 键注册表逐项列出（开关会标 `(开关)`） | 0 |
| 容量试算 | `llmopt_server --model-profile <json> --capacity-only` | 只打印 `estimate`/`decision`/`capacity-only: allow_start` | 0 / 3 / 4 |
| 构建 | `python tools/dev.py build [--target X]` | 包装 CMake+Ninja，带进度与 ETA | 0 成功 / 非 0 编译失败 |
| 测试 | `python tools/dev.py test [-L fast]` | ctest 包装，失败汇总到 junit | 0 全绿 / 1 失败 |
| 诊断 | `python tools/dev.py diagnose` | 扫描日志，输出「现象/原因/下一步命令」 | 0 |
| 环境体检 | `python tools/dev.py env` | 工具链/GPU/内存探测，写 `build/env.json` | 0 |
| 同步清单 | `python tools/dev.py sync` | 校验依赖清单与目录骨架 | 0 / 非 0 |
| 启动并跟随 | `python tools/dev.py run [--attach]` | 起服务；`--attach` 轮询 `/healthz` 并跟随日志 | 0 / 2（无二进制） |
| 慢请求回放 | `python tools/dev.py trace <req_id> [--dir D] [--json]` | 读 `logs/slow/<req_id>.jsonl`，打印四段耗时 | 0 找到 / 2 找不到 / 3 参数错 |
| 调试脚本 | `python tools/dev.py debug [--pid N]` | 生成 gdb 脚本；Windows 无 gdb 时给 WSL 命令 | 0 / 2 |
| 清理 | `python tools/dev.py clean [--yes]` | 默认 dry-run，`--yes` 才删，models/vendor 排除 | 0 |

退出码语义（服务进程）：`1`=模型不可读/后端未编译；`2`=用法错误（缺 `--model`、未知参数、缺取值、`--config` 未知键/坏 JSON/越界）；`3`=`required > free`、端口被占、TLS 配置不可用；`4`=门控拒绝（`auto` 软超预算、`force_on` 未带 `--allow-force-on`、快照缺字段）。

## 3 子命令与参数

### 3.1 服务参数（按用途分组）

启动与规模：

| 参数 | 默认 | 合法范围 | 说明 |
| --- | --- | --- | --- |
| `--model` | 必填 | 存在的 GGUF | 只读元数据做预算，不在此加载权重 |
| `--model-profile` | 空 | JSON，6 个正整数键 | 权重不在本机时声明元数据；只能与 `--capacity-only` 搭配 |
| `--capacity-only` | 关（开关） | — | 打印预算结论后退出，不监听 |
| `--host` | `127.0.0.1` | 可绑定地址 | 默认只听本机 |
| `--port` | `8080` | 1–65535 | 被占用退出 3 |
| `--ctx-tokens` | `2048` | 128–131072 | 单序列上下文，同时是拒绝阈值 |
| `--max-concurrency` | `8` | 1–64 | running 序列上限 |
| `--queue-capacity` | `32` | 1–1024，且 `max-concurrency+queue-capacity+3 ≤ 256` | waiting 上限，满即 429 `QUEUE_FULL` |
| `--max-num-batched-tokens` | `2048` | 1–8192，且 ≥ `max-concurrency` | 单步 token 预算，可小于 `ctx-tokens` |
| `--chunk-size` | `128` | 仅 32/64/128/256 | chunked prefill 宽度 |

调度与 KV：

| 参数 | 默认 | 合法范围 | 说明 |
| --- | --- | --- | --- |
| `--batch-policy` | `continuous` | `continuous`/`static` | `static` 仅用于对照实验 |
| `--block-size` | `16` | 4–128 | KV 逻辑块 token 数，也是前缀命中粒度 |
| `--watermark-ratio` | `0.05` | 0–0.2 | 准入预留空闲块比例 |
| `--disable-prefix-cache` | 关（开关） | — | 关闭满块哈希与前缀复用 |
| `--donor-slots` | `4` | ≥0 | 前缀复用 donor 槽数；仅 GPU 层数与前缀复用都开启时生效 |
| `--kv-cache-type` | `f16` | `f16`/`q8_0`/`f32` | 预算按 2 / ≈1.0625 / 4 B 折算 |
| `--fairness-aging` | 关（开关） | — | 无进展超阈值的请求不再被抢占 |
| `--aging-threshold-ms` | `30000` | 1–3600000 | 老化阈值 |
| `--max-consecutive-preemptions` | `3` | 1–32 | 连续被抢占上限，达到转 `Error` |
| `--preempt-trace` | 空 | 可写路径 | 每次抢占写一行 CSV |

时间与保护：

| 参数 | 默认 | 合法范围 | 说明 |
| --- | --- | --- | --- |
| `--request-deadline-seconds` | `120` | 1–3600 | 请求总时限，到点由调度以 `TIMEOUT` 收尾 |
| `--completion-timeout-ms` | `30000` | 0–86400000 | 单步后端回投超时；0=关 |
| `--drain-deadline-seconds` | `30` | 1–300 | SIGTERM 排空上限 |
| `--rate-limit-rps` | `0` | 0–1e6 | 全局令牌桶，0=关；超额 429 `RATE_LIMITED` |
| `--rate-limit-burst` | `32` | 1–1024 | 桶容量 |

后端与门控：

| 参数 | 默认 | 合法范围 | 说明 |
| --- | --- | --- | --- |
| `--backend` | `llama` | `llama`/`stub` | `stub` 需 `LLMOPT_ALLOW_STUB=1` |
| `--threads` | `0` | 0–256 | 0 交给 llama.cpp |
| `--n-gpu-layers` | `0` | ≥0 | 0=纯 CPU，负数退出 2 |
| `--mode` | `off` | `off`/`auto`/`force_on` | 预算门控三档 |
| `--safety-ratio` | `0.80` | 0.80–0.95 | 软预算系数 |
| `--allow-force-on` | 关（开关） | — | `force_on` 的二次确认 |

鉴权、安全与可观测：

| 参数 | 默认 | 合法范围 | 说明 |
| --- | --- | --- | --- |
| `--api-key` | 空 | ≤512 字符 | 单 token Bearer；与 `--api-keys-file` 互斥 |
| `--api-keys-file` | 空 | JSON 路径 | 多租户 key 文件，只存 SHA-256，POSIX 权限必须 0600 |
| `--api-keys-reload-seconds` | `0` | ≥0 | Windows 无 SIGHUP 时按秒轮询热加载 |
| `--slow-request-ms` | `0` | 0–600000 | 慢请求阈值；>0 时命中即强制落 dump |
| `--slow-dump-dir` | `logs/slow` | 可写目录 | dump 目录，单文件 8 MB、保留 20 份 |
| `--step-trace` | 空 | 可写路径 | 每 step 一行 JSONL |
| `--step-trace-rate` | `0` | 0–64 | 每 N 步采样；0=关 |
| `--log-level` | `info` | spdlog 级别 | 压测建议 `warn` |
| `--control-port` | `0` | 0–65535，≠`--port` | >0 时另起 loopback 控制面 |
| `--tls-cert` / `--tls-key` | 空 | PEM 路径，必须成对 | 需 `-DLLMOPT_ENABLE_TLS=ON`，否则退出 3 |

`--config` 单独属于文件层，见第 4 节。被拒绝的参数：`--seed` / `--max-tokens`（请求级字段不做服务默认）；重复参数后者覆盖前者，不报错。

### 3.2 请求字段（约束摘要）

| 字段 | 默认 | 约束 |
| --- | --- | --- |
| `prompt` | 必填 | 非空，分词后不能为空 |
| `id` | 自动 `req-<hex>` | 1–64，仅 `[A-Za-z0-9._:-]`；活跃期重复即 400 |
| `stream` | `false` | 非布尔即 400 |
| `max_tokens` / `n_predict` | `128` | 1–4096 |
| `temperature` / `top_p` / `min_p` / `repeat_penalty` / `top_k` / `seed` | `0.7` / `0.95` / `0.05` / `1.0` / `40` / `-1` | 见附录 A |
| `stop` | 无 | 最多 4 条，每条 1–32 字符 |
| `n` | `1` | 1–4；`stream=true` 且 `n>1` 返回 400 `UNSUPPORTED_FIELD` |

## 4 Profile 与配置加载

这里的 "Profile" 有两层含义，不要混：

1. **配置分层**（R-07）：`CLI > LLMOPT_* 环境变量 > --config 文件 > 内置默认`。文件先于 env 应用，CLI 最后覆盖；启动第一行打印生效配置，随后逐项打印 `config source: <键>=cli|env|file|default`，可直接复核每个值从哪来。
2. **模型容量 Profile**（`--model-profile`）：一份 JSON 声明 `file_bytes / block_count / head_count / head_count_kv / key_length / value_length`，在权重未落盘时替代 GGUF 元数据做预算；只允许与 `--capacity-only` 同时使用。

配置文件的规则：

- 顶层平铺键，与 CLI 同名去 `--`；kebab 与 snake 等价（`ctx-tokens` ≡ `ctx_tokens`）。
- 未知键、类型不符、越界、坏 JSON 一律退出 2，即使后面 CLI 能覆盖也不静默放行。
- 文件 ≤ 16 MiB，超限按配置错误退出 2。
- `api-key` 只打印 `set/unset`；`api-keys-file` 只打印来源，不打印路径内容与 key。

## 5 插件与后端管理

- 推理后端是**编译期开关**，不是运行期插件：`-DLLMOPT_ENABLE_LLAMA=ON` 编入真实 llama.cpp（固定 tag `b12557`），`OFF` 只保留 stub。
- 没有动态库加载、没有插件目录扫描，避免运行期 ABI 与版本漂移；想换后端就是重新构建 + 切 `--backend`。
- 第三方依赖由 CMake FetchContent 拉到 `third_party/_deps`（不入库），版本清单在 `cmake/deps.cmake`；`tools/dev.py sync` 校验清单与实际目录一致。
- 发布镜像同口径：`scripts/release/Dockerfile` 的 `--build-arg LLMOPT_ENABLE_LLAMA=ON` 在 builder 内编 llama.cpp 并只把 `libllama*`/`libggml*` 拷进 runtime（`LD_LIBRARY_PATH=/app/lib`），runtime 仍是非 root uid 10001。

## 6 环境变量与权限控制

| 变量 | 作用 | 默认 |
| --- | --- | --- |
| `LLMOPT_<参数名大写、连字符变下划线>` | 与 CLI 同名同义，如 `LLMOPT_CTX_TOKENS`、`LLMOPT_SLOW_REQUEST_MS`、`LLMOPT_SLOW_DUMP_DIR` | 见第 3 节 |
| `LLMOPT_CONFIG` | 配置文件路径（CLI `--config` 优先） | 空 |
| `LLMOPT_ALLOW_STUB=1` | 允许 stub 后端（测试/CI 用） | 未设置即拒绝 |
| `LLMOPT_LLAMA_ROOT` | CMake 查 llama.cpp 头/库的根 | 由构建参数传入 |

权限边界：

- 默认只绑 `127.0.0.1`；对公网必须前置反向代理承担 TLS/mTLS。
- 单 token 模式 `--api-key`；多租户用 `--api-keys-file`：`version:1` + `keys[]`，每项 `id`（`[a-z0-9-]{1,32}`）/`sha256`（64 hex）/`quota`。文件不存明文 key，POSIX 权限必须 0600，否则退出 3（Windows 跳过权限检查）。
- `quota` 字段：`max_concurrency`、`queue_capacity`、`rate_limit_rps`、`rate_limit_burst`、`kv_blocks`，以及 **`tokens_per_hour` / `tokens_per_day`**（整数 ≥0，0 或省略=不限；只计 completion tokens，按自然小时/日固定窗口滚动；超限 429 `QUOTA_EXCEEDED` + `Retry-After`）。未知键仍拒绝。
- 控制面 `--control-port>0` 只绑 loopback；不要把控制端口暴露到公网。
- 日志与 dump：不打 key 明文，`/healthz`、`/readyz`、`/metrics` 免鉴权供探针抓取。

## 7 可直接复制的示例

安装（Windows，stub 档，无需 GPU）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
python tools/dev.py test -L fast
```

安装（Linux/WSL，真后端）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLMOPT_ENABLE_LLAMA=ON -DLLMOPT_LLAMA_ROOT=/root/llm-src/llama.cpp
cmake --build build --parallel --target llmopt_server
./build/llmopt_server --version
# 真后端构建期望：llmopt 0.13.0 (llama revision b12557)
# stub-only 构建显示 llama revision unknown —— 这是宏的合法取值，不是配置错误
```

初始化（生成配置与 stub 自检）：

```powershell
python tests\fixtures\models\make_fake_gguf.py tests\fixtures\models\fake.gguf
$env:LLMOPT_ALLOW_STUB = "1"
.\build\llmopt_server.exe --model tests\fixtures\models\fake.gguf --backend stub `
  --port 18080 --ctx-tokens 512 --mode off --log-level warn
# 另一终端：curl -s http://127.0.0.1:18080/readyz
```

日常操作（真实模型 + 流式调用 + 探活）：

```bash
LLMOPT_LLAMA_ROOT=/root/llm-src/llama.cpp ./build/llmopt_server \
  --model /root/models/Qwen3-8B-Q3_K_M.gguf --ctx-tokens 1024 \
  --max-num-batched-tokens 1024 --queue-capacity 32 \
  --n-gpu-layers 37 --threads 0 --mode auto --log-level warn \
  --control-port 8081 --slow-request-ms 1000

curl -N -X POST http://127.0.0.1:8080/completion \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"用一句话解释 KV cache","stream":true,"max_tokens":64}'

curl -s http://127.0.0.1:8081/readyz
curl -s http://127.0.0.1:8081/metrics | grep -E 'llmopt_latency_(ttft|itl)_ms_count'
```

脚本化调用（非流式取文本 + 失败即非零）：

```bash
set -euo pipefail
text=$(curl -sf -X POST http://127.0.0.1:8080/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"prompt":"hello","max_tokens":8}' | jq -r '.choices[0].text')
[ -n "$text" ] && echo "ok: $text"
```

脚本化排障（慢请求 → 按 req_id 回放）：

```bash
curl -s -X POST http://127.0.0.1:8080/v1/completions \
  -H 'Content-Type: application/json' \
  -d '{"id":"req-demo-1","prompt":"长文本...","max_tokens":128}'
python tools/dev.py trace req-demo-1 --dir logs/slow --json
# 退出 0=有 dump；2=没有该 req_id 的 dump
```

租户 token 预算（key 文件片段 + 超限验证）：

```json
{"version":1,"keys":[{"id":"tenant-a","sha256":"<64 hex>",
  "quota":{"max_concurrency":2,"queue_capacity":4,"tokens_per_hour":10000,"tokens_per_day":100000}}]}
```

```bash
LLMOPT_ALLOW_STUB=1 ./build/llmopt_server --model fake.gguf --backend stub --port 18080 \
  --api-keys-file keys.json
# 超额时：HTTP 429，body {"error":{"code":"QUOTA_EXCEEDED",...}}，头带 Retry-After
```

容器（stub / 真后端两档）：

```bash
docker build -f scripts/release/Dockerfile -t llmopt:0.13.0 .
docker build -f scripts/release/Dockerfile --build-arg LLMOPT_ENABLE_LLAMA=ON -t llmopt:0.13.0-llama .
docker run --rm llmopt:0.13.0 --version
```

## 附录 A 接口契约与错误面

端点：`POST /v1/completions`、`POST /completion`（同解析路径）、`POST /v1/chat/completions`（纯文本消息）、`GET /v1/models`、`GET /healthz`、`GET /readyz`、`GET /metrics`。请求体上限 4 MB；读超时 5 s；写超时 120 s。

流式帧：每 token 一帧增量，末帧 `finish_reason` 取 `stop|length|abort|error`，随后补 `data: [DONE]`。chat 帧的 `object` 是 `chat.completion.chunk`，文本在 `choices[].delta.content`。

错误响应统一为 `{"error":{"code":"...","message":"...","retryable":true|false}}`，映射以 `src/common.cpp` 为准：

| ErrCode | HTTP | code | 可重试 |
| --- | --- | --- | --- |
| InvalidJson / ConfigInvalid / PromptTooLong / ContextOverflow / UnsupportedField | 400 | `INVALID_JSON` 等 | 否 |
| Unauthorized | 401 | `UNAUTHORIZED` | 否 |
| NotSupported | 404 | `NOT_SUPPORTED` | 否 |
| PayloadTooLarge | 413 | `PAYLOAD_TOO_LARGE` | 否 |
| QueueFull / RateLimited / QuotaExceeded / KvBelowWatermark | 429 | `QUEUE_FULL` 等 | 是 |
| Cancelled | 499 | `REQUEST_CANCELLED` | 否 |
| ExecutionFailed | 500 | `EXECUTION_FAILED` | 否 |
| ModelNotLoaded / Unavailable | 503 | `MODEL_NOT_LOADED` / `SERVICE_UNAVAILABLE` | 是 |
| Timeout | 504 | `TIMEOUT` | 是 |
| KvInsufficient / ResourceInsufficient | 507 | `KV_INSUFFICIENT` / `RESOURCE_INSUFFICIENT` | 否 |

429 与 503 的分界：**429 = 排队位满**（进了准入预算）；**503 = 连接连排队位都没进**（传输层 shed，带 `Retry-After: 1`，不读 body、不占调度预算，`/healthz`、`/readyz`、`/metrics` 仍放行）。

完整错误面语义见 `docs/错误面规范.md`，逐条回归用例见 `docs/testing.md`。
