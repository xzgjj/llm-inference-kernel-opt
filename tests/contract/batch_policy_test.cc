// batch_policy_test.cc — 批次策略对照：static 只在 running 排空后整批准入。
// 用途是把"完成即补位"这一项收益从连续批的总账里隔离出来；默认必须是 continuous。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../support/preempt_harness.hpp"
#include "doctest/doctest.h"
#include "llmopt/scheduler.hpp"

using namespace llmopt; // NOLINT：用例短名与契约同名
using namespace llmopt::test;

namespace {

SchedulerConfig base_config() {
    // max_seqs=4, token 预算 64, 队列 32, 前缀缓存开, chunk=8。
    return SchedulerConfig{4, 64, 32, true, 8};
}

} // namespace

TEST_CASE("static 批次策略：running 未排空前不再准入新请求") {
    SchedulerConfig config = base_config();
    config.continuous_batching = false;
    PreemptHarness harness(config, KVConfig{16, 16}, true);

    auto first_a = harness.submit("static-a", 4, 2);
    auto first_b = harness.submit("static-b", 4, 2);
    REQUIRE(harness.run_once(20));
    REQUIRE(harness.core.running_count() == 2);

    // 第二批：容量还有空位，但静态批必须等当前这一波跑空。
    auto second_c = harness.submit("static-c", 4, 2);
    auto second_d = harness.submit("static-d", 4, 2);
    // submit 只是把 AddRequest 投递进 mailbox，要先让 Core 处理事件才进 waiting。
    REQUIRE(harness.run_until([&] { return harness.core.waiting_count() == 2; }));
    REQUIRE(harness.run_once(20));
    CHECK(harness.core.running_count() == 2);
    CHECK(drain(second_c).empty());
    CHECK(drain(second_d).empty());

    // 第一波收尾后，下一波整批进场，而不是逐个补位。
    harness.pump(first_a);
    harness.pump(first_b);
    REQUIRE(harness.run_until([&] { return harness.core.running_count() == 2; }));
    CHECK(harness.core.waiting_count() == 0);
    harness.pump(second_c);
    harness.pump(second_d);
    CHECK(harness.core.running_count() == 0);
}

TEST_CASE("continuous 批次策略：有空位立刻补位（默认行为不变）") {
    SchedulerConfig config = base_config();
    REQUIRE(config.continuous_batching);
    PreemptHarness harness(config, KVConfig{16, 16}, true);

    auto first_a = harness.submit("continuous-a", 4, 2);
    auto first_b = harness.submit("continuous-b", 4, 2);
    REQUIRE(harness.run_once(20));
    REQUIRE(harness.core.running_count() == 2);

    auto second_c = harness.submit("continuous-c", 4, 2);
    auto second_d = harness.submit("continuous-d", 4, 2);
    // 有空位就补：两轮 run_once 内必须看到 4 个 running（事件处理与 pump 同轮完成）。
    REQUIRE(harness.run_until([&] { return harness.core.running_count() == 4; }));
    CHECK(harness.core.waiting_count() == 0);

    harness.pump(first_a);
    harness.pump(first_b);
    harness.pump(second_c);
    harness.pump(second_d);
    CHECK(harness.core.running_count() == 0);
}
