// config.cpp — C-09/R-07/R-08：CLI > env > JSON 文件 > default，逐项记录来源。
#include "llmopt/config.hpp"

#include "llmopt/common.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace llmopt {
namespace {

using json = nlohmann::json;

// 只有一个取值函数的配置项；flag 表示 JSON 里用 bool、CLI 里不带值。
struct FieldSpec {
    const char* key;
    bool flag;
};

constexpr FieldSpec kFields[] = {
        {"model", false},
        {"host", false},
        {"port", false},
        {"ctx-tokens", false},
        {"max-concurrency", false},
        {"max-num-batched-tokens", false},
        {"chunk-size", false},
        {"fairness-aging", true},
        {"aging-threshold-ms", false},
        {"max-consecutive-preemptions", false},
        {"preempt-trace", false},
        {"step-trace", false},
        {"queue-capacity", false},
        {"block-size", false},
        {"watermark-ratio", false},
        {"disable-prefix-cache", true},
        {"mode", false},
        {"safety-ratio", false},
        {"allow-force-on", true},
        {"log-level", false},
        {"threads", false},
        {"n-gpu-layers", false},
        {"backend", false},
        {"rate-limit-rps", false},
        {"rate-limit-burst", false},
        {"drain-deadline-seconds", false},
        {"request-deadline-seconds", false},
        {"api-key", false},
        {"step-trace-rate", false},
};

constexpr const char* kConfigKeys[] = {
        "model",
        "host",
        "port",
        "ctx-tokens",
        "max-concurrency",
        "max-num-batched-tokens",
        "chunk-size",
        "fairness-aging",
        "aging-threshold-ms",
        "max-consecutive-preemptions",
        "preempt-trace",
        "step-trace",
        "queue-capacity",
        "block-size",
        "watermark-ratio",
        "disable-prefix-cache",
        "mode",
        "safety-ratio",
        "allow-force-on",
        "log-level",
        "threads",
        "n-gpu-layers",
        "backend",
        "rate-limit-rps",
        "rate-limit-burst",
        "drain-deadline-seconds",
        "request-deadline-seconds",
        "api-key",
        "step-trace-rate",
};

bool is_flag_key(const std::string& key) {
    for (const FieldSpec& field : kFields) {
        if (key == field.key) {
            return field.flag;
        }
    }
    return false;
}

bool known_key(const std::string& key) {
    for (const char* item : kConfigKeys) {
        if (key == item) {
            return true;
        }
    }
    return false;
}

void record_source(std::vector<ConfigSourceEntry>* sources, const std::string& key,
                   ConfigSource source) {
    if (sources == nullptr) {
        return;
    }
    for (ConfigSourceEntry& entry : *sources) {
        if (entry.key == key) {
            entry.source = source;
            return;
        }
    }
    sources->push_back(ConfigSourceEntry{key, source});
}

void fill_default_sources(std::vector<ConfigSourceEntry>& sources) {
    sources.clear();
    for (const char* key : kConfigKeys) {
        sources.push_back(ConfigSourceEntry{key, ConfigSource::Default});
    }
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

bool parse_bool_text(const std::string& text, bool& out) {
    if (text == "1" || text == "true") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false") {
        out = false;
        return true;
    }
    return false;
}

bool is_sched_key(const std::string& key) {
    return key == "--max-num-batched-tokens" || key == "--queue-capacity" ||
           key == "--block-size" || key == "--watermark-ratio" || key == "--chunk-size" ||
           key == "--aging-threshold-ms" || key == "--max-consecutive-preemptions" ||
           key == "--preempt-trace" || key == "--step-trace" || key == "--disable-prefix-cache" ||
           key == "--fairness-aging";
}

ErrCode apply_sched_arg(ServerConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "--max-num-batched-tokens") {
        return parse_int(value, cfg.max_num_batched_tokens) ? ErrCode::Ok : ErrCode::InvalidJson;
    }
    if (key == "--chunk-size") {
        return parse_int(value, cfg.chunk_size) ? ErrCode::Ok : ErrCode::InvalidJson;
    }
    if (key == "--aging-threshold-ms") {
        return parse_int(value, cfg.aging_threshold_ms) ? ErrCode::Ok : ErrCode::InvalidJson;
    }
    if (key == "--max-consecutive-preemptions") {
        return parse_int(value, cfg.max_consecutive_preemptions) ? ErrCode::Ok
                                                                 : ErrCode::InvalidJson;
    }
    if (key == "--preempt-trace") {
        cfg.preempt_trace_path = value;
        return ErrCode::Ok;
    }
    if (key == "--step-trace") {
        cfg.step_trace_path = value;
        return ErrCode::Ok;
    }
    if (key == "--fairness-aging") {
        cfg.fairness_aging = true;
        return ErrCode::Ok;
    }
    if (key == "--queue-capacity") {
        return parse_int(value, cfg.queue_capacity) ? ErrCode::Ok : ErrCode::InvalidJson;
    }
    if (key == "--block-size") {
        return parse_int(value, cfg.block_tokens) ? ErrCode::Ok : ErrCode::InvalidJson;
    }
    if (key == "--watermark-ratio") {
        return parse_double(value, cfg.watermark_ratio) ? ErrCode::Ok : ErrCode::InvalidJson;
    }
    cfg.enable_prefix_cache = false;
    return ErrCode::Ok;
}

