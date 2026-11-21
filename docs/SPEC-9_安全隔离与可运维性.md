# SPEC-9：安全隔离与可运维性

- 状态：TLS、控制面隔离、Completion watchdog、真后端 nightly 与多租户静态 key/配额已实现；断连取消、长稳仍为规划
- 分支：`9`（从 `8` @ `9037f61` 切出）
- 关联调研：`docs/notes/阶段9-安全隔离调研.md`
- 原则：默认行为不变；能力以开关/配置启用；每项先有可失败测试；不在同一提交混多个结构改造

## 1 目标与非目标

### 1.1 目标

1. 数据面可选 TLS，至少覆盖 `/healthz`、推理端点、`/metrics`。
2. 控制面可独立监听与独立线程池，慢连接/长流不得饿死探针。
3. 明确单 token 与多租户的差距，给出可实施的鉴权/配额契约。
4. 补非流式断连取消、Completion watchdog、真后端夜间回归、长稳压测四项可运维能力。

### 1.2 非目标

- 不做分布式推理、不做多模型同进程、不做训练/微调。
- 不做完整企业级 IAM（OIDC/SAML/LDAP）；多租户只做“静态 key → tenant + 配额”的最小模型。
- 不在本阶段做 mTLS、证书热轮换、OCSP stapling；公网部署建议前置 Nginx/Envoy。
- 不把控制面改成 gRPC 或独立进程；仍是一个进程内两个 listener。

## 2 现状与差距

| 能力 | 阶段 8 状态 | 阶段 9 状态 | 差距/风险 |
| --- | --- | --- | --- |
| 数据面 TLS | 无；httplib 关闭 OpenSSL | 可选 `SSLServer`，`-DLLMOPT_ENABLE_TLS=ON` | 证书轮换、mTLS、OCSP 未做 |
| 控制面隔离 | 与推理共享一个池，多留 3 线程 | `--control-port` 另起 loopback listener + 独立 `ThreadPool(4)` | 默认仍关闭；跨机控制面需代理 |
| 鉴权 | 单 Bearer token，空=不鉴权 | 阶段 11：静态 key 文件（SHA-256）+ 租户配额 + KV 域隔离；`--api-key` 兼容 | token 日配额、热加载未做 |
| 非流式断连 | 客户端断开后继续生成到 120 s 时限 | 不变 | 浪费算力，可被低成本放大 |
| Completion 丢失 | 无 watchdog | 不变 | Executor 卡死时 shutdown 永久等待 |
| 真后端 CI | 只编译 `runner_llama.cpp` | 不变 | 运行时回归盲区 |
| 长稳/soak | 手工/特定环境 | 不变 | 无 30 min 级内存/KV/延迟证据 |

## 3 契约

### 3.1 TLS 数据面（已实现）

**构建**

- `LLMOPT_ENABLE_TLS=OFF`（默认）：不链接 OpenSSL，Windows/MSVC 可构建；配置 `--tls-cert/--tls-key` 时启动退出 3。
- `LLMOPT_ENABLE_TLS=ON`：`find_package(OpenSSL REQUIRED)`，`httplib.h` 定义 `CPPHTTPLIB_OPENSSL_SUPPORT`，数据面使用 `httplib::SSLServer`。

**配置**

| 键 | 环境变量 | 默认 | 规则 |
| --- | --- | --- | --- |
| `tls-cert` | `LLMOPT_TLS_CERT` | 空 | PEM 证书链路径；与 `tls-key` 必须成对 |
| `tls-key` | `LLMOPT_TLS_KEY` | 空 | PEM 私钥路径；与 `tls-cert` 必须成对 |

**行为**

- 配置 TLS 后，数据面所有路由 scheme 变为 `https`；SSE、错误体、`/metrics` 内容形状不变。
- 证书/私钥无效或不可读：启动失败，退出码 3，日志只打印路径，不打印密钥内容。
- 同一端口不接受明文 HTTP 200；握手失败由 httplib 关闭连接。
- 控制面保持 loopback 明文；如控制面跨机，必须在代理层终止 TLS。

**验收**

- `integration_server_tls`：HTTPS `/healthz` 200、`/completion` 200 且含 `choices`、`/metrics` 200 且 `text/plain`；明文同端口不得 200。
- `integration_server_tls_config_error`：非 TLS 构建配置证书退出 3 且信息含“未启用 TLS”；TLS 构建证书不可用同样退出 3，错误信息标明证书/私钥无效。

### 3.2 控制面隔离（已实现）

| 键 | 环境变量 | 默认 | 规则 |
| --- | --- | --- | --- |
| `control-port` | `LLMOPT_CONTROL_PORT` | `0` | `0` 关闭；`1..65535` 且不得等于数据面 `port` |

**行为**

