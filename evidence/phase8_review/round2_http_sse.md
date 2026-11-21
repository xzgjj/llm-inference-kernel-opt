# Phase 8 第 2 轮独立审查：HTTP/SSE 对外契约与错误面（只读静态）

- 轮次：R2（三轮独立审查之第 2 轮）
- 仓库：`D:\c++_myproject\llm-inference-kernel-opt`（WSL `/mnt/d/c++_myproject/llm-inference-kernel-opt`），分支 `8`
- 审查口径：未提交改动（`git diff` + 未跟踪文件）及其紧邻的 HTTP/SSE 上下文
- 方法：`git diff` 核对 + 源码通读 + 捆绑 cpp-httplib v0.18.3 行为核对 + 既有测试断言比对；**未执行** cmake/ctest/启动服务/占端口，未改任何源码、脚本、测试
- 时间基准：2026-11-21（UTC+8）
- 约束遵守：未 `git add/commit/push/checkout/stash/reset`；未修改 `tools/bench/matrix.py`、`scripts/bench/matrix/*.json`、`docs/**`、`bench/results/**`；唯一可写文件为本报告

## 1. 结论概览

| 严重度 | 数量 | 编号 |
| --- | --- | --- |
| P0（确认致命：契约破裂/可被外部打穿/数据外泄） | 0 | — |
| P1（确认高优：状态码/帧/错误体与既有测试或明文契约冲突） | 0 | — |
| P2（确认静态缺陷或待真服务验证的缺口） | 7 | R2-01、R2-02、R2-03、R2-04、R2-05、R2-06、R2-07 |
| P3（观察项/口径提示，不阻断） | 2 | R2-08、R2-09 |

**总体判断**：阶段 8 改动（`capacity-only` 早退、`model-profile` 配置、`kv_used_blocks_peak`/`vram_device_mib` 两项新指标）本身不触及 HTTP 出帧路径；既有接入层在已被测试覆盖的主路径上契约自洽——SSE 帧格式、429/401/503 状态映射、统一错误 JSON 结构、重复 ID 拒绝、慢客户端不阻塞生产者，均与 `tests/integration/*`、`tests/e2e/test_real_service_contract.py`、`CLI.md` §3/§4、`docs/错误面规范.md` 对得上。本轮发现集中在**测试未覆盖的边界出口**（413、未知路由/错误方法、非流式断连）与**错误面纯度/字段回显**两类，均为 P2/P3，未见 P0/P1。

**最危险的破口**：外部可直接构造的两类请求（>4MB body、未登记路径/错误方法）绕过统一错误面，拿到 httplib 默认空体响应，客户端无法按 `error.code` 解析；其余缺口要么需要真服务压测才能确认影响面，要么只是文档/口径不一致。

## 2. 发现项明细

### R2-01 [P2] 超 4MB body 回 413 空体，绕过统一错误 JSON 契约

- **文件:行**：`src/http_server.cpp:450`（`server.set_payload_max_length(std::size_t{4} * 1024 * 1024);`）；行为来自捆绑 httplib `third_party/_deps/httplib-src/httplib.h:4400-4403, 6567`
- **代码原文**：
  ```cpp
  server.set_payload_max_length(std::size_t{4} * 1024 * 1024);
  // httplib: len > payload_max_length → exceed_payload_max_length = true; ret = false;
  //          status = PayloadTooLarge_413（handler 未被调用，send_error 不走）
  ```
- **触发请求**：`POST /v1/completions`（或 `/v1/chat/completions`、`/completion`），`Content-Length` 声明 >4MB 的 body。
- **期望 vs 实际**：
  - 期望：按 `docs/错误面规范.md` §1/§3 与 `CLI.md:161`，错误出口统一为 `{"error":{"code","message","retryable"}}` + `Content-Type: application/json`；413 语义上应属可重试的 `QUEUE_FULL` 之外的参数/体量类拒绝，至少要有可解析错误体。
  - 实际：handler 不执行，`send_error()`（`http_server.cpp:82-89`）不被调用；httplib 默认 413，响应体为空、`Content-Length: 0`、无 `Content-Type`、无 `Retry-After`，客户端拿不到 `error.code`。
