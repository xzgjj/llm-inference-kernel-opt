// log.hpp — spdlog 门面；未初始化时降级 stderr，析构不抛。
#ifndef LLMOPT_LOG_HPP
#define LLMOPT_LOG_HPP

#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace llmopt {

void init_logging(const std::string& level, const std::string& log_dir = "logs/server");
std::shared_ptr<spdlog::logger> logger();
void shutdown_logging();

} // namespace llmopt

#endif // LLMOPT_LOG_HPP
