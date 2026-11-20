// main.cpp — M10 启动编排：配置 → L2 估算 → 模型加载 → 调度/HTTP → 排空退出。
// 退出码契约：0 正常 / 1 启动失败 / 2 用法 / 3 资源不足 / 4 门控拒绝。
#include "llmopt/config.hpp"
#include "llmopt/gate.hpp"
#include "llmopt/gguf.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/log.hpp"
#include "llmopt/metrics.hpp"
#include "llmopt/resource.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"
#include "llmopt/server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace llmopt; // NOLINT：main 是装配层，短名可读性优先

constexpr std::int32_t kExitOk = 0;
constexpr std::int32_t kExitStartFail = 1;
constexpr std::int32_t kExitUsage = 2;
constexpr std::int32_t kExitResource = 3;
constexpr std::int32_t kExitGate = 4;
constexpr std::int32_t kDonorSlots = 4; // 前缀缓存最多保留的 donor 序列槽

// MiB 换算只用于启动摘要，避免把字节数直接怼给人看。
std::int64_t to_mib(std::int64_t bytes) {
    return bytes / (1024LL * 1024);
}

// 配置错误到退出码：用法类 2，取值类 1。
std::int32_t exit_for(ErrCode code) {
    if (code == ErrCode::NotSupported || code == ErrCode::PromptTooLong) {
        return kExitUsage;
    }
    return kExitStartFail;
}

// 打印生效配置，保证 CLI/env 合并结果可复核（CLI.md §2）。
void print_config(const ServerConfig& cfg) {
    std::cout << "config: model=" << cfg.model_path << " host=" << cfg.host << " port=" << cfg.port
              << " ctx=" << cfg.ctx_tokens << " max_concurrency=" << cfg.max_concurrency
              << " batch_tokens=" << cfg.max_num_batched_tokens << " queue=" << cfg.queue_capacity
              << " block=" << cfg.block_tokens << " chunk=" << cfg.chunk_size
              << " aging=" << (cfg.fairness_aging ? 1 : 0) << " aging_ms=" << cfg.aging_threshold_ms
              << " preempt_cap=" << cfg.max_consecutive_preemptions
              << " watermark=" << cfg.watermark_ratio
              << " prefix_cache=" << (cfg.enable_prefix_cache ? 1 : 0)
              << " mode=" << gate_mode_name(cfg.mode) << " safety_ratio=" << cfg.safety_ratio
              << " backend=" << cfg.backend << " log_level=" << cfg.log_level << "\n";
}

