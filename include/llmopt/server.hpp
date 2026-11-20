// server.hpp — HTTP 接入层入口；只做解析、准入与写回，不碰 context/KV。
#ifndef LLMOPT_SERVER_HPP
#define LLMOPT_SERVER_HPP

#include "llmopt/config.hpp"
#include "llmopt/gate.hpp"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"

namespace llmopt {

// 阻塞运行到 SIGINT/SIGTERM；返回 0 正常，3 端口/监听失败。
int run_server(const ServerConfig& config, ModelRunner& runner, MetricsRegistry& metrics,
               SchedulerCore& core, const GateResult& gate);

} // namespace llmopt

#endif // LLMOPT_SERVER_HPP