// 基础连接/上下文参数；未知键返回 NotSupported，交给下一组继续判定。
ErrCode apply_core_arg(ServerConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "--model") {
        cfg.model_path = value;
    } else if (key == "--host") {
        cfg.host = value;
    } else if (key == "--port") {
        return parse_int(value, cfg.port) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--ctx-tokens") {
        return parse_int(value, cfg.ctx_tokens) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--max-concurrency") {
        return parse_int(value, cfg.max_concurrency) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--safety-ratio") {
        return parse_double(value, cfg.safety_ratio) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--threads") {
        return parse_int(value, cfg.threads) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--n-gpu-layers") {
        return parse_int(value, cfg.n_gpu_layers) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--donor-slots") {
        return parse_int(value, cfg.donor_slots) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--kv-cache-type") {
        cfg.kv_cache_type = value;
    } else if (key == "--log-level") {
        cfg.log_level = value;
    } else if (key == "--backend") {
        cfg.backend = value;
    } else {
        return ErrCode::NotSupported;
    }
    return ErrCode::Ok;
}

ErrCode apply_gate_arg(ServerConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "--mode") {
        return parse_gate_mode(value, cfg.mode) ? ErrCode::Ok : ErrCode::InvalidJson;
    }
    if (key == "--allow-force-on") {
        cfg.allow_force_on = true;
        return ErrCode::Ok;
    }
    return ErrCode::NotSupported;
}

// 阶段 6 服务边界参数：限流/排空/鉴权/采样；与调度预算解耦。
ErrCode apply_service_arg(ServerConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "--config") {
        cfg.config_path = value;
    } else if (key == "--rate-limit-rps") {
        return parse_double(value, cfg.rate_limit_rps) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--rate-limit-burst") {
        return parse_int(value, cfg.rate_limit_burst) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--drain-deadline-seconds") {
        return parse_int(value, cfg.drain_deadline_seconds) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--request-deadline-seconds") {
        return parse_int(value, cfg.request_deadline_seconds) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else if (key == "--api-key") {
        cfg.api_key = value;
    } else if (key == "--step-trace-rate") {
        return parse_int(value, cfg.step_trace_rate) ? ErrCode::Ok : ErrCode::InvalidJson;
    } else {
        return ErrCode::NotSupported;
    }
    return ErrCode::Ok;
}

ErrCode apply_arg(ServerConfig& cfg, const std::string& key, const std::string& value) {
    if (is_sched_key(key)) {
        return apply_sched_arg(cfg, key, value);
    }
    if (ErrCode code = apply_core_arg(cfg, key, value); code != ErrCode::NotSupported) {
        return code;
    }
    if (ErrCode code = apply_gate_arg(cfg, key, value); code != ErrCode::NotSupported) {
        return code;
    }
    if (ErrCode code = apply_service_arg(cfg, key, value); code != ErrCode::NotSupported) {
        return code;
    }
    return ErrCode::NotSupported;
}

