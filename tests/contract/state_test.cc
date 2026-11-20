// state_test.cc — 3-G4：recompute 后输出完整、长度不超 max_tokens、stale 不重复计数。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../support/preempt_harness.hpp"
#include "doctest/doctest.h"

#include <memory>
#include <vector>

using namespace llmopt;       // NOLINT：用例短名与契约同名
using namespace llmopt::test; // NOLINT：测试装配

namespace {

KVConfig small_pool() {
    return KVConfig{8, 16, 0.0, false, 0};
}

SchedulerConfig pressure_config() {
    SchedulerConfig config;
    config.max_concurrency = 2;
    config.max_num_batched_tokens = 8;
    config.chunk_size = 128;
    config.queue_capacity = 8;
    config.enable_prefix_cache = false;
    return config;
}

// 同 prompt/seed 的无抢占基线，用于逐 token 比对恢复结果。
std::vector<std::int32_t> baseline_tokens(std::int32_t prompt_tokens, std::int32_t max_tokens,
                                          std::int32_t seed = 7) {
    PreemptHarness baseline(pressure_config(), small_pool(), true);
    const auto sink = baseline.submit("baseline", prompt_tokens, max_tokens, seed);
    baseline.pump(sink);
    return token_ids(drain(sink));
}

// 让 A 产出至少 1 个 token，再用 B 挤出 A；返回被抢占时的重算长度。
std::int32_t preempt_with_a(PreemptHarness& harness, const std::shared_ptr<BoundedSink>& a,
                            std::int32_t prompt_tokens, std::int32_t max_tokens) {
    REQUIRE(harness.run_until(
            [&] { return harness.metrics.get("llmopt_tokens_generated_total") >= 1; }, 8));
    (void)harness.submit("b", 16, 100, 7);
    const bool preempted = harness.run_until(
            [&] { return harness.metrics.get("llmopt_preemptions_total") == 1; }, 40);
    REQUIRE(preempted);
    const std::int32_t recompute =
            static_cast<std::int32_t>(harness.metrics.get("llmopt_preempted_tokens_total"));
    CHECK(recompute >= prompt_tokens);
    CHECK(recompute <= prompt_tokens + max_tokens - 1);
    CHECK(!a->is_closed()); // 抢占不是终止
    return recompute;
}

} // namespace

TEST_CASE("recompute 恢复后输出与未中断基线逐 token 一致且不超 max_tokens") {
    PreemptHarness harness(pressure_config(), small_pool(), true);
    const auto a = harness.submit("a", 8, 100, 7);
    (void)preempt_with_a(harness, a, 8, 100);
    CHECK(harness.core.waiting_count() == 1);

    // B 仍 pending，先取消它；A 在 B 收尾后的同一 tick 被重新准入并重放。
    harness.core.cancel("b");
    harness.pump(a, 200);

    const std::vector<Delta> frames = drain(a);
    REQUIRE(frames.size() == 100);
    CHECK(frames.back().finish.has_value());
    CHECK(*frames.back().finish == FinishReason::LengthCapped);
    CHECK(token_ids(frames) == baseline_tokens(8, 100)); // 完整、无重复、无丢失
    CHECK(harness.metrics.get("llmopt_preempt_recovered_total") == 1);
    CHECK(harness.core.running_count() == 0);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}

TEST_CASE("recompute 只回填已生成 token，末块不再重复采样") {
    PreemptHarness harness(pressure_config(), small_pool(), true);
    const auto a = harness.submit("a", 8, 100, 11);
    (void)preempt_with_a(harness, a, 8, 100);

    harness.core.cancel("b");
    harness.pump(a, 120);
    const std::vector<Delta> frames = drain(a);
    REQUIRE(frames.size() == 100);
    // 若末块重复采样，会多出 token 或首个 token 重复，基线比对会失败。
    CHECK(token_ids(frames) == baseline_tokens(8, 100, 11));
}
