// log.cpp — spdlog 初始化与全局 logger；落盘失败降级 stderr，析构不抛。
#include "llmopt/log.hpp"

#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#include <filesystem>
#include <memory>
#include <string>

namespace llmopt {
namespace {

std::shared_ptr<spdlog::logger> g_logger;

// 把文本级别映射到 spdlog 级别；未知值按 info，避免启动即失败。
spdlog::level::level_enum parse_level(const std::string& level) {
    if (level == "warn") {
        return spdlog::level::warn;
    }
    if (level == "debug") {
        return spdlog::level::debug;
    }
    if (level == "error") {
        return spdlog::level::err;
    }
    return spdlog::level::info;
}

} // namespace

// 初始化：控制台 + 20MB×5 轮转文件；目录不可写时只用控制台。
void init_logging(const std::string& level, const std::string& log_dir) {
    const auto spd_level = parse_level(level);
    try {
        std::filesystem::create_directories(log_dir);
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_dir + "/llmopt.log", 20 * 1024 * 1024, 5);
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        g_logger = std::make_shared<spdlog::logger>("llmopt",
                                                    spdlog::sinks_init_list{console, file_sink});
    } catch (const spdlog::spdlog_ex&) {
        g_logger = spdlog::stdout_color_mt("llmopt");
    }
    g_logger->set_level(spd_level);
    g_logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(g_logger);
}

// 取全局 logger；未初始化时按需建一个控制台 logger，保证调用点不空。
std::shared_ptr<spdlog::logger> logger() {
    if (!g_logger) {
        g_logger = spdlog::stdout_color_mt("llmopt");
        g_logger->set_level(spdlog::level::info);
    }
    return g_logger;
}

// 关闭前冲刷队列；spdlog 全局析构本身不抛，这里只做显式 flush。
void shutdown_logging() {
    if (g_logger) {
        g_logger->flush();
    }
    spdlog::shutdown();
}

} // namespace llmopt