- `control-port=0`：保持旧行为，`/healthz`、`/readyz`、`/metrics` 仍在数据面端口。
- `control-port>0`：在 `127.0.0.1:<control-port>` 另起 listener；独立 `ThreadPool(4)`；只注册三个只读端点；不挂推理端点、不鉴权。
- 数据面仍保留三个控制端点，兼容旧探针/脚本。
- 两个 listener 共享 `ServiceController`；SIGTERM 时同时 stop。
- 控制 listener 绑定失败：启动退出 3。

**验收**

- `integration_server_control_isolation`：`max-concurrency=1`、4 条长 SSE 流占满数据面 `1+3` handler；独立控制面 2 s 内返回 `/healthz` 200、`/metrics` 200。

### 3.3 鉴权与多租户（已实现，阶段 11）

目标模型：静态 key 文件，只存 SHA-256；`--api-key` 保留为单租户 `default` 的简写。

```json
{
  "version": 1,
  "keys": [
    {"id": "tenant-a",
     "sha256": "3d3133c6e9b0220392a81a0b8bda5f91872ae26e244401cc7ac4d9616fd533f3",
     "quota": {"max_concurrency": 1, "queue_capacity": 1,
               "rate_limit_rps": 0.5, "rate_limit_burst": 1, "kv_blocks": 0}},
    {"id": "tenant-b",
     "sha256": "1a256545f370bc6ba136a7eebbdb5b137bf172ea46a4ff3baafae875d42ae3e9",
     "quota": {"max_concurrency": 2, "queue_capacity": 2}}
  ]
}
```

契约：

- `--api-keys-file` 指向 0600 文件（POSIX；Windows 跳过权限检查）；启动时读取，缺失/坏 JSON/未知字段/重复 id 或 sha/越界一律退出 3；不打印 key 与摘要。
- 客户端 key 为 `lk_` + 32 hex，只出现在 `Authorization: Bearer <key>`；服务端做 SHA-256 后与文件内摘要定长比较，未知 key 回 401 `UNAUTHORIZED`。
- 每 key 配额 `max_concurrency` / `queue_capacity` / `rate_limit_rps` / `rate_limit_burst` / `kv_blocks`；live 准入上限为 `max_concurrency + queue_capacity`，超限 429 `QUOTA_EXCEEDED`；单请求 KV 需求或租户总量超 `kv_blocks` 同样 429；租户限流 429 `RATE_LIMITED`。
- 调度器按 tenant 记录 live/running，单 tenant 的 `max_concurrency` 不得超过全局；租户 KV 只做块级标记与配额计量，不拆物理池。
- KV 前缀哈希混入租户域 salt，跨租户同前缀不命中；空租户保持旧行为。
- `/metrics` 增加 `llmopt_tenant_requests_total`、`llmopt_tenant_inflight`、`llmopt_tenant_queue_depth`、`llmopt_tenant_kv_used_blocks`、`llmopt_tenant_quota_rejected_total`，标签只取配置内 `id`，不把 key 作为标签。
- 管理面（增删 key）不在本阶段；先用文件 + 重启。token 日配额暂缓，`kv_blocks` 与并发/队列/限流先落地。

### 3.4 非流式断连取消（规划，未实现）

现状：非流式请求在 handler 内等待完成；客户端断开后 handler 仍持有线程直到响应写回失败/120 s 请求时限。

方案 A（推荐）：在 `ResponseSink` 增加 `client_aborted()` 探测：

- 流式：SSE 写失败即 `core.cancel(req_id)`（已有部分路径）。
- 非流式：httplib 提供 `req.is_connection_closed()` 时，在等待循环每 100 ms 检查一次；断连即 cancel。
- 若 httplib 无该 API，退化为“写回失败后 cancel”，只能省后续生成，不能省等待时间。

验收：客户端发起非流式请求后 0.2 s 断开；服务端在 1 s 内 `llmopt_requests_cancelled_total` +1，KV 块归还，后续请求正常。

### 3.5 Completion watchdog（已实现）

现状：`StepExecutor` 某步不回投 Completion，`pending_serials_/pending_remaining_/pending_step_` 无法清零，后续调度与 shutdown 永久等待。

契约：

- 新增 `--completion-timeout-ms`（默认 30,000，0=关）。
- Core 记录 `pending_since_ns`；每 tick 检查超时；超时后：
  - 向对应请求 push `EXECUTION_FAILED` 错误帧并关闭 sink，HTTP 侧立即回 500；
  - `llmopt_completion_timeouts_total` +1；
  - **不提前 `finish`**：执行线程可能仍持有该序列的 KV/后端槽，提前释放会造成 UAF；
  - 迟到 Completion 回到 Core 后走正常收尾，释放 KV/后端槽；
  - 不直接杀执行线程（避免 UB）。
- 局限：若执行线程永久挂死，watchdog 只能让客户端拿到 500，shutdown 仍会等执行线程；彻底解决需要 runner 级进程隔离。

