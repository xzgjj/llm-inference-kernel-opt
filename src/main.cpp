// main.cpp — M10 启动编排：配置 → L2 估算 → 模型加载 → 调度/HTTP → 排空退出。
// 退出码契约：0 正常 / 1 启动失败 / 2 用法 / 3 资源不足 / 4 门控拒绝。
#include "llmopt/config.hpp"
#include "llmopt/crash.hpp"
#include "llmopt/gate.hpp"
#include "llmopt/gguf.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/log.hpp"
#include "llmopt/metrics.hpp"
#include "llmopt/resource.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"
#include "llmopt/server.hpp"
#include "llmopt/thread_guard.hpp"
#include "llmopt/version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "spdlog/spdlog.h"

namespace {

using namespace llmopt; // NOLINT：main 是装配层，短名可读性优先

constexpr std::int32_t kExitOk = 0;
constexpr std::int32_t kExitStartFail = 1;
constexpr std::int32_t kExitUsage = 2;
constexpr std::int32_t kExitResource = 3;
constexpr std::int32_t kExitGate = 4;

#ifndef _WIN32
// 阶段 12 S4：SIGHUP 只置标志；真正重读在 reload 线程里做（handler 不得做 IO/分配）。
std::atomic<bool> g_sighup_reload{false};
void sighup_reload_handler(int) {
    g_sighup_reload.store(true, std::memory_order_relaxed);
}
#endif

// MiB 换算只用于启动摘要，避免把字节数直接怼给人看。
std::int64_t to_mib(std::int64_t bytes) {
    return bytes / (1024LL * 1024);
}

// 高分辨率文件修改时间（纳秒刻度）。libstdc++ 在 Windows 上把
// std::filesystem::last_write_time 截断到整秒，同秒内改写会被永久误判为未变化；
// 这里直接走平台原生 stat/GetFileTime 保留亚秒精度。同一文件两次取值只做相等比较，
// 不依赖 epoch 换算。文件不可读时返回 false。
bool file_mtime_ns(const std::string& path, std::uint64_t& out) noexcept {
#ifdef _WIN32
    const int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        return false;
    }
    std::wstring wpath(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), len);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILETIME ft{};
    const BOOL ok = GetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    out = li.QuadPart * 100ULL; // FILETIME 为 100ns 刻度，换算成纳秒
    return true;
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    out = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1'000'000'000ULL +
          static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
    return true;
#endif
}

// DEBUG-04：Core 线程持有守卫。正常路径与异常展开路径都先 shutdown 再 join；
// 不允许出现 joinable 的 std::thread 在栈展开时析构（那会 std::terminate）。
using CoreThreadGuard = ScopedThread;

// 配置错误到退出码：用法类 2，取值类 1。
std::int32_t exit_for(ErrCode code) {
    if (code == ErrCode::NotSupported || code == ErrCode::PromptTooLong) {
        return kExitUsage;
    }
    return kExitStartFail;
}

// 打印生效配置，保证 CLI/env/file 合并结果可复核（CLI.md §2）。
void print_config(const ServerConfig& cfg) {
    std::cout << "config: model=" << cfg.model_path << " host=" << cfg.host << " port=" << cfg.port
              << " ctx=" << cfg.ctx_tokens << " max_concurrency=" << cfg.max_concurrency
              << " batch_tokens=" << cfg.max_num_batched_tokens << " queue=" << cfg.queue_capacity
              << " batch_policy=" << cfg.batch_policy << " block=" << cfg.block_tokens
              << " chunk=" << cfg.chunk_size << " aging=" << (cfg.fairness_aging ? 1 : 0)
              << " aging_ms=" << cfg.aging_threshold_ms
              << " preempt_cap=" << cfg.max_consecutive_preemptions
              << " watermark=" << cfg.watermark_ratio
              << " prefix_cache=" << (cfg.enable_prefix_cache ? 1 : 0)
              << " mode=" << gate_mode_name(cfg.mode) << " safety_ratio=" << cfg.safety_ratio
              << " backend=" << cfg.backend << " log_level=" << cfg.log_level
              << " rate_rps=" << cfg.rate_limit_rps << " rate_burst=" << cfg.rate_limit_burst
              << " drain_s=" << cfg.drain_deadline_seconds
              << " request_deadline_s=" << cfg.request_deadline_seconds
              << " completion_timeout_ms=" << cfg.completion_timeout_ms
              << " step_trace_rate=" << cfg.step_trace_rate << " control_port=" << cfg.control_port
              << " tls=" << (cfg.tls_cert_path.empty() ? "off" : "on")
              << " capacity_only=" << (cfg.capacity_only ? 1 : 0)
              << " api_key=" << (cfg.api_key.empty() ? "unset" : "set") << "\n";
}

