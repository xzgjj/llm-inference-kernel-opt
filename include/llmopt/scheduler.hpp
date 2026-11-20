// scheduler.hpp — C-12 有界 Mailbox、C-P2-13 双预算与单线程 SchedulerCore。
// 连续批处理：每 tick 把 running 的 prefill/decode 与待准入请求合成一个不可变 Batch。
#ifndef LLMOPT_SCHEDULER_HPP
#define LLMOPT_SCHEDULER_HPP

#include "llmopt/batch.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/sink.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llmopt {

struct SchedulerConfig {
    std::int32_t max_concurrency = 8;
    std::int32_t max_num_batched_tokens = 2048;
    std::int32_t queue_capacity = 64;
    bool enable_prefix_cache = true;
};

// 请求数与 token 双预算：独立判定、互不替代（INV-07）。
class TokenBudget {
public:
    TokenBudget(std::int32_t max_seqs, std::int32_t max_tokens) noexcept;
    bool can_add(std::int32_t seqs, std::int32_t tokens) const noexcept;
    void commit(std::int32_t seqs, std::int32_t tokens) noexcept;
    void reset() noexcept;
    std::int32_t seqs_remain() const noexcept;
    std::int32_t tokens_remain() const noexcept;

private:
    std::int32_t max_seqs_;
    std::int32_t max_tokens_;
    std::int32_t seqs_used_ = 0;
    std::int32_t tokens_used_ = 0;
};

struct SequenceGroup {
    RequestId req_id;
    std::vector<std::int32_t> prompt_ids;
    SamplingParams sampling;
    bool stream = false;
    std::shared_ptr<ResponseSink> sink;
    Sequence seq;
    bool admitted = false;
    RequestId prefix_source;
    std::int32_t prefix_tokens = 0;
    // 半个 UTF-8 字符先攒在这里，完整后再下发，避免 JSON 编码异常。
    std::string utf8_pending;
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
    // 测试入口：处理一批事件并推进，返回是否有进展。
    bool run_once(int wait_ms);
    // 单序列投影，保留给纯逻辑用例。
    Batch build_batch(const Sequence& seq) const;

    std::int32_t running_count() const noexcept;
    std::int32_t waiting_count() const noexcept;

private:
    enum class DeliverResult : std::uint8_t { Continue, Finished, Failed };

    void handle(Event& event);
    void pump();
    bool admit(const std::shared_ptr<SequenceGroup>& group, TokenBudget& budget);
    void append_step(Batch& batch, const std::shared_ptr<SequenceGroup>& group) const;
    void apply_completion(const Completion& completion);
    DeliverResult deliver(const std::shared_ptr<SequenceGroup>& group, const SampledToken& token);
    void finish(const std::shared_ptr<SequenceGroup>& group, FinishReason reason,
                bool final_frame_pushed = false);
    void reap_donors();
    void reconcile(const Sequence& seq);
    std::shared_ptr<SequenceGroup> find_running(const RequestId& req_id) const;
    RequestId find_live_donor(const PrefixHit& hit) const;

    ModelRunner& runner_;
    KVBlockManager& kv_;
    MetricsRegistry& metrics_;
    SchedulerConfig config_;
    SchedulerMailbox mailbox_;
    StepExecutor executor_;
    std::deque<std::shared_ptr<SequenceGroup>> waiting_;
    std::vector<std::shared_ptr<SequenceGroup>> running_;
    std::vector<std::int32_t> pending_serials_;
    std::int32_t pending_remaining_ = 0;
    bool pending_step_ = false;
    bool shutting_down_ = false;
};

} // namespace llmopt

#endif // LLMOPT_SCHEDULER_HPP
