// main.cpp — M10 启动编排：配置 → L2 估算 → 模型加载 → 调度/HTTP → 排空退出。
// 退出码契约：0 正常 / 1 启动失败 / 2 用法 / 3 资源不足 / 4 门控拒绝。
#include "llmopt/config.hpp"
#include "llmopt/gate.hpp"
#include "llmopt/gguf.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/log.hpp"
#include "llmopt/metrics.hpp"
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
constexpr std::int32_t kBlockTokens = 16;

// MiB 换算只用于启动摘要，避免把字节数直接怼给人看。
std::int64_t to_mib(std::int64_t bytes) {
    return bytes / (1024 * 1024);
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

// 组装并运行：Gate 通过后才加载权重；HTTP 与调度线程按 http→core→executor 顺序收尾。
std::int32_t run(const ServerConfig& cfg) {
    const GgufMeta meta = read_gguf_meta(cfg.model_path);
    if (!meta.ok) {
        std::cerr << "读取模型元数据失败: " << meta.error << "\n";
        return kExitStartFail;
    }
    EstBudget budget;
    budget.model_bytes = meta.file_bytes;
    budget.kv_bytes = estimate_kv_bytes(meta, cfg.ctx_tokens);
    budget.required_bytes = budget.model_bytes + budget.kv_bytes;
    budget.safety_ratio = cfg.safety_ratio;
    const CapabilitySnapshot snapshot = probe_capabilities();
    GateResult gate = evaluate_gate(snapshot, budget, cfg.mode, cfg.allow_force_on);
    print_budget(gate);
    if (!gate.allow_start) {
        return gate.budget.required_bytes > gate.budget.free_bytes ? kExitResource : kExitGate;
    }

    std::string message;
    std::unique_ptr<ModelRunner> runner = make_runner(cfg, message);
    if (!runner) {
        std::cerr << message << "\n";
        return kExitStartFail;
    }
    ModelConfig model_cfg;
    model_cfg.model_path = cfg.model_path;
    model_cfg.n_ctx = cfg.ctx_tokens;
    model_cfg.n_batch = cfg.ctx_tokens;
    model_cfg.n_ubatch = 512;
    model_cfg.threads = cfg.threads;
    model_cfg.n_gpu_layers = cfg.n_gpu_layers;
    if (runner->load_model(model_cfg) != ErrCode::Ok) {
        std::cerr << "模型加载失败\n";
        return kExitStartFail;
    }

    std::int32_t concurrency = cfg.max_concurrency;
    if (gate.downgrade_concurrency) {
        concurrency = std::max<std::int32_t>(1, cfg.max_concurrency / 2);
        std::cout << "FORCE_ON 横幅：max_concurrency 降至 " << concurrency << "\n";
    }
    const std::int32_t blocks = cfg.ctx_tokens / kBlockTokens + 8;
    KVBlockManager kv(KVConfig{kBlockTokens, blocks});
    MetricsRegistry metrics;
    SchedulerConfig scheduler_cfg;
    scheduler_cfg.max_concurrency = 1; // SPEC-1：阶段 1 batch=1
    scheduler_cfg.queue_capacity = 64;
    SchedulerCore core(*runner, kv, metrics, scheduler_cfg);

    std::thread core_thread([&core] { core.run(); });
    const std::int32_t rc = run_server(cfg, *runner, metrics, core, gate);
    core.shutdown();
    core_thread.join();
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
    const std::int32_t rc = run(cfg);
    llmopt::shutdown_logging();
    return rc;
}
