// preempt_test.cc — 3-G3：基线零抢占、压力档抢占可回溯、cap 终止。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../support/preempt_harness.hpp"
#include "doctest/doctest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace llmopt;       // NOLINT：用例短名与契约同名
using namespace llmopt::test; // NOLINT：测试装配

namespace {

// 小池配置：16 块 × 8 token，关闭前缀缓存，避免 donor 干扰 victim 计数。
KVConfig small_pool() {
    return KVConfig{8, 16, 0.0, false, 0};
}

SchedulerConfig pressure_config(const std::string& trace_path = "") {
    SchedulerConfig config;
    config.max_concurrency = 2;
    config.max_num_batched_tokens = 8;
    config.chunk_size = 128;
    config.queue_capacity = 8;
    config.enable_prefix_cache = false;
    config.preempt_trace_path = trace_path;
    return config;
}

// A 先产出至少 1 个 token；B 的大块需求随后触发一次抢占。
// 用 run_until 等待，避免 run_once 跨过“pending=false 才准入”的窗口。
void preempt_once(PreemptHarness& harness) {
    (void)harness.submit("a", 8, 100);
    REQUIRE(harness.run_until(
            [&] { return harness.metrics.get("llmopt_tokens_generated_total") >= 1; }, 8));
    (void)harness.submit("b", 16, 100);
    REQUIRE(harness.run_until([&] { return harness.metrics.get("llmopt_preemptions_total") == 1; },
                              40));
    REQUIRE(harness.core.running_count() == 1);
    REQUIRE(harness.core.waiting_count() == 1);
}

} // namespace

TEST_CASE("基线无压力时抢占计数恒为 0") {
    PreemptHarness harness(pressure_config(), small_pool(), true);
    const auto sink = harness.submit("solo", 8, 4);
    harness.pump(sink);
    CHECK(harness.metrics.get("llmopt_preemptions_total") == 0);
    CHECK(harness.metrics.get("llmopt_preempt_recompute_total") == 0);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
}

TEST_CASE("压力档每次抢占落 trace：victim/块数/重算 token/代价可回溯") {
    const std::filesystem::path trace =
            std::filesystem::temp_directory_path() / "llmopt_preempt_trace_test.csv";
    std::filesystem::remove(trace);
    std::int32_t recompute = 0;
    {
        PreemptHarness harness(pressure_config(trace.string()), small_pool(), true);
        preempt_once(harness);
        recompute = static_cast<std::int32_t>(harness.metrics.get("llmopt_preempted_tokens_total"));
        // A 已有若干采样 token；已写 KV 长度落在 prompt 与 max_tokens 之间。
        CHECK(recompute >= 8);
        CHECK(recompute <= 8 + 100 - 1);
        CHECK(harness.metrics.get("llmopt_preempt_recompute_total") == 1);
    }

    std::ifstream input(trace);
    REQUIRE(input.good());
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string content = buffer.str();
    input.close();
    CHECK(content.find("ts_ns,trigger,victim,free_before,need_blocks,blocks_released,"
                       "recompute_tokens,path,cost_ms,cap_error") != std::string::npos);
    // A 占 14 块，free_before=2；B 需 15 块，释放 14 块，代价未标定为 NA。
    const std::string expected =
            ",admission,a,2,15,14," + std::to_string(recompute) + ",recompute,NA,0";
    CHECK(content.find(expected) != std::string::npos);
    if (std::getenv("LLMOPT_KEEP_PREEMPT_TRACE") == nullptr) {
        std::filesystem::remove(trace);
    }
}

TEST_CASE("连续抢占达到 cap 后请求以 Error 终止且块全部归还") {
    SchedulerConfig config = pressure_config();
    config.max_consecutive_preemptions = 1;
    PreemptHarness harness(config, small_pool(), true);
    const auto a = harness.submit("a", 8, 100);
    REQUIRE(harness.run_until(
            [&] { return harness.metrics.get("llmopt_tokens_generated_total") >= 1; }, 8));
    (void)harness.submit("b", 16, 100);
    REQUIRE(harness.run_until([&] { return harness.metrics.get("llmopt_preemptions_total") == 1; },
                              40));
    CHECK(harness.metrics.get("llmopt_preempt_cap_errors_total") == 1);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"error\"}") == 1);
    CHECK(a->is_closed());
    CHECK(harness.core.waiting_count() == 0);
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}