- **判定依据**：`CLI.md:102` 写明"请求体（application/json，上限 4 MB）"，但未规定超限报文形态；既有测试（smoke/auth/admission/rate_limit）均未构造超限 body，该出口无断言。
- **确认状态**：**静态确认 httplib 行为**（源码路径已核）；413 实际响应头/体需真服务抓包复核。验证方法：起 stub 服务，`curl -i --data-binary <5MB 文件> -H "Content-Type: application/json"`，记录状态码、`Content-Type`、body。
- **最小修复建议**：在 httplib 层注册错误处理器（`server.set_error_handler`）或自行在 Content-Length 预检处返回统一错误体；413 映射建议归入新增/既有体量类 `ErrCode`，`retryable=false`，message 给出上限（4 MB）。
- **最小回归用例**：integration 用例「超限 body 回统一错误体」——`test_server_smoke.py` 旁新增：POST 5MB body，断言状态 413（或约定的 400/413）、body 可 `json.loads`、含 `error.code` 与 `retryable` 字段。

### R2-02 [P2] 未知路由 / 方法不允许回 404 空体，无统一错误 JSON，也不区分 404/405

- **文件:行**：`src/http_server.cpp:453-485`（仅注册 3 个 POST + 3 个 GET）；兜底来自 httplib `httplib.h:7211`（`if (res.status == -1) { res.status = NotFound_404; }`）与 `httplib.h:6437-6448`（空体不设 Content-Type，`Content-Length: 0`）
- **代码原文**：
  ```cpp
  server.Post("/v1/completions", ...);
  server.Post("/completion", ...);
  server.Post("/v1/chat/completions", ...);
  server.Get("/v1/models", ...);
  server.Get("/healthz", ...);
  server.Get("/readyz", ...);
  server.Get("/metrics", ...);
  // 未匹配：res.status 保持 -1 → 兜底 404，空 body
  ```
- **触发请求**：`GET /v1/completions`（方法不匹配）、`POST /v1/unknown`（路径未登记）、`OPTIONS /v1/completions`。
- **期望 vs 实际**：
  - 期望：未开放端点按 `docs/错误面规范.md` §3 `NotSupported → 404 NOT_SUPPORTED`，至少回统一错误 JSON；方法不允许与路径不存在应可区分（405/404）。
  - 实际：两类都落到 httplib 默认 404，空体、无 `error` 对象、无 `Allow` 头；`NotSupported` 这条领域码只在内部适配器返回时使用，路由层从未产生。
- **判定依据**：`test_server_smoke.py:1` 注释声称"校验路由/SSE/非流式/404"，但 `run_checks`（`test_server_smoke.py:87-125`）并未实际请求未登记路径，404 出口无断言；`CLI.md:92-100` 路由表之外未定义兜底报文。
- **确认状态**：**静态确认**（httplib 路由与兜底源码已核）；响应报文细节待真服务抓包。
- **最小修复建议**：注册 httplib 默认错误处理器，对未匹配路由回 `404 {"error":{"code":"NOT_SUPPORTED",...}}`；方法不匹配时回 405 并带 `Allow` 头（若保留现状至少统一错误体）。
- **最小回归用例**：integration 用例「未登记路径与错误方法的兜底报文」——断言 `GET /v1/completions` 与 `POST /nope` 均返回可解析的 `error` JSON；若补 405，断言响应带 `Allow: POST`。

### R2-03 [P2] 非流式客户端断连不触发取消，handler 线程占用至生成结束或请求 deadline

