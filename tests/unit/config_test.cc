// config_test.cc — C-09 配置优先级与取值校验；stub 放行开关不走默认路径。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#ifdef _WIN32
#include <stdlib.h>
#endif

#include "llmopt/config.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 跨平台改环境变量；空值表示清除。
void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    if (value[0] == '\0') {
        unsetenv(key);
    } else {
        setenv(key, value, 1);
    }
#endif
}

// 清掉上一例留下的变量，避免用例互相污染。
void clear_env() {
    for (const char* key : {"LLMOPT_MODEL",
                            "LLMOPT_CONFIG",
                            "LLMOPT_HOST",
                            "LLMOPT_PORT",
                            "LLMOPT_CTX_TOKENS",
                            "LLMOPT_MAX_CONCURRENCY",
                            "LLMOPT_MODE",
                            "LLMOPT_BACKEND",
                            "LLMOPT_SAFETY_RATIO",
                            "LLMOPT_ALLOW_STUB",
                            "LLMOPT_ALLOW_FORCE_ON",
                            "LLMOPT_MAX_NUM_BATCHED_TOKENS",
                            "LLMOPT_QUEUE_CAPACITY",
                            "LLMOPT_BLOCK_SIZE",
                            "LLMOPT_WATERMARK_RATIO",
                            "LLMOPT_CHUNK_SIZE",
                            "LLMOPT_DISABLE_PREFIX_CACHE",
                            "LLMOPT_FAIRNESS_AGING",
                            "LLMOPT_AGING_THRESHOLD_MS",
                            "LLMOPT_MAX_CONSECUTIVE_PREEMPTIONS",
                            "LLMOPT_PREEMPT_TRACE",
                            "LLMOPT_STEP_TRACE",
                            "LLMOPT_RATE_LIMIT_RPS",
                            "LLMOPT_RATE_LIMIT_BURST",
                            "LLMOPT_DRAIN_DEADLINE_SECONDS",
                            "LLMOPT_API_KEY",
                            "LLMOPT_STEP_TRACE_RATE"}) {
        set_env(key, "");
    }
}

// 把字符串参数转成 argv 形态，调用完即可释放。
ErrCode load(const std::vector<std::string>& args, ServerConfig& cfg, std::string& message,
             std::vector<ConfigSourceEntry>* sources = nullptr) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("llmopt_server"));
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    return load_config(static_cast<int>(argv.size()), argv.data(), cfg, message, sources);
}

std::string write_config(const std::string& name, const std::string& body) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return path.string();
}

const char* source_of(const std::vector<ConfigSourceEntry>& sources, const std::string& key) {
    for (const ConfigSourceEntry& entry : sources) {
        if (entry.key == key) {
            return config_source_name(entry.source);
        }
    }
    return "missing";
}

} // namespace

TEST_CASE("缺 model 返回用法级错误") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({}, cfg, message) == ErrCode::PromptTooLong);
    CHECK(message.find("--model") != std::string::npos);
}

TEST_CASE("CLI 覆盖环境变量，环境变量覆盖默认值") {
    clear_env();
    set_env("LLMOPT_PORT", "9000");
    set_env("LLMOPT_MODE", "auto");
    set_env("LLMOPT_ALLOW_STUB", "1");
    ServerConfig cfg;
    std::string message;
    const ErrCode code =
            load({"--model", "m.gguf", "--port", "8081", "--backend", "stub"}, cfg, message);
    CHECK(code == ErrCode::Ok);
    CHECK(cfg.port == 8081);
    CHECK(cfg.mode == GateMode::Auto);
    CHECK(cfg.backend == "stub");
    clear_env();
}

TEST_CASE("未知参数与缺值都按 NotSupported 处理") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--config", "x.json"}, cfg, message) == ErrCode::NotSupported);
    CHECK(load({"--model"}, cfg, message) == ErrCode::NotSupported);
    CHECK(load({"--model", "m.gguf", "--port", "abc"}, cfg, message) == ErrCode::InvalidJson);
}

TEST_CASE("端口、上下文、并发与安全比例做范围校验") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--port", "0"}, cfg, message) == ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--ctx-tokens", "64"}, cfg, message) == ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--max-concurrency", "65"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--safety-ratio", "0.9"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--mode", "nope"}, cfg, message) == ErrCode::InvalidJson);
}

