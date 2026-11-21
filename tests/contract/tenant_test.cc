// tenant_test.cc — 阶段 11：每 key 的 live/并发与 KV 块配额在 Core 边界生效。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../support/preempt_harness.hpp"
#include "doctest/doctest.h"
#include "llmopt/scheduler.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {

llmopt::ErrCode submit_one(llmopt::test::PreemptHarness& harness, const std::string& id,
                           std::int32_t prompt_tokens, std::int32_t max_tokens,
                           const std::string& tenant, const llmopt::TenantQuota& quota) {
    std::vector<std::int32_t> prompt(static_cast<std::size_t>(prompt_tokens), 321);
    llmopt::InferenceRequest request;
    request.req_id = id;
    request.sampling.max_tokens = max_tokens;
    request.sampling.seed = 7;
    request.stream = true;
    request.tenant = tenant;
    request.tenant_quota = quota;
    auto sink = std::make_shared<llmopt::BoundedSink>(128);
    return harness.core.submit(request, std::move(prompt), sink);
}

std::optional<llmopt::ErrCode> error_code(const std::shared_ptr<llmopt::BoundedSink>& sink) {
    std::vector<llmopt::Delta> frames = llmopt::test::drain(sink);
    for (const llmopt::Delta& frame : frames) {
        if (frame.error.has_value()) {
            return frame.error->code;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("同租户超过 max_concurrency+queue_capacity 立即 QUOTA_EXCEEDED，其他租户不受影响") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 4;
    config.queue_capacity = 8;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{4, 32, 0.0, false, 0}, true);

    llmopt::TenantQuota tight;
    tight.max_concurrency = 1;
    tight.queue_capacity = 1;
    CHECK(submit_one(harness, "a-0", 4, 2, "tenant-a", tight) == llmopt::ErrCode::Ok);
    CHECK(submit_one(harness, "a-1", 4, 2, "tenant-a", tight) == llmopt::ErrCode::Ok);
    CHECK(submit_one(harness, "a-2", 4, 2, "tenant-a", tight) == llmopt::ErrCode::QuotaExceeded);

    llmopt::TenantQuota roomy;
    roomy.max_concurrency = 2;
    roomy.queue_capacity = 2;
    CHECK(submit_one(harness, "b-0", 4, 2, "tenant-b", roomy) == llmopt::ErrCode::Ok);

    const std::vector<llmopt::TenantStat> stats = harness.core.tenant_stats();
    llmopt::TenantStat a_stat{"tenant-a", 0, 0, 0, 0};
    llmopt::TenantStat b_stat{"tenant-b", 0, 0, 0, 0};
    for (const llmopt::TenantStat& stat : stats) {
        if (stat.tenant == "tenant-a") {
            a_stat = stat;
        } else if (stat.tenant == "tenant-b") {
            b_stat = stat;
        }
    }
    CHECK_EQ(a_stat.live, 2);
    CHECK_EQ(a_stat.running, 0);
    CHECK_EQ(a_stat.queued, 2);
    CHECK_EQ(b_stat.live, 1);
    harness.core.shutdown();
}

TEST_CASE("单租户 running 达到 max_concurrency 时让位给其他租户") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 2;
    config.queue_capacity = 4;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{4, 32, 0.0, false, 0}, true);

    llmopt::TenantQuota quota;
    quota.max_concurrency = 1;
    quota.queue_capacity = 2;
    CHECK(submit_one(harness, "a-0", 8, 4, "tenant-a", quota) == llmopt::ErrCode::Ok);
    CHECK(submit_one(harness, "a-1", 8, 4, "tenant-a", quota) == llmopt::ErrCode::Ok);
    CHECK(submit_one(harness, "b-0", 8, 4, "tenant-b", quota) == llmopt::ErrCode::Ok);

    CHECK(harness.run_until([&] { return harness.core.running_count() == 2; }));
    CHECK_EQ(harness.core.waiting_count(), 1);
    CHECK(harness.core.running_count() <= config.max_concurrency);
    harness.core.shutdown();
}

TEST_CASE("单请求 KV 需求超过租户配额时以 QUOTA_EXCEEDED 终态收尾") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 2;
    config.queue_capacity = 2;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{4, 16, 0.0, false, 0}, true);

    llmopt::TenantQuota quota;
    quota.max_concurrency = 1;
    quota.queue_capacity = 1;
    quota.kv_blocks = 2;

    std::vector<std::int32_t> prompt(12, 321);
    llmopt::InferenceRequest request;
    request.req_id = "kv-over";
    request.sampling.max_tokens = 8;
    request.stream = true;
    request.tenant = "tenant-a";
    request.tenant_quota = quota;
    auto sink = std::make_shared<llmopt::BoundedSink>(128);
    REQUIRE(harness.core.submit(request, std::move(prompt), sink) == llmopt::ErrCode::Ok);
    harness.pump(sink);

    CHECK(harness.core.running_count() == 0);
    CHECK(harness.kv.used_blocks_for("tenant-a") == 0);
    const auto code = error_code(sink);
    REQUIRE(code.has_value());
    CHECK(*code == llmopt::ErrCode::QuotaExceeded);
    harness.core.shutdown();
}