- **文件:行**：`src/http_server.cpp:97-135`（`drain_sink` 只从 sink 取帧，无连接存活探测）、`src/http_server.cpp:367-388`（非流式逐条 `drain_sink` 阻塞等待）；对比流式释放回调 `src/http_server.cpp:280-284`
- **代码原文**：
  ```cpp
  // 非流式：drain_sink 内只有 sink->pop_frame(delta, 100) / sink->is_closed() 判定；
  // 没有像流式那样的 DataSink 完成回调感知断连。
  std::string drain_sink(..., SchedulerCore& core, ..., int& gen_tokens,
                         FinishReason& finish, Error& error, ...) {
      while (true) {
          if (sink->pop_frame(delta, 100)) { ... }
          else if (sink->is_closed()) { break; }
          if (now > deadline) { core.cancel(...); break; }
      }
  }
  // 流式才有：[ctx, req](bool success) { if (!success) ctx->core->cancel(req_id); }
  ```
- **触发时序**：非流式请求已入队、客户端在首 token 前/生成中途关闭 TCP 连接；sink 未溢出（消费侧虽然断连，但 drain 线程仍在取帧，队列不会满），runner 继续生成直到 EOS / `max_tokens` / 调度侧 `request_deadline_ns`（默认 120s，`config.hpp:65`）才收尾；该 handler 线程在整段时间内被占住。
- **期望 vs 实际**：
  - 期望：客户端断开属于 `Cancelled`（499，"你走了"，错误面规范 §3），应尽快取消在飞序列并释放线程；流式路径已有完成回调，非流式路径缺失对称处理。
  - 实际：断连后无通知通道到 `drain_sink`；线程释放只依赖生成自然结束或调度 deadline，慢断连请求会长时间占用一个接入线程。
- **判定依据**：`test_server_slow_client.py` 只覆盖流式慢读；`test_drain.py` 覆盖排空期在飞终态；无任何用例断言"非流式中途断连被快速取消"。
- **确认状态**：**待验证**。静态可确认非流式路径没有断连回调；线程实际占用时长、调度 deadline 是否会先于生成结束兜底，需真服务/TSan 场景复测。验证方法：起 stub 服务并发 1 条非流式请求，读到首字节后 `RST` 连接，观测 core 在飞序列何时清零、handler 线程何时释放。
- **最小修复建议**：非流式在请求对象上挂连接存活探测（httplib `Request` 断开标记或 socket 轮询），`drain_sink` 每轮检查连接是否仍可写，不可写即 `core.cancel` 并按 `Cancelled` 收尾；或把非流式也改为 provider/回调驱动。
- **最小回归用例**：integration 用例「非流式中途断连快速取消」——提交非流式请求，中途关闭连接；断言在飞序列在短阈值内转 `cancelled`、`llmopt_requests_total{status="cancelled"}` 入账，线程不被挂到 120s deadline。

### R2-04 [P2] 响应体未回显请求的 `model` 字段，与 CLI.md 字段表"仅回显"不符

- **文件:行**：`src/protocol.cpp:115`（`out.model_echo = body.value("model", "");`）、`src/protocol.cpp:367-392`（`render_body` 只写 `id/object/choices/usage`，未序列化 `model_echo`）
- **代码原文**：
  ```cpp
  out.model_echo = body.value("model", "");
  // render_body: body["id"]=...; body["object"]=...; choices; usage; —— 无 body["model"]
  ```
- **触发请求**：`POST /v1/completions` 或 `/v1/chat/completions`，body 带 `"model": "任意名"`。
- **期望 vs 实际**：
  - 期望：`CLI.md:109` 字段表规定 `model | string | "" | 仅回显，不校验`，即响应应原样带回请求的 model 值（OpenAI 兼容形状通常也在响应里带 `model`）。
  - 实际：`model_echo` 解析后从未进入 `render_body`/`render_frame`，成功响应与 SSE 帧均无 `model` 字段；字段被静默丢弃。
- **判定依据**：`test_compat_openai.py:72-83` 断言 chat 响应 `object/id/finish_reason/usage`，未断言 `model`；`protocol_test.cc:232-253` 帧/体形状用例也未覆盖。属文档与实现不一致，既有契约测试未捕获。
- **确认状态**：**静态确认**（`model_echo` 全仓仅一处赋值、无读取，已 Grep 核实）。
- **最小修复建议**：`render_body`（及必要时流式首帧）补 `body["model"] = req.model_echo`；若决定不回显，应同步修订 `CLI.md:109` 字段表，避免两套事实。
- **最小回归用例**：单测 `protocol_test.cc`「响应回显请求 model」——构造带 `model="x"` 的请求，断言 `render_body` 输出含 `"model":"x"`；兼容用例补一条响应 `model` 字段比对。

