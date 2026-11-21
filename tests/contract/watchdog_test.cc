// watchdog_test.cc — Completion watchdog：执行线程不回投时请求按 500 收尾，
// 但资源必须等迟到 Completion 回到 Core 后再释放，避免 UAF。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"
#include "llmopt/sink.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace llmopt;

namespace {

// 每步固定睡过 watchdog 时限，再正常返回；用真实线程模拟“回投迟到”而不是永久挂死。
class SlowRunner final : public ModelRunner {
public:
    explicit SlowRunner(std::int32_t delay_ms) : delay_ms_(delay_ms), inner_(make_stub_runner()) {}

    ErrCode load_model(const ModelConfig& config) override {
        return inner_->load_model(config);
    }
    ErrCode add_sequence(const Sequence& seq) override {
        return inner_->add_sequence(seq);
    }
    RunnerStepOutput decode_step(const Batch& batch) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        return inner_->decode_step(batch);
    }
    void release_sequence(const BackendId& backend_id, std::int32_t keep_tokens) override {
        inner_->release_sequence(backend_id, keep_tokens);
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
    std::int32_t seq_pos_max(const BackendId& backend_id) const override {
        return inner_->seq_pos_max(backend_id);
    }
    bool supports_live_prefix() const noexcept override {
        return inner_->supports_live_prefix();
    }
    bool supports_prefix_share() const noexcept override {
        return inner_->supports_prefix_share();
    }
    const char* backend_name() const noexcept override {
        return inner_->backend_name();
    }

private:
    std::int32_t delay_ms_;
    std::unique_ptr<ModelRunner> inner_;
};

} // namespace

TEST_CASE("Completion 超时先回 500，迟到 Completion 再释放 KV，shutdown 不挂") {
    SlowRunner runner(1200);
    ModelConfig model;
    model.n_ctx = 256;
    REQUIRE(runner.load_model(model) == ErrCode::Ok);

    KVConfig kv_config;
    kv_config.num_blocks = 64;
    kv_config.enable_prefix_cache = false;
    KVBlockManager kv(kv_config);
    MetricsRegistry metrics;

    SchedulerConfig config;
    config.max_concurrency = 1;
    config.queue_capacity = 1;
    config.enable_prefix_cache = false;
    config.request_deadline_ns = 10'000'000'000LL;
    config.completion_timeout_ns = 300'000'000LL;
    SchedulerCore core(runner, kv, metrics, config);
    std::thread core_thread([&core] { core.run(); });

    InferenceRequest request;
    request.req_id = "watchdog-req";
    request.stream = true;
    request.sampling.max_tokens = 8;
    auto sink = std::make_shared<BoundedSink>(64);
    std::vector<std::int32_t> prompt(8, 512);
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(core.submit(request, std::move(prompt), sink) == ErrCode::Ok);

    bool closed = false;
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(1)) {
        if (sink->is_closed()) {
            closed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(closed);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));

    Error error;
    Delta delta;
    while (sink->pop_frame(delta, 0)) {
        if (delta.error) {
            error = *delta.error;
        }
    }
    CHECK(error.code == ErrCode::ExecutionFailed);
    CHECK(error.message.find("超时") != std::string::npos);

    const auto shutdown_started = std::chrono::steady_clock::now();
    core.shutdown();
    core_thread.join();
    CHECK(std::chrono::steady_clock::now() - shutdown_started < std::chrono::seconds(5));
    CHECK(kv.num_free_blocks() == kv.num_total_blocks());
    CHECK(metrics.get(MetricId::CompletionTimeoutsTotal) == 1);
}
