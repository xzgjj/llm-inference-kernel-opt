// continuous_batch_test.cc — C-P2-13 契约：连续批、双预算、前缀复用、取消隔离与对账。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 记录每次下发的 Batch，再委托 stub 执行；用于观察批组成而不是黑盒计时。
class RecordingRunner final : public ModelRunner {
public:
    RecordingRunner() : inner_(make_stub_runner()) {}

    ErrCode load_model(const ModelConfig& config) override {
        return inner_->load_model(config);
    }
    ErrCode add_sequence(const Sequence& seq) override {
        return inner_->add_sequence(seq);
    }
    RunnerStepOutput decode_step(const Batch& batch) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batches_.push_back(batch);
        }
        return inner_->decode_step(batch);
    }
    void release_sequence(const RequestId& req_id, std::int32_t keep_tokens) override {
        inner_->release_sequence(req_id, keep_tokens);
    }
    void release() override {
        inner_->release();
    }
    ErrCode tokenize(const std::string& text, std::vector<std::int32_t>& ids) override {
        return inner_->tokenize(text, ids);
    }
    std::string detokenize(std::int32_t token_id) override {
        return inner_->detokenize(token_id);
    }
    bool is_eos(std::int32_t token_id) const override {
        return inner_->is_eos(token_id);
    }
    std::int32_t n_ctx() const noexcept override {
        return inner_->n_ctx();
    }
    std::int32_t seq_pos_max(const RequestId& req_id) const override {
        return inner_->seq_pos_max(req_id);
    }
    const char* backend_name() const noexcept override {
        return "recording-stub";
    }
    bool supports_live_prefix() const noexcept override {
        return inner_->supports_live_prefix();
    }
    bool supports_prefix_share() const noexcept override {
        return inner_->supports_prefix_share();
    }

    std::vector<Batch> batches() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return batches_;
    }

private:
    std::unique_ptr<ModelRunner> inner_;
    mutable std::mutex mutex_;
    std::vector<Batch> batches_;
};

// 装配：块大小 4，prompt 由 token 基址生成，便于构造同前缀。
struct CbHarness {
    CbHarness(std::int32_t max_seqs, std::int32_t budget, std::int32_t blocks,
              double watermark = 0.0, std::int32_t chunk_size = 128)
        : runner(std::make_unique<RecordingRunner>()), kv(KVConfig{4, blocks, watermark, true, 2}),
          core(*runner, kv, metrics, SchedulerConfig{max_seqs, budget, 64, true, chunk_size}) {
        ModelConfig model;
        model.n_ctx = 4096;
        REQUIRE(runner->load_model(model) == ErrCode::Ok);
    }

    // prompt 基址相同即同前缀；base 区分不同请求。
    std::shared_ptr<BoundedSink> submit(const std::string& id, std::int32_t prompt_len,
                                        std::int32_t max_tokens, std::int32_t base = 100) {
        InferenceRequest request;
        request.req_id = id;
        request.sampling.max_tokens = max_tokens;
        request.sampling.seed = 7;
        request.stream = true;
        std::vector<std::int32_t> prompt(static_cast<std::size_t>(prompt_len));
        for (std::int32_t index = 0; index < prompt_len; ++index) {
            prompt[static_cast<std::size_t>(index)] = base + index;
        }
        auto sink = std::make_shared<BoundedSink>(64);
        REQUIRE(core.submit(request, std::move(prompt), sink) == ErrCode::Ok);
        return sink;
    }

    // 推进到所有 sink 关闭；超时即失败，避免死循环拖住 CI。
    void run_until_closed(const std::vector<std::shared_ptr<BoundedSink>>& sinks,
                          int rounds = 400) {
        for (int round = 0; round < rounds; ++round) {
            bool done = true;
            for (const auto& sink : sinks) {
                done = done && sink->is_closed();
            }
            if (done) {
                return;
            }
            (void)core.run_once(20);
        }
        for (const auto& sink : sinks) {
            REQUIRE(sink->is_closed());
        }
    }

    // 关停并跑完收尾：释放 donor、回收全部块。
    void shutdown_flush() {
        core.shutdown();
        core.run();
    }

    std::unique_ptr<RecordingRunner> runner;
    KVBlockManager kv;
    MetricsRegistry metrics;
    SchedulerCore core;
};

// 等执行线程真正收到 batch 再断言，避免调度已提交但记录尚未落盘的竞态。
std::vector<Batch> wait_batches(CbHarness& harness, std::size_t count, int rounds = 200) {
    for (int round = 0; round < rounds; ++round) {
        std::vector<Batch> batches = harness.runner->batches();
        if (batches.size() >= count) {
            return batches;
        }
        (void)harness.core.run_once(20);
    }
    std::vector<Batch> batches = harness.runner->batches();
    REQUIRE(batches.size() >= count);
    return batches;
}