### R2-05 [P2] nlohmann 异常 `what()` 进入 HTTP message 外发，违反错误面规范的 detail 归属

- **文件:行**：`src/protocol.cpp:271`、`src/protocol.cpp:318`
- **代码原文**：
  ```cpp
  } catch (const std::exception& exc) {
      message = std::string("解析异常: ") + exc.what();
      return ErrCode::InvalidJson;
  }
  ```
- **触发请求**：触发 nlohmann 类型/解析异常的畸形 body（如嵌套类型不符、非法转义位置）。
- **期望 vs 实际**：
  - 期望：`docs/错误面规范.md` §4.2 明确"nlohmann `parse` 抛异常 → InvalidJson，`detail=exc.what()`"；§1.1 规定 `detail` 只进日志、不进 HTTP body，`message` 面向客户端只写可决策的通用语义（如"请求体不是合法 JSON 对象"）。
  - 实际：`exc.what()`（形如 `[json.exception.type_error.302] type must be string, but is number`，含第三方库异常码/内部定位）被拼进 `message` 直接外发；`Error.detail` 未设置，运维侧反而丢失结构化 cause。
- **判定依据**：错误面规范 §4.2 表行；`protocol_test.cc:47-48` 只断言码为 `InvalidJson`，未约束 message 纯度。
- **确认状态**：**静态归属确认**；实际外发文本需真服务抓包确认具体内容（nlohmann 版本随 FetchContent 锁定）。
- **最小修复建议**：catch 内构造 `Error{InvalidJson, "请求体 JSON 非法", exc.what()}`，message 用通用文案，`what()` 进 detail；日志侧按 §6 带 detail。
- **最小回归用例**：单测「解析异常 message 不含第三方库名」——畸形 body 触发 catch 后，断言 message 不含 `json.exception`/`type_error`/`parse_error`，detail 保留 `exc.what()`。

### R2-06 [P2] chat 分片 `text` 类型非法时抛 nlohmann 类型异常，兜底为通用"解析异常"，无字段级 message

- **文件:行**：`src/protocol.cpp:156-162`（`text += part["text"].get<std::string>();` 未先判类型）、兜底 `src/protocol.cpp:317-320`
- **代码原文**：
  ```cpp
  for (const json& part : *content) {
      if (!part.is_object() || part.value("type", "") != "text" || !part.contains("text")) {
          message = "unsupported_field: messages[].content 分片（仅支持 type=text）";
          return ErrCode::UnsupportedField;
      }
      text += part["text"].get<std::string>();  // text 为 number/object/数组时此处抛
  }
  ```
- **触发请求**：`POST /v1/chat/completions`，`messages[].content = [{"type":"text","text":123}]` 或 `"text":{"a":1}`。
- **期望 vs 实际**：
  - 期望：按错误面规范 R2，message 应指到字段与期望类型，如 `messages[].content[].text 必须是字符串`；非法类型落 `InvalidJson`/`UnsupportedField` 均可，但应有字段级提示。
  - 实际：`get<std::string>()` 抛 `type_error`，被外层 catch 收成通用 `"解析异常: <nlohmann what()>"`，字段路径丢失，并叠加 R2-05 的第三方文本外发。
- **判定依据**：`protocol_test.cc:186-189` 只覆盖 image_url 分片与 tool role，未覆盖 `text` 本身类型非法。
- **确认状态**：**静态确认**（类型判断缺失可直接读出；异常文本同 R2-05 待抓包）。
- **最小修复建议**：取值前判 `part["text"].is_string()`，否则 message=`messages[].content[].text 必须是字符串`，返回 `InvalidJson`。
- **最小回归用例**：单测「content 分片 text 非字符串被字段级拒绝」——断言返回 `InvalidJson`，message 含 `text` 与"字符串"，不含库名。

