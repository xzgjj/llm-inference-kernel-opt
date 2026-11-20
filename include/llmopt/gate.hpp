// gate.hpp — M11 L0–L3：L1 快照、L2 三态判据、L3 off/auto/force_on 窗口。
#ifndef LLMOPT_GATE_HPP
#define LLMOPT_GATE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace llmopt {

enum class GateMode : std::uint8_t { Off, Auto, ForceOn };

struct CapabilitySnapshot {
    bool gpu_present = false;
    std::int64_t vram_total_bytes = 0;
    std::int64_t vram_free_bytes = 0;
    std::int64_t ram_total_bytes = 0;
    std::int64_t ram_available_bytes = 0;
    std::int32_t logical_cores = 0;
    bool is_wsl = false;
    std::vector<std::string> missing_fields;
};

struct EstBudget {
    std::int64_t model_bytes = 0;
    std::int64_t kv_bytes = 0;
    std::int64_t required_bytes = 0;
    std::int64_t free_bytes = 0;
    double safety_ratio = 0.80;
};

enum class GateDecision : std::uint8_t { Eligible, Degraded, Blocked };

struct GateResult {
    GateDecision decision = GateDecision::Eligible;
    EstBudget budget;
    bool allow_start = true;
    bool downgrade_concurrency = false;
    std::vector<std::string> reasons;
};

CapabilitySnapshot probe_capabilities();
GateResult evaluate_gate(const CapabilitySnapshot& snapshot, const EstBudget& budget, GateMode mode,
                         bool allow_force_on);
const char* gate_decision_name(GateDecision decision) noexcept;
const char* gate_mode_name(GateMode mode) noexcept;

} // namespace llmopt

#endif // LLMOPT_GATE_HPP
