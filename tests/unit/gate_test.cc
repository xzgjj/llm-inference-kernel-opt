// gate_test.cc — L2 三态判据与 L3 force_on 窗口；不依赖真实硬件。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/gate.hpp"

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 完整快照：1 GiB 可用内存，无 GPU。
CapabilitySnapshot snapshot_with(std::int64_t free_bytes) {
    CapabilitySnapshot snapshot;
    snapshot.ram_total_bytes = 2LL * 1024 * 1024 * 1024;
    snapshot.ram_available_bytes = free_bytes;
    snapshot.logical_cores = 8;
    return snapshot;
}

// 预算：模型 100 MiB + KV 100 MiB。
EstBudget budget_with(std::int64_t required_bytes) {
    EstBudget budget;
    budget.model_bytes = required_bytes / 2;
    budget.kv_bytes = required_bytes - budget.model_bytes;
    budget.required_bytes = required_bytes;
    budget.safety_ratio = 0.80;
    return budget;
}

constexpr std::int64_t kMib = 1024 * 1024;

} // namespace

TEST_CASE("快照缺字段时任何模式都拒绝") {
    CapabilitySnapshot snapshot = snapshot_with(1024 * kMib);
    snapshot.missing_fields.push_back("ram.total");
    const GateResult result = evaluate_gate(snapshot, budget_with(10 * kMib), GateMode::Off, true);
    CHECK(result.decision == GateDecision::Blocked);
    CHECK_FALSE(result.allow_start);
}

TEST_CASE("预算低于 free×safety 时为 Eligible") {
    const GateResult result = evaluate_gate(snapshot_with(1024 * kMib), budget_with(100 * kMib),
                                            GateMode::Auto, false);
    CHECK(result.decision == GateDecision::Eligible);
    CHECK(result.allow_start);
    CHECK_FALSE(result.downgrade_concurrency);
}

TEST_CASE("软超预算：off 放行、auto 拒绝、force_on 需开关并降并发") {
    const GateResult off = evaluate_gate(snapshot_with(1024 * kMib), budget_with(900 * kMib),
                                         GateMode::Off, false);
    CHECK(off.decision == GateDecision::Degraded);
    CHECK(off.allow_start);

    const GateResult auto_result = evaluate_gate(snapshot_with(1024 * kMib),
                                                 budget_with(900 * kMib), GateMode::Auto, false);
    CHECK(auto_result.decision == GateDecision::Degraded);
    CHECK_FALSE(auto_result.allow_start);

    const GateResult forced_off = evaluate_gate(snapshot_with(1024 * kMib), budget_with(900 * kMib),
                                                GateMode::ForceOn, false);
    CHECK_FALSE(forced_off.allow_start);

    const GateResult forced = evaluate_gate(snapshot_with(1024 * kMib), budget_with(900 * kMib),
                                            GateMode::ForceOn, true);
    CHECK(forced.allow_start);
    CHECK(forced.downgrade_concurrency);
}

TEST_CASE("硬超预算即使用 off 也不放行") {
    const GateResult result =
            evaluate_gate(snapshot_with(64 * kMib), budget_with(65 * kMib), GateMode::Off, true);
    CHECK(result.decision == GateDecision::Blocked);
    CHECK_FALSE(result.allow_start);
}

TEST_CASE("有 GPU 时以显存空闲量为判据") {
    CapabilitySnapshot snapshot = snapshot_with(1 * kMib);
    snapshot.gpu_present = true;
    snapshot.vram_free_bytes = 4096 * kMib;
    const GateResult result =
            evaluate_gate(snapshot, budget_with(100 * kMib), GateMode::Auto, false);
    CHECK(result.decision == GateDecision::Eligible);
    CHECK(result.budget.free_bytes == 4096 * kMib);
}

TEST_CASE("部分卸载：显存与内存分池判据，全模型不再压显存") {
    CapabilitySnapshot snapshot = snapshot_with(2048 * kMib);
    snapshot.gpu_present = true;
    snapshot.vram_free_bytes = 1024 * kMib;
    EstBudget budget;
    budget.model_bytes = 1000 * kMib;
    budget.kv_bytes = 200 * kMib;
    budget.required_bytes = 1200 * kMib;
    budget.safety_ratio = 0.80;
    budget.model_vram_bytes = 700 * kMib;
    budget.kv_vram_bytes = 100 * kMib;
    budget.host_bytes = 400 * kMib;
    // 单池口径会判 1200 > 1024 硬拦；分池后 800/1024 与 400/2048 都放得下。
    const GateResult result = evaluate_gate(snapshot, budget, GateMode::Auto, false);
    CHECK(result.decision == GateDecision::Eligible);
    CHECK(result.allow_start);
}

TEST_CASE("部分卸载：任一池硬超都拒绝") {
    CapabilitySnapshot snapshot = snapshot_with(2048 * kMib);
    snapshot.gpu_present = true;
    snapshot.vram_free_bytes = 1024 * kMib;
    EstBudget budget;
    budget.model_bytes = 1000 * kMib;
    budget.kv_bytes = 200 * kMib;
    budget.required_bytes = 1200 * kMib;
    budget.safety_ratio = 0.80;
    budget.model_vram_bytes = 900 * kMib;
    budget.kv_vram_bytes = 140 * kMib;
    budget.host_bytes = 160 * kMib;
    const GateResult vram_over = evaluate_gate(snapshot, budget, GateMode::Off, true);
    CHECK(vram_over.decision == GateDecision::Blocked);
    CHECK_FALSE(vram_over.allow_start);
}