### R2-07 [P2] HTTP 线程池按调度并发上限绑定，慢/断连接长期占线程，极端情况下控制端点可能被饿死

- **文件:行**：`src/http_server.cpp:448-449`、`src/http_server.cpp:451-452`（read 5s / write 120s）
- **代码原文**：
  ```cpp
  const std::size_t handler_threads =
      static_cast<std::size_t>(core.concurrency_limit() + 3);
  server.new_task_queue = [handler_threads] { return new httplib::ThreadPool(handler_threads); };
  server.set_write_timeout(120, 0);
  ```
- **触发时序**：`max-concurrency` 较小（如 1）时池 = 4 线程；多条慢读 SSE 连接各占一条线程持续到 write timeout 或生成结束，非流式慢连接同理（见 R2-03）。调度预算（并发+队列）挡住的是推理槽位，挡不住已建连的 handler 线程；预留的 3 条控制端点线程在推理连接占满后也可能被耗尽，`/healthz`、`/readyz`、`/metrics` 排队。
- **期望 vs 实际**：
  - 期望：控制探针有独立于推理连接的额度，慢消费者不影响探活（设计注释 `http_server.cpp:445-447` 即此意）。
  - 实际：池内线程共享，注释假设"3 条留给控制端点"，但推理慢连接不会主动让出线程；线程耗尽后控制请求只能排队等空闲。
- **判定依据**：`test_server_slow_client.py` 断言快客户端能读完，但未压"慢连接数量达到池上限时探活是否仍即时响应"；`test_drain.py` 排空场景不涉及线程池耗尽。
- **确认状态**：**待验证**。需真服务压测：开 `--max-concurrency 1`，建立 N+1 条慢读连接，观测 `/healthz` 响应延迟与线程排队。
- **最小修复建议**：控制端点使用独立小线程池/独立 listener；或在 provider 入口检测连接不可写即快速失败释放线程；池容量与慢连接占用策略写入 CLI.md。
- **最小回归用例**：integration 用例「慢连接占满池时探活不饿死」——并发打满慢 SSE 连接后，断言 `/healthz`/`/readyz` 在短阈值内返回 200/503，不被推理连接排队阻塞。

### R2-08 [P3] SSE 与 JSON 响应的 Content-Type 未带 charset

- **文件:行**：`src/http_server.cpp:276`（`"text/event-stream"`）、`src/http_server.cpp:88,397,469,474,481`（`"application/json"`）
- **代码原文**：
  ```cpp
  response.set_chunked_content_provider("text/event-stream", ...);
  res.set_content(render_error_body(payload), "application/json");
  ```
- **触发请求**：任意流式/非流式/错误响应。
- **期望 vs 实际**：主流客户端按 UTF-8 解码即可工作；当前中文 message/中文生成文本以 UTF-8 裸字节出帧，未显式 `charset=utf-8`。契约测试（`test_server_metrics.py:38`）只断言 `/metrics` 含 `text/plain`，未校验 SSE/JSON 字符集。
- **判定依据**：`CLI.md:133-149` 流式帧示例未规定 charset；OpenAI 实际回包亦常省略 charset。
- **确认状态**：**观察项**，不阻断。
- **最小修复建议**：统一写 `text/event-stream; charset=utf-8` 与 `application/json; charset=utf-8`（注意先核 httplib 不对 content-type 做改写）。
- **最小回归用例**：可选；集成用例补断言 SSE 响应头含 `text/event-stream`。

### R2-09 [P3] QueueFull / ContextOverflow 的 message 缺深度与边界数值，数值只进 detail

- **文件:行**：`src/http_server.cpp:231-234`、`src/http_server.cpp:239-240`、`src/http_server.cpp:248-249`
- **代码原文**：
  ```cpp
  return make_error(ErrCode::ContextOverflow, "prompt 与 max_tokens 之和超过 ctx-tokens",
                    "prompt=" + ... + " max_tokens=" + ... + " ctx=" + ...);
  return make_error(ErrCode::QueueFull, "等待队列已满，请退避重试",
                    "cap=" + std::to_string(ctx.core->admission_capacity()));
  ```