// 调度参数集中校验：预算、chunk、队列、块与 watermark；与上下文上限解耦。
ErrCode validate_sched_config(const ServerConfig& cfg, std::string& message) {
    if (cfg.max_num_batched_tokens < 1 || cfg.max_num_batched_tokens > 8192) {
        message = "max-num-batched-tokens 必须在 1..8192";
        return ErrCode::InvalidJson;
    }
    if (cfg.max_num_batched_tokens < cfg.max_concurrency) {
        message = "max-num-batched-tokens 不能小于 max-concurrency";
        return ErrCode::InvalidJson;
    }
    if (!is_valid_chunk_size(cfg.chunk_size)) {
        message = "chunk-size 只接受 32/64/128/256";
        return ErrCode::InvalidJson;
    }
    if (cfg.queue_capacity < 1 || cfg.queue_capacity > 1024) {
        message = "queue-capacity 必须在 1..1024";
        return ErrCode::InvalidJson;
    }
    if (cfg.block_tokens < 4 || cfg.block_tokens > 128) {
        message = "block-size 必须在 4..128";
        return ErrCode::InvalidJson;
    }
    if (cfg.watermark_ratio < 0.0 || cfg.watermark_ratio > 0.2) {
        message = "watermark-ratio 必须在 0.0..0.2";
        return ErrCode::InvalidJson;
    }
    if (cfg.aging_threshold_ms < 1 || cfg.aging_threshold_ms > 3'600'000) {
        message = "aging-threshold-ms 必须在 1..3600000";
        return ErrCode::InvalidJson;
    }
    if (cfg.max_consecutive_preemptions < 1 || cfg.max_consecutive_preemptions > 32) {
        message = "max-consecutive-preemptions 必须在 1..32";
        return ErrCode::InvalidJson;
    }
    return ErrCode::Ok;
}