// R-07：逐项打印来源，便于 6-G2 对账；api-key 只打来源，不打印值。
void print_config_sources(const std::vector<ConfigSourceEntry>& sources) {
    for (const ConfigSourceEntry& entry : sources) {
        std::cout << "config source: " << entry.key << "=" << config_source_name(entry.source)
                  << "\n";
    }
    std::cout.flush(); // 子进程被强杀时仍要留下来源证据（6-G2）
}

// L2 估算分项：模型文件 + KV，超预算时打印每一项便于定位；free 取判定时实际使用的值。
void print_budget(const GateResult& gate) {
    const EstBudget& budget = gate.budget;
    std::cout << "estimate: model=" << to_mib(budget.model_bytes)
              << "MiB kv=" << to_mib(budget.kv_bytes)
              << "MiB required=" << to_mib(budget.required_bytes)
              << "MiB free=" << to_mib(budget.free_bytes) << "MiB safety=" << budget.safety_ratio
              << " decision=" << gate_decision_name(gate.decision) << "\n";
    if (budget.host_bytes > 0) {
        std::cout << "estimate-split: model_vram=" << to_mib(budget.model_vram_bytes)
                  << "MiB kv_vram=" << to_mib(budget.kv_vram_bytes)
                  << "MiB host=" << to_mib(budget.host_bytes) << "MiB\n";
    }
    for (const std::string& reason : gate.reasons) {
        std::cout << "gate: " << reason << "\n";
    }
}

// 按 backend 选择实现；未编译 llama 时明确失败而不是假装能跑。
std::unique_ptr<ModelRunner> make_runner(const ServerConfig& cfg, std::string& message) {
    (void)message;
    if (cfg.backend == "stub") {
        return make_stub_runner();
    }
#ifdef LLMOPT_ENABLE_LLAMA
    return make_llama_runner();
#else
    message = "本构建未启用 llama 后端（LLMOPT_ENABLE_LLAMA=OFF），只能使用 --backend stub";
    return nullptr;
#endif
}

// 前缀复用：只有后端确认能安全借用 KV 才启用；请求开启但后端不支持时降级为关闭。
bool resolve_prefix_cache(const ServerConfig& cfg, const ModelRunner& runner) {
    const bool enabled =
            effective_prefix_cache(cfg.enable_prefix_cache, runner.supports_prefix_share());
    if (cfg.enable_prefix_cache && !enabled) {
        std::cout << "前缀复用已关闭：当前后端不能安全借用 KV（逻辑块共享仍需后端复制/预留）\n";
    }
    return enabled;
}

// K-9：门控结论一次落指标；矩阵侧据此判断启动时是否发生降级。
void report_gate_metrics(MetricsRegistry& metrics, const GateResult& gate) {
    switch (gate.decision) {
    case GateDecision::Eligible:
        metrics.inc(MetricId::GateEligible);
        break;
    case GateDecision::Degraded:
        metrics.inc(MetricId::GateDegraded);
        break;
    case GateDecision::Blocked:
        metrics.inc(MetricId::GateBlocked);
        break;
    }
    metrics.set(MetricId::Degraded, gate.downgrade_concurrency ? 1 : 0);
}