// 收集增量：token 序列与结束原因各取所需。
std::vector<Delta> drain(const std::shared_ptr<BoundedSink>& sink) {
    std::vector<Delta> frames;
    Delta delta;
    while (sink->pop_frame(delta, 0)) {
        frames.push_back(delta);
    }
    return frames;
}

} // namespace

TEST_CASE("后续 prefill chunk 只提交一次预算") {
    CbHarness harness(1, 5, 64, 0.0, 2);
    auto sink = harness.submit("req-budget", 4, 1);
    harness.run_until_closed({sink});
    const std::vector<Batch> batches = harness.runner->batches();
    REQUIRE(batches.size() == 2);
    CHECK(batches[0].total_tokens == 2);
    CHECK(batches[1].total_tokens == 2);
    harness.shutdown_flush();
}

TEST_CASE("活跃请求 ID 不得重复，结束后可复用且不遗留后端槽位") {
    CbHarness harness(2, 64, 64);
    auto first = harness.submit("req-dup", 2, 2);

    InferenceRequest duplicate;
    duplicate.req_id = "req-dup";
    duplicate.sampling.max_tokens = 1;
    duplicate.stream = true;
    auto duplicate_sink = std::make_shared<BoundedSink>(16);
    CHECK(harness.core.submit(duplicate, {100, 101}, duplicate_sink) == ErrCode::InvalidJson);

    harness.run_until_closed({first});
    auto reused = harness.submit("req-dup", 2, 2);
    harness.run_until_closed({reused});
    CHECK(harness.core.running_count() == 0);
    harness.shutdown_flush();
}

