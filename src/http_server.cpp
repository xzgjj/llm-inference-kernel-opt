// http_server.cpp — M1 接入层：路由、准入、SSE 写回；handler 只构造值对象入队。
// 线程归属：socket 写只发生在持连接的 httplib 线程；推理线程只 push 有界帧队列。
#include "httplib.h"
#include "llmopt/log.hpp"
#include "llmopt/protocol.hpp"
#include "llmopt/server.hpp"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace llmopt {
namespace {

// 一次进程内共享的接入上下文；生命周期与 run_server 相同。
struct AppContext {
    const ServerConfig* config = nullptr;
    ModelRunner* runner = nullptr;
    MetricsRegistry* metrics = nullptr;
    SchedulerCore* core = nullptr;
    GateResult gate;
    std::atomic<bool> stopping{false};
};

httplib::Server* g_server = nullptr;

// 信号处理只做 stop()，避免在 handler 里做任何分配。
void handle_signal(int) {
    if (g_server) {
        g_server->stop();
    }
}

// 统一错误回包：状态码与 code 由 C-03 映射，禁止各路由自行编造。
// 429 必须带 Retry-After：客户端可据此退避，而不是把队列满误当成执行错误重试。
void send_error(httplib::Response& res, const Error& error) {
    const Error payload = error.ok() ? make_error(ErrCode::ExecutionFailed, "服务内部错误") : error;
    res.status = http_status(payload.code);
    if (res.status == 429) {
        res.set_header("Retry-After", "1");
    }
    res.set_content(render_error_body(payload), "application/json");
}

// 把 SSE 帧写到当前连接；写失败即断连，由调用方投递 Cancel。
bool write_frame(httplib::DataSink& sink, const std::string& frame) {
    return sink.write(frame.data(), frame.size());
}

// 非流式：排空 sink 并拼接文本；stop 命中即取消，避免继续消耗算力。
std::string drain_sink(const std::shared_ptr<ResponseSink>& sink, const InferenceRequest& req,
                       SchedulerCore& core, StopFilter& filter, int& gen_tokens,
                       FinishReason& finish, Error& error) {
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (true) {
        Delta delta;
        if (sink->pop_frame(delta, 100)) {
            if (delta.error && !delta.error->ok()) {
                error = *delta.error;
            }
            if (delta.token_id >= 0) {
                ++gen_tokens;
            }
            bool hit = false;
            text += filter.feed(delta.text, &hit);
            if (delta.finish) {
                finish = *delta.finish;
                text += filter.flush();
            }
            if (hit) {
                core.cancel(req.req_id);
                finish = FinishReason::Stopped;
                break;
            }
        } else if (sink->is_closed()) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            core.cancel(req.req_id);
            error = make_error(ErrCode::Timeout, "等待生成结果超时（120s）", "req=" + req.req_id);
            finish = FinishReason::Aborted;
            break;
        }
    }
    return text;
}

// 流式 provider：按需取帧、逐帧写；sink 关闭后补 [DONE] 并显式结束 chunked 流。
bool stream_provider(const std::shared_ptr<AppContext>& ctx, const InferenceRequest& req,
                     const std::shared_ptr<ResponseSink>& sink, const std::shared_ptr<bool>& done,
                     httplib::DataSink& data_sink) {
    if (*done) {
        return false;
    }
    StopFilter filter(req.stop);
    Delta delta;
    for (int index = 0; index < 32 && sink->pop_frame(delta, 20); ++index) {
        bool hit = false;
        Delta out = delta;
        out.text = filter.feed(delta.text, &hit);
        if (delta.finish) {
            out.text += filter.flush();
        }
        if (hit) {
            out.finish = FinishReason::Stopped;
        }
        if (!out.text.empty() || out.finish || out.error) {
            if (!write_frame(data_sink, render_frame(req, out))) {
                ctx->core->cancel(req.req_id);
                *done = true;
                return false;
            }
        }
        if (hit || out.error) {
            if (hit) {
                ctx->core->cancel(req.req_id);
            }
            data_sink.done();
            *done = true;
            return false;
        }
    }
    if (!sink->is_closed()) {
        return true;
    }
    if (!write_frame(data_sink, render_done_frame())) {
        ctx->core->cancel(req.req_id);
    }
    data_sink.done();
    *done = true;
    return false;
}

