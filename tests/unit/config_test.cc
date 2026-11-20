// config_test.cc — C-09 配置优先级与取值校验；stub 放行开关不走默认路径。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#ifdef _WIN32
#include <stdlib.h>
#endif

#include "llmopt/config.hpp"

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
    for (const char* key :
         {"LLMOPT_MODEL", "LLMOPT_HOST", "LLMOPT_PORT", "LLMOPT_CTX_TOKENS",
          "LLMOPT_MAX_CONCURRENCY", "LLMOPT_MODE", "LLMOPT_BACKEND", "LLMOPT_SAFETY_RATIO",
          "LLMOPT_ALLOW_STUB", "LLMOPT_ALLOW_FORCE_ON", "LLMOPT_MAX_NUM_BATCHED_TOKENS",
          "LLMOPT_QUEUE_CAPACITY", "LLMOPT_BLOCK_SIZE", "LLMOPT_WATERMARK_RATIO",
          "LLMOPT_DISABLE_PREFIX_CACHE"}) {
        set_env(key, "");
    }
}

// 把字符串参数转成 argv 形态，调用完即可释放。
ErrCode load(const std::vector<std::string>& args, ServerConfig& cfg, std::string& message) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("llmopt_server"));
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    return load_config(static_cast<int>(argv.size()), argv.data(), cfg, message);
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
    set_env("LLMOPT_QUEUE_CAPACITY", "4");
    set_env("LLMOPT_BLOCK_SIZE", "8");
    set_env("LLMOPT_WATERMARK_RATIO", "0.0");
    set_env("LLMOPT_DISABLE_PREFIX_CACHE", "1");
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf"}, cfg, message) == ErrCode::Ok);
    CHECK(cfg.max_num_batched_tokens == 512);
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

// 回归：预算小于 ctx 时，长 prompt 在队首反复 break、既不推进也不拒绝，会挂死并
// 队头阻塞；阶段 2 无 chunked prefill，启动期必须拒绝该组合。
TEST_CASE("批 token 预算不得小于上下文长度") {
    clear_env();
    ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--ctx-tokens", "4096", "--max-num-batched-tokens", "2048"},
               cfg, message) == ErrCode::InvalidJson);
    CHECK(load({"--model", "m.gguf", "--ctx-tokens", "1024", "--max-num-batched-tokens", "2048"},
               cfg, message) == ErrCode::Ok);
}

// 回归：llama 纯 CPU 后端没有统一 KV stream，seq_cp 会跨 stream 复制并重置目标
// cells，实测触发 4 条 ExecutionFailed。后端声明不支持共享时必须强制关闭复用。
TEST_CASE("后端不支持共享时前缀复用降级为关闭") {
    CHECK_FALSE(effective_prefix_cache(true, false));
    CHECK_FALSE(effective_prefix_cache(false, true));
    CHECK_FALSE(effective_prefix_cache(false, false));
    CHECK(effective_prefix_cache(true, true));
}