ErrCode validate_service_limits(const ServerConfig& cfg, std::string& message) {
    if (cfg.rate_limit_rps < 0.0 || cfg.rate_limit_rps > 1'000'000.0) {
        message = "rate-limit-rps 必须在 0..1000000";
        return ErrCode::InvalidJson;
    }
    if (cfg.rate_limit_burst < 1 || cfg.rate_limit_burst > 1024) {
        message = "rate-limit-burst 必须在 1..1024";
        return ErrCode::InvalidJson;
    }
    if (cfg.drain_deadline_seconds < 1 || cfg.drain_deadline_seconds > 300) {
        message = "drain-deadline-seconds 必须在 1..300";
        return ErrCode::InvalidJson;
    }
    if (cfg.request_deadline_seconds < 1 || cfg.request_deadline_seconds > 3600) {
        message = "request-deadline-seconds 必须在 1..3600";
        return ErrCode::InvalidJson;
    }
    if (cfg.step_trace_rate < 0 || cfg.step_trace_rate > 64) {
        message = "step-trace-rate 必须在 0..64";
        return ErrCode::InvalidJson;
    }
    if (cfg.api_key.size() > 512) {
        message = "api-key 长度不能超过 512";
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
    if (validate_sched_config(cfg, message) != ErrCode::Ok) {
        return ErrCode::InvalidJson;
    }
    if (validate_service_limits(cfg, message) != ErrCode::Ok) {
        return ErrCode::InvalidJson;
    }
    if (cfg.safety_ratio < 0.80 || cfg.safety_ratio > 0.95) {
        message = "safety-ratio 必须在 0.80..0.95";
        return ErrCode::InvalidJson;
    }
    if (cfg.donor_slots < 0 || cfg.donor_slots > 8) {
        message = "donor-slots 必须在 0..8";
        return ErrCode::InvalidJson;
    }
    if (cfg.donor_slots == 0 && cfg.enable_prefix_cache && cfg.n_gpu_layers > 0) {
        message = "donor-slots=0 需同时 --disable-prefix-cache（否则保留序列占满后端槽位）";
        return ErrCode::InvalidJson;
    }
    if (cfg.kv_cache_type != "f16" && cfg.kv_cache_type != "q8_0" && cfg.kv_cache_type != "f32") {
        message = "kv-cache-type 必须是 f16 / q8_0 / f32";
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

const char* env_string(const char* name, std::string& out) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) {
        return nullptr;
    }
    out = raw;
    return raw;
}

ErrCode load_env_strings(ServerConfig& cfg, std::vector<ConfigSourceEntry>* sources) {
    struct StringBinding {
        const char* env;
        const char* key;
        std::string* target;
    };
    const StringBinding strings[] = {
            {"LLMOPT_MODEL", "model", &cfg.model_path},
            {"LLMOPT_HOST", "host", &cfg.host},
            {"LLMOPT_LOG_LEVEL", "log-level", &cfg.log_level},
            {"LLMOPT_BACKEND", "backend", &cfg.backend},
            {"LLMOPT_PREEMPT_TRACE", "preempt-trace", &cfg.preempt_trace_path},
            {"LLMOPT_STEP_TRACE", "step-trace", &cfg.step_trace_path},
            {"LLMOPT_API_KEY", "api-key", &cfg.api_key},
    };
    for (const StringBinding& binding : strings) {
        if (env_string(binding.env, *binding.target) != nullptr) {
            record_source(sources, binding.key, ConfigSource::Env);
        }
    }
    return ErrCode::Ok;
}

ErrCode load_env_ints(ServerConfig& cfg, std::vector<ConfigSourceEntry>* sources,
                      std::string& message) {
    struct IntBinding {
        const char* env;
        const char* key;
        std::int32_t* target;
    };
    const IntBinding ints[] = {
            {"LLMOPT_PORT", "port", &cfg.port},
            {"LLMOPT_CTX_TOKENS", "ctx-tokens", &cfg.ctx_tokens},
            {"LLMOPT_MAX_CONCURRENCY", "max-concurrency", &cfg.max_concurrency},
            {"LLMOPT_MAX_NUM_BATCHED_TOKENS", "max-num-batched-tokens",
             &cfg.max_num_batched_tokens},
            {"LLMOPT_CHUNK_SIZE", "chunk-size", &cfg.chunk_size},
            {"LLMOPT_AGING_THRESHOLD_MS", "aging-threshold-ms", &cfg.aging_threshold_ms},
            {"LLMOPT_MAX_CONSECUTIVE_PREEMPTIONS", "max-consecutive-preemptions",
             &cfg.max_consecutive_preemptions},
            {"LLMOPT_QUEUE_CAPACITY", "queue-capacity", &cfg.queue_capacity},
            {"LLMOPT_BLOCK_SIZE", "block-size", &cfg.block_tokens},
            {"LLMOPT_THREADS", "threads", &cfg.threads},
            {"LLMOPT_N_GPU_LAYERS", "n-gpu-layers", &cfg.n_gpu_layers},
            {"LLMOPT_DONOR_SLOTS", "donor-slots", &cfg.donor_slots},
            {"LLMOPT_RATE_LIMIT_BURST", "rate-limit-burst", &cfg.rate_limit_burst},
            {"LLMOPT_DRAIN_DEADLINE_SECONDS", "drain-deadline-seconds",
             &cfg.drain_deadline_seconds},
            {"LLMOPT_REQUEST_DEADLINE_SECONDS", "request-deadline-seconds",
             &cfg.request_deadline_seconds},
            {"LLMOPT_STEP_TRACE_RATE", "step-trace-rate", &cfg.step_trace_rate},
    };
    for (const IntBinding& binding : ints) {
        const char* raw = std::getenv(binding.env);
        if (raw == nullptr || *raw == '\0') {
            continue;
        }
        std::int32_t value = 0;
        if (!parse_int(raw, value)) {
            message = std::string(binding.env) + " 取值非法";
            return ErrCode::InvalidJson;
        }
        *binding.target = value;
        record_source(sources, binding.key, ConfigSource::Env);
    }
    return ErrCode::Ok;
}

ErrCode load_env_doubles(ServerConfig& cfg, std::vector<ConfigSourceEntry>* sources,
                         std::string& message) {
    struct DoubleBinding {
        const char* env;
        const char* key;
        double* target;
    };
    const DoubleBinding doubles[] = {
            {"LLMOPT_SAFETY_RATIO", "safety-ratio", &cfg.safety_ratio},
            {"LLMOPT_WATERMARK_RATIO", "watermark-ratio", &cfg.watermark_ratio},
            {"LLMOPT_RATE_LIMIT_RPS", "rate-limit-rps", &cfg.rate_limit_rps},
    };
    for (const DoubleBinding& binding : doubles) {
        const char* raw = std::getenv(binding.env);
        if (raw == nullptr || *raw == '\0') {
            continue;
        }
        if (!parse_double(raw, *binding.target)) {
            message = std::string(binding.env) + " 取值非法";
            return ErrCode::InvalidJson;
        }
        record_source(sources, binding.key, ConfigSource::Env);
    }
    return ErrCode::Ok;
}

ErrCode load_env_flag(const char* env, const char* key, bool* target,
                      std::vector<ConfigSourceEntry>* sources, std::string& message) {
    const char* raw = std::getenv(env);
    if (raw == nullptr || *raw == '\0') {
        return ErrCode::Ok;
    }
    bool flag = false;
    if (!parse_bool_text(raw, flag)) {
        message = std::string(env) + " 取值非法";
        return ErrCode::InvalidJson;
    }
    *target = flag;
    record_source(sources, key, ConfigSource::Env);
    return ErrCode::Ok;
}

ErrCode load_env_flags(ServerConfig& cfg, std::vector<ConfigSourceEntry>* sources,
                       std::string& message) {
    bool disable_prefix = false;
    ErrCode code = load_env_flag("LLMOPT_DISABLE_PREFIX_CACHE", "disable-prefix-cache",
                                 &disable_prefix, sources, message);
    if (code != ErrCode::Ok) {
        return code;
    }
    if (const char* raw = std::getenv("LLMOPT_DISABLE_PREFIX_CACHE"); raw != nullptr && *raw) {
        cfg.enable_prefix_cache = !disable_prefix;
    }
    code = load_env_flag("LLMOPT_FAIRNESS_AGING", "fairness-aging", &cfg.fairness_aging, sources,
                         message);
    if (code != ErrCode::Ok) {
        return code;
    }
    code = load_env_flag("LLMOPT_ALLOW_FORCE_ON", "allow-force-on", &cfg.allow_force_on, sources,
                         message);
    if (code != ErrCode::Ok) {
        return code;
    }
    std::string mode;
    if (env_string("LLMOPT_MODE", mode)) {
        if (!parse_gate_mode(mode, cfg.mode)) {
            message = "LLMOPT_MODE 取值非法";
            return ErrCode::InvalidJson;
        }
        record_source(sources, "mode", ConfigSource::Env);
    }
    return ErrCode::Ok;
}

ErrCode load_env(ServerConfig& cfg, std::vector<ConfigSourceEntry>* sources, std::string& message) {
    if (ErrCode code = load_env_strings(cfg, sources); code != ErrCode::Ok) {
        return code;
    }
    if (ErrCode code = load_env_ints(cfg, sources, message); code != ErrCode::Ok) {
        return code;
    }
    if (ErrCode code = load_env_doubles(cfg, sources, message); code != ErrCode::Ok) {
        return code;
    }
    return load_env_flags(cfg, sources, message);
}

// JSON 键转字符串：数字用最小表示，布尔只允许出现在 flag 键。
bool json_value_text(const json& value, std::string& out, bool flag) {
    if (flag) {
        if (!value.is_boolean()) {
            return false;
        }
        out = value.get<bool>() ? "1" : "0";
        return true;
    }
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    if (value.is_number_integer()) {
        out = std::to_string(value.get<long long>());
        return true;
    }
    if (value.is_number_unsigned()) {
        out = std::to_string(value.get<unsigned long long>());
        return true;
    }
    if (value.is_number_float()) {
        out = value.dump();
        return true;
    }
    return false;
}

ErrCode load_config_file(const std::string& path, ServerConfig& cfg,
                         std::vector<ConfigSourceEntry>* sources, std::string& message) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        message = "无法读取配置文件 " + path;
        return ErrCode::NotSupported;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const json parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        message = "配置文件不是合法 JSON 对象：" + path;
        return ErrCode::NotSupported;
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        const std::string key = canonical_config_key(it.key());
        if (key.empty()) {
            message = "配置文件未知键：" + it.key();
            return ErrCode::NotSupported;
        }
        std::string value;
        if (!json_value_text(it.value(), value, is_flag_key(key))) {
            message = "配置文件键类型非法：" + it.key();
            return ErrCode::NotSupported;
        }
        if (it.value().is_boolean() && !it.value().get<bool>() && is_flag_key(key)) {
            record_source(sources, key, ConfigSource::File);
            continue; // false 表示维持默认；disable-prefix-cache=false 即不禁用
        }
        const ErrCode code = apply_arg(cfg, "--" + key, value);
        if (code != ErrCode::Ok) {
            message = "配置文件键取值非法：" + it.key();
            return ErrCode::NotSupported;
        }
        record_source(sources, key, ConfigSource::File);
    }
    // R-08：文件层自身越界即失败，即使后续 CLI 会覆盖也不静默。
    ServerConfig probe = cfg;
    if (probe.model_path.empty()) {
        probe.model_path = "<config-file>";
    }
    probe.backend = "llama"; // stub 放行由环境变量在最终校验处理
    std::string why;
    if (validate_config(probe, why) != ErrCode::Ok) {
        message = "配置文件取值越界：" + why;
        return ErrCode::NotSupported;
    }
    return ErrCode::Ok;
}

