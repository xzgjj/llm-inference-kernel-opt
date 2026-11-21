// http_server.cpp — M1 接入层：路由、准入、SSE 写回；handler 只构造值对象入队。
// 线程归属：socket 写只发生在持连接的 httplib 线程；推理线程只 push 有界帧队列。
#include "httplib.h"
#include "llmopt/auth.hpp"
#include "llmopt/log.hpp"
#include "llmopt/protocol.hpp"
#include "llmopt/rate_limit.hpp"
#include "llmopt/server.hpp"
#include "llmopt/service.hpp"
#include "llmopt/tenant.hpp"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
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
    ServiceController service;
    std::unique_ptr<RateLimiter> limiter;
    TenantQuota default_quota;
    std::unique_ptr<TenantMetrics> tenant_metrics;
    std::unordered_map<std::string, std::unique_ptr<RateLimiter>> tenant_limiters;
    std::atomic<bool> stopping{false};
    std::atomic<bool> drain_requested{false};
    std::atomic<bool> listen_done{false};
};

std::atomic<bool> g_signal_requested{false};

// 信号处理只置标志，真正的排空在监视线程里做（signal handler 不得做 IO/等待）。
void handle_signal(int) {
    g_signal_requested.store(true, std::memory_order_relaxed);
}

std::int32_t inflight(const SchedulerCore& core) {
    return core.running_count() + core.waiting_count();
}

// 收到 SIGTERM/SIGINT 后：Draining（/readyz 与准入立刻 503）→ 等在飞清零 →
// 超过 drain_deadline_seconds 记 drain_timeout_total 后强制停止。
void drain_loop(const std::shared_ptr<AppContext>& ctx, httplib::Server* data,
                httplib::Server* control) {
    while (!g_signal_requested.load(std::memory_order_relaxed) && !ctx->listen_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!g_signal_requested.load(std::memory_order_relaxed)) {
        return;
    }
    ctx->service.transit(ServiceState::Draining);
    ctx->stopping.store(true);
    logger()->info("draining: 停止准入，等待在飞完成 deadline={}s",
                   ctx->config->drain_deadline_seconds);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(ctx->config->drain_deadline_seconds);
    while (inflight(*ctx->core) > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const bool timed_out = inflight(*ctx->core) > 0;
    if (timed_out) {
        ctx->metrics->inc(MetricId::DrainTimeoutTotal);
        logger()->warn("drain 超时：inflight={}，强制停止", inflight(*ctx->core));
    } else {
        logger()->info("draining: 在飞请求已清零");
    }
    ctx->service.transit(ServiceState::Stopped);
    if (control != nullptr) {
        control->stop();
    }
    if (data != nullptr) {
        data->stop();
    }
}

// 拼接 Allow 头取值：按注册顺序去重列出允许的方法。
std::string join_methods(const std::vector<std::string>& methods) {
    std::string out;
    for (std::size_t index = 0; index < methods.size(); ++index) {
        if (index > 0) {
            out += ", ";
        }
        out += methods[index];
    }
    return out;
}

// 统一错误回包：状态码与 code 由 C-03 映射，禁止各路由自行编造。
// 429 必须带 Retry-After：客户端可据此退避，而不是把队列满误当成执行错误重试。
void send_error(httplib::Response& res, const Error& error) {
    const Error payload = error.ok() ? make_error(ErrCode::ExecutionFailed, "服务内部错误") : error;
    res.status = http_status(payload.code);
    if (res.status == 429) {
        res.set_header("Retry-After", "1");
    }
    res.set_content(render_error_body(payload), "application/json; charset=utf-8");
}

// 把 SSE 帧写到当前连接；写失败即断连，由调用方投递 Cancel。
bool write_frame(httplib::DataSink& sink, const std::string& frame) {
    return sink.write(frame.data(), frame.size());
}

// 非流式：排空 sink 并拼接文本；stop 命中即取消，避免继续消耗算力。
std::string drain_sink(const std::shared_ptr<ResponseSink>& sink, const InferenceRequest& req,
                       SchedulerCore& core, StopFilter& filter, int& gen_tokens,
                       FinishReason& finish, Error& error, std::int32_t deadline_seconds) {
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(deadline_seconds);
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
            error = make_error(ErrCode::Timeout,
                               "等待生成结果超时（" + std::to_string(deadline_seconds) + "s）",
                               "req=" + req.req_id);
            finish = FinishReason::Aborted;
            break;
        }
    }
    return text;
}

