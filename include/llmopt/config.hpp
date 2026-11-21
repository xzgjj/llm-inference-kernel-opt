// config.hpp — C-09 配置优先级 CLI > env(LLMOPT_*) > file > default（阶段 6 起读 --config）。
#ifndef LLMOPT_CONFIG_HPP
#define LLMOPT_CONFIG_HPP

#include "llmopt/common.hpp"
#include "llmopt/gate.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace llmopt {

// R-07：每个生效字段的来源，启动时逐项打印；与 SPEC-6 6-G2 对齐。
enum class ConfigSource : std::uint8_t { Default, File, Env, Cli };

struct ConfigSourceEntry {
    std::string key; // 与 CLI 同名的规范键，不含 "--"
    ConfigSource source = ConfigSource::Default;
};

const char* config_source_name(ConfigSource source) noexcept;

struct ServerConfig {
    std::string config_path;
    std::string model_path;
    std::string host = "127.0.0.1";
    std::int32_t port = 8080;
    std::int32_t ctx_tokens = 2048;
    std::int32_t max_concurrency = 8;
    std::int32_t max_num_batched_tokens = 2048;
    std::int32_t chunk_size = 128;
    bool fairness_aging = false;
    std::int32_t aging_threshold_ms = 30000;
    std::int32_t max_consecutive_preemptions = 3;
    std::string preempt_trace_path;
    std::string step_trace_path; // 步级 JSONL；p99 只离线复算，不在线聚合
    std::int32_t queue_capacity = 32;
    std::int32_t block_tokens = 16;
    double watermark_ratio = 0.05;
    bool enable_prefix_cache = true;
    GateMode mode = GateMode::Off;
    double safety_ratio = 0.80;
    bool allow_force_on = false;
    std::string log_level = "info";
    std::int32_t threads = 0;
    std::int32_t n_gpu_layers = 0;
    std::string backend = "llama"; // llama|stub；stub 需 LLMOPT_ALLOW_STUB=1
    // 阶段 6 服务边界：限流、排空、鉴权与每步 trace 采样。
    double rate_limit_rps = 0.0; // 0=关
    std::int32_t rate_limit_burst = 32;
    std::int32_t drain_deadline_seconds = 30;
    // 请求级总时限：从 submit 入队起算，流式/非流式共用；超时由 Scheduler 收尾。
    std::int32_t request_deadline_seconds = 120;
    std::string api_key;              // 空=不鉴权；不写日志、不打印
    std::int32_t step_trace_rate = 0; // 1/N 采样；0=关
};

// 返回 Ok 或用例错误；message 带具体参数名，main 负责映射退出码。
// sources 非空时填满每个字段的来源；配置错误止于第一个非法项。
ErrCode load_config(int argc, char** argv, ServerConfig& out, std::string& message,
                    std::vector<ConfigSourceEntry>* sources = nullptr) noexcept;
bool parse_gate_mode(const std::string& text, GateMode& out) noexcept;
// chunk 宽度只允许 32/64/128/256；CLI、env 与调度配置共用同一判定。
bool is_valid_chunk_size(std::int32_t value) noexcept;
// 前缀复用只有在后端能安全借用 KV 时才启用；请求开启但后端不支持时降级为关闭。
bool effective_prefix_cache(bool requested, bool backend_supports) noexcept;
const char* server_backend_env();
// JSON 配置键（kebab 或 snake）到规范 CLI 键；未知键返回空。供加载与打印共用。
std::string canonical_config_key(const std::string& key);

// 单一真源派生：main 不再逐字段手工搬运，默认值/单位换算只在这里定义一次。
[[nodiscard]] SchedulerConfig make_scheduler_config(const ServerConfig& cfg,
                                                    std::int32_t concurrency, bool prefix_cache);
[[nodiscard]] ModelConfig make_model_config(const ServerConfig& cfg, std::int32_t concurrency);
[[nodiscard]] KVConfig make_kv_config(const ServerConfig& cfg, std::int32_t concurrency,
                                      bool prefix_cache);

} // namespace llmopt

#endif // LLMOPT_CONFIG_HPP