TEST_CASE("stub 后端必须显式放行") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--backend", "stub"}, cfg, message) == ErrCode::NotSupported);
    set_env("LLMOPT_ALLOW_STUB", "1");
    CHECK(load({"--model", "m.gguf", "--backend", "stub"}, cfg, message) == ErrCode::Ok);
    clear_env();
}

TEST_CASE("gate 模式文本解析只接受三档") {
    GateMode mode = GateMode::Auto;
    CHECK(parse_gate_mode("off", mode));
    CHECK(mode == GateMode::Off);
    CHECK(parse_gate_mode("force_on", mode));
    CHECK(mode == GateMode::ForceOn);
    CHECK_FALSE(parse_gate_mode("ON", mode));
}

TEST_CASE("阶段 2 调度参数默认值与 CLI 覆盖") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf"}, cfg, message) == ErrCode::Ok);
    CHECK(cfg.max_num_batched_tokens == 2048);
    CHECK(cfg.queue_capacity == 32);
    CHECK(cfg.block_tokens == 16);
    CHECK(cfg.watermark_ratio == doctest::Approx(0.05));
    CHECK(cfg.enable_prefix_cache);
    CHECK(cfg.chunk_size == 128);

    CHECK(load({"--model", "m.gguf", "--ctx-tokens", "1024", "--max-num-batched-tokens", "1024",
                "--queue-capacity", "8", "--block-size", "32", "--watermark-ratio", "0.10",
                "--disable-prefix-cache"},
               cfg, message) == ErrCode::Ok);
    CHECK(cfg.max_num_batched_tokens == 1024);
    CHECK(cfg.queue_capacity == 8);
    CHECK(cfg.block_tokens == 32);
    CHECK(cfg.watermark_ratio == doctest::Approx(0.10));
    CHECK_FALSE(cfg.enable_prefix_cache);
}

TEST_CASE("阶段 2 环境变量生效，越界取值被拒") {
    clear_env();
    set_env("LLMOPT_CTX_TOKENS", "512");
    set_env("LLMOPT_MAX_NUM_BATCHED_TOKENS", "512");
    set_env("LLMOPT_CHUNK_SIZE", "32");
    set_env("LLMOPT_QUEUE_CAPACITY", "4");
    set_env("LLMOPT_BLOCK_SIZE", "8");
    set_env("LLMOPT_WATERMARK_RATIO", "0.0");
    set_env("LLMOPT_DISABLE_PREFIX_CACHE", "1");
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf"}, cfg, message) == ErrCode::Ok);
    CHECK(cfg.max_num_batched_tokens == 512);
    CHECK(cfg.chunk_size == 32);
    CHECK(cfg.ctx_tokens == 512);
    CHECK(cfg.queue_capacity == 4);
    CHECK(cfg.block_tokens == 8);
    CHECK_FALSE(cfg.enable_prefix_cache);
    clear_env();

    CHECK(load({"--model", "m.gguf", "--max-num-batched-tokens", "0"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--max-concurrency", "8", "--max-num-batched-tokens", "4"},
               cfg, message) == ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--queue-capacity", "0"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--block-size", "2"}, cfg, message) == ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--watermark-ratio", "0.3"}, cfg, message) ==
          ErrCode::InvalidJson);
}

// 阶段 3 行为变更：chunked prefill 允许 max-num-batched-tokens < ctx-tokens；
// 启动期改为校验 chunk-size 四档，避免非法宽度绕过切块器。
TEST_CASE("chunk-size 只接受四档，预算可小于上下文") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--ctx-tokens", "4096", "--max-num-batched-tokens", "2048"},
               cfg, message) == ErrCode::Ok);
    for (const std::int32_t size : {32, 64, 128, 256}) {
        CHECK(is_valid_chunk_size(size));
        CHECK(load({"--model", "m.gguf", "--chunk-size", std::to_string(size)}, cfg, message) ==
              ErrCode::Ok);
        CHECK(cfg.chunk_size == size);
    }
    for (const std::int32_t size : {0, 16, 48, 512, -1}) {
        CHECK_FALSE(is_valid_chunk_size(size));
        CHECK(load({"--model", "m.gguf", "--chunk-size", std::to_string(size)}, cfg, message) ==
              ErrCode::InvalidJson);
    }
    // CLI 覆盖 env；env 单独生效时同样走四档校验。
    set_env("LLMOPT_CHUNK_SIZE", "32");
    CHECK(load({"--model", "m.gguf", "--chunk-size", "256"}, cfg, message) == ErrCode::Ok);
    CHECK(cfg.chunk_size == 256);
    CHECK(load({"--model", "m.gguf"}, cfg, message) == ErrCode::Ok);
    CHECK(cfg.chunk_size == 32);
    clear_env();
}