- **触发请求**：超上下文 / 队列满拒绝。
- **期望 vs 实际**：错误面规范 R2 与 §3 表要求 message 回答"谁、什么、边界值"（ContextOverflow：两个数都要给；QueueFull：当前深度/容量）。当前深度/容量/prompt 数都落在 `detail`（不外发），客户端 message 只有通用文案，无法据此判断该裁多少 token 或退避到什么程度。
- **判定依据**：`test_server_admission.py:67-68` 只断言 `code=QUEUE_FULL`，不断言 message 带容量；e2e 只断言 `CONTEXT_OVERFLOW` 码。
- **确认状态**：**观察项/口径提示**。
- **最小修复建议**：把不敏感的边界值（ctx 上限、队列容量）并入 message；原始 req_id/内部计数仍留 detail。
- **最小回归用例**：单测/集成断言 QueueFull message 含容量、ContextOverflow message 含 prompt 与 ctx 数值。

## 3. 已覆盖但未发现问题的子项（按六项判据核对记录）

### 3.1 SSE 帧格式（判据 1）

- 帧结构：`data: <JSON>\n\n`，单行 `data:` 前缀 + 空行分隔（`src/protocol.cpp:338,355,359`）；单测锁定末帧 `data: [DONE]\n\n` 14 字节（`tests/unit/protocol_test.cc:74-95`）。无 `event:`/`id:` 行，`CLI.md:135-140` 帧示例也未要求，当前契约一致。
- 不切碎 JSON：每帧一次 `nlohmann::json::dump()`，引号/反斜杠/LF/CR/控制字符由 JSON 转义；帧内不会出现裸换行导致分片。多字节 UTF-8 由调度侧 `utf8_pending` 攒整字符后再下发（`include/llmopt/scheduler.hpp:96`、`src/scheduler.cpp:1230-1234`），不存在半个字符进 JSON。
- 即时 flush：chunked provider 每次 `sink.write` 经 `write_data → SocketStream::write → send_socket` 直写套接字（`httplib.h:4444-4454, 5931-5938`），无用户态缓冲滞留；每帧即一个 chunk。
- `[DONE] 唯一性`：正常结束（sink closed）、stop 命中、错误帧、写失败四条路径均只补一次 `[DONE]`（`src/http_server.cpp:156-183`）；错误帧先于 `[DONE]` 下发，符合错误面规范 §7.6。客户端断连路径不补发 `[DONE]`（连接已不可写），语义正确。
- Content-Type/编码：流式走 `Transfer-Encoding: chunked`（httplib `httplib.h:6958-6962`），与 `Content-Length` 不共存；未发现两者冲突。

### 3.2 HTTP 状态与头（判据 2）

- 状态码单点映射：`http_status()` 一处维护（`src/common.cpp:6-40`），handler 不自造码；映射表与 `CLI.md:169-184`、错误面规范 §3 逐条核对一致：400/401/404/429/503/507/499/500/504/400(UnsupportedField)。
- 429 统一带 `Retry-After: 1`（`src/http_server.cpp:85-87`），覆盖 `QueueFull`/`KvBelowWatermark`/`RateLimited` 三种 429；`test_rate_limit.py:32`、`test_server_admission.py:66` 断言一致。
- 401 带 `WWW-Authenticate: Bearer realm="llmopt"`（`src/http_server.cpp:298,464`）；`test_auth.py:37` 断言前缀。
- 路由分组：推理三端点走鉴权 + 限流；`/healthz`、`/readyz`、`/metrics` 独立路由不鉴权（`src/http_server.cpp:471-485`），与 `CLI.md:186` 一致。
- 就绪探针：非 Ready 一律 503（`src/http_server.cpp:477-482`）；排空期新请求 `Unavailable → 503 SERVICE_UNAVAILABLE`（`src/http_server.cpp:192-193`），`test_drain.py:66-68` 断言一致。
- keep-alive：httplib 默认长连，read 超时 5s、write 超时 120s（`src/http_server.cpp:451-452`）；错误/普通响应 `set_content` 带 Body 与 `Content-Length`，未发现长度头冲突。
- 既有测试覆盖的状态出口：200 流式/非流式、400 空 prompt/越界字段/非法 JSON、400 重复 ID、401、429 queue_full/rate_limited、503 排空、400 UNSUPPORTED_FIELD，均与断言吻合。

