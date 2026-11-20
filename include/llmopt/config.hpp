// config.hpp — C-09 配置优先级 CLI > env(LLMOPT_*) > 自动；阶段 1 不读配置文件。
#ifndef LLMOPT_CONFIG_HPP
#define LLMOPT_CONFIG_HPP

#include "llmopt/common.hpp"
#include "llmopt/gate.hpp"

#include <cstdint>
#include <string>

namespace llmopt {

struct ServerConfig {
    std::string model_path;
    std::string host = "127.0.0.1";
    std::int32_t port = 8080;
    std::int32_t ctx_tokens = 2048;
    std::int32_t max_concurrency = 8;
    GateMode mode = GateMode::Off;
    double safety_ratio = 0.80;
    bool allow_force_on = false;
    std::string log_level = "info";
    std::int32_t threads = 0;
    std::int32_t n_gpu_layers = 0;
    std::string backend = "llama"; // llama|stub；stub 需 LLMOPT_ALLOW_STUB=1
};

// 返回 Ok 或用例错误；message 带具体参数名，main 负责映射退出码。
ErrCode load_config(int argc, char** argv, ServerConfig& out, std::string& message) noexcept;
bool parse_gate_mode(const std::string& text, GateMode& out) noexcept;
const char* server_backend_env();

} // namespace llmopt

#endif // LLMOPT_CONFIG_HPP