// 解析 → 分词/模板 → 上下文校验 → 入口准入 → 入队；返回 Ok 表示响应已接管。
// 准入在这里判定：队列满直接 429，不再"先 200 建流、再在调度器里静默失败"。
Error submit_request(AppContext& ctx, const std::string& body, bool chat,
                     const std::string& forced_id, InferenceRequest& req,
                     std::shared_ptr<ResponseSink>& sink, int& prompt_tokens) {
    std::string message;
    if (!forced_id.empty()) {
        req.req_id = forced_id;
    }
    ErrCode code = chat ? parse_chat_body(body, req, message) : parse_body(body, req, message);
    if (code != ErrCode::Ok) {
        return make_error(code, message.empty() ? "请求体非法" : message);
    }
    if (!forced_id.empty()) {
        req.req_id = forced_id;
    }
    if (chat) {
        code = ctx.runner->render_chat(req.messages, req.prompt);
        if (code != ErrCode::Ok) {
            return make_error(code, "chat 模板渲染失败",
                              "op=render_chat backend=" + std::string(ctx.runner->backend_name()));
        }
        if (req.prompt.empty()) {
            return make_error(ErrCode::InvalidJson, "messages 渲染结果为空");
        }
    }
    std::vector<std::int32_t> ids;
    code = ctx.runner->tokenize(req.prompt, ids);
    if (code != ErrCode::Ok) {
        return make_error(code, "分词失败",
                          "op=tokenize backend=" + std::string(ctx.runner->backend_name()));
    }
    if (ids.empty()) {
        return make_error(ErrCode::PromptTooLong, "分词结果为空",
                          "prompt_chars=" + std::to_string(req.prompt.size()));
    }
    const std::int32_t need = static_cast<std::int32_t>(ids.size()) + req.sampling.max_tokens;
    const std::int32_t limit = ctx.runner->n_ctx();
    if (need > limit) {
        return make_error(ErrCode::ContextOverflow, "prompt 与 max_tokens 之和超过 ctx-tokens",
                          "prompt=" + std::to_string(ids.size()) +
                                  " max_tokens=" + std::to_string(req.sampling.max_tokens) +
                                  " ctx=" + std::to_string(limit));
    }
    if (!ctx.core->accepting()) {
        ctx.metrics->inc(MetricId::AdmissionQueue);
        ctx.metrics->inc(MetricId::RequestsQueueFull);
        return make_error(ErrCode::QueueFull, "等待队列已满，请退避重试",
                          "cap=" + std::to_string(ctx.core->admission_capacity()));
    }
    sink = std::make_shared<BoundedSink>(256);
    prompt_tokens = static_cast<int>(ids.size());
    code = ctx.core->submit(req, std::move(ids), sink);
    if (code != ErrCode::Ok) {
        if (code == ErrCode::QueueFull) {
            // 预留竞争由 Core 记账，避免同一拒绝在两层各加一次。
            return make_error(ErrCode::QueueFull, "等待队列已满，请退避重试",
                              "cap=" + std::to_string(ctx.core->admission_capacity()));
        }
        return make_error(code, code == ErrCode::ResourceInsufficient ? "服务资源不足，请稍后重试"
                                                                      : "请求无法入队");
    }
    return Error{};
}

// n>1 的候选共用原始 id，但调度器需要唯一键：后续候选统一加 -i 后缀。
std::string choice_id(const InferenceRequest& req, std::int32_t index) {
    if (index == 0) {
        return req.req_id;
    }
    return req.req_id + "-" + std::to_string(index);
}