验收：注入慢回投 fake runner（1.2 s）；请求在 1 s 内收到 `EXECUTION_FAILED`；迟到回投后 KV 归零；shutdown 在 5 s 内完成。

### 3.6 真后端 CI（已实现）

现状：CI 的 `llama-compile` 只守头文件兼容；`real-backend` nightly 守真实运行时行为。

契约：

- nightly（或 `workflow_dispatch`）新增 `real-backend` 作业：
  - 固定下载官方 `Qwen/Qwen2.5-0.5B-Instruct-GGUF` 的 `qwen2.5-0.5b-instruct-q4_k_m.gguf`（491,400,032 bytes），sha256 必须等于 `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db`；
  - 固定 llama.cpp `b12557`，CPU shared 构建 `llama`/`ggml`，再以 `LLMOPT_ENABLE_LLAMA=ON` 构建 `llmopt_server`；
  - `tests/e2e/nightly_real_backend.py` 起真实服务，跑 healthz/readyz、非流式、SSE、客户端断开取消、`/metrics` 五组；
  - 取消断言：断开后 `llmopt_requests_total{status="cancelled"}` 增长，取消后服务仍可正常完成请求，`llmopt_kv_used_blocks` 回零。
- 失败必须红灯，不允许“环境跑不了”跳过；下载/构建/运行失败都保留日志与 `real-backend.json`。
- 与 `llama-compile` 分离：编译作业守头文件兼容，运行作业守行为。

### 3.7 长稳/soak（规划，未实现）

- 30 min、P=4、固定 prompt/输出长度、每 5 min 采样 RSS/KV/延迟。
- 判据：RSS 增长 <5%、`kv_used` 无单调泄漏、p99 漂移 <10%、0 崩溃/0 未关闭连接。
- 产物：`evidence/soak_*.csv` + 报告；CI 可在 nightly 跑 5 min 短版。

## 4 测试矩阵

| 用例 | 构建 | 标签 | 断言 |
| --- | --- | --- | --- |
| `unit_config` 控制面/TLS 成对校验 | 默认 | unit/fast | 端口冲突、负数、单证书拒绝；合法值通过 |
| `integration_server_control_isolation` | 默认/TLS | integration/fast | 数据面饱和时控制面仍 200 |
| `integration_server_tls` | TLS | integration/fast | HTTPS 三端点；明文不 200 |
| `integration_server_tls_config_error` | 默认/TLS | integration/fast | 证书不可用时退出 3 + 明确 TLS 错误 |
| `tenant_keys_test` | 默认 | integration/fast | 三租户 401/429、A 限流、C KV 配额、`llmopt_tenant_*` 标签 |
| 非流式断连 | 默认 | integration | 1 s 内 cancel + KV 归还 |
| `completion_watchdog_test` | 默认 | contract/fast | 1 s 内 `EXECUTION_FAILED`；迟到回投后 KV 归零；shutdown <5 s |
| `nightly_real_backend.py` | LLAMA=ON | nightly | 固定模型 sha256；health/ready、非流式、SSE、cancel、metrics 全 PASS |
| soak | 默认 | slow | 30 min 无泄漏/漂移 |

## 5 实施顺序

1. **已完成**：TLS 数据面 + 控制面隔离 + 两条回归。
2. **已完成（R1-01）**：Completion watchdog，`completion_watchdog_test` 覆盖 500、迟到回收与 shutdown。
3. **已完成（P1）**：真后端 nightly，固定 Qwen2.5-0.5B Q4_K_M + sha256，CPU 跑五组运行时检查。
4. **已完成（P2）**：多租户静态 key + 并发/队列/限流/KV 配额；KV 单池 + 块级租户标记 + 前缀哈希域分离。
5. **P2**：soak 与 Docker/systemd 真机验证。
6. **P1（阻塞）**：非流式断连取消；需要 httplib 暴露连接探测或网关层取消。

## 6 需要用户确认的决策

1. 控制面是否默认开启 `control-port=8081`，还是保持 `0` 由部署显式开启？
2. 进程内 TLS 是否只作为“直连小部署”，公网统一要求前置代理？（推荐：是）
3. 多租户 key 采用静态 JSON 文件 + 重启生效（已确认，阶段 11 落地）；热加载/管理 API 后置。
4. 非流式断连取消是否允许改变响应语义（例如客户端断开后不再保留完整响应）？

## 7 验收口径

- 默认构建：`ctest -L fast` 173 条全绿；全量 175 注册、2 条真服务用例跳过；新增用例不改变旧行为。
- TLS 构建：`-DLLMOPT_ENABLE_TLS=ON` 编译 0 warning；TLS + 控制面 + 证书失败三条用例 PASS。
- 所有新配置项：CLI/env/JSON 三层等价；非法值启动退出 2；端口/证书失败退出 3。
- 文档：`CLI.md`、`README.md`、`docs/testing.md` 与本 SPEC 同步；不把“计划”写成“已支持”。