TEST_CASE("同一 tick 到达的 4 条请求合成一个 prefill 批") {
    CbHarness harness(4, 64, 64);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 4; ++index) {
        sinks.push_back(harness.submit("req-" + std::to_string(index), 6, 2, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    const std::vector<Batch> batches = wait_batches(harness, 1);
    REQUIRE(!batches.empty());
    CHECK(batches[0].steps.size() == 4);
    CHECK(batches[0].new_reqs.size() == 4);
    CHECK(batches[0].cached_reqs.empty());
    CHECK(batches[0].total_tokens == 24);
    harness.run_until_closed(sinks);
    CHECK(harness.core.running_count() == 0);
}

TEST_CASE("预算 8 时两个 6-token prompt 按 chunk 分走预算，第三条留队") {
    CbHarness harness(4, 8, 64, 0.0, 8);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 3; ++index) {
        sinks.push_back(harness.submit("req-b" + std::to_string(index), 6, 1, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    CHECK(harness.core.running_count() == 2);
    CHECK(harness.core.waiting_count() == 1);
    const std::vector<Batch> batches = wait_batches(harness, 1);
    REQUIRE(!batches.empty());
    CHECK(batches[0].total_tokens == 8);
    CHECK(batches[0].steps.size() == 2);
    harness.run_until_closed(sinks);
}

TEST_CASE("max_concurrency=2 时第三、四条排队，结束后补位") {
    CbHarness harness(2, 64, 64);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 4; ++index) {
        sinks.push_back(harness.submit("req-c" + std::to_string(index), 4, 2, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    CHECK(harness.core.running_count() == 2);
    CHECK(harness.core.waiting_count() == 2);
    harness.run_until_closed(sinks);
    CHECK(harness.core.running_count() == 0);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"ok\"}") == 4);
}

TEST_CASE("第二 tick 批内同时包含 decode 与新生 prefill") {
    CbHarness harness(2, 64, 64);
    const auto first = harness.submit("req-mix-a", 4, 3, 110);
    (void)harness.core.run_once(20);
    const auto second = harness.submit("req-mix-b", 4, 2, 120);
    bool found = false;
    for (int round = 0; round < 20 && !found; ++round) {
        (void)harness.core.run_once(20);
        for (const Batch& batch : harness.runner->batches()) {
            if (batch.cached_reqs.size() == 1 && batch.new_reqs.size() == 1) {
                found = true;
                CHECK(batch.steps.size() == 2);
            }
        }
    }
    CHECK(found);
    harness.run_until_closed({first, second});
}

TEST_CASE("一条结束不打断同批其他序列") {
    CbHarness harness(2, 64, 32);
    const auto short_req = harness.submit("req-short", 4, 1, 110);
    const auto long_req = harness.submit("req-long", 4, 4, 120);
    harness.run_until_closed({short_req, long_req});
    CHECK(drain(short_req).size() == 1);
    CHECK(drain(long_req).size() == 4);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"ok\"}") == 2);
    CHECK(harness.metrics.get("llmopt_kv_reconcile_diffs_total") == 0);
}

TEST_CASE("取消 4 路中的一路只影响该请求") {
    CbHarness harness(4, 64, 64);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 4; ++index) {
        sinks.push_back(harness.submit("req-x" + std::to_string(index), 4, 3, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    REQUIRE(harness.core.running_count() == 4);
    harness.core.cancel("req-x2");
    harness.run_until_closed(sinks);
    const std::vector<Delta> cancelled = drain(sinks[2]);
    CHECK(!cancelled.empty());
    CHECK(*cancelled.back().finish == FinishReason::Aborted);
    CHECK(drain(sinks[0]).size() == 3);
    CHECK(drain(sinks[1]).size() == 3);
    CHECK(drain(sinks[3]).size() == 3);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"cancelled\"}") == 1);
}

TEST_CASE("同前缀二次请求命中 8 token，只算剩余 2 token 且输出可复现") {
    CbHarness harness(1, 64, 16);
    const auto first = harness.submit("req-pre-a", 10, 2, 130);
    harness.run_until_closed({first});
    const auto second = harness.submit("req-pre-b", 10, 2, 130);
    harness.run_until_closed({second});

    const std::vector<Batch> batches = harness.runner->batches();
    bool hit = false;
    for (const Batch& batch : batches) {
        if (batch.new_reqs.empty()) {
            continue;
        }
        const NewRequestData& data = batch.new_reqs.front();
        if (data.req_id == "req-pre-b") {
            hit = true;
            CHECK(data.prefix_tokens == 8);
            CHECK(data.prefix_source == "req-pre-a");
            CHECK(data.prefill_token_ids.size() == 2);
        }
    }
    CHECK(hit);
    CHECK(harness.metrics.get("llmopt_prefix_hits_total") >= 1);
    CHECK(drain(first)[0].token_id == drain(second)[0].token_id);
    const KVUsage usage = harness.kv.usage();
    CHECK(usage.cached_blocks > 0);
    CHECK(usage.shared_blocks == 0);
    harness.shutdown_flush();
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
}

TEST_CASE("无前缀时批内块 id 不重叠，关停后零泄漏") {
    CbHarness harness(4, 64, 24);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 4; ++index) {
        sinks.push_back(harness.submit("req-u" + std::to_string(index), 4, 2, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    const Batch batch = wait_batches(harness, 1).front();
    std::vector<std::int32_t> ids;
    for (const NewRequestData& data : batch.new_reqs) {
        for (const BlockRef& ref : data.block_ids) {
            ids.push_back(ref.block_id);
        }
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    CHECK(static_cast<std::int32_t>(ids.size()) == 8);
    harness.run_until_closed(sinks);
    harness.shutdown_flush();
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    std::string why;
    CHECK(harness.kv.check_invariants(why));
}

// 回归：watermark 只在 KVUsage 里算出、调度层忘了 set，/metrics 会恒为 0，
// 导致 2-G2 的 watermark 证据失真。这里锁定 gauge 与块表真实值一致。
TEST_CASE("KV watermark 指标随准入上报") {
    CbHarness harness(1, 16, 16, 0.05);
    const auto sink = harness.submit("req-w", 8, 2, 120);
    harness.run_until_closed({sink});
    const KVUsage usage = harness.kv.usage();
    CHECK(usage.watermark_blocks > 0);
    CHECK(harness.metrics.get("llmopt_kv_watermark_blocks") == usage.watermark_blocks);
}

// 3-G1：长 prompt 切块后，输出必须与整段一次 prefill 的逐 token 结果一致。
TEST_CASE("切块输出与整段 prefill 逐 token 一致") {
    CbHarness chunked(1, 512, 256, 0.0, 128);
    const auto chunked_sink = chunked.submit("req-eq-chunk", 320, 4, 100);
    chunked.run_until_closed({chunked_sink});

    CbHarness whole(1, 512, 256, 0.0, 1024);
    const auto whole_sink = whole.submit("req-eq-whole", 320, 4, 100);
    whole.run_until_closed({whole_sink});

    const std::vector<Delta> chunked_frames = drain(chunked_sink);
    const std::vector<Delta> whole_frames = drain(whole_sink);
    REQUIRE(chunked_frames.size() == whole_frames.size());
    for (std::size_t index = 0; index < chunked_frames.size(); ++index) {
        CHECK(chunked_frames[index].token_id == whole_frames[index].token_id);
        CHECK(chunked_frames[index].text == whole_frames[index].text);
    }
    CHECK(chunked.metrics.get("llmopt_prefill_tokens_total") == 320);
    CHECK(chunked.metrics.get("llmopt_chunk_prefill_steps_total") == 2);
    CHECK(chunked.metrics.get("llmopt_chunk_prefill_tokens_total") == 256);
    CHECK(whole.metrics.get("llmopt_chunk_prefill_steps_total") == 0);
    CHECK(chunked.metrics.get("llmopt_kv_reconcile_diffs_total") == 0);
}

// 3-G1：chunk 宽大于单步预算时按预算截断，跨 tick 推进；只有末块需要采样。
TEST_CASE("chunk 宽大于预算时按预算截断并跨 tick 推进") {
    CbHarness harness(1, 8, 64, 0.0, 128);
    const auto sink = harness.submit("req-chunk-budget", 20, 2, 100);
    harness.run_until_closed({sink});
    const std::vector<Batch> batches = harness.runner->batches();
    REQUIRE(batches.size() == 4);
    CHECK(batches[0].total_tokens == 8);
    REQUIRE(batches[0].new_reqs.size() == 1);
    CHECK(batches[0].new_reqs.front().prefill_token_ids.size() == 8);
    CHECK_FALSE(batches[0].steps.front().needs_sampling);
    CHECK_FALSE(batches[1].steps.front().needs_sampling);
    CHECK(batches[2].steps.front().needs_sampling);
    CHECK(batches[2].steps.front().n_tokens == 4);
    CHECK(harness.metrics.get("llmopt_prefill_tokens_total") == 20);
    CHECK(harness.metrics.get("llmopt_chunk_prefill_steps_total") == 2);
    CHECK(harness.metrics.get("llmopt_chunk_prefill_tokens_total") == 16);
    CHECK(drain(sink).size() == 2);
    harness.shutdown_flush();
}

// 3-G1 / F-03：在跑 decode 先保位，续 prefill 取下一 chunk，两者同批且总量受预算约束。
TEST_CASE("长 prompt 切块与在跑 decode 混批，decode 保位") {
    CbHarness harness(2, 256, 256, 0.0, 128);
    const auto long_req = harness.submit("req-mix-long", 320, 2, 100);
    const auto short_req = harness.submit("req-mix-short", 4, 4, 200);
    harness.run_until_closed({long_req, short_req});

    bool mixed = false;
    std::int32_t chunk_sizes = 0;
    for (const Batch& batch : harness.runner->batches()) {
        CHECK(batch.total_tokens <= 256);
        if (batch.cached_reqs.size() == 1 && batch.new_reqs.size() == 1) {
            mixed = true;
            CHECK(batch.steps.front().phase == Phase::Decode);
            CHECK(batch.steps.back().phase != Phase::Decode);
            CHECK(batch.steps.back().n_tokens <= 128);
            chunk_sizes += batch.new_reqs.front().prefill_token_ids.size();
        }
    }
    CHECK(mixed);
    // 首个 chunk 128 与短请求 prefill 同批；混批阶段只剩 128 + 64 = 192。
    CHECK(chunk_sizes == 192);
    CHECK(harness.metrics.get("llmopt_batch_prefill_tokens_last") >= 0);
    CHECK(harness.metrics.get("llmopt_batch_decode_seqs_last") >= 1);
}

// 3-G1：chunk 进行中取消不能等末块，必须按 Aborted 收尾并释放块。
TEST_CASE("chunked prefill 中途取消按 Aborted 收尾") {
    CbHarness harness(1, 256, 256, 0.0, 128);
    const auto sink = harness.submit("req-cancel-chunk", 320, 4, 100);
    (void)harness.core.run_once(20);
    (void)wait_batches(harness, 1);
    harness.core.cancel("req-cancel-chunk");
    harness.run_until_closed({sink});
    const std::vector<Delta> frames = drain(sink);
    REQUIRE(!frames.empty());
    REQUIRE(frames.back().finish.has_value());
    CHECK(*frames.back().finish == FinishReason::Aborted);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"cancelled\"}") == 1);
    harness.shutdown_flush();
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
}

TEST_CASE("调度与 KV 指标随批次和收尾更新") {
    CbHarness harness(4, 64, 64);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 4; ++index) {
        sinks.push_back(harness.submit("req-m" + std::to_string(index), 6, 2, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    (void)wait_batches(harness, 1);
    CHECK(harness.metrics.get("llmopt_batch_size_peak") == 4);
    CHECK(harness.metrics.get("llmopt_batch_tokens_peak") == 24);
    CHECK(harness.metrics.get("llmopt_scheduler_steps_total") >= 1);
    CHECK(harness.metrics.get("llmopt_schedule_step_ns_last") >= 0);
    CHECK(harness.metrics.get("llmopt_kv_held_blocks") ==
          harness.metrics.get("llmopt_kv_used_blocks") -
                  harness.metrics.get("llmopt_kv_cached_blocks"));
    harness.run_until_closed(sinks);
    CHECK(harness.metrics.get("llmopt_kv_held_blocks") == 0);
    CHECK(harness.metrics.get("llmopt_kv_reconcile_diffs_total") == 0);
}
