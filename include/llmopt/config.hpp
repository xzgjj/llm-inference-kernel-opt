// config.hpp — C-09 配置优先级 CLI > env(LLMOPT_*) > file > default（阶段 6 起读 --config）。
#ifndef LLMOPT_CONFIG_HPP
#define LLMOPT_CONFIG_HPP

#include "llmopt/common.hpp"
#include "llmopt/gate.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"
#include "llmopt/tenant.hpp"

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
    // continuous=完成即补位（默认）；static=整批跑空才准入，只用于批次策略对照实验。
    std::string batch_policy = "continuous";
    std::int32_t block_tokens = 16;
    double watermark_ratio = 0.05;
    bool enable_prefix_cache = true;
    GateMode mode = GateMode::Off;
    double safety_ratio = 0.80;
    bool allow_force_on = false;
    std::string log_level = "info";
    std::int32_t threads = 0;
    std::int32_t n_gpu_layers = 0;
    // 容量档位：权重未落盘时用声明的 GGUF 尺寸/层数替代文件元数据，只做启动判据。
    std::string model_profile_path;
    // 只跑容量判据：打印 estimate/decision 后退出，不加载权重、不监听端口。
    bool capacity_only = false;
    // 前缀 donor 槽条数（每条按 ctx_tokens 预留 KV）；8B 以上建议 0–1，默认 4 沿用旧口径。
    std::int32_t donor_slots = 4;
    // KV 缓存类型：f16（默认）/ q8_0（减半，8B 以上档推荐）/ f32。
    std::string kv_cache_type = "f16";
    std::string backend = "llama"; // llama|stub；stub 需 LLMOPT_ALLOW_STUB=1
    // 阶段 6 服务边界：限流、排空、鉴权与每步 trace 采样。
    double rate_limit_rps = 0.0; // 0=关
    std::int32_t rate_limit_burst = 32;
    std::int32_t drain_deadline_seconds = 30;
    // 请求级总时限：从 submit 入队起算，流式/非流式共用；超时由 Scheduler 收尾。
    std::int32_t request_deadline_seconds = 120;
    // 单步 Completion 回投超时；0=关闭。默认 30s，只影响“执行线程不回投”的故障路径。
    std::int32_t completion_timeout_ms = 30'000;
    std::string api_key; // 空=不鉴权；不写日志、不打印
    // 阶段 11：静态 key 文件（只存 sha256）；与 api_key 互斥，加载后统一走 api_keys 快照。
    std::string api_keys_file;
    KeyTable api_keys;
    // 阶段 12 S4：key 文件热加载轮询周期（秒）；0=关闭。POSIX 另可由 SIGHUP 触发重读。
    std::int32_t api_keys_reload_seconds = 0;
    std::int32_t step_trace_rate = 0; // 1/N 采样；0=关
    // 控制面隔离：>0 时在 loopback 上另起一个独立线程池的 /healthz /readyz /metrics。
    // 0 保持旧行为（控制端点仍挂在数据面端口，兼容现有客户端）。
    std::int32_t control_port = 0;
    // TLS：两项必须同时设置；空=明文 HTTP。仅 LLMOPT_ENABLE_TLS=ON 的构建可用。
    std::string tls_cert_path;
    std::string tls_key_path;
};

// 返回 Ok 或用例错误；message 带具体参数名，main 负责映射退出码。
// sources 非空时填满每个字段的来源；配置错误止于第一个非法项。
ErrCode load_config(int argc, char** argv, ServerConfig& out, std::string& message,
                    std::vector<ConfigSourceEntry>* sources = nullptr) noexcept;
// 阶段 12 S4：重读 api-keys 文件；成功返回 generation 递增的新快照，失败返回 nullptr 并填 message。
[[nodiscard]] std::shared_ptr<const KeySnapshot> reload_api_keys(const ServerConfig& cfg,
                                                                 std::string& message) noexcept;
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
// 未配置 key 文件/单 token 时的默认租户配额：沿用全局预算，KV 不额外设限。
[[nodiscard]] TenantQuota make_default_tenant_quota(const ServerConfig& cfg);

} // namespace llmopt

#endif // LLMOPT_CONFIG_HPP
