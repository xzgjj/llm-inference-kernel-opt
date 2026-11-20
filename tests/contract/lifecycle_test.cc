// lifecycle_test.cc — 3-G6：断连/取消/关停后的块回收与零泄漏。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../support/preempt_harness.hpp"
#include "doctest/doctest.h"

using namespace llmopt;       // NOLINT：用例短名与契约同名
using namespace llmopt::test; // NOLINT：测试装配

namespace {

KVConfig small_pool() {
    return KVConfig{8, 8, 0.0, false, 0};
}

SchedulerConfig base_config() {
    SchedulerConfig config;
    config.max_concurrency = 2;
    config.max_num_batched_tokens = 8;
    config.chunk_size = 128;
    config.queue_capacity = 8;
    config.enable_prefix_cache = false;
    return config;
}

} // namespace

TEST_CASE("客户端断连后下一步终止并归还全部块") {
    PreemptHarness harness(base_config(), small_pool(), true);
    const auto sink = harness.submit("disconnect", 8, 4);
    harness.run_once(20); // 准入 + prefill
    harness.run_once(20); // 首 token 入队
    REQUIRE(!sink->is_closed());
    sink->abort(); // 模拟 SSE 写端断开
    REQUIRE(harness.run_until([&] { return harness.core.running_count() == 0; }, 100));
    CHECK(sink->is_closed());
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"cancelled\"}") == 1);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}

TEST_CASE("排队请求取消后不占块，等待队列清空") {
    SchedulerConfig config = base_config();
    config.max_concurrency = 1; // 单槽让第二条请求稳定留在等待队列
    PreemptHarness harness(config, small_pool(), true);
    const auto first = harness.submit("first", 8, 56);
    const auto queued = harness.submit("queued", 8, 4);
    harness.run_once(20);
    REQUIRE(harness.core.running_count() == 1);
    REQUIRE(harness.core.waiting_count() == 1);
    harness.core.cancel("queued");
    REQUIRE(harness.run_until([&] { return queued->is_closed(); }, 20));
    CHECK(harness.core.waiting_count() == 0);
    CHECK(harness.kv.num_free_blocks() + 1 < harness.kv.num_total_blocks()); // first 仍持块
    harness.core.cancel("first");
    harness.pump(first);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
}

TEST_CASE("超上下文请求以 Error 终止且不留在等待队列") {
    PreemptHarness harness(base_config(), small_pool(), true);
    const auto big = harness.submit("big", 8, 100); // 108 token > 8×8 块容量
    REQUIRE(harness.run_until([&] { return big->is_closed(); }, 20));
    CHECK(harness.core.waiting_count() == 0);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}

TEST_CASE("生成达到 max_tokens 后按长度收尾并归还全部块") {
    PreemptHarness harness(base_config(), small_pool(), true);
    const auto sink = harness.submit("length", 8, 2);
    REQUIRE(harness.run_until([&] { return sink->is_closed(); }, 80));
    const std::vector<Delta> frames = drain(sink);
    REQUIRE(frames.size() == 2);
    REQUIRE(frames.back().finish.has_value());
    CHECK(*frames.back().finish == FinishReason::LengthCapped);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}

TEST_CASE("压力抢占后关停无悬挂序列、无泄漏块") {
    PreemptHarness harness(base_config(), small_pool(), true);
    (void)harness.submit("a", 8, 44);
    REQUIRE(harness.run_until(
            [&] { return harness.metrics.get("llmopt_tokens_generated_total") >= 1; }, 8));
    (void)harness.submit("b", 16, 40);
    REQUIRE(harness.run_until([&] { return harness.metrics.get("llmopt_preemptions_total") == 1; },
                              40));
    REQUIRE(harness.core.waiting_count() == 1);

    harness.core.shutdown();
    REQUIRE(harness.run_until(
            [&] { return harness.core.running_count() == 0 && harness.core.waiting_count() == 0; },
            200));
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    CHECK(harness.metrics.get("llmopt_running_sequences") == 0);
    CHECK(harness.metrics.get("llmopt_queue_depth") == 0);
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}