// 启动前预算检查：读模型元数据、按 L2 口径估算并返回退出码（0 表示放行）。
std::int32_t gate_startup(const ServerConfig& cfg, std::int32_t requested, std::int32_t& ctx_total,
                          GateResult& gate) {
    GgufMeta meta;
    if (cfg.model_profile_path.empty()) {
        meta = read_gguf_meta(cfg.model_path);
    } else {
        // 容量档位：权重可不在本机，用声明值替换文件元数据；结论只覆盖容量，不覆盖吞吐。
        meta = read_model_profile(cfg.model_profile_path);
        if (meta.ok) {
            std::cout << "capacity-profile: path=" << cfg.model_profile_path
                      << " name=" << meta.architecture << " bytes=" << meta.file_bytes
                      << " layers=" << meta.block_count << "\n";
        }
    }
    if (!meta.ok) {
        std::cerr << "读取模型元数据失败: " << meta.error << "\n";
        return kExitStartFail;
    }
    // KV 预算按 n_seq_max 计，含 donor 槽；与 make_model_config 的 n_ctx_total 保持一致。
    const ModelConfig model_cfg = make_model_config(cfg, requested);
    ctx_total = model_cfg.n_ctx_total;
    EstBudget budget;
    budget.model_bytes = meta.file_bytes;
    budget.kv_bytes = estimate_kv_bytes(meta, ctx_total, cfg.kv_cache_type);
    budget.required_bytes = budget.model_bytes + budget.kv_bytes;
    budget.safety_ratio = cfg.safety_ratio;
    // 部分卸载：按 n_gpu_layers 把权重与 KV 拆成显存/内存两池，其余仍走单池口径。
    // 负值已在配置校验阶段拒绝（DEBUG-03），这里不再做第二种解释。
    const std::int32_t layers = meta.block_count;
    std::int32_t offloaded = cfg.n_gpu_layers;
    if (layers > 0 && offloaded > layers) {
        offloaded = layers;
    }
    if (layers > 0 && offloaded < layers) {
        const double ratio = static_cast<double>(offloaded) / static_cast<double>(layers);
        budget.model_vram_bytes =
                static_cast<std::int64_t>(static_cast<double>(budget.model_bytes) * ratio);
        budget.kv_vram_bytes =
                static_cast<std::int64_t>(static_cast<double>(budget.kv_bytes) * ratio);
        budget.host_bytes = (budget.model_bytes - budget.model_vram_bytes) +
                            (budget.kv_bytes - budget.kv_vram_bytes);
    }
    const CapabilitySnapshot snapshot = probe_capabilities();
    gate = evaluate_gate(snapshot, budget, cfg.mode, cfg.allow_force_on);
    print_budget(gate);
    if (!gate.allow_start) {
        bool over = gate.budget.required_bytes > gate.budget.free_bytes;
        if (gate.budget.host_bytes > 0) {
            const std::int64_t vram_used = gate.budget.model_vram_bytes + gate.budget.kv_vram_bytes;
            over = vram_used > gate.budget.free_bytes ||
                   gate.budget.host_bytes > snapshot.ram_available_bytes;
        }
        return over ? kExitResource : kExitGate;
    }
    return kExitOk;
}

// 关停收尾：清空 donor 缓存并打印块表终态，作为零泄漏与对账证据。
void report_shutdown(KVBlockManager& kv, ModelRunner& runner) {
    for (const BackendId& donor : kv.flush_cache()) {
        runner.release_sequence(donor, 0);
    }
    const KVUsage final_usage = kv.usage();
    std::string invariant_why;
    const bool invariants_ok = kv.check_invariants(invariant_why);
    std::cout << "shutdown: kv_used=" << final_usage.used_blocks
              << " cached=" << final_usage.cached_blocks << " free=" << final_usage.free_blocks
              << " total=" << final_usage.total_blocks << " shared=" << final_usage.shared_blocks
              << " invariants=" << (invariants_ok ? "ok" : invariant_why) << "\n";
}

