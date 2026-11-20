// http_server.cpp — M1 接入层：路由、准入、SSE 写回；handler 只构造值对象入队。
// 线程归属：socket 写只发生在持连接的 httplib 线程；推理线程只 push 有界帧队列。
#include "httplib.h"
#include "llmopt/log.hpp"
#include "llmopt/protocol.hpp"
#include "llmopt/server.hpp"
#include "spdlog/spdlog.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>

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
void send_error(httplib::Response& res, ErrCode code, const std::string& message) {
    res.status = http_status(code);
    res.set_content(render_error_body(code, message), "application/json");
}

// 把 SSE 帧写到当前连接；写失败即断连，由调用方投递 Cancel。
bool write_frame(httplib::DataSink& sink, const std::string& frame) {
    return sink.write(frame.data(), frame.size());
}

// 非流式：排空 sink 并拼接文本，最后一次性回 JSON；超时投递 Cancel。
std::string drain_sink(const std::shared_ptr<ResponseSink>& sink, const InferenceRequest& req,
                       SchedulerCore& core, int& gen_tokens, FinishReason& finish) {
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (true) {
        Delta delta;
        if (sink->pop_frame(delta, 100)) {
            text += delta.text;
            if (delta.token_id >= 0) {
                ++gen_tokens;
            }
            if (delta.finish) {
                finish = *delta.finish;
            }
        } else if (sink->is_closed()) {
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            core.cancel(req.req_id);
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
    Delta delta;
    for (int index = 0; index < 32 && sink->pop_frame(delta, 20); ++index) {
        if (!write_frame(data_sink, render_frame(req, delta))) {
            ctx->core->cancel(req.req_id);
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

// 解析 → 分词 → 上下文校验 → 入队；返回 Ok 表示响应已接管。
ErrCode submit_request(AppContext& ctx, const std::string& body, InferenceRequest& req,
                       std::shared_ptr<ResponseSink>& sink, int& prompt_tokens,
                       std::string& message) {
    ErrCode code = parse_body(body, req, message);
    if (code != ErrCode::Ok) {
        return code;
    }
    std::vector<std::int32_t> ids;
    code = ctx.runner->tokenize(req.prompt, ids);
    if (code != ErrCode::Ok) {
        message = "分词失败";
        return code;
    }
    if (ids.empty()) {
        message = "分词结果为空";
        return ErrCode::PromptTooLong;
    }
    if (static_cast<std::int32_t>(ids.size()) + req.sampling.max_tokens > ctx.runner->n_ctx()) {
        message = "prompt + max_tokens 超过 ctx-tokens";
        return ErrCode::ContextOverflow;
    }
    sink = std::make_shared<BoundedSink>(256);
    prompt_tokens = static_cast<int>(ids.size());
    code = ctx.core->submit(req, std::move(ids), sink);
    if (code != ErrCode::Ok) {
        message = "队列已满，请退避重试";
    }
    return code;
}

// 两个补全端点共用：stream 走 SSE，非流式排空后一次性回包。
void completions_handler(const std::shared_ptr<AppContext>& ctx, const httplib::Request& request,
                         httplib::Response& response) {
    ctx->metrics->inc("llmopt_http_requests_total");
    InferenceRequest req;
    std::shared_ptr<ResponseSink> sink;
    std::string message;
    int prompt_tokens = 0;
    const ErrCode code = submit_request(*ctx, request.body, req, sink, prompt_tokens, message);
    if (code != ErrCode::Ok) {
        send_error(response, code, message);
        return;
    }
    if (req.stream) {
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
        return;
    }
    int gen_tokens = 0;
    FinishReason finish = FinishReason::None;
    const std::string text = drain_sink(sink, req, *ctx->core, gen_tokens, finish);
    if (finish == FinishReason::None) {
        finish = FinishReason::Error;
    }
    response.status = finish == FinishReason::Aborted ? 499 : 200;
    response.set_content(render_completion_body(req, text, finish, prompt_tokens, gen_tokens),
                         "application/json");
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
    server.set_payload_max_length(4 * 1024 * 1024);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(120, 0);
    server.Post("/v1/completions", [ctx](const httplib::Request& req, httplib::Response& res) {
        completions_handler(ctx, req, res);
    });
    server.Post("/completion", [ctx](const httplib::Request& req, httplib::Response& res) {
        completions_handler(ctx, req, res);
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
    server.Post("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        send_error(res, ErrCode::NotSupported, "/v1/chat/completions 在阶段 1 未启用");
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
