// config.cpp — C-09 优先级 CLI > LLMOPT_* > 默认；阶段 1 不读配置文件。
#include "llmopt/config.hpp"

#include "llmopt/common.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace llmopt {
namespace {

bool env_int(const char* name, std::int32_t& out) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return false;
    }
    int value = 0;
    const auto result = std::from_chars(raw, raw + std::strlen(raw), value);
    if (result.ec != std::errc{} || *result.ptr != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool env_double(const char* name, double& out) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(raw, &end);
    if (end == raw || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool env_bool(const char* name, bool& out) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return false;
    }
    if (std::strcmp(raw, "1") == 0 || std::strcmp(raw, "true") == 0) {
        out = true;
        return true;
    }
    if (std::strcmp(raw, "0") == 0 || std::strcmp(raw, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool parse_int(const std::string& text, std::int32_t& out) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

bool parse_double(const std::string& text, double& out) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

ErrCode apply_arg(ServerConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "--model") {
        cfg.model_path = value;
    } else if (key == "--host") {
        cfg.host = value;
    } else if (key == "--port") {
        if (!parse_int(value, cfg.port)) {
            return ErrCode::InvalidJson;
        }
    } else if (key == "--ctx-tokens") {
        if (!parse_int(value, cfg.ctx_tokens)) {
            return ErrCode::InvalidJson;
        }
    } else if (key == "--max-concurrency") {
        if (!parse_int(value, cfg.max_concurrency)) {
            return ErrCode::InvalidJson;
        }
    } else if (key == "--safety-ratio") {
        if (!parse_double(value, cfg.safety_ratio)) {
            return ErrCode::InvalidJson;
        }
    } else if (key == "--threads") {
        if (!parse_int(value, cfg.threads)) {
            return ErrCode::InvalidJson;
        }
    } else if (key == "--n-gpu-layers") {
        if (!parse_int(value, cfg.n_gpu_layers)) {
            return ErrCode::InvalidJson;
        }
    } else if (key == "--log-level") {
        cfg.log_level = value;
    } else if (key == "--backend") {
        cfg.backend = value;
    } else if (key == "--mode") {
        if (!parse_gate_mode(value, cfg.mode)) {
            return ErrCode::InvalidJson;
        }
    } else if (key == "--allow-force-on") {
        cfg.allow_force_on = true;
    } else {
        return ErrCode::NotSupported;
    }
    return ErrCode::Ok;
}

const char* env_string(const char* name, std::string& out) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return nullptr;
    }
    out = raw;
    return raw;
}

ErrCode load_env(ServerConfig& cfg, std::string& message) {
    (void)env_string("LLMOPT_MODEL", cfg.model_path);
    (void)env_string("LLMOPT_HOST", cfg.host);
    (void)env_string("LLMOPT_LOG_LEVEL", cfg.log_level);
    (void)env_string("LLMOPT_BACKEND", cfg.backend);
    std::int32_t number = 0;
    if (env_int("LLMOPT_PORT", number)) {
        cfg.port = number;
    }
    if (env_int("LLMOPT_CTX_TOKENS", number)) {
        cfg.ctx_tokens = number;
    }
    if (env_int("LLMOPT_MAX_CONCURRENCY", number)) {
        cfg.max_concurrency = number;
    }
    if (env_int("LLMOPT_THREADS", number)) {
        cfg.threads = number;
    }
    if (env_int("LLMOPT_N_GPU_LAYERS", number)) {
        cfg.n_gpu_layers = number;
    }
    double ratio = 0.0;
    if (env_double("LLMOPT_SAFETY_RATIO", ratio)) {
        cfg.safety_ratio = ratio;
    }
    (void)env_bool("LLMOPT_ALLOW_FORCE_ON", cfg.allow_force_on);
    std::string mode;
    if (env_string("LLMOPT_MODE", mode) && !parse_gate_mode(mode, cfg.mode)) {
        message = "LLMOPT_MODE 取值非法";
        return ErrCode::InvalidJson;
    }
    return ErrCode::Ok;
}

ErrCode validate_config(const ServerConfig& cfg, std::string& message) {
    if (cfg.model_path.empty()) {
        message = "缺少 --model 或 LLMOPT_MODEL";
        return ErrCode::PromptTooLong;
    }
    if (cfg.port < 1 || cfg.port > 65535) {
        message = "端口必须在 1..65535";
        return ErrCode::InvalidJson;
    }
    if (cfg.ctx_tokens < 128 || cfg.ctx_tokens > 131072) {
        message = "ctx-tokens 必须在 128..131072";
        return ErrCode::InvalidJson;
    }
    if (cfg.max_concurrency < 1 || cfg.max_concurrency > 64) {
        message = "max-concurrency 必须在 1..64";
        return ErrCode::InvalidJson;
    }
    if (cfg.safety_ratio < 0.80 || cfg.safety_ratio > 0.85) {
        message = "safety-ratio 必须在 0.80..0.85";
        return ErrCode::InvalidJson;
    }
    if (cfg.backend != "llama" && cfg.backend != "stub") {
        message = "backend 必须是 llama 或 stub";
        return ErrCode::InvalidJson;
    }
    if (cfg.backend == "stub") {
        const char* allow = std::getenv("LLMOPT_ALLOW_STUB");
        if (!allow || std::strcmp(allow, "1") != 0) {
            message = "stub 后端仅供测试：需设置 LLMOPT_ALLOW_STUB=1";
            return ErrCode::NotSupported;
        }
    }
    return ErrCode::Ok;
}

} // namespace

bool parse_gate_mode(const std::string& text, GateMode& out) noexcept {
    if (text == "off") {
        out = GateMode::Off;
        return true;
    }
    if (text == "auto") {
        out = GateMode::Auto;
        return true;
    }
    if (text == "force_on") {
        out = GateMode::ForceOn;
        return true;
    }
    return false;
}

const char* server_backend_env() {
    return std::getenv("LLMOPT_ALLOW_STUB");
}

ErrCode load_config(int argc, char** argv, ServerConfig& out, std::string& message) noexcept {
    try {
        out = ServerConfig{};
        if (load_env(out, message) != ErrCode::Ok) {
            return ErrCode::InvalidJson;
        }
        for (int index = 1; index < argc; ++index) {
            const std::string key = argv[index];
            std::string value;
            const bool needs_value = key != "--allow-force-on";
            if (needs_value) {
                if (index + 1 >= argc) {
                    message = "参数 " + key + " 缺少取值";
                    return ErrCode::NotSupported;
                }
                value = argv[++index];
            }
            const ErrCode code = apply_arg(out, key, value);
            if (code != ErrCode::Ok) {
                message = code == ErrCode::NotSupported ? "未知参数 " + key : "参数取值非法 " + key;
                return code;
            }
        }
        const ErrCode validated = validate_config(out, message);
        if (validated != ErrCode::Ok) {
            return validated;
        }
        return ErrCode::Ok;
    } catch (const std::exception& exc) {
        message = std::string("配置解析异常: ") + exc.what();
        return ErrCode::InvalidJson;
    }
}

} // namespace llmopt