// 单候选流式回包：SSE 立即接管连接。
void start_stream(const std::shared_ptr<AppContext>& ctx, const InferenceRequest& req,
                  const std::shared_ptr<ResponseSink>& sink, httplib::Response& response) {
    response.set_header("Cache-Control", "no-cache");
    response.set_header("X-Accel-Buffering", "no");
    auto done = std::make_shared<bool>(false);
    response.set_chunked_content_provider(
            "text/event-stream",
            [ctx, req, sink, done](size_t, httplib::DataSink& data_sink) {
                return stream_provider(ctx, req, sink, done, data_sink);
            },
            [ctx, req](bool success) {
                if (!success) {
                    ctx->core->cancel(req.req_id);
                }
            });
}

// 两个补全端点共用：stream 走 SSE；n>1 只在非流式生效，逐候选独立入队。
// 失败一律按 Error 的真实状态码回包，不再出现 "200 + finish_reason=error"。
void inference_handler(const std::shared_ptr<AppContext>& ctx, const httplib::Request& request,
                       httplib::Response& response, bool chat) {
    bool responded = false;
    std::vector<std::string> submitted;
    try {
        ctx->metrics->inc(MetricId::HttpRequests);
        std::string request_id;
        if (request.has_header("X-Request-Id")) {
            request_id = request.get_header_value("X-Request-Id");
        }
        InferenceRequest base;
        std::shared_ptr<ResponseSink> sink;
        int prompt_tokens = 0;
        const Error submit_error =
                submit_request(*ctx, request.body, chat, request_id, base, sink, prompt_tokens);
        if (!submit_error.ok()) {
            logger()->warn("http reject id={} code={} path={}", base.req_id,
                           error_code_name(submit_error.code), request.path);
            send_error(response, submit_error);
            return;
        }
        logger()->info("http request id={} path={} stream={} n={}", base.req_id, request.path,
                       base.stream, base.n);
        if (base.stream) {
            start_stream(ctx, base, sink, response);
            responded = true;
            return;
        }

        std::vector<std::string> texts;
        std::vector<FinishReason> finishes;
        std::vector<int> gen_tokens;
        std::vector<std::shared_ptr<ResponseSink>> sinks;
        std::vector<InferenceRequest> reqs;
        texts.reserve(static_cast<std::size_t>(base.n));
        finishes.reserve(static_cast<std::size_t>(base.n));
        gen_tokens.reserve(static_cast<std::size_t>(base.n));
        sinks.reserve(static_cast<std::size_t>(base.n));
        reqs.reserve(static_cast<std::size_t>(base.n));
        reqs.push_back(base);
        sinks.push_back(sink);
        submitted.push_back(base.req_id);
        for (std::int32_t index = 1; index < base.n; ++index) {
            InferenceRequest extra = base;
            extra.req_id = choice_id(base, index);
            std::shared_ptr<ResponseSink> extra_sink;
            int ignored_prompt = 0;
            const Error error = submit_request(*ctx, request.body, chat, extra.req_id, extra,
                                               extra_sink, ignored_prompt);
            if (!error.ok()) {
                for (const std::string& id : submitted) {
                    ctx->core->cancel(id);
                }
                logger()->warn("http reject id={} code={} path={}", extra.req_id,
                               error_code_name(error.code), request.path);
                send_error(response, error);
                return;
            }
            reqs.push_back(std::move(extra));
            sinks.push_back(std::move(extra_sink));
            submitted.push_back(reqs.back().req_id);
        }

        Error first_error;
        for (std::size_t index = 0; index < reqs.size(); ++index) {
            int generated = 0;
            FinishReason finish = FinishReason::None;
            Error error;
            StopFilter filter(reqs[index].stop);
            const std::string text = drain_sink(sinks[index], reqs[index], *ctx->core, filter,
                                                generated, finish, error);
            if (finish == FinishReason::Error && error.ok()) {
                error = make_error(ErrCode::ExecutionFailed, "推理失败（无详细原因）");
            } else if (finish == FinishReason::Aborted && error.ok()) {
                error = make_error(ErrCode::Cancelled, "请求已取消");
            } else if (finish == FinishReason::None && error.ok()) {
                error = make_error(ErrCode::ExecutionFailed, "推理未产生结束原因");
            }
            if (!error.ok() && first_error.ok()) {
                first_error = error;
            }
            texts.push_back(text);
            finishes.push_back(finish);
            gen_tokens.push_back(generated);
        }
        if (!first_error.ok()) {
            logger()->warn("http failed id={} code={} path={}", base.req_id,
                           error_code_name(first_error.code), request.path);
            send_error(response, first_error);
            return;
        }
        response.status = 200;
        response.set_content(render_body(base, texts, finishes, prompt_tokens, gen_tokens),
                             "application/json");
        logger()->info("http done id={} finish={} choices={} path={}", base.req_id,
                       finish_reason_name(finishes.empty() ? FinishReason::Error : finishes[0]),
                       texts.size(), request.path);
        responded = true;
    } catch (const std::exception& exc) {
        logger()->error("inference handler 异常: {}", exc.what());
        for (const std::string& id : submitted) {
            ctx->core->cancel(id);
        }
        if (!responded) {
            send_error(response, make_error(ErrCode::ExecutionFailed, "服务内部异常"));
        }
    } catch (...) {
        logger()->error("inference handler 未知异常");
        for (const std::string& id : submitted) {
            ctx->core->cancel(id);
        }
        if (!responded) {
            send_error(response, make_error(ErrCode::ExecutionFailed, "服务内部异常"));
        }
    }
}