TEST_CASE("预算等于并发数可启动，低于并发数拒绝") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--max-concurrency", "8", "--max-num-batched-tokens", "8",
                "--ctx-tokens", "2048"},
               cfg, message) == ErrCode::Ok);
    CHECK(load({"--model", "m.gguf", "--max-concurrency", "8", "--max-num-batched-tokens", "7"},
               cfg, message) == ErrCode::InvalidJson);
}

TEST_CASE("阶段 3 抢占与公平性参数：无值开关、env 与范围校验") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf"}, cfg, message) == ErrCode::Ok);
    CHECK_FALSE(cfg.fairness_aging);
    CHECK(cfg.aging_threshold_ms == 30000);
    CHECK(cfg.max_consecutive_preemptions == 3);
    CHECK(cfg.preempt_trace_path.empty());

    CHECK(load({"--model", "m.gguf", "--fairness-aging", "--aging-threshold-ms", "100",
                "--max-consecutive-preemptions", "5", "--preempt-trace", "trace.csv"},
               cfg, message) == ErrCode::Ok);
    CHECK(cfg.fairness_aging);
    CHECK(cfg.aging_threshold_ms == 100);
    CHECK(cfg.max_consecutive_preemptions == 5);
    CHECK(cfg.preempt_trace_path == "trace.csv");
    CHECK(load({"--model", "m.gguf", "--step-trace", "steps.jsonl"}, cfg, message) == ErrCode::Ok);
    CHECK(cfg.step_trace_path == "steps.jsonl");

    set_env("LLMOPT_FAIRNESS_AGING", "1");
    set_env("LLMOPT_AGING_THRESHOLD_MS", "250");
    set_env("LLMOPT_MAX_CONSECUTIVE_PREEMPTIONS", "4");
    set_env("LLMOPT_PREEMPT_TRACE", "env_trace.csv");
    set_env("LLMOPT_STEP_TRACE", "env_steps.jsonl");
    CHECK(load({"--model", "m.gguf"}, cfg, message) == ErrCode::Ok);
    CHECK(cfg.fairness_aging);
    CHECK(cfg.aging_threshold_ms == 250);
    CHECK(cfg.max_consecutive_preemptions == 4);
    CHECK(cfg.preempt_trace_path == "env_trace.csv");
    CHECK(cfg.step_trace_path == "env_steps.jsonl");
    // CLI 覆盖 env。
    CHECK(load({"--model", "m.gguf", "--aging-threshold-ms", "50", "--max-consecutive-preemptions",
                "2"},
               cfg, message) == ErrCode::Ok);
    CHECK(cfg.aging_threshold_ms == 50);
    CHECK(cfg.max_consecutive_preemptions == 2);
    clear_env();

    CHECK(load({"--model", "m.gguf", "--aging-threshold-ms", "0"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--aging-threshold-ms", "3600001"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--max-consecutive-preemptions", "0"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--max-consecutive-preemptions", "33"}, cfg, message) ==
          ErrCode::InvalidJson);
}

// 回归：llama 纯 CPU 后端没有统一 KV stream，seq_cp 会跨 stream 复制并重置目标
// cells，实测触发 4 条 ExecutionFailed。后端声明不支持共享时必须强制关闭复用。
TEST_CASE("后端不支持共享时前缀复用降级为关闭") {
    CHECK_FALSE(effective_prefix_cache(true, false));
    CHECK_FALSE(effective_prefix_cache(false, true));
    CHECK_FALSE(effective_prefix_cache(false, false));
    CHECK(effective_prefix_cache(true, true));
}

// 6-G2：四层优先级矩阵 8 例，逐项核对值与来源。文件用 snake_case 键，证明别名等价。
TEST_CASE("配置四层优先级矩阵与来源") {
    clear_env();
    const std::string file =
            write_config("llmopt_config_matrix.json",
                         R"({"model":"file.gguf","port":8081,"host":"0.0.0.0","ctx_tokens":512})");
    ServerConfig cfg;
    std::string message;
    std::vector<ConfigSourceEntry> sources;

    // 1) 全默认
    CHECK(load({"--model", "m.gguf"}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 8080);
    CHECK(std::string(source_of(sources, "port")) == "default");

    // 2) 仅文件
    CHECK(load({"--config", file}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 8081);
    CHECK(cfg.host == "0.0.0.0");
    CHECK(cfg.ctx_tokens == 512);
    CHECK(std::string(source_of(sources, "port")) == "file");

    // 3) 仅 env
    set_env("LLMOPT_PORT", "9000");
    CHECK(load({"--model", "m.gguf"}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 9000);
    CHECK(std::string(source_of(sources, "port")) == "env");

    // 4) 仅 CLI
    CHECK(load({"--model", "m.gguf", "--port", "8082"}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 8082);
    CHECK(std::string(source_of(sources, "port")) == "cli");

    // 5) 文件 + env → env
    CHECK(load({"--config", file}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 9000);
    CHECK(std::string(source_of(sources, "port")) == "env");

    // 6) 文件 + CLI → CLI
    CHECK(load({"--config", file, "--port", "8082"}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 8082);
    CHECK(std::string(source_of(sources, "port")) == "cli");

    // 7) env + CLI → CLI
    CHECK(load({"--model", "m.gguf", "--port", "8082"}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 8082);
    CHECK(std::string(source_of(sources, "port")) == "cli");

    // 8) 文件 + env + CLI → CLI
    CHECK(load({"--config", file, "--port", "8082"}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.port == 8082);
    CHECK(std::string(source_of(sources, "port")) == "cli");

    clear_env();
    std::filesystem::remove(file);
}

TEST_CASE("配置文件未知键、越界与坏 JSON 一律拒绝") {
    clear_env();
    const std::string unknown = write_config("llmopt_bad_key.json", R"({"nope":1})");
    const std::string out_of_range = write_config("llmopt_bad_range.json", R"({"port":70000})");
    const std::string broken = write_config("llmopt_broken.json", "{not json");
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--config", unknown}, cfg, message) == ErrCode::NotSupported);
    CHECK(message.find("未知键") != std::string::npos);
    CHECK(load({"--model", "m.gguf", "--config", out_of_range}, cfg, message) ==
          ErrCode::NotSupported);
    CHECK(message.find("越界") != std::string::npos);
    CHECK(load({"--model", "m.gguf", "--config", broken}, cfg, message) == ErrCode::NotSupported);
    for (const std::string& path : {unknown, out_of_range, broken}) {
        std::filesystem::remove(path);
    }
}

// 6-G2 新增项：限流、排空、trace 采样与 api-key 也走同一优先级。
TEST_CASE("服务边界参数四层优先级与范围") {
    clear_env();
    const std::string file =
            write_config("llmopt_service_config.json",
                         R"({"model":"file.gguf","rate_limit_rps":10.0,"rate_limit_burst":16,
                "drain_deadline_seconds":20,"step_trace_rate":8,"api_key":"file-token"})");
    ServerConfig cfg;
    std::string message;
    std::vector<ConfigSourceEntry> sources;
    CHECK(load({"--config", file}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.rate_limit_rps == doctest::Approx(10.0));
    CHECK(cfg.rate_limit_burst == 16);
    CHECK(cfg.drain_deadline_seconds == 20);
    CHECK(cfg.step_trace_rate == 8);
    CHECK(cfg.api_key == "file-token");

    set_env("LLMOPT_RATE_LIMIT_RPS", "20.5");
    set_env("LLMOPT_DRAIN_DEADLINE_SECONDS", "40");
    CHECK(load({"--config", file}, cfg, message, &sources) == ErrCode::Ok);
    CHECK(cfg.rate_limit_rps == doctest::Approx(20.5));
    CHECK(cfg.drain_deadline_seconds == 40);

    CHECK(load({"--config", file, "--rate-limit-rps", "30", "--drain-deadline-seconds", "5"}, cfg,
               message, &sources) == ErrCode::Ok);
    CHECK(cfg.rate_limit_rps == doctest::Approx(30.0));
    CHECK(cfg.drain_deadline_seconds == 5);
    CHECK(std::string(source_of(sources, "rate-limit-rps")) == "cli");
    CHECK(std::string(source_of(sources, "drain-deadline-seconds")) == "cli");

    set_env("LLMOPT_RATE_LIMIT_BURST", "abc");
    CHECK(load({"--model", "m.gguf"}, cfg, message) == ErrCode::InvalidJson);
    clear_env();
    CHECK(load({"--model", "m.gguf", "--drain-deadline-seconds", "0"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--rate-limit-burst", "0"}, cfg, message) ==
          ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--step-trace-rate", "65"}, cfg, message) ==
          ErrCode::InvalidJson);
    std::filesystem::remove(file);
}
