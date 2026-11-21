// scheduler_test.cc — C-12/C-13 契约：批次投影、生命周期、取消与背压。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/scheduler.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 一套完整装配：stub 后端 + 块表 + 指标 + Core。
struct Harness {
    explicit Harness(SchedulerConfig config = SchedulerConfig{1, 8})
        : runner(make_stub_runner()), kv(KVConfig{8, 8}), core(*runner, kv, metrics, config) {
        ModelConfig model;
        model.n_ctx = 512;
        REQUIRE(runner->load_model(model) == ErrCode::Ok);
    }

    // 提交一条请求，返回可观测的 sink。
    std::shared_ptr<BoundedSink> submit(const std::string& id, std::int32_t max_tokens) {
        InferenceRequest request;
        request.req_id = id;
        request.sampling.max_tokens = max_tokens;
        request.stream = true;
        auto sink = std::make_shared<BoundedSink>(64);
        REQUIRE(core.submit(request, {256 + 104, 256 + 105}, sink) == ErrCode::Ok);
        return sink;
    }

    // 推进事件直到 sink 关闭，避免测试里写死次数。
    void pump(const std::shared_ptr<BoundedSink>& sink, int rounds = 400) {
        for (int index = 0; index < rounds && !sink->is_closed(); ++index) {
            (void)core.run_once(20);
        }
        REQUIRE(sink->is_closed());
    }

    std::unique_ptr<ModelRunner> runner;
    KVBlockManager kv;
    MetricsRegistry metrics;
    SchedulerCore core;
};

// 故障注入后端：按开关让 add_sequence 或 decode_step 失败，用于验证回收与回投。
class FailingRunner : public ModelRunner {
public:
    explicit FailingRunner(bool fail_add) : fail_add_(fail_add) {}

    ErrCode load_model(const ModelConfig&) override {
        return ErrCode::Ok;
    }
    ErrCode add_sequence(const Sequence&) override {
        return fail_add_ ? ErrCode::ExecutionFailed : ErrCode::Ok;
    }
    RunnerStepOutput decode_step(const Batch&) override {
        RunnerStepOutput output;
        output.error = make_error(ErrCode::ExecutionFailed, "failing runner");
        return output;
    }
    void release_sequence(const RequestId&, std::int32_t) override {}
    void release() override {}
    ErrCode tokenize(const std::string&, std::vector<std::int32_t>&) override {
        return ErrCode::Ok;
    }
    std::string detokenize(std::int32_t) override {
        return "";
    }
    bool is_eos(std::int32_t) const override {
        return false;
    }
    std::int32_t n_ctx() const noexcept override {
        return 512;
    }
    const char* backend_name() const noexcept override {
        return "failing";
    }

private:
    bool fail_add_;
};

// 装配自定义后端，复用与 Harness 相同的块表与指标。
struct FaultHarness {
    explicit FaultHarness(bool fail_add, KVConfig kv_config = KVConfig{8, 8})
        : runner(std::make_unique<FailingRunner>(fail_add)), kv(kv_config),
          core(*runner, kv, metrics, SchedulerConfig{1, 8}) {}

    std::shared_ptr<BoundedSink> submit() {
        InferenceRequest request;
        request.req_id = "req-fault";
        request.stream = true;
        auto sink = std::make_shared<BoundedSink>(8);
        REQUIRE(core.submit(request, {1, 2, 3}, sink) == ErrCode::Ok);
        return sink;
    }

    std::unique_ptr<ModelRunner> runner;
    KVBlockManager kv;
    MetricsRegistry metrics;
    SchedulerCore core;
};

// 收集全部增量帧，供断言 token 数与结束原因。
std::vector<Delta> drain(const std::shared_ptr<BoundedSink>& sink) {
    std::vector<Delta> frames;
    Delta delta;
    while (sink->pop_frame(delta, 0)) {
        frames.push_back(delta);
    }
    return frames;
}

} // namespace

TEST_CASE("Prefill 投影带采样点，Decode 投影只带新 token") {
    Harness harness;
    Sequence seq;
    seq.group_id = "req-proj";
    seq.prompt_len = 5;
    seq.prompt_token_ids = {10, 11, 12, 13, 14};

    const Batch prefill = harness.core.build_batch(seq);
    REQUIRE(prefill.new_reqs.size() == 1);
    CHECK(prefill.cached_reqs.empty());
    CHECK(prefill.total_tokens == 5);
    REQUIRE(prefill.steps.size() == 1);
    CHECK(prefill.steps.front().phase == Phase::PrefillLast);
    CHECK(prefill.steps.front().needs_sampling);
    CHECK(prefill.steps.front().sample_pos == 4);
    CHECK(prefill.new_reqs.front().prefill_token_ids.size() == 5);

    seq.num_computed = 5;
    seq.output_token_ids = {99};
    const Batch decode = harness.core.build_batch(seq);
    REQUIRE(decode.cached_reqs.size() == 1);
    CHECK(decode.new_reqs.empty());
    CHECK(decode.total_tokens == 1);
    CHECK(decode.steps.front().phase == Phase::Decode);
    CHECK(decode.steps.front().pos_begin == 5);
    CHECK(decode.cached_reqs.front().new_token_ids.front() == 99);
}

