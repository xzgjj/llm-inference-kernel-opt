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
    CbHarness(std::int32_t max_seqs, std::int32_t budget, std::int32_t blocks)
        : runner(std::make_unique<RecordingRunner>()), kv(KVConfig{4, blocks, 0.0, true, 2}),
          core(*runner, kv, metrics, SchedulerConfig{max_seqs, budget, 64, true}) {
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

TEST_CASE("同一 tick 到达的 4 条请求合成一个 prefill 批") {
    CbHarness harness(4, 64, 64);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 4; ++index) {
        sinks.push_back(harness.submit("req-" + std::to_string(index), 6, 2, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    const std::vector<Batch> batches = wait_batches(harness, 1);
    REQUIRE(batches.size() == 1);
    CHECK(batches[0].steps.size() == 4);
    CHECK(batches[0].new_reqs.size() == 4);
    CHECK(batches[0].cached_reqs.empty());
    CHECK(batches[0].total_tokens == 24);
    harness.run_until_closed(sinks);
    CHECK(harness.core.running_count() == 0);
}

TEST_CASE("预算 8 时 6-token prefill 只准入一条，其余留队") {
    CbHarness harness(4, 8, 64);
    std::vector<std::shared_ptr<BoundedSink>> sinks;
    for (std::int32_t index = 0; index < 3; ++index) {
        sinks.push_back(harness.submit("req-b" + std::to_string(index), 6, 1, 100 + index * 10));
    }
    (void)harness.core.run_once(20);
    CHECK(harness.core.running_count() == 1);
    CHECK(harness.core.waiting_count() == 2);
    CHECK(harness.metrics.get("llmopt_admission_rejected_total{reason=\"token_budget\"}") >= 1);
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