### 3.3 错误面（判据 3）

- 统一错误体：`{"error":{"code","message","retryable"}}`（`src/protocol.cpp:457-462`）；流式错误帧同构（`src/protocol.cpp:329-338`）。
- detail 不外发：`render_error_body`/错误帧只取 `code/message/retryable`，`detail` 不序列化；单测锁定 `op=process rc=1`、`cap=32` 不出现在 body（`tests/unit/protocol_test.cc:106-126`）。
- 路径不外泄：`/v1/models` 的模型 id 取文件名去扩展名，绝对路径不回给客户端（`src/http_server.cpp:422-426,468`）。
- 异常兜底：handler 双层 `catch (std::exception&)`/`catch (...)`，兜底 `ExecutionFailed` 且取消已提交 id（`src/http_server.cpp:402-418`），异常文本不进 body。
- 日志可回溯：拒绝/完成日志带 `req_id` 与 `code`（`src/http_server.cpp:319-320,356-357,390-391`）；`test_compat_openai.py:203-207` 断言日志可检索到请求 ID。
- 残留问题集中在 R2-05（第三方 `what()` 混入 message）与 R2-09（边界值在 detail），其余结构统一。

### 3.4 边界输入（判据 4）

- 整数/浮点范围：`max_tokens/n_predict` 1–4096、`top_k` 0–256、`seed` -1–1e9、`temperature` 0–2、`top_p/min_p` 0–1、`repeat_penalty` 0.5–2、`n` 1–4（`src/protocol.cpp:20-51,124-129,167-211`）；负数、越界、非整数/非数字一律 400，字段名 + 合法区间进 message。
- 枚举/结构校验：`stream` 非布尔拒绝；`stop` 最多 4 条、每条 1–32 字符；`id` 1–64 字节且仅 `[A-Za-z0-9._:-]`（`src/protocol.cpp:240-255`）；chat role 与 content 分片结构校验（`src/protocol.cpp:141-165,295-312`）。
- 体量上限：body 上限 4MB（`src/http_server.cpp:450`）；超限出口报文缺陷见 R2-01。
- 数字精度：`token_id` 为 int32；usage 计数为 int，`n≤4` 且单候选 ≤4096，`completion_tokens` 累加无溢出路径；`total = prompt + completion` 自洽（`src/protocol.cpp:386-390`，e2e `test_real_service_contract.py:77-79` 断言）。
- 未发现：非法 JSON 穿透成 500、类型错误被静默忽略、控制字符切碎帧的路径。

### 3.5 中途取消 / 慢客户端（判据 5）

- 生产者不被 socket 阻塞：调度/执行线程只 push 有界 `BoundedSink`（容量 256），满即关闭并记账，绝不等待（`src/sink.cpp:11-24`、`include/llmopt/sink.hpp:18-19`）；溢出增量上报 `llmopt_sink_overflow_total`（`src/scheduler.cpp:1253-1262`）。
- 流式断连：provider 写失败即 `core->cancel(req_id)` 并置 `done`（`src/http_server.cpp:156-160,167-170,178-180`）；httplib 完成回调在非成功时再次取消（`src/http_server.cpp:280-284`）。
- 慢读隔离：`test_server_slow_client.py` 验证慢客户端不拖垮快请求、溢出计数单调不减；sink 溢出后序列按 `Aborted` 收尾归还块/槽（`src/scheduler.cpp:1221-1223`）。
- 非流式收尾：stop 命中、超时、错误均取消在飞请求并落终态（`src/http_server.cpp:117-131`）；非流式断连探测缺口见 R2-03。
- 写失败收尾：取消事件走 mailbox 关键通道；未发现写失败后悬挂序列或泄漏块（与 R1 报告 §3.4/§3.5 核对一致，不重复展开）。