TEST_CASE("单请求跑到 max_tokens 后释放块并关闭 sink") {
    Harness harness;
    const auto sink = harness.submit("req-life", 3);
    harness.pump(sink);
    const std::vector<Delta> frames = drain(sink);
    REQUIRE(frames.size() == 3);
    CHECK(frames.back().finish.has_value());
    CHECK(*frames.back().finish == FinishReason::LengthCapped);
    CHECK(harness.core.running_count() == 0);
    CHECK(harness.core.waiting_count() == 0);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"ok\"}") == 1);
    CHECK(harness.metrics.get("llmopt_tokens_generated_total") == 3);
}

TEST_CASE("取消排队请求不影响正在运行的请求") {
    Harness harness;
    const auto first = harness.submit("req-a", 5);
    const auto second = harness.submit("req-b", 2);
    (void)harness.core.run_once(20);
    REQUIRE(harness.core.running_count() == 1);
    harness.core.cancel("req-b");
    harness.pump(first);
    harness.pump(second);

    const std::vector<Delta> first_frames = drain(first);
    REQUIRE(first_frames.size() == 5);
    CHECK(*first_frames.back().finish == FinishReason::LengthCapped);
    const std::vector<Delta> second_frames = drain(second);
    REQUIRE(second_frames.size() == 1);
    CHECK(*second_frames.back().finish == FinishReason::Aborted);
    CHECK(harness.core.running_count() == 0);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"cancelled\"}") == 1);
}

TEST_CASE("取消运行中请求在下一步结束并回收") {
    Harness harness;
    const auto sink = harness.submit("req-run", 20);
    Delta frame;
    bool got = false;
    for (int index = 0; index < 100 && !got; ++index) {
        (void)harness.core.run_once(20);
        got = sink->pop_frame(frame, 0);
    }
    REQUIRE(got);
    harness.core.cancel("req-run");
    harness.pump(sink);
    const std::vector<Delta> frames = drain(sink);
    CHECK(!frames.empty());
    CHECK(frames.back().finish.has_value());
    CHECK(*frames.back().finish == FinishReason::Aborted);
    CHECK(harness.core.running_count() == 0);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"cancelled\"}") == 1);
}

TEST_CASE("mailbox 有界且先进先出") {
    SchedulerMailbox mailbox(2);
    Event first;
    first.kind = EventKind::AddRequest;
    first.seq_id = "a";
    Event second;
    second.kind = EventKind::AddRequest;
    second.seq_id = "b";
    CHECK(mailbox.push(first));
    CHECK(mailbox.push(second));
    CHECK_FALSE(mailbox.try_push(first));
    CHECK(mailbox.size() == 2);
    Event out;
    REQUIRE(mailbox.try_pop(out));
    CHECK(out.seq_id == "a");
    REQUIRE(mailbox.try_pop(out));
    CHECK(out.seq_id == "b");
}

TEST_CASE("关停会中止运行中与排队中的请求") {
    Harness harness;
    const auto first = harness.submit("req-s1", 6);
    const auto second = harness.submit("req-s2", 4);
    (void)harness.core.run_once(20);
    REQUIRE(harness.core.running_count() == 1);
    harness.core.shutdown();
    for (int index = 0; index < 200 && (!first->is_closed() || !second->is_closed()); ++index) {
        (void)harness.core.run_once(20);
    }
    REQUIRE(first->is_closed());
    REQUIRE(second->is_closed());
    const std::vector<Delta> second_frames = drain(second);
    REQUIRE(second_frames.size() == 1);
    CHECK(*second_frames.back().finish == FinishReason::Aborted);
    CHECK(harness.core.waiting_count() == 0);
}

TEST_CASE("add_sequence 失败时归还已申请的 KV 块") {
    FaultHarness harness(true);
    const auto sink = harness.submit();
    (void)harness.core.run_once(50);
    REQUIRE(sink->is_closed());
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    CHECK(harness.core.running_count() == 0);
    CHECK(harness.metrics.get("llmopt_requests_total{status=\"error\"}") == 1);
}

TEST_CASE("decode_step 失败仍回投完成事件并关闭 sink") {
    // 块表给足，确保用例终点是"后端采样失败"而不是准入被 ContextOverflow 拦下。
    FaultHarness harness(false, KVConfig{32, 8});
    const auto sink = harness.submit();
    for (int index = 0; index < 20 && !sink->is_closed(); ++index) {
        (void)harness.core.run_once(20);
    }
    REQUIRE(sink->is_closed());
    CHECK(harness.core.running_count() == 0);
    CHECK(harness.kv.num_free_blocks() == harness.kv.num_total_blocks());
    // 失败必须先落一条错误帧，客户端才能区分"正常结束"与"后端失败"。
    const std::vector<Delta> frames = drain(sink);
    bool saw_error = false;
    for (const Delta& frame : frames) {
        if (frame.error && frame.error->code == ErrCode::ExecutionFailed) {
            saw_error = true;
        }
    }
    CHECK(saw_error);
}

TEST_CASE("准入预算满时 submit 返回 QueueFull") {
    Harness harness(SchedulerConfig{1, 2, 2});
    // 预算 = max_concurrency(1) + queue_capacity(2)；mailbox 与预算同宽，不再提前背压。
    for (int index = 0; index < 3; ++index) {
        InferenceRequest request;
        request.req_id = "admit-" + std::to_string(index);
        CHECK(harness.core.submit(request, {}, nullptr) == ErrCode::Ok);
    }
    InferenceRequest overflow;
    overflow.req_id = "admit-overflow";
    CHECK(harness.core.submit(overflow, {}, nullptr) == ErrCode::QueueFull);
}