struct CliArg {
    std::string key;
    std::string value;
    bool has_value = true;
};

ErrCode parse_argv(int argc, char** argv, std::vector<CliArg>& out, std::string& message) {
    for (int index = 1; index < argc; ++index) {
        CliArg arg;
        arg.key = argv[index];
        const std::string canonical = canonical_config_key(arg.key);
        const bool flag = !canonical.empty() && is_flag_key(canonical);
        arg.has_value = !flag;
        if (arg.has_value) {
            if (index + 1 >= argc) {
                message = "参数 " + arg.key + " 缺少取值";
                return ErrCode::NotSupported;
            }
            arg.value = argv[++index];
        }
        out.push_back(arg);
    }
    return ErrCode::Ok;
}

std::string cli_config_path(const std::vector<CliArg>& args) {
    for (const CliArg& arg : args) {
        if (arg.key == "--config") {
            return arg.value;
        }
    }
    return {};
}

} // namespace

const char* config_source_name(ConfigSource source) noexcept {
    switch (source) {
    case ConfigSource::File:
        return "file";
    case ConfigSource::Env:
        return "env";
    case ConfigSource::Cli:
        return "cli";
    case ConfigSource::Default:
    default:
        return "default";
    }
}

std::string canonical_config_key(const std::string& key) {
    std::string normalized = key;
    if (normalized.rfind("--", 0) == 0) {
        normalized.erase(0, 2);
    }
    std::replace(normalized.begin(), normalized.end(), '_', '-');
    return known_key(normalized) ? normalized : std::string{};
}

