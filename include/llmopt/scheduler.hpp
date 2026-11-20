// scheduler.hpp — C-12 有界 Mailbox 与单线程 SchedulerCore；状态只由 Core 写。
#ifndef LLMOPT_SCHEDULER_HPP
#define LLMOPT_SCHEDULER_HPP

#include "llmopt/batch.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/sink.hpp"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llmopt {

struct SchedulerConfig {
    std::int32_t max_concurrency = 1; // 阶段 1 固定 1
    std::int32_t queue_capacity = 64;
};

struct SequenceGroup {
    RequestId req_id;
    std::vector<std::int32_t> prompt_ids;
    SamplingParams sampling;
    bool stream = false;
    std::shared_ptr<ResponseSink> sink;
    Sequence seq;
    bool admitted = false;
};

enum class EventKind : std::uint8_t { AddRequest, Cancel, Completion, Shutdown };

struct Event {
    EventKind kind = EventKind::AddRequest;
    RequestId seq_id;
    std::shared_ptr<SequenceGroup> group;
    Completion completion;
};

// 有界 MPMC：满即背压；只有入队与出队，不做状态判断。
class SchedulerMailbox {
public:
    explicit SchedulerMailbox(std::int32_t capacity);
    bool push(Event event);
    bool try_push(Event event);
    bool wait_pop(Event& out);
    bool wait_pop_for(Event& out, int timeout_ms);
    bool try_pop(Event& out);
    std::int32_t size() const noexcept;

private:
    std::deque<Event> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::int32_t capacity_;
};

// 执行线程只做 batch→decode_step→Completion 回投。
class StepExecutor {
public:
    StepExecutor(ModelRunner& runner, SchedulerMailbox& mailbox, MetricsRegistry& metrics);
    ~StepExecutor();
    bool submit(Batch batch);
    void stop();

private:
    void loop();
    ModelRunner& runner_;
    SchedulerMailbox& mailbox_;
    MetricsRegistry& metrics_;
    std::deque<Batch> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stopping_ = false;
};

class SchedulerCore {
public:
    SchedulerCore(ModelRunner& runner, KVBlockManager& kv, MetricsRegistry& metrics,
                  SchedulerConfig config);
    ~SchedulerCore();

    ErrCode submit(InferenceRequest req, std::vector<std::int32_t> prompt_ids,
                   std::shared_ptr<ResponseSink> sink);
    void cancel(const RequestId& req_id) noexcept;
    void shutdown() noexcept;

    // 线程体：运行到 Shutdown 且无在飞序列为止。
    void run();
    // 测试入口：处理一个事件并推进状态机，返回是否还有进展。
    bool run_once(int wait_ms);
    // 纯投影：阶段 1 batch=1，PrefillLast 采样、Decode 采样，PrefillChunk 不采样。
    Batch build_batch(const Sequence& seq) const;

    std::int32_t running_count() const noexcept;
    std::int32_t waiting_count() const noexcept;

private:
    void handle(Event& event);
    void start_next();
    void admit(const std::shared_ptr<SequenceGroup>& group);
    void dispatch(const Sequence& seq);
    void apply_completion(const Completion& completion);
    void finish(const std::shared_ptr<SequenceGroup>& group, FinishReason reason,
                bool final_frame_pushed = false);
    bool has_pending() const noexcept;

    ModelRunner& runner_;
    KVBlockManager& kv_;
    MetricsRegistry& metrics_;
    SchedulerConfig config_;
    SchedulerMailbox mailbox_;
    StepExecutor executor_;
    std::deque<std::shared_ptr<SequenceGroup>> waiting_;
    std::shared_ptr<SequenceGroup> running_;
    std::int32_t last_step_tokens_ = 0;
    bool pending_step_ = false;
    bool cancel_pending_ = false;
    bool shutting_down_ = false;
};

} // namespace llmopt

#endif // LLMOPT_SCHEDULER_HPP