// 加载失败落地：把适配器保留的 message/detail 打到 stderr，并映射退出码。
std::int32_t report_load_failure(ErrCode code, const Error& error) {
    std::cerr << "模型加载失败: " << (error.message.empty() ? error_code_name(code) : error.message)
              << "\n";
    if (!error.detail.empty()) {
        std::cerr << "  detail: " << error.detail << "\n";
    }
    return code == ErrCode::ResourceInsufficient ? kExitResource : kExitStartFail;
}

// 阶段 12 S4：key 热加载线程。POSIX 由 SIGHUP 置标志触发；其余平台按
// --api-keys-reload-seconds 轮询文件 mtime。失败保留旧快照并累加失败指标。
// 返回的 ScopedThread 在 run() 返回前 reset()，由 RAII 停止并 join。
std::unique_ptr<ScopedThread> start_key_reload(ServerConfig& cfg, MetricsRegistry& metrics,
                                               std::atomic<bool>& reload_stop) {
    const bool reload_enabled = !cfg.api_keys_file.empty() && (cfg.api_keys_reload_seconds > 0
#ifndef _WIN32
                                                               || true // POSIX：轮询周期为 0 时
                                                                       // SIGHUP 仍可触发重读
#endif
                                                              );
    if (!reload_enabled) {
        return nullptr;
    }
#ifndef _WIN32
    struct sigaction sighup_action{};
    sighup_action.sa_handler = sighup_reload_handler;
    sigemptyset(&sighup_action.sa_mask);
    sighup_action.sa_flags = 0;
    sigaction(SIGHUP, &sighup_action, nullptr);
#endif
    std::uint64_t last_mtime_ns = 0;
    bool have_mtime = file_mtime_ns(cfg.api_keys_file, last_mtime_ns);
    auto reload_keys = [&] {
        auto last_check = std::chrono::steady_clock::now();
        logger()->info("api-keys reload 线程启动 interval={}s file={}", cfg.api_keys_reload_seconds,
                       cfg.api_keys_file);
        while (!reload_stop.load(std::memory_order_relaxed)) {
            bool trigger = false;
#ifndef _WIN32
            if (g_sighup_reload.exchange(false, std::memory_order_relaxed)) {
                trigger = true;
            }
#endif
            if (!trigger && cfg.api_keys_reload_seconds > 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_check >= std::chrono::seconds(cfg.api_keys_reload_seconds)) {
                    last_check = now;
                    std::uint64_t mt = 0;
                    const bool ok = file_mtime_ns(cfg.api_keys_file, mt);
                    const bool changed = ok && (!have_mtime || mt != last_mtime_ns);
                    if (changed) {
                        have_mtime = true;
                        last_mtime_ns = mt;
                        trigger = true;
                    }
                }
            }
            if (trigger) {
                std::string reload_message;
                auto next = reload_api_keys(cfg, reload_message);
                if (next) {
                    const std::uint64_t generation = next->generation;
                    cfg.api_keys.store(std::move(next));
                    logger()->info("api-keys 热加载成功 generation={}", generation);
                } else {
                    metrics.inc(MetricId::ApiKeysReloadFailedTotal);
                    logger()->warn("api-keys 热加载失败，保留旧表: {}", reload_message);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    };
    return std::make_unique<ScopedThread>(std::move(reload_keys), [&reload_stop] {
        reload_stop.store(true, std::memory_order_relaxed);
    });
}

// 组装并运行：Gate 通过后才加载权重；HTTP 与调度线程按 http→core→executor 顺序收尾。
// cfg 为非 const 引用：阶段 12 S4 的 reload 线程在运行期原子替换 key 快照。
std::int32_t run(ServerConfig& cfg) {
    const std::int32_t requested = std::max<std::int32_t>(cfg.max_concurrency, 1);
    std::int32_t ctx_total = 0;
    GateResult gate;
    const std::int32_t gate_rc = gate_startup(cfg, requested, ctx_total, gate);
    if (gate_rc != kExitOk) {
        if (cfg.capacity_only) {
            std::cout << "capacity-only: allow_start=0\n";
        }
        return gate_rc;
    }
    if (cfg.capacity_only) {
        // 只验证“这档模型 + 这张卡 + 这组参数”是否进得了预算；不加载权重、不监听端口。
        std::cout << "capacity-only: allow_start=1\n";
        return kExitOk;
    }
    // K-9：门控结论一次落指标，矩阵侧可直接对账启动时是否发生降级。
    MetricsRegistry metrics;
    report_gate_metrics(metrics, gate);
    // K-8：采样线程在 load_model 之前起，峰值才包含权重落地的那一段。
    ResourceSampler sampler(metrics);
    sampler.start();
    std::string message;
    std::unique_ptr<ModelRunner> runner = make_runner(cfg, message);
    if (!runner) {
        std::cerr << message << "\n";
        return kExitStartFail;
    }
    std::int32_t concurrency = requested;
    if (gate.downgrade_concurrency) {
        concurrency = std::max<std::int32_t>(1, cfg.max_concurrency / 2);
        std::cout << "FORCE_ON 横幅：max_concurrency 降至 " << concurrency << "\n";
    }
    const ModelConfig model_cfg = make_model_config(cfg, concurrency);
    const ErrCode load_code = runner->load_model(model_cfg);
    if (load_code != ErrCode::Ok) {
        // 适配器保留的原始 cause 在这里落地，启动失败不再只有一句"模型加载失败"。
        return report_load_failure(load_code, runner->last_error());
    }
    const bool prefix_cache = resolve_prefix_cache(cfg, *runner);
    KVBlockManager kv(make_kv_config(cfg, concurrency, prefix_cache));
    SchedulerCore core(*runner, kv, metrics, make_scheduler_config(cfg, concurrency, prefix_cache));

    // DEBUG-04：core 线程的 join 必须是 RAII——run_server 抛异常时先 shutdown 再 join，
    // 否则 std::thread 在展开路径析构会直接 terminate，退出码契约失效。
    CoreThreadGuard core_guard([&core] { core.run(); }, [&core] { core.shutdown(); });

    // 阶段 12 S4：key 热加载（POSIX SIGHUP / 其他平台轮询），RAII 保证 run() 前 join。
    std::atomic<bool> reload_stop{false};
    auto reload_guard = start_key_reload(cfg, metrics, reload_stop);

    const std::int32_t rc = run_server(cfg, *runner, metrics, core, gate);
    reload_guard.reset();
    core_guard.stop_and_join();
    // 关停后显式清空 donor 缓存再释放后端，日志里的 used/free 是“零泄漏”证据。
    report_shutdown(kv, *runner);
    runner->release();
    return rc;
}

} // namespace

// 入口：先解析配置再初始化日志，保证日志级别尽早生效。
int main(int argc, char** argv) {
    // S6：--version 在 load_config 之前扫 argv；只输出版本一行后退出 0。
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--version") {
            std::cout << "llmopt " << llmopt::kVersion << " (llama revision "
                      << llmopt::kLlamaRevision << ")\n";
            return kExitOk;
        }
    }
    llmopt::ServerConfig cfg;
    std::string message;
    std::vector<llmopt::ConfigSourceEntry> sources;
    const llmopt::ErrCode code = llmopt::load_config(argc, argv, cfg, message, &sources);
    if (code != llmopt::ErrCode::Ok) {
        std::cerr << "配置错误: " << message << "\n";
        return exit_for(code);
    }
    llmopt::init_logging(cfg.log_level);
    // S5：崩溃信号捕获；dump 与运行日志同根目录（logs/crash）。
    llmopt::install_crash_handler("logs/server");
    print_config(cfg);
    print_config_sources(sources);
    // 启动边界：线程创建、模型加载、监听阶段的异常在这里统一转退出码，不让它逃逸成 terminate。
    std::int32_t rc = kExitStartFail;
    try {
        rc = run(cfg);
    } catch (const std::exception& exc) {
        std::cerr << "启动失败: " << exc.what() << "\n";
        rc = kExitStartFail;
    } catch (...) {
        std::cerr << "启动失败: 未知异常\n";
        rc = kExitStartFail;
    }
    llmopt::shutdown_logging();
    return rc;
}