bool is_valid_chunk_size(std::int32_t value) noexcept {
    return value == 32 || value == 64 || value == 128 || value == 256;
}

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

ErrCode load_config(int argc, char** argv, ServerConfig& out, std::string& message,
                    std::vector<ConfigSourceEntry>* sources) noexcept {
    try {
        out = ServerConfig{};
        if (sources != nullptr) {
            fill_default_sources(*sources);
        }
        std::vector<CliArg> args;
        const ErrCode argv_code = parse_argv(argc, argv, args, message);
        if (argv_code != ErrCode::Ok) {
            return ErrCode::ConfigInvalid;
        }
        // 文件层先于 env：CLI 的 --config 决定读哪个文件；未给时接受 LLMOPT_CONFIG。
        std::string path = cli_config_path(args);
        if (!path.empty()) {
            record_source(sources, "config", ConfigSource::Cli);
        } else if (const char* raw = std::getenv("LLMOPT_CONFIG"); raw && *raw) {
            path = raw;
            record_source(sources, "config", ConfigSource::Env);
        }
        if (!path.empty()) {
            if (const ErrCode file_code = load_config_file(path, out, sources, message);
                file_code != ErrCode::Ok) {
                return ErrCode::ConfigInvalid;
            }
        }
        if (const ErrCode env_code = load_env(out, sources, message); env_code != ErrCode::Ok) {
            return ErrCode::ConfigInvalid;
        }
        for (const CliArg& arg : args) {
            const ErrCode code = apply_arg(out, arg.key, arg.value);
            if (code != ErrCode::Ok) {
                message = code == ErrCode::NotSupported ? "未知参数 " + arg.key
                                                        : "参数取值非法 " + arg.key;
                return ErrCode::ConfigInvalid;
            }
            std::string canonical = canonical_config_key(arg.key);
            if (!canonical.empty()) {
                record_source(sources, arg.key == "--config" ? "config" : canonical,
                              ConfigSource::Cli);
            }
        }
        const ErrCode validated = validate_config(out, message);
        return validated == ErrCode::Ok ? ErrCode::Ok : ErrCode::ConfigInvalid;
    } catch (const std::exception& exc) {
        message = std::string("配置解析异常: ") + exc.what();
        return ErrCode::ConfigInvalid;
    }
}

