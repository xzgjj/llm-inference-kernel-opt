// scheduler.cpp — 单线程状态机 + 单线程执行；状态只由 Core 写，Completion 只回投。
#include "llmopt/scheduler.hpp"

#include <atomic>
#include <chrono>

namespace llmopt {
namespace {

std::atomic<std::int32_t> g_seq_id{0};

Event make_event(EventKind kind, RequestId id) {
    Event event;
    event.kind = kind;
    event.seq_id = std::move(id);
    return event;
}

} // namespace

SchedulerMailbox::SchedulerMailbox(std::int32_t capacity) : capacity_(capacity) {
    if (capacity_ < 1) {
        capacity_ = 1;
    }
}

bool SchedulerMailbox::push(Event event) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (static_cast<std::int32_t>(queue_.size()) >= capacity_) {
        return false;
    }
    queue_.push_back(std::move(event));
    lock.unlock();
    cv_.notify_one();
    return true;
}

bool SchedulerMailbox::try_push(Event event) {
    return push(std::move(event));
}

bool SchedulerMailbox::wait_pop(Event& out) {
    return wait_pop_for(out, -1);
}

bool SchedulerMailbox::wait_pop_for(Event& out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout_ms < 0) {
        cv_.wait(lock, [this] { return !queue_.empty(); });
    } else {
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this] { return !queue_.empty(); });
    }
    if (queue_.empty()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool SchedulerMailbox::try_pop(Event& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

std::int32_t SchedulerMailbox::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::int32_t>(queue_.size());
}

StepExecutor::StepExecutor(ModelRunner& runner, SchedulerMailbox& mailbox, MetricsRegistry& metrics)
    : runner_(runner), mailbox_(mailbox), metrics_(metrics) {
    thread_ = std::thread([this] { loop(); });
}

StepExecutor::~StepExecutor() {
    stop();
}

bool StepExecutor::submit(Batch batch) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        queue_.push_back(std::move(batch));
    }
    cv_.notify_one();
    return true;
}

void StepExecutor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void StepExecutor::loop() {
    while (true) {
        Batch batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            batch = std::move(queue_.front());
            queue_.pop_front();
        }
        RunnerStepOutput output = runner_.decode_step(batch);
        metrics_.inc("llmopt_forward_steps_total");
        // 即使 runner 提前失败（req_ids 为空）也要回投，避免 Core 永久等待 pending。
        for (std::size_t i = 0; i < batch.steps.size(); ++i) {
            const SequenceStep& step = batch.steps[i];
            Event event = make_event(EventKind::Completion, step.route.req_id);
            event.completion.seq_id = step.route.req_id;
            event.completion.seq_serial = step.kv.seq_id;
            event.completion.err = output.err;
            if (i < output.sampled.size()) {
                event.completion.sampled.push_back(output.sampled[i]);
            }
            mailbox_.push(std::move(event));
        }
    }
}

SchedulerCore::SchedulerCore(ModelRunner& runner, KVBlockManager& kv, MetricsRegistry& metrics,
                             SchedulerConfig config)
    : runner_(runner), kv_(kv), metrics_(metrics), config_(config), mailbox_(config.queue_capacity),
      executor_(runner, mailbox_, metrics) {}

SchedulerCore::~SchedulerCore() {
    shutdown();
}

ErrCode SchedulerCore::submit(InferenceRequest req, std::vector<std::int32_t> prompt_ids,
                              std::shared_ptr<ResponseSink> sink) {
    auto group = std::make_shared<SequenceGroup>();
    group->req_id = req.req_id;
    group->prompt_ids = std::move(prompt_ids);
    group->sampling = req.sampling;
    group->stream = req.stream;
    group->sink = std::move(sink);
    group->seq.id = g_seq_id.fetch_add(1, std::memory_order_relaxed);
    group->seq.group_id = group->req_id;
    group->seq.prompt_len = static_cast<std::int32_t>(group->prompt_ids.size());
    group->seq.prompt_token_ids = group->prompt_ids;
    group->seq.sampling = group->sampling;
    Event event = make_event(EventKind::AddRequest, group->req_id);
    event.group = std::move(group);
    if (!mailbox_.push(std::move(event))) {
        metrics_.inc("llmopt_requests_total{status=\"queue_full\"}");
        return ErrCode::QueueFull;
    }
    return ErrCode::Ok;
}