### 3.6 并发契约（判据 6）

- 同连接：cpp-httplib 每连接串行处理请求，不存在同连接并发入队。
- 重复 ID：活跃 ID 二次提交 → 400 `INVALID_JSON`，message 提示换新 id（`src/scheduler.cpp:300-304`、`src/http_server.cpp:251-253`）；请求结束后 ID 可复用，内部 `BackendId` 不复用（`test_compat_openai.py:142-163`）。
- 鉴权开关：`api_key` 为空恒放行；非空走定长比较，不按首字节提前返回（`include/llmopt/auth.hpp:11-32`）；缺失/错误 Bearer → 401。
- 配置来源：CLI > env > file > default，启动期一次性加载，逐项打印来源（`src/config.cpp:752-800`）；本轮新增 `model-profile`/`capacity-only` 的 CLI/env/JSON 三通道与校验由 `tests/unit/config_test.cc:103-119` 覆盖。服务期不热改配置，无运行时差异口径问题。
- 指标对外字段：本轮新增 `llmopt_kv_used_blocks_peak`、`llmopt_vram_device_mib` 均带 HELP/TYPE，枚举与定义表索引对齐（`src/metrics.cpp:53-54,160-161`；R1 §3.6 已核对 ALIGNED）；`/metrics` 仅 counter/gauge，未知序列不动态注册（`test_server_metrics.py:96`）。

## 4. 必须靠真服务才能确认的问题（阻塞/待验证清单）

| 编号 | 待验证项 | 验证方式 | 阻塞程度 |
| --- | --- | --- | --- |
| R2-01 | 413 超限 body 的实际状态码、头、空体形态 | stub 服务 + `curl -i` 发 5MB body 抓包 | 待验证；不影响现有 165 用例 |
| R2-02 | 未登记路径/错误方法的 404 空体与无 `Allow` 头 | stub 服务分别 `GET /v1/completions`、`POST /nope` 抓包 | 待验证 |
| R2-03 | 非流式中途断连后在飞序列清零与线程释放时延 | 并发 1 条非流式，首字节后 RST 连接，观测 core 在飞数与线程占用 | 待验证；需避开他人 GPU/端口，单独环境跑 |
| R2-05/R2-06 | nlohmann `what()` 外发文本的实际内容 | 构造类型非法 body 抓 400 报文 | 待验证 |
| R2-07 | 慢连接打满 handler 池后 `/healthz` 探活时延 | `--max-concurrency 1` 下建满慢 SSE 连接，轮询探活 | 待压测 |
| 附 | 长生成 SSE 是否受 write timeout 120s 截断 | 真模型长输出用例核对 | 观察项 |

## 5. 本轮通读文件

- `src/http_server.cpp`、`src/protocol.cpp`、`src/common.cpp`、`src/sink.cpp`、`src/scheduler.cpp`（准入/收尾/溢出段）、`src/main.cpp`、`src/config.cpp`、`src/metrics.cpp`、`src/resource.cpp`（新指标落地点）
- `include/llmopt/{protocol,common,auth,service,rate_limit,sink,server,runner,config,metrics,scheduler}.hpp`
- 契约参照：`tests/integration/{test_server_smoke,test_server_chunked,test_server_metrics,test_compat_openai,test_auth,test_rate_limit,test_server_admission,test_server_concurrency,test_server_slow_client,test_drain,test_server_mixed,test_server_matrix,test_server_config_sources}.py`、`tests/e2e/test_real_service_contract.py`、`tests/unit/protocol_test.cc`、`tests/unit/config_test.cc`、`tests/unit/metrics_test.cc`、`CLI.md`、`docs/错误面规范.md`
- 第三方行为核对：`third_party/_deps/httplib-src/httplib.h` v0.18.3（路由兜底、chunked 写出、413、默认响应头）