// 前缀复用的有效开关：后端不支持共享时请求开启也降级为关闭，避免不可用路径。
bool effective_prefix_cache(bool requested, bool backend_supports) noexcept {
    return requested && backend_supports;
}

namespace {

std::int32_t donor_slots_for(const ServerConfig& cfg) noexcept {
    return (cfg.enable_prefix_cache && cfg.n_gpu_layers > 0) ? cfg.donor_slots : 0;
}

} // namespace

SchedulerConfig make_scheduler_config(const ServerConfig& cfg, std::int32_t concurrency,
                                      bool prefix_cache) {
    SchedulerConfig scheduler_cfg;
    scheduler_cfg.max_concurrency = std::max<std::int32_t>(concurrency, 1);
    scheduler_cfg.max_num_batched_tokens = cfg.max_num_batched_tokens;
    scheduler_cfg.queue_capacity = cfg.queue_capacity;
    scheduler_cfg.enable_prefix_cache = prefix_cache;
    scheduler_cfg.chunk_size = cfg.chunk_size;
    scheduler_cfg.fairness_aging_enabled = cfg.fairness_aging;
    scheduler_cfg.aging_threshold_ns =
            static_cast<std::int64_t>(cfg.aging_threshold_ms) * 1'000'000;
    scheduler_cfg.max_consecutive_preemptions = cfg.max_consecutive_preemptions;
    scheduler_cfg.request_deadline_ns =
            static_cast<std::int64_t>(cfg.request_deadline_seconds) * 1'000'000'000;
    scheduler_cfg.preempt_trace_path = cfg.preempt_trace_path;
    scheduler_cfg.step_trace_path = cfg.step_trace_path;
    return scheduler_cfg;
}

ModelConfig make_model_config(const ServerConfig& cfg, std::int32_t concurrency) {
    ModelConfig model_cfg;
    model_cfg.model_path = cfg.model_path;
    model_cfg.n_ctx = cfg.ctx_tokens;
    model_cfg.n_seq_max = std::max<std::int32_t>(concurrency, 1) + donor_slots_for(cfg);
    model_cfg.n_ctx_total = cfg.ctx_tokens * model_cfg.n_seq_max;
    model_cfg.n_batch = std::max<std::int32_t>(cfg.max_num_batched_tokens, cfg.chunk_size);
    model_cfg.n_ubatch = 512;
    model_cfg.threads = cfg.threads;
    model_cfg.n_gpu_layers = cfg.n_gpu_layers;
    model_cfg.kv_cache_type = cfg.kv_cache_type;
    return model_cfg;
}

KVConfig make_kv_config(const ServerConfig& cfg, std::int32_t concurrency, bool prefix_cache) {
    const std::int32_t tokens = cfg.ctx_tokens * std::max<std::int32_t>(concurrency, 1);
    KVConfig kv_config;
    kv_config.block_tokens = cfg.block_tokens;
    kv_config.num_blocks = std::max<std::int32_t>(tokens / cfg.block_tokens, 1);
    kv_config.watermark_ratio = cfg.watermark_ratio;
    kv_config.enable_prefix_cache = prefix_cache;
    kv_config.max_donors = prefix_cache ? cfg.donor_slots : 0;
    return kv_config;
}

} // namespace llmopt
