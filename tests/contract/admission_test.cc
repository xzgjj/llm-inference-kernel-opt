// admission_test.cc — 4-G0-6：入口准入预算 = max_concurrency + queue_capacity。
// 突发提交在 HTTP 判定前就要占住名额，否则 64 并发会先 200 再在调度器里失败。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../support/preempt_harness.hpp"
#include "doctest/doctest.h"
#include "llmopt/scheduler.hpp"

#include <string>
#include <vector>

namespace {

llmopt::InferenceRequest make_request(const std::string& id, std::int32_t prompt_tokens,
                                      std::int32_t max_tokens) {
    llmopt::InferenceRequest request;
    request.req_id = id;
    request.sampling.max_tokens = max_tokens;
    request.sampling.seed = 7;
    request.stream = true;
    request.prompt = std::string(static_cast<std::size_t>(prompt_tokens), 'a');
    return request;
}

std::vector<std::int32_t> prompt_ids(std::int32_t count) {
    return std::vector<std::int32_t>(static_cast<std::size_t>(count), 321);
}

} // namespace

TEST_CASE("突发提交按 running+waiting+在飞 的总额拒绝，且只计一次 queue_full") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 1;
    config.queue_capacity = 2;
    config.chunk_size = 4;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{4, 32, 0.05, false, 0}, true);

    const std::int32_t capacity = harness.core.admission_capacity();
    REQUIRE(capacity == 3);

    std::vector<llmopt::ErrCode> codes;
    for (std::int32_t index = 0; index < 6; ++index) {
        const std::string id = "req-" + std::to_string(index);
        auto sink = std::make_shared<llmopt::BoundedSink>(8);
        codes.push_back(harness.core.submit(make_request(id, 4, 2), prompt_ids(4), sink));
    }
    CHECK(codes[0] == llmopt::ErrCode::Ok);
    CHECK(codes[1] == llmopt::ErrCode::Ok);
    CHECK(codes[2] == llmopt::ErrCode::Ok);
    CHECK(codes[3] == llmopt::ErrCode::QueueFull);
    CHECK(codes[4] == llmopt::ErrCode::QueueFull);
    CHECK(codes[5] == llmopt::ErrCode::QueueFull);
    CHECK_FALSE(harness.core.accepting());
    CHECK(harness.metrics.get(llmopt::MetricId::RequestsQueueFull) == 3);
    CHECK(harness.metrics.get(llmopt::MetricId::AdmissionQueue) == 3);
    CHECK(harness.metrics.get(llmopt::MetricId::RequestsAccepted) == 0);

    // 未被 Core 处理的拒绝不得重复计数；shutdown 只负责收尾已有 3 条。
    harness.core.shutdown();
    CHECK(harness.metrics.get(llmopt::MetricId::RequestsQueueFull) == 3);
}

TEST_CASE("关停闸门置位后 accepting 关闭，Core 边界拒绝新准入") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 1;
    config.queue_capacity = 1;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{2, 32, 0.05, false, 0}, true);

    auto sink = harness.submit("shut-1", 2, 1);
    REQUIRE(harness.run_once());

    harness.core.shutdown();
    // 处理 Shutdown 事件后 shutting_down_ 置位。
    REQUIRE(harness.run_once());
    CHECK_FALSE(harness.core.accepting());

    // 关停后直达 Core 的提交也必须被闸门拒绝，不能再占预留名额。
    std::vector<llmopt::ErrCode> codes;
    for (std::int32_t index = 0; index < 2; ++index) {
        auto rejected = std::make_shared<llmopt::BoundedSink>(8);
        codes.push_back(harness.core.submit(make_request("shut-" + std::to_string(index), 2, 1),
                                            prompt_ids(2), rejected));
    }
    CHECK(codes[0] == llmopt::ErrCode::QueueFull);
    CHECK(codes[1] == llmopt::ErrCode::QueueFull);
    harness.pump(sink);
}

TEST_CASE("handler 兑现预留后名额释放，accepting 恢复") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 1;
    config.queue_capacity = 1;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{2, 32, 0.05, false, 0}, true);

    for (std::int32_t index = 0; index < 2; ++index) {
        auto sink = std::make_shared<llmopt::BoundedSink>(8);
        REQUIRE(harness.core.submit(make_request("ok-" + std::to_string(index), 2, 1),
                                    prompt_ids(2), sink) == llmopt::ErrCode::Ok);
    }
    CHECK_FALSE(harness.core.accepting());
    // 处理一条 AddRequest：预留转成等待队列，inflight 减一，但总额仍是 2/2。
    REQUIRE(harness.run_once());
    CHECK_FALSE(harness.core.accepting());
    // 两条都已受理；Core 把其中一条推进为 running，另一条留在 waiting。
    CHECK(harness.metrics.get(llmopt::MetricId::RequestsAccepted) == 2);
    CHECK(harness.metrics.get(llmopt::MetricId::RequestsQueueFull) == 0);
}
