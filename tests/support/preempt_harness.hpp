// preempt_harness.hpp — 抢占/恢复用例共用装配：stub 后端 + 小 KV 池 + 可控配置。
// 只在测试内使用；所有成员公开，便于用例断言块表、指标与 trace。
#ifndef LLMOPT_TEST_PREEMPT_HARNESS_HPP
#define LLMOPT_TEST_PREEMPT_HARNESS_HPP

#include "doctest/doctest.h"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace llmopt::test {

// 可暂停的 stub 后端：每放行一次只执行一批，避免 run_once 把整条请求跑完。
class ControlledRunner final : public ModelRunner {
public:
    explicit ControlledRunner(std::unique_ptr<ModelRunner> inner) : inner_(std::move(inner)) {}

    ErrCode load_model(const ModelConfig& config) override {
        return inner_->load_model(config);
    }
    ErrCode add_sequence(const Sequence& seq) override {
        return inner_->add_sequence(seq);
    }
    RunnerStepOutput decode_step(const Batch& batch) override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return permits_ > 0; });
        --permits_;
        lock.unlock();
        return inner_->decode_step(batch);
    }
    void release_sequence(const RequestId& req_id, std::int32_t keep_tokens = 0) override {
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
    bool supports_live_prefix() const noexcept override {
        return inner_->supports_live_prefix();
    }
    bool supports_prefix_share() const noexcept override {
        return inner_->supports_prefix_share();
    }
    const char* backend_name() const noexcept override {
        return inner_->backend_name();
    }

    void release_steps(std::int32_t count = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        permits_ = std::max(permits_, count);
        cv_.notify_all();
    }

private:
    std::unique_ptr<ModelRunner> inner_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::int32_t permits_ = 0;
};

// 组装一套可重复的调度环境：KV 块数与 token 宽度由调用方指定。
struct PreemptHarness {
    explicit PreemptHarness(SchedulerConfig scheduler, KVConfig kv_config,
                            bool controlled_mode = false)
        : runner(make_runner(controlled_mode)),
          controlled(controlled_mode ? static_cast<ControlledRunner*>(runner.get()) : nullptr),
          kv(kv_config), core(*runner, kv, metrics, std::move(scheduler)) {
        ModelConfig model;
        model.n_ctx = 512;
        REQUIRE(runner->load_model(model) == ErrCode::Ok);
    }

    ~PreemptHarness() {
        // 放行执行线程，避免析构时卡在 decode_step 等待。
        if (controlled != nullptr) {
            controlled->release_steps(1024);
        }
    }

    static std::unique_ptr<ModelRunner> make_runner(bool controlled_mode) {
        if (controlled_mode) {
            return std::make_unique<ControlledRunner>(make_stub_runner());
        }
        return make_stub_runner();
    }

    bool run_once(int wait_ms = 20) {
        if (controlled != nullptr) {
            controlled->release_steps(1);
        }
        return core.run_once(wait_ms);
    }

    // prompt 长度 = token 数；seed 固定后，抢占前后可逐 token 比对。
    std::shared_ptr<BoundedSink> submit(const std::string& id, std::int32_t prompt_tokens,
                                        std::int32_t max_tokens, std::int32_t seed = 7) {
        std::vector<std::int32_t> prompt;
        prompt.reserve(static_cast<std::size_t>(prompt_tokens));
        for (std::int32_t index = 0; index < prompt_tokens; ++index) {
            prompt.push_back(256 + 65 + index % 26);
        }
        InferenceRequest request;
        request.req_id = id;
        request.sampling.max_tokens = max_tokens;
        request.sampling.seed = seed;
        request.stream = true;
        auto sink = std::make_shared<BoundedSink>(128);
        REQUIRE(core.submit(request, std::move(prompt), sink) == ErrCode::Ok);
        return sink;
    }

    // 推进到谓词成立或轮次耗尽；返回是否满足。
    bool run_until(const std::function<bool()>& done, int rounds = 400, int wait_ms = 20) {
        for (int index = 0; index < rounds; ++index) {
            if (done()) {
                return true;
            }
            (void)run_once(wait_ms);
        }
        return done();
    }

    void pump(const std::shared_ptr<BoundedSink>& sink, int rounds = 400) {
        REQUIRE(run_until([&] { return sink->is_closed(); }, rounds));
    }

    std::unique_ptr<ModelRunner> runner;
    ControlledRunner* controlled = nullptr;
    KVBlockManager kv;
    MetricsRegistry metrics;
    SchedulerCore core;
};

// 收集全部增量帧，供 token 序列与结束原因断言。
inline std::vector<Delta> drain(const std::shared_ptr<BoundedSink>& sink) {
    std::vector<Delta> frames;
    Delta delta;
    while (sink->pop_frame(delta, 0)) {
        frames.push_back(delta);
    }
    return frames;
}

// 只保留 token id，便于与未中断基线逐项比较。
inline std::vector<std::int32_t> token_ids(const std::vector<Delta>& frames) {
    std::vector<std::int32_t> ids;
    ids.reserve(frames.size());
    for (const Delta& frame : frames) {
        ids.push_back(frame.token_id);
    }
    return ids;
}

} // namespace llmopt::test

#endif // LLMOPT_TEST_PREEMPT_HARNESS_HPP
