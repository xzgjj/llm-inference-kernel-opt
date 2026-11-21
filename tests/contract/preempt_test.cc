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

TEST_CASE("抢占预判不足时不改动 running 块表") {
    KVBlockManager kv(KVConfig{4, 4, 0.0, true, 2});
    Sequence donor;
    donor.id = 1;
    donor.backend_id = "donor";
    donor.prompt_len = 8;
    donor.prompt_token_ids = {1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(kv.acquire(donor, 8, 0, true).ok());
    kv.mark_written(donor, 8);
    REQUIRE(kv.retain(donor) > 0);

    Sequence victim;
    victim.id = 2;
    victim.backend_id = "victim";
    victim.status = SeqStatus::Running;
    REQUIRE(kv.acquire(victim, 8, 0, true).ok());
    SequenceGroup group;
    group.req_id = "victim";
    group.backend_id = victim.backend_id;
    group.seq.id = victim.id;
    group.seq.backend_id = victim.backend_id;
    group.seq.status = SeqStatus::Running;
    group.seq.block_table = victim.block_table;
    std::vector<SequenceGroup*> groups{&group};

    VictimPolicy policy;
    FairnessState fairness{0, false, 0, 0};
    const PreemptDecision decision = try_preempt(groups, 4, kv, policy, fairness);
    CHECK_FALSE(decision.retry_ok);
    CHECK(decision.victims.empty());
    CHECK(decision.blocks_released == 0);
    CHECK(group.seq.block_table.size() == 2);
    CHECK(kv.num_free_blocks() == 0);
}

TEST_CASE("共享前缀块不计入可释放量：预判不足时零副作用") {
    // 症状：预判用 block_table.size() 高估可释放量；共享块 free 只减引用、不回空闲链，
    // 于是先清空 victim 后仍不足。这里 donor 与 victim 共享 2 块、池内只剩 3 块。
    KVBlockManager kv(KVConfig{4, 6, 0.0, true, 2});
    Sequence donor;
    donor.id = 1;
    donor.backend_id = "donor";
    donor.prompt_len = 12;
    donor.prompt_token_ids = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    REQUIRE(kv.acquire(donor, 12, 0, true).ok());
    kv.mark_written(donor, 12);
    REQUIRE(kv.retain(donor) > 0);

    Sequence victim;
    victim.id = 2;
    victim.backend_id = "victim";
    victim.status = SeqStatus::Running;
    victim.prompt_len = 12;
    victim.prompt_token_ids = donor.prompt_token_ids;
    const PrefixHit hit = kv.lookup_prefix(victim);
    REQUIRE(hit.donor == "donor");
    REQUIRE(kv.attach_prefix(victim, hit) == 8);
    REQUIRE(victim.block_table.size() == 2);
    REQUIRE(kv.releasable_blocks(victim) == 0);

    SequenceGroup group;
    group.req_id = "victim";
    group.backend_id = victim.backend_id;
    group.seq.id = victim.id;
    group.seq.backend_id = victim.backend_id;
    group.seq.status = SeqStatus::Running;
    group.seq.prompt_len = victim.prompt_len;
    group.seq.prompt_token_ids = victim.prompt_token_ids;
    group.seq.block_table = victim.block_table;
    std::vector<SequenceGroup*> groups{&group};

    VictimPolicy policy;
    FairnessState fairness{0, false, 0, 0};
    const PreemptDecision decision = try_preempt(groups, 4, kv, policy, fairness);
    CHECK_FALSE(decision.retry_ok);
    CHECK(decision.victims.empty());
    CHECK(decision.blocks_released == 0);
    CHECK(group.seq.block_table.size() == 2);
    CHECK(kv.num_free_blocks() == 3);
}

TEST_CASE("请求 id 结束后可复用且不撞同名前缀 donor") {
    // 症状：旧请求按前缀缓存留置 runner donor 状态，新请求复用同一外部 id 时，
    // add_sequence 把旧 donor 当成“活跃重复”，整条请求以错误收尾。
    SchedulerConfig config;
    config.max_concurrency = 1;
    config.max_num_batched_tokens = 16;
    config.chunk_size = 8;
    config.queue_capacity = 4;
    config.enable_prefix_cache = true;
    PreemptHarness harness(config, KVConfig{8, 16, 0.0, true, 2}, false);

    const auto first = harness.submit("reuse-donor", 8, 2, 7);
    harness.pump(first);
    REQUIRE(harness.core.running_count() == 0);
    REQUIRE(harness.kv.usage().cached_blocks > 0);

    const auto second = harness.submit("reuse-donor", 8, 2, 7);
    harness.pump(second);
    const std::vector<Delta> frames = drain(second);
    bool has_error = false;
    for (const Delta& frame : frames) {
        has_error = has_error || static_cast<bool>(frame.error);
    }
    CHECK_FALSE(has_error);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"error\"}") == 0);
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}

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
    // victim 字段落内部 backend_id（seq-<serial>），不再泄露/依赖外部请求 id。
    const std::string expected = ",admission,seq-";
    CHECK(content.find(expected) != std::string::npos);
    CHECK(content.find(",2,15,14," + std::to_string(recompute) + ",recompute,NA,0") !=
          std::string::npos);
    CHECK(content.find(",admission,a,2,15,14,") == std::string::npos);
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