// L2 估算分项：模型文件 + KV，超预算时打印每一项便于定位；free 取判定时实际使用的值。
void print_budget(const GateResult& gate) {
    const EstBudget& budget = gate.budget;
    std::cout << "estimate: model=" << to_mib(budget.model_bytes)
              << "MiB kv=" << to_mib(budget.kv_bytes)
              << "MiB required=" << to_mib(budget.required_bytes)
              << "MiB free=" << to_mib(budget.free_bytes) << "MiB safety=" << budget.safety_ratio
              << " decision=" << gate_decision_name(gate.decision) << "\n";
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

// donor 槽只在 GPU offload（统一 KV stream）且请求了前缀复用时才需要；
// CPU 后端不支持零拷贝共享，前缀缓存会被降级关闭，多留 4 个槽只会稀释单序列上下文。
std::int32_t donor_slots_for(const ServerConfig& cfg) {
    return (cfg.enable_prefix_cache && cfg.n_gpu_layers > 0) ? kDonorSlots : 0;
}

// 组装后端配置：n_ctx_total 按“每序列上下文 × 全部序列槽”计，donor 槽也各占一份，
// 否则 llama.cpp 会把 n_ctx_total 均分给 n_seq_max，单序列有效上下文被 donor 稀释。
ModelConfig build_model_config(const ServerConfig& cfg, std::int32_t concurrency) {
    ModelConfig model_cfg;
    model_cfg.model_path = cfg.model_path;
    model_cfg.n_ctx = cfg.ctx_tokens;
    model_cfg.n_seq_max = concurrency + donor_slots_for(cfg);
    model_cfg.n_ctx_total = cfg.ctx_tokens * model_cfg.n_seq_max;
    // 单步物理批只需容纳调度预算；不再按 ctx 放大，避免大上下文配置白占缓冲。
    model_cfg.n_batch = std::max<std::int32_t>(cfg.max_num_batched_tokens, cfg.chunk_size);
    model_cfg.n_ubatch = 512;
    model_cfg.threads = cfg.threads;
    model_cfg.n_gpu_layers = cfg.n_gpu_layers;
    return model_cfg;
}

// 后端不支持零拷贝共享时把前缀复用降级为关闭，并明确告知，避免静默走不可用路径。
bool resolve_prefix_cache(const ServerConfig& cfg, const ModelRunner& runner) {
    const bool enabled =
            effective_prefix_cache(cfg.enable_prefix_cache, runner.supports_prefix_share());
    if (cfg.enable_prefix_cache && !enabled) {
        std::cout << "前缀复用已关闭：当前后端不支持零拷贝 KV 共享\n";
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
    const GgufMeta meta = read_gguf_meta(cfg.model_path);
    if (!meta.ok) {
        std::cerr << "读取模型元数据失败: " << meta.error << "\n";
        return kExitStartFail;
    }
    // KV 预算按 n_seq_max 计，含 donor 槽；与 build_model_config 的 n_ctx_total 保持一致。
    const std::int32_t kv_slots = requested + donor_slots_for(cfg);
    ctx_total = cfg.ctx_tokens * kv_slots;
    EstBudget budget;
    budget.model_bytes = meta.file_bytes;
    budget.kv_bytes = estimate_kv_bytes(meta, ctx_total);
    budget.required_bytes = budget.model_bytes + budget.kv_bytes;
    budget.safety_ratio = cfg.safety_ratio;
    const CapabilitySnapshot snapshot = probe_capabilities();
    gate = evaluate_gate(snapshot, budget, cfg.mode, cfg.allow_force_on);
    print_budget(gate);
    if (!gate.allow_start) {
        return gate.budget.required_bytes > gate.budget.free_bytes ? kExitResource : kExitGate;
    }
    return kExitOk;
}

// 关停收尾：清空 donor 缓存并打印块表终态，作为零泄漏与对账证据。
void report_shutdown(KVBlockManager& kv, ModelRunner& runner) {
    for (const RequestId& donor : kv.flush_cache()) {
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

// 组装并运行：Gate 通过后才加载权重；HTTP 与调度线程按 http→core→executor 顺序收尾。
std::int32_t run(const ServerConfig& cfg) {
    const std::int32_t requested = std::max<std::int32_t>(cfg.max_concurrency, 1);
    std::int32_t ctx_total = 0;
    GateResult gate;
    const std::int32_t gate_rc = gate_startup(cfg, requested, ctx_total, gate);
    if (gate_rc != kExitOk) {
        return gate_rc;
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
    const std::int32_t kv_tokens = cfg.ctx_tokens * concurrency;
    const ModelConfig model_cfg = build_model_config(cfg, concurrency);
    const ErrCode load_code = runner->load_model(model_cfg);
    if (load_code != ErrCode::Ok) {
        // 适配器保留的原始 cause 在这里落地，启动失败不再只有一句"模型加载失败"。
        return report_load_failure(load_code, runner->last_error());
    }

    const std::int32_t blocks = std::max<std::int32_t>(kv_tokens / cfg.block_tokens, 1);
    const bool prefix_cache = resolve_prefix_cache(cfg, *runner);
    KVBlockManager kv(
            KVConfig{cfg.block_tokens, blocks, cfg.watermark_ratio, prefix_cache, kDonorSlots});
    SchedulerConfig scheduler_cfg;
    scheduler_cfg.max_concurrency = concurrency;
    scheduler_cfg.max_num_batched_tokens = cfg.max_num_batched_tokens;
    scheduler_cfg.queue_capacity = cfg.queue_capacity;
    scheduler_cfg.enable_prefix_cache = prefix_cache;
    scheduler_cfg.chunk_size = cfg.chunk_size;
    scheduler_cfg.fairness_aging_enabled = cfg.fairness_aging;
    scheduler_cfg.aging_threshold_ns =
            static_cast<std::int64_t>(cfg.aging_threshold_ms) * 1'000'000;
    scheduler_cfg.max_consecutive_preemptions = cfg.max_consecutive_preemptions;
    scheduler_cfg.preempt_trace_path = cfg.preempt_trace_path;
    SchedulerCore core(*runner, kv, metrics, scheduler_cfg);

    std::thread core_thread([&core] { core.run(); });
    const std::int32_t rc = run_server(cfg, *runner, metrics, core, gate);
    core.shutdown();
    core_thread.join();
    // 关停后显式清空 donor 缓存再释放后端，日志里的 used/free 是“零泄漏”证据。
    report_shutdown(kv, *runner);
    runner->release();
    return rc;
}

} // namespace

// 入口：先解析配置再初始化日志，保证日志级别尽早生效。
int main(int argc, char** argv) {
    llmopt::ServerConfig cfg;
    std::string message;
    const llmopt::ErrCode code = llmopt::load_config(argc, argv, cfg, message);
    if (code != llmopt::ErrCode::Ok) {
        std::cerr << "配置错误: " << message << "\n";
        return exit_for(code);
    }
    llmopt::init_logging(cfg.log_level);
    print_config(cfg);
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