void SchedulerCore::cancel(const RequestId& req_id) noexcept {
    Event event = make_event(EventKind::Cancel, req_id);
    (void)mailbox_.push(std::move(event));
}

void SchedulerCore::shutdown() noexcept {
    Event event = make_event(EventKind::Shutdown, "");
    (void)mailbox_.push(std::move(event));
}

void SchedulerCore::run() {
    while (true) {
        (void)run_once(50);
        if (shutting_down_ && !running_ && !pending_step_) {
            break;
        }
    }
    executor_.stop();
}

bool SchedulerCore::run_once(int wait_ms) {
    Event event;
    if (!mailbox_.wait_pop_for(event, wait_ms)) {
        return false;
    }
    handle(event);
    return true;
}

void SchedulerCore::handle(Event& event) {
    switch (event.kind) {
    case EventKind::AddRequest:
        metrics_.inc("llmopt_requests_total{status=\"accepted\"}");
        if (running_) {
            waiting_.push_back(std::move(event.group));
            metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
        } else {
            admit(event.group);
        }
        break;
    case EventKind::Cancel:
        if (running_ && running_->req_id == event.seq_id) {
            running_->seq.cancel_requested.store(true);
            if (pending_step_) {
                cancel_pending_ = true;
            } else {
                finish(running_, FinishReason::Aborted);
            }
        } else {
            for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
                if ((*it)->req_id == event.seq_id) {
                    finish(*it, FinishReason::Aborted);
                    waiting_.erase(it);
                    metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
                    break;
                }
            }
        }
        break;
    case EventKind::Completion:
        if (running_ && running_->req_id == event.completion.seq_id) {
            apply_completion(event.completion);
        }
        break;
    case EventKind::Shutdown:
        shutting_down_ = true;
        if (running_) {
            if (pending_step_) {
                running_->seq.cancel_requested.store(true);
                cancel_pending_ = true;
            } else {
                finish(running_, FinishReason::Aborted);
            }
        }
        while (!waiting_.empty()) {
            finish(waiting_.front(), FinishReason::Aborted);
            waiting_.pop_front();
        }
        metrics_.set("llmopt_queue_depth", 0);
        break;
    default:
        break;
    }
}

void SchedulerCore::start_next() {
    if (shutting_down_ || running_ || waiting_.empty()) {
        return;
    }
    auto group = waiting_.front();
    waiting_.pop_front();
    metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
    admit(group);
}

void SchedulerCore::admit(const std::shared_ptr<SequenceGroup>& group) {
    Sequence& seq = group->seq;
    if (!seq.block_table.empty()) {
        kv_.free(seq);
    }
    if (!kv_.acquire(seq, seq.prompt_len).has_value()) {
        finish(group, FinishReason::Error);
        return;
    }
    if (runner_.add_sequence(seq) != ErrCode::Ok) {
        kv_.free(seq);
        finish(group, FinishReason::Error);
        return;
    }
    group->admitted = true;
    seq.status = SeqStatus::Running;
    running_ = group;
    metrics_.set("llmopt_running_sequences", 1);
    metrics_.set("llmopt_kv_free_blocks", kv_.num_free_blocks());
    dispatch(seq);
}

void SchedulerCore::dispatch(const Sequence& seq) {
    Batch batch = build_batch(seq);
    last_step_tokens_ = batch.total_tokens;
    if (batch.total_tokens <= 0 || !executor_.submit(std::move(batch))) {
        finish(running_, FinishReason::Error);
        return;
    }
    pending_step_ = true;
}

