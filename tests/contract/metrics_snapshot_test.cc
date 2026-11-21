// metrics_snapshot_test.cc — DEBUG-01/02：租户 KV 用量必须走 Core 快照，名额失败不得泄漏。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../support/preempt_harness.hpp"
#include "doctest/doctest.h"
#include "llmopt/scheduler.hpp"

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

std::map<std::string, std::int32_t> stats_kv(const std::vector<llmopt::TenantStat>& stats) {
    std::map<std::string, std::int32_t> out;
    for (const llmopt::TenantStat& stat : stats) {
        out[stat.tenant] = stat.kv_used;
    }
    return out;
}

// 允许断言 ErrCode 的提交入口：PreemptHarness::submit 会 REQUIRE(Ok)，测拒绝路径不能复用。
llmopt::ErrCode submit_req(llmopt::test::PreemptHarness& harness, const std::string& id,
                           const std::string& tenant, const llmopt::TenantQuota& quota) {
    std::vector<std::int32_t> prompt(4, 321);
    llmopt::InferenceRequest request;
    request.req_id = id;
    request.sampling.max_tokens = 2;
    request.sampling.seed = 7;
    request.stream = true;
    request.tenant = tenant;
    request.tenant_quota = quota;
    return harness.core.submit(request, std::move(prompt),
                               std::make_shared<llmopt::BoundedSink>(64));
}

} // namespace

TEST_CASE("tenant_stats 的 kv_used 与 Core 快照一致，不在 HTTP 侧扫块表") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 4;
    config.queue_capacity = 8;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{4, 64, 0.0, false, 0}, true);

    llmopt::TenantQuota quota;
    quota.max_concurrency = 4;
    quota.queue_capacity = 8;
    harness.submit("a-0", 8, 4, 7, "tenant-a", quota);
    harness.submit("b-0", 8, 4, 7, "tenant-b", quota);

    for (int round = 0; round < 6; ++round) {
        (void)harness.run_once(10);
        const std::map<std::string, std::int32_t> snapshot = stats_kv(harness.core.tenant_stats());
        // 单线程测试里可以直接读块表，作为快照的独立基准。
        const std::int32_t expected_a = harness.kv.used_blocks_for("tenant-a");
        const std::int32_t expected_b = harness.kv.used_blocks_for("tenant-b");
        CHECK_EQ(snapshot.count("tenant-a"), 1U);
        CHECK_EQ(snapshot.at("tenant-a"), expected_a);
        CHECK_EQ(snapshot.at("tenant-b"), expected_b);
    }
    harness.core.shutdown();
}

TEST_CASE("租户配额拒绝不泄漏准入名额，也不会留下空计数器") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 4;
    config.queue_capacity = 8;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{4, 64, 0.0, false, 0}, true);

    llmopt::TenantQuota tight;
    tight.max_concurrency = 1;
    tight.queue_capacity = 1;
    llmopt::TenantQuota roomy;

    CHECK(submit_req(harness, "a-0", "tenant-a", tight) == llmopt::ErrCode::Ok);
    CHECK(submit_req(harness, "a-1", "tenant-a", tight) == llmopt::ErrCode::Ok);
    CHECK(submit_req(harness, "a-2", "tenant-a", tight) == llmopt::ErrCode::QuotaExceeded);
    // 拒绝后同一进程仍能接收其他租户，且 /metrics 快照不崩。
    CHECK(submit_req(harness, "b-0", "tenant-b", roomy) == llmopt::ErrCode::Ok);
    const std::vector<llmopt::TenantStat> stats = harness.core.tenant_stats();
    CHECK_EQ(stats.size(), 2U);
    for (const llmopt::TenantStat& stat : stats) {
        CHECK(stat.live >= 0);
        CHECK(stat.kv_used >= 0);
    }
    harness.core.shutdown();
}

TEST_CASE("多线程抓取 tenant_stats 与 Core 推进并发不崩溃") {
    llmopt::SchedulerConfig config;
    config.max_concurrency = 2;
    config.queue_capacity = 4;
    config.chunk_size = 2;
    llmopt::test::PreemptHarness harness(config, llmopt::KVConfig{4, 64, 0.0, false, 0}, true);
    harness.submit("a-0", 6, 3, 7, "tenant-a");
    harness.submit("b-0", 6, 3, 7, "tenant-b");

    std::atomic<bool> stop{false};
    std::atomic<std::int32_t> reads{0};
    std::atomic<bool> bad{false};
    std::vector<std::thread> readers;
    for (int index = 0; index < 3; ++index) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const std::vector<llmopt::TenantStat> stats = harness.core.tenant_stats();
                for (const llmopt::TenantStat& stat : stats) {
                    if (stat.kv_used < 0 || stat.live < 0) {
                        bad.store(true, std::memory_order_relaxed);
                    }
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (int round = 0; round < 30; ++round) {
        (void)harness.run_once(5);
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) {
        reader.join();
    }
    CHECK_FALSE(bad.load(std::memory_order_relaxed));
    CHECK(reads.load(std::memory_order_relaxed) > 0);
    harness.core.shutdown();
}