// 模型 id 取文件名去扩展名，避免把绝对路径回给外部客户端。
std::string model_id_from_path(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

} // namespace

// 安装路由并监听；SIGINT/SIGTERM 触发 stop，返回后由 main 关闭调度线程。
int run_server(const ServerConfig& config, ModelRunner& runner, MetricsRegistry& metrics,
               SchedulerCore& core, const GateResult& gate) {
    auto ctx = std::make_shared<AppContext>();
    ctx->config = &config;
    ctx->runner = &runner;
    ctx->metrics = &metrics;
    ctx->core = &core;
    ctx->gate = gate;

    httplib::Server server;
    server.set_payload_max_length(std::size_t{4} * 1024 * 1024);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(120, 0);
    server.Post("/v1/completions", [ctx](const httplib::Request& req, httplib::Response& res) {
        inference_handler(ctx, req, res, false);
    });
    server.Post("/completion", [ctx](const httplib::Request& req, httplib::Response& res) {
        inference_handler(ctx, req, res, false);
    });
    server.Post("/v1/chat/completions", [ctx](const httplib::Request& req, httplib::Response& res) {
        inference_handler(ctx, req, res, true);
    });
    server.Get("/v1/models", [ctx](const httplib::Request&, httplib::Response& res) {
        res.set_content(render_models_body(model_id_from_path(ctx->config->model_path)),
                        "application/json");
    });
    server.Get("/healthz", [ctx](const httplib::Request&, httplib::Response& res) {
        if (ctx->stopping.load()) {
            res.status = 503;
            res.set_content("{\"status\":\"stopping\"}", "application/json");
        } else {
            res.set_content("{\"status\":\"ready\"}", "application/json");
        }
    });
    server.Get("/metrics", [ctx](const httplib::Request&, httplib::Response& res) {
        res.set_content(ctx->metrics->render(), "text/plain; version=0.0.4");
    });

    g_server = &server;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    logger()->info("listening on http://{}:{} backend={}", config.host, config.port,
                   runner.backend_name());
    const bool ok = server.listen(config.host, config.port);
    g_server = nullptr;
    ctx->stopping.store(true);
    if (!ok) {
        logger()->error("listen 失败：端口 {} 可能被占用", config.port);
        return 3;
    }
    logger()->info("server stopped");
    return 0;
}

} // namespace llmopt