Batch SchedulerCore::build_batch(const Sequence& seq) const {
    Batch batch;
    SequenceStep step;
    const bool prefill = seq.num_computed == 0;
    if (prefill) {
        NewRequestData data;
        data.req_id = seq.group_id;
        data.prefill_token_ids = seq.prompt_token_ids;
        data.block_ids = seq.block_table;
        data.sampling = seq.sampling;
        batch.total_tokens = seq.prompt_len;
        batch.new_reqs.push_back(std::move(data));
        step.phase = Phase::PrefillLast;
        step.pos_begin = 0;
        step.pos_end = seq.prompt_len;
    } else {
        CachedRequestData data;
        data.req_id = seq.group_id;
        data.new_token_ids.push_back(seq.output_token_ids.back());
        batch.total_tokens = 1;
        batch.cached_reqs.push_back(std::move(data));
        step.phase = Phase::Decode;
        step.pos_begin = seq.num_computed;
        step.pos_end = seq.num_computed + 1;
    }
    step.seq_id = seq.group_id;
    step.n_tokens = batch.total_tokens;
    step.needs_sampling = true;
    step.sample_pos = step.pos_end - 1;
    step.route.req_id = seq.group_id;
    step.kv = KVPen{seq.id, 0};
    batch.steps.push_back(step);
    return batch;
}

void SchedulerCore::apply_completion(const Completion& completion) {
    // 过期事件：请求已结束或被取消后，同 req_id 的新请求不能消费旧 token。
    if (!pending_step_ || !running_ || running_->seq.id != completion.seq_serial) {
        return;
    }
    pending_step_ = false;
    if (!running_) {
        return;
    }
    Sequence& seq = running_->seq;
    if (completion.err != ErrCode::Ok || completion.sampled.empty()) {
        finish(running_, FinishReason::Error);
        return;
    }
    const SampledToken token = completion.sampled.front();
    seq.num_computed += last_step_tokens_;
    seq.num_output += 1;
    seq.output_token_ids.push_back(token.token_id);
    metrics_.inc("llmopt_tokens_generated_total");
    if (seq.num_computed == seq.prompt_len) {
        metrics_.inc("llmopt_prefill_tokens_total", seq.prompt_len);
    }
    const std::string text = runner_.detokenize(token.token_id);
    FinishReason reason = FinishReason::None;
    if (cancel_pending_ || seq.cancel_requested.load()) {
        reason = FinishReason::Aborted;
    } else if (runner_.is_eos(token.token_id)) {
        reason = FinishReason::Stopped;
    } else if (seq.num_output >= seq.sampling.max_tokens) {
        reason = FinishReason::LengthCapped;
    }
    Delta delta;
    delta.token_id = token.token_id;
    delta.text = text;
    if (reason != FinishReason::None) {
        delta.finish = reason;
    }
    if (!running_->sink || !running_->sink->push_frame(delta)) {
        finish(running_, FinishReason::Aborted);
        return;
    }
    if (reason != FinishReason::None) {
        finish(running_, reason, true);
        return;
    }
    dispatch(seq);
}

void SchedulerCore::finish(const std::shared_ptr<SequenceGroup>& group, FinishReason reason,
                           bool final_frame_pushed) {
    if (!group) {
        return;
    }
    Sequence& seq = group->seq;
    if (seq.status == SeqStatus::Finished) {
        return;
    }
    seq.finish = reason;
    seq.status = SeqStatus::Finished;
    if (!final_frame_pushed) {
        Delta delta;
        delta.finish = reason;
        if (group->sink) {
            (void)group->sink->push_frame(delta);
        }
    }
    if (group->sink) {
        group->sink->close();
    }
    if (group->admitted) {
        runner_.release_sequence(seq.group_id);
        kv_.free(seq);
    }
    if (running_ == group) {
        metrics_.set("llmopt_running_sequences", 0);
        metrics_.set("llmopt_kv_free_blocks", kv_.num_free_blocks());
        running_.reset();
        pending_step_ = false;
        cancel_pending_ = false;
    }
    if (reason == FinishReason::Aborted) {
        metrics_.inc("llmopt_requests_total{status=\"cancelled\"}");
    } else if (reason == FinishReason::Error) {
        metrics_.inc("llmopt_requests_total{status=\"error\"}");
    } else {
        metrics_.inc("llmopt_requests_total{status=\"ok\"}");
    }
    if (!shutting_down_) {
        start_next();
    }
}

bool SchedulerCore::has_pending() const noexcept {
    return pending_step_;
}

std::int32_t SchedulerCore::running_count() const noexcept {
    return running_ ? 1 : 0;
}

std::int32_t SchedulerCore::waiting_count() const noexcept {
    return static_cast<std::int32_t>(waiting_.size());
}

} // namespace llmopt