// 流式 provider：按需取帧、逐帧写；StopFilter 必须跨 provider 回调存活。
bool stream_provider(const std::shared_ptr<AppContext>& ctx, const InferenceRequest& req,
                     const std::shared_ptr<ResponseSink>& sink, const std::shared_ptr<bool>& done,
                     const std::shared_ptr<StopFilter>& filter, httplib::DataSink& data_sink) {
    if (*done) {
        return false;
    }
    auto record_ok = [&ctx, &req]() {
        if (!req.tenant.empty()) {
            ctx->tenant_metrics->inc_request(req.tenant, "ok");
        }
    };
    auto record_error = [&ctx, &req](const Error* error) {
        if (req.tenant.empty()) {
            return;
        }
        ctx->tenant_metrics->inc_request(req.tenant, "error");
        if (error != nullptr && error->code == ErrCode::QuotaExceeded) {
            ctx->tenant_metrics->inc_rejected(req.tenant, "quota");
        }
    };
    Delta delta;
    for (int index = 0; index < 32 && sink->pop_frame(delta, 20); ++index) {
        bool hit = false;
        Delta out = delta;
        out.text = filter->feed(delta.text, &hit);
        if (delta.finish) {
            out.text += filter->flush();
        }
        if (hit) {
            out.finish = FinishReason::Stopped;
        }
        if (!out.text.empty() || out.finish || out.error) {
            if (!write_frame(data_sink, render_frame(req, out))) {
                ctx->core->cancel(req.req_id);
                record_error(out.error ? &*out.error : nullptr);
                *done = true;
                return false;
            }
        }
        if (hit || out.error) {
            if (hit) {
                ctx->core->cancel(req.req_id);
            }
            // stop/错误同样是终止事件：先写 finish/error 帧，再补 [DONE]，不静默断流。
            if (!write_frame(data_sink, render_done_frame())) {
                ctx->core->cancel(req.req_id);
            }
            data_sink.done();
            *done = true;
            if (out.error) {
                record_error(&*out.error);
            } else {
                record_ok();
            }
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
    record_ok();
    return false;
}

// 解析 → 分词/模板 → 上下文校验 → 入口准入 → 入队；返回 Ok 表示响应已接管。
// 准入在这里判定：队列满直接 429，不再"先 200 建流、再在调度器里静默失败"。
Error submit_request(AppContext& ctx, const std::string& body, bool chat,
                     const std::string& forced_id, InferenceRequest& req,
                     std::shared_ptr<ResponseSink>& sink, int& prompt_tokens) {
    std::string message;
    std::string parse_detail;
    if (!ctx.service.accepting()) {
        return make_error(ErrCode::Unavailable, "服务正在排空，停止接收新请求");
    }
    ErrCode code = chat ? parse_chat_body(body, req, message, &parse_detail)
                        : parse_body(body, req, message, &parse_detail);
    if (code != ErrCode::Ok) {
        return make_error(code, message.empty() ? "请求体非法" : message, parse_detail);
    }
    if (!forced_id.empty()) {
        req.req_id = forced_id;
    }
    if (const ErrCode id_code = validate_request_id(req.req_id, message); id_code != ErrCode::Ok) {
        return make_error(id_code, message);
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
        return make_error(ErrCode::ContextOverflow,
                          "prompt=" + std::to_string(ids.size()) +
                                  " + max_tokens=" + std::to_string(req.sampling.max_tokens) +
                                  " 超过 ctx-tokens=" + std::to_string(limit),
                          "prompt=" + std::to_string(ids.size()) +
                                  " max_tokens=" + std::to_string(req.sampling.max_tokens) +
                                  " ctx=" + std::to_string(limit));
    }
    if (!ctx.core->accepting()) {
        ctx.metrics->inc(MetricId::AdmissionQueue);
        ctx.metrics->inc(MetricId::RequestsQueueFull);
        return make_error(ErrCode::QueueFull,
                          "等待队列已满（" + std::to_string(ctx.core->waiting_count()) + "/" +
                                  std::to_string(ctx.core->admission_capacity()) +
                                  "），请退避后重试",
                          "cap=" + std::to_string(ctx.core->admission_capacity()));
    }
    sink = std::make_shared<BoundedSink>(256);
    prompt_tokens = static_cast<int>(ids.size());
    code = ctx.core->submit(req, std::move(ids), sink);
    if (code != ErrCode::Ok) {
        if (code == ErrCode::QueueFull) {
            // 预留竞争由 Core 记账，避免同一拒绝在两层各加一次。
            return make_error(ErrCode::QueueFull,
                              "等待队列已满（" + std::to_string(ctx.core->waiting_count()) + "/" +
                                      std::to_string(ctx.core->admission_capacity()) +
                                      "），请退避后重试",
                              "cap=" + std::to_string(ctx.core->admission_capacity()));
        }
        if (code == ErrCode::InvalidJson) {
            return make_error(ErrCode::InvalidJson, "id 已被活跃请求占用，请使用新的 id");
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
    auto filter = std::make_shared<StopFilter>(req.stop);
    response.set_chunked_content_provider(
            "text/event-stream; charset=utf-8",
            [ctx, req, sink, done, filter](size_t, httplib::DataSink& data_sink) {
                return stream_provider(ctx, req, sink, done, filter, data_sink);
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
    std::string tenant;
    try {
        ctx->metrics->inc(MetricId::HttpRequests);
        // D6-2/阶段 11：按静态 key 解析 tenant；未知 key 不进入限流与队列。
        const AuthDecision auth = resolve_bearer(request.get_header_value("Authorization"),
                                                 ctx->config->api_keys, ctx->default_quota);
        if (!auth.ok) {
            logger()->warn("http unauthorized path={}", request.path);
            response.set_header("WWW-Authenticate", "Bearer realm=\"llmopt\"");
            send_error(response, make_error(ErrCode::Unauthorized, "缺少或无效的 Bearer token"));
            return;
        }
        tenant = auth.tenant;
        ctx->tenant_metrics->inc_request(tenant, "accepted");
        // 限流先于解析：超额请求既不建 sink 也不占调度预算（C-15）。
        if (ctx->limiter && !ctx->limiter->try_acquire()) {
            ctx->metrics->inc(MetricId::RateLimitedTotal);
            logger()->warn("http rate limited path={} rps={}", request.path, ctx->limiter->rps());
            ctx->tenant_metrics->inc_request(tenant, "error");
            ctx->tenant_metrics->inc_rejected(tenant, "rate_limit");
            send_error(response, make_error(ErrCode::RateLimited, "请求超过限流速率，请退避重试"));
            return;
        }
        const auto tenant_limiter = ctx->tenant_limiters.find(auth.tenant);
        if (tenant_limiter != ctx->tenant_limiters.end() &&
            !tenant_limiter->second->try_acquire()) {
            ctx->metrics->inc(MetricId::RateLimitedTotal);
            logger()->warn("http tenant rate limited path={} tenant={} rps={}", request.path,
                           auth.tenant, tenant_limiter->second->rps());
            ctx->tenant_metrics->inc_request(tenant, "error");
            ctx->tenant_metrics->inc_rejected(tenant, "rate_limit");
            send_error(response,
                       make_error(ErrCode::RateLimited, "租户请求超过限流速率，请退避重试"));
            return;
        }
        std::string request_id;
        if (request.has_header("X-Request-Id")) {
            request_id = request.get_header_value("X-Request-Id");
        }
        InferenceRequest base;
        base.tenant = auth.tenant;
        base.tenant_quota = auth.quota;
        std::shared_ptr<ResponseSink> sink;
        int prompt_tokens = 0;
        const Error submit_error =
                submit_request(*ctx, request.body, chat, request_id, base, sink, prompt_tokens);
        if (!submit_error.ok()) {
            logger()->warn("http reject id={} code={} path={}", base.req_id,
                           error_code_name(submit_error.code), request.path);
            ctx->tenant_metrics->inc_request(tenant, "error");
            ctx->tenant_metrics->inc_rejected(
                    tenant, submit_error.code == ErrCode::QueueFull ? "queue" : "quota");
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
                ctx->tenant_metrics->inc_request(tenant, "error");
                ctx->tenant_metrics->inc_rejected(
                        tenant, error.code == ErrCode::QueueFull ? "queue" : "quota");
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
            const std::string text =
                    drain_sink(sinks[index], reqs[index], *ctx->core, filter, generated, finish,
                               error, ctx->config->request_deadline_seconds);
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
            ctx->tenant_metrics->inc_request(tenant, "error");
            if (first_error.code == ErrCode::QuotaExceeded) {
                ctx->tenant_metrics->inc_rejected(tenant, "quota");
            } else if (first_error.code == ErrCode::QueueFull) {
                ctx->tenant_metrics->inc_rejected(tenant, "queue");
            }
            send_error(response, first_error);
            return;
        }
        response.status = 200;
        ctx->tenant_metrics->inc_request(tenant, "ok");
        response.set_content(render_body(base, texts, finishes, prompt_tokens, gen_tokens),
                             "application/json; charset=utf-8");
        logger()->info("http done id={} finish={} choices={} path={}", base.req_id,
                       finish_reason_name(finishes.empty() ? FinishReason::Error : finishes[0]),
                       texts.size(), request.path);
        responded = true;
    } catch (const std::exception& exc) {
        logger()->error("inference handler 异常: {}", exc.what());
        if (!tenant.empty()) {
            ctx->tenant_metrics->inc_request(tenant, "error");
        }
        for (const std::string& id : submitted) {
            ctx->core->cancel(id);
        }
        if (!responded) {
            send_error(response, make_error(ErrCode::ExecutionFailed, "服务内部异常"));
        }
    } catch (...) {
        logger()->error("inference handler 未知异常");
        if (!tenant.empty()) {
            ctx->tenant_metrics->inc_request(tenant, "error");
        }
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

// 控制面路由只读状态；独立 listener 用独立 ThreadPool，避免被推理长连接占满。
void register_control_routes(httplib::Server& server, const std::shared_ptr<AppContext>& ctx) {
    server.Get("/healthz", [ctx](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"" + std::string(service_state_name(ctx->service.state())) +
                                "\"}",
                        "application/json; charset=utf-8");
    });
    server.Get("/readyz", [ctx](const httplib::Request&, httplib::Response& res) {
        const ServiceState state = ctx->service.state();
        res.status = state == ServiceState::Ready ? 200 : 503;
        res.set_content("{\"status\":\"" + std::string(service_state_name(state)) + "\"}",
                        "application/json; charset=utf-8");
    });
    server.Get("/metrics", [ctx](const httplib::Request&, httplib::Response& res) {
        res.set_content(ctx->metrics->render() +
                                ctx->tenant_metrics->render(ctx->core->tenant_stats()),
                        "text/plain; version=0.0.4");
    });
}

// 数据面可按 TLS 配置选择 SSLServer；控制面始终只绑 loopback，保持明文以满足本机探针。
bool make_data_server(const ServerConfig& config, std::unique_ptr<httplib::Server>& out) {
    if (config.tls_cert_path.empty()) {
        out = std::make_unique<httplib::Server>();
        return true;
    }
#ifdef LLMOPT_HAS_TLS
    auto ssl = std::make_unique<httplib::SSLServer>(config.tls_cert_path.c_str(),
                                                    config.tls_key_path.c_str());
    if (!ssl->is_valid()) {
        logger()->error("TLS 证书或私钥无效：cert={} key={}", config.tls_cert_path,
                        config.tls_key_path);
        return false;
    }
    out = std::move(ssl);
    return true;
#else
    logger()->error("当前构建未启用 TLS；请用 -DLLMOPT_ENABLE_TLS=ON 重新构建");
    return false;
#endif
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
    ctx->default_quota = make_default_tenant_quota(config);
    std::vector<std::string> tenant_ids;
    for (const ApiKeyEntry& entry : config.api_keys) {
        tenant_ids.push_back(entry.id);
    }
    if (tenant_ids.empty()) {
        tenant_ids.push_back("default");
    }
    ctx->tenant_metrics = std::make_unique<TenantMetrics>(tenant_ids);
    for (const ApiKeyEntry& entry : config.api_keys) {
        if (entry.quota.rate_limit_rps > 0.0) {
            ctx->tenant_limiters.emplace(
                    entry.id, std::make_unique<RateLimiter>(entry.quota.rate_limit_rps,
                                                            entry.quota.rate_limit_burst));
        }
    }
    if (config.rate_limit_rps > 0.0) {
        ctx->limiter =
                std::make_unique<RateLimiter>(config.rate_limit_rps, config.rate_limit_burst);
    }

    std::unique_ptr<httplib::Server> server;
    if (!make_data_server(config, server)) {
        return 3;
    }
    // 数据面并发上限与 Scheduler 的活跃序列上限绑定；控制面不再共享这组线程。
    const std::size_t handler_threads = static_cast<std::size_t>(core.concurrency_limit() + 3);
    server->new_task_queue = [handler_threads] { return new httplib::ThreadPool(handler_threads); };
    server->set_payload_max_length(std::size_t{4} * 1024 * 1024);
    server->set_read_timeout(5, 0);
    server->set_write_timeout(120, 0);

    // 已登记路由表：错误处理器据此区分 404（路径未登记）与 405（路径存在、方法不对）。
    std::map<std::string, std::vector<std::string>> route_methods;
    auto register_route = [&route_methods](const char* method, const char* path) {
        route_methods[path].push_back(method);
    };
    register_route("POST", "/v1/completions");
    server->Post("/v1/completions", [ctx](const httplib::Request& req, httplib::Response& res) {
        inference_handler(ctx, req, res, false);
    });
    register_route("POST", "/completion");
    server->Post("/completion", [ctx](const httplib::Request& req, httplib::Response& res) {
        inference_handler(ctx, req, res, false);
    });
    register_route("POST", "/v1/chat/completions");
    server->Post("/v1/chat/completions",
                 [ctx](const httplib::Request& req, httplib::Response& res) {
                     inference_handler(ctx, req, res, true);
                 });
    register_route("GET", "/v1/models");
    server->Get("/v1/models", [ctx](const httplib::Request& req, httplib::Response& res) {
        // DEBUG-11：与推理端点共用同一鉴权解析；只配 key 文件时也必须带合法 key。
        const AuthDecision auth = resolve_bearer(req.get_header_value("Authorization"),
                                                 ctx->config->api_keys, ctx->default_quota);
        if (!auth.ok) {
            logger()->warn("http unauthorized path={}", req.path);
            res.set_header("WWW-Authenticate", "Bearer realm=\"llmopt\"");
            send_error(res, make_error(ErrCode::Unauthorized, "缺少或无效的 Bearer token"));
            return;
        }
        res.set_content(render_models_body(model_id_from_path(ctx->config->model_path)),
                        "application/json; charset=utf-8");
    });
    register_route("GET", "/healthz");
    register_route("GET", "/readyz");
    register_route("GET", "/metrics");
    register_control_routes(*server, ctx);

    // httplib 在 handler 不被调用的出口（413 超限、未匹配路由）直接回默认空体；
    // 这里兜底成统一错误 JSON。已由 handler 回包的 4xx 不覆盖其错误体。
    server->set_error_handler([&route_methods](const httplib::Request& req,
                                               httplib::Response& res) {
        if (!res.body.empty()) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        if (res.status == 413) {
            send_error(res,
                       make_error(ErrCode::PayloadTooLarge, "请求体超过 4 MB 上限，请减小输入体量",
                                  "payload_max=4194304"));
            return httplib::Server::HandlerResponse::Handled;
        }
        if (res.status == 404) {
            const auto it = route_methods.find(req.path);
            if (it != route_methods.end()) {
                const std::string allow = join_methods(it->second);
                res.set_header("Allow", allow);
                send_error(res, make_error(ErrCode::NotSupported, "该端点不支持 " + req.method +
                                                                          " 方法（允许: " + allow +
                                                                          "）"));
                res.status = 405;
            } else {
                send_error(res, make_error(ErrCode::NotSupported, "未登记的端点: " + req.path));
            }
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    g_signal_requested.store(false);
    ctx->listen_done.store(false);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // 独立控制面：只绑 loopback，自己的 ThreadPool；数据面再拥挤也不会占满这组线程。
    httplib::Server control;
    std::thread control_thread;
    const bool control_enabled = config.control_port > 0;
    if (control_enabled) {
        control.new_task_queue = [] { return new httplib::ThreadPool(4); };
        control.set_read_timeout(5, 0);
        control.set_write_timeout(30, 0);
        register_control_routes(control, ctx);
        control.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
            if (res.body.empty()) {
                send_error(res,
                           make_error(ErrCode::NotSupported, "控制面未登记的端点: " + req.path));
            }
            return httplib::Server::HandlerResponse::Handled;
        });
        if (!control.bind_to_port("127.0.0.1", config.control_port)) {
            logger()->error("control-port {} 绑定失败（需 loopback 可用）", config.control_port);
            return 3;
        }
        control_thread = std::thread([&control] { control.listen_after_bind(); });
    }

    if (!server->bind_to_port(config.host, config.port)) {
        logger()->error("listen 失败：端口 {} 可能被占用", config.port);
        if (control_enabled) {
            control.stop();
            control_thread.join();
        }
        return 3;
    }

    ctx->service.transit(ServiceState::Ready);
    logger()->info("listening on {}://{}:{} backend={}",
                   config.tls_cert_path.empty() ? "http" : "https", config.host, config.port,
                   runner.backend_name());
    if (control_enabled) {
        logger()->info("control listening on http://127.0.0.1:{} (isolated pool=4)",
                       config.control_port);
    }
    std::thread drain_thread(
            [ctx, data = server.get(), ctrl = control_enabled ? &control : nullptr] {
                drain_loop(ctx, data, ctrl);
            });
    const bool ok = server->listen_after_bind();
    ctx->listen_done.store(true);
    if (control_enabled) {
        control.stop();
    }
    drain_thread.join();
    if (control_thread.joinable()) {
        control_thread.join();
    }
    ctx->stopping.store(true);
    if (ctx->service.state() == ServiceState::Ready) {
        ctx->service.transit(ServiceState::Draining);
        ctx->service.transit(ServiceState::Stopped);
    } else if (ctx->service.state() == ServiceState::Draining) {
        ctx->service.transit(ServiceState::Stopped);
    }
    if (!ok) {
        logger()->error("listen 失败：端口 {} 可能被占用", config.port);
        return 3;
    }
    logger()->info("server stopped");
    return 0;
}

} // namespace llmopt
