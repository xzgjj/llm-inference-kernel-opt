// scheduler.cpp — 单线程状态机 + 单线程执行；每 tick 合成连续批，状态只由 Core 写。
#include "llmopt/scheduler.hpp"

#include "llmopt/text.hpp"

#include <algorithm>
#include <atomic>

namespace llmopt {
namespace {

std::atomic<std::int32_t> g_seq_id{0};

Event make_event(EventKind kind, RequestId id) {
    Event event;
    event.kind = kind;
    event.seq_id = std::move(id);
    return event;
}

// 供日志/指标使用的简易原因名，避免分支里写字符串字面量。
const char* alloc_reason_name(AllocFailReason reason) {
    switch (reason) {
    case AllocFailReason::NoFreeBlock:
        return "no_free_block";
    case AllocFailReason::BelowWatermark:
        return "below_watermark";
    case AllocFailReason::ContextOverflow:
        return "context_overflow";
    case AllocFailReason::SequenceTooLong:
        return "sequence_too_long";
    default:
        return "none";
    }
}

} // namespace

TokenBudget::TokenBudget(std::int32_t max_seqs, std::int32_t max_tokens) noexcept
    : max_seqs_(max_seqs), max_tokens_(max_tokens) {}

bool TokenBudget::can_add(std::int32_t seqs, std::int32_t tokens) const noexcept {
    return seqs_used_ + seqs <= max_seqs_ && tokens_used_ + tokens <= max_tokens_;
}

void TokenBudget::commit(std::int32_t seqs, std::int32_t tokens) noexcept {
    seqs_used_ += seqs;
    tokens_used_ += tokens;
}

void TokenBudget::reset() noexcept {
    seqs_used_ = 0;
    tokens_used_ = 0;
}

std::int32_t TokenBudget::seqs_remain() const noexcept {
    return max_seqs_ - seqs_used_;
}

std::int32_t TokenBudget::tokens_remain() const noexcept {
    return max_tokens_ - tokens_used_;
}

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
        // 即使 runner 提前失败也要逐条回投，避免 Core 永久等待 pending。
        for (std::size_t i = 0; i < batch.steps.size(); ++i) {
            const SequenceStep& step = batch.steps[i];
            Event event = make_event(EventKind::Completion, step.route.req_id);
            event.completion.seq_id = step.route.req_id;
            event.completion.seq_serial = step.kv.seq_id;
            event.completion.computed_tokens = step.pos_end;
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
        if (shutting_down_ && !pending_step_ && running_.empty()) {
            break;
        }
    }
    for (const RequestId& donor : kv_.flush_cache()) {
        runner_.release_sequence(donor, 0);
    }
    executor_.stop();
}

bool SchedulerCore::run_once(int wait_ms) {
    Event event;
    if (!mailbox_.wait_pop_for(event, wait_ms)) {
        return false;
    }
    handle(event);
    Event extra;
    while (mailbox_.try_pop(extra)) {
        handle(extra);
    }
    pump();
    return true;
}

void SchedulerCore::handle(Event& event) {
    switch (event.kind) {
    case EventKind::AddRequest: {
        auto group = std::move(event.group);
        metrics_.inc("llmopt_requests_total{status=\"accepted\"}");
        if (static_cast<std::int32_t>(waiting_.size()) >= config_.queue_capacity) {
            metrics_.inc("llmopt_admission_rejected_total{reason=\"queue\"}");
            finish(group, FinishReason::Error);
            break;
        }
        waiting_.push_back(std::move(group));
        metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
        break;
    }
    case EventKind::Cancel:
        if (auto running = find_running(event.seq_id)) {
            running->seq.cancel_requested.store(true);
            if (!pending_step_) {
                finish(running, FinishReason::Aborted);
            }
            break;
        }
        for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
            if ((*it)->req_id == event.seq_id) {
                finish(*it, FinishReason::Aborted);
                waiting_.erase(it);
                metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
                break;
            }
        }
        break;
    case EventKind::Completion:
        apply_completion(event.completion);
        break;
    case EventKind::Shutdown:
        shutting_down_ = true;
        for (const auto& group : running_) {
            group->seq.cancel_requested.store(true);
        }
        if (!pending_step_) {
            for (const auto& group : running_) {
                finish(group, FinishReason::Aborted);
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

void SchedulerCore::pump() {
    if (pending_step_ || shutting_down_) {
        return;
    }
    // 上一 tick 淘汰的 donor 此刻才能真正释放：本批若还要借前缀，必须等复制完成。
    reap_donors();
    Batch batch;
    TokenBudget budget(config_.max_concurrency, config_.max_num_batched_tokens);
    for (const auto& group : running_) {
        append_step(batch, group);
    }
    while (!waiting_.empty()) {
        if (static_cast<std::int32_t>(running_.size()) >= config_.max_concurrency) {
            break;
        }
        auto group = waiting_.front();
        const std::int32_t remaining = group->seq.prompt_len - group->seq.num_computed;
        if (remaining > config_.max_num_batched_tokens) {
            metrics_.inc("llmopt_admission_rejected_total{reason=\"token_budget\"}");
            break;
        }
        if (!budget.can_add(1, remaining)) {
            metrics_.inc("llmopt_admission_rejected_total{reason=\"token_budget\"}");
            break;
        }
        if (!admit(group, budget)) {
            break;
        }
        waiting_.pop_front();
        metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
        append_step(batch, group);
    }
    if (batch.steps.empty()) {
        return;
    }
    pending_serials_.clear();
    for (const SequenceStep& step : batch.steps) {
        pending_serials_.push_back(step.kv.seq_id);
    }
    pending_remaining_ = static_cast<std::int32_t>(batch.steps.size());
    const bool submitted = executor_.submit(std::move(batch));
    if (!submitted) {
        pending_remaining_ = 0;
        return;
    }
    pending_step_ = true;
    metrics_.set("llmopt_running_sequences", static_cast<std::int64_t>(running_.size()));
    metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
    KVUsage usage = kv_.usage();
    metrics_.set("llmopt_kv_used_blocks", usage.used_blocks);
    metrics_.set("llmopt_kv_cached_blocks", usage.cached_blocks);
    metrics_.set("llmopt_kv_shared_blocks", usage.shared_blocks);
    metrics_.set("llmopt_kv_free_blocks", usage.free_blocks);
    metrics_.set("llmopt_kv_watermark_blocks", usage.watermark_blocks);
}

bool SchedulerCore::admit(const std::shared_ptr<SequenceGroup>& group, TokenBudget& budget) {
    Sequence& seq = group->seq;
    std::int32_t hit_tokens = 0;
    if (config_.enable_prefix_cache && seq.num_computed == 0) {
        const PrefixHit hit = kv_.lookup_prefix(seq);
        RequestId donor = hit.donor;
        if (donor.empty() && hit.hit_tokens > 0 && runner_.supports_live_prefix()) {
            donor = find_live_donor(hit);
        }
        if (hit.hit_tokens > 0 && !donor.empty()) {
            hit_tokens = kv_.attach_prefix(seq, hit);
            if (hit_tokens > 0) {
                seq.num_computed = hit_tokens;
                group->prefix_source = donor;
                group->prefix_tokens = hit_tokens;
                metrics_.inc("llmopt_prefix_hits_total");
                metrics_.inc("llmopt_prefix_hit_tokens_total", hit_tokens);
            }
        } else {
            metrics_.inc("llmopt_prefix_misses_total");
        }
    }
    const std::int32_t total_tokens = seq.prompt_len + seq.sampling.max_tokens;
    const AllocResult alloc = kv_.acquire(seq, total_tokens, 0, true);
    if (!alloc.ok()) {
        if (hit_tokens > 0) {
            kv_.free(seq);
            seq.num_computed = 0;
            group->prefix_source.clear();
            group->prefix_tokens = 0;
        }
        metrics_.inc(std::string("llmopt_admission_rejected_total{reason=\"") +
                     alloc_reason_name(alloc.reason) + "\"}");
        if (alloc.reason == AllocFailReason::ContextOverflow) {
            finish(group, FinishReason::Error);
        }
        return false;
    }
    if (runner_.add_sequence(seq) != ErrCode::Ok) {
        kv_.free(seq);
        finish(group, FinishReason::Error);
        return false;
    }
    group->admitted = true;
    seq.status = SeqStatus::Running;
    running_.push_back(group);
    budget.commit(1, seq.prompt_len - seq.num_computed);
    return true;
}

void SchedulerCore::append_step(Batch& batch, const std::shared_ptr<SequenceGroup>& group) const {
    const Sequence& seq = group->seq;
    SequenceStep step;
    step.seq_id = group->req_id;
    step.route.req_id = group->req_id;
    step.kv = KVPen{seq.id, 0};
    if (seq.num_computed < seq.prompt_len) {
        NewRequestData data;
        data.req_id = group->req_id;
        data.prefill_token_ids.assign(seq.prompt_token_ids.begin() + seq.num_computed,
                                      seq.prompt_token_ids.end());
        data.block_ids = seq.block_table;
        data.sampling = seq.sampling;
        data.prefix_source = group->prefix_source;
        data.prefix_tokens = group->prefix_tokens;
        step.phase = Phase::PrefillLast;
        step.pos_begin = seq.num_computed;
        step.pos_end = seq.prompt_len;
        step.n_tokens = seq.prompt_len - seq.num_computed;
        batch.new_reqs.push_back(std::move(data));
    } else {
        CachedRequestData data;
        data.req_id = group->req_id;
        data.new_token_ids.push_back(seq.output_token_ids.back());
        data.new_block_ids = seq.block_table;
        step.phase = Phase::Decode;
        step.pos_begin = seq.num_computed;
        step.pos_end = seq.num_computed + 1;
        step.n_tokens = 1;
        batch.cached_reqs.push_back(std::move(data));
    }
    step.needs_sampling = true;
    step.sample_pos = step.pos_end - 1;
    batch.total_tokens += step.n_tokens;
    batch.steps.push_back(step);
}

Batch SchedulerCore::build_batch(const Sequence& seq) const {
    Batch batch;
    SequenceStep step;
    if (seq.num_computed < seq.prompt_len) {
        NewRequestData data;
        data.req_id = seq.group_id;
        data.prefill_token_ids = seq.prompt_token_ids;
        data.block_ids = seq.block_table;
        data.sampling = seq.sampling;
        step.phase = Phase::PrefillLast;
        step.pos_end = seq.prompt_len;
        batch.total_tokens = seq.prompt_len;
        batch.new_reqs.push_back(std::move(data));
    } else {
        CachedRequestData data;
        data.req_id = seq.group_id;
        data.new_token_ids.push_back(seq.output_token_ids.back());
        step.phase = Phase::Decode;
        step.pos_end = seq.num_computed + 1;
        batch.total_tokens = 1;
        batch.cached_reqs.push_back(std::move(data));
    }
    step.seq_id = seq.group_id;
    step.pos_begin = seq.num_computed;
    step.n_tokens = batch.total_tokens;
    step.needs_sampling = true;
    step.sample_pos = step.pos_end - 1;
    step.route.req_id = seq.group_id;
    step.kv = KVPen{seq.id, 0};
    batch.steps.push_back(step);
    return batch;
}

void SchedulerCore::apply_completion(const Completion& completion) {
    auto it = std::find(pending_serials_.begin(), pending_serials_.end(), completion.seq_serial);
    if (it == pending_serials_.end()) {
        return;
    }
    pending_serials_.erase(it);
    pending_remaining_ -= 1;
    auto group = find_running(completion.seq_id);
    const bool batch_done = pending_remaining_ == 0;
    if (!group || group->seq.id != completion.seq_serial) {
        if (batch_done) {
            pending_step_ = false;
            pump();
        }
        return;
    }
    Sequence& seq = group->seq;
    if (completion.err != ErrCode::Ok || completion.sampled.empty()) {
        finish(group, FinishReason::Error);
        if (batch_done) {
            pending_step_ = false;
            pump();
        }
        return;
    }
    const std::int32_t previous = seq.num_computed;
    seq.num_computed = completion.computed_tokens;
    seq.num_output += 1;
    seq.output_token_ids.push_back(completion.sampled.front().token_id);
    kv_.mark_written(seq, seq.num_computed);
    metrics_.inc("llmopt_tokens_generated_total");
    if (previous < seq.prompt_len && seq.num_computed >= seq.prompt_len) {
        metrics_.inc("llmopt_prefill_tokens_total", seq.prompt_len - previous);
    }
    reconcile(seq);
    DeliverResult delivered = deliver(group, completion.sampled.front());
    if (delivered == DeliverResult::Failed) {
        finish(group, FinishReason::Aborted);
    } else if (delivered == DeliverResult::Finished) {
        // finish 已在 deliver 内完成。
    }
    if (batch_done) {
        pending_step_ = false;
        pump();
    }
}

SchedulerCore::DeliverResult SchedulerCore::deliver(const std::shared_ptr<SequenceGroup>& group,
                                                    const SampledToken& token) {
    Sequence& seq = group->seq;
    const std::string piece = runner_.detokenize(token.token_id);
    group->utf8_pending += piece;
    const std::size_t complete = utf8_complete_prefix(group->utf8_pending);
    const std::string text = group->utf8_pending.substr(0, complete);
    group->utf8_pending.erase(0, complete);
    FinishReason reason = FinishReason::None;
    if (seq.cancel_requested.load() || shutting_down_) {
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
    if (!group->sink || !group->sink->push_frame(delta)) {
        return DeliverResult::Failed;
    }
    if (reason == FinishReason::None) {
        return DeliverResult::Continue;
    }
    finish(group, reason, true);
    return DeliverResult::Finished;
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
    if (!final_frame_pushed && group->sink) {
        Delta delta;
        delta.finish = reason;
        (void)group->sink->push_frame(delta);
    }
    if (group->sink) {
        group->sink->close();
    }
    if (group->admitted) {
        std::int32_t keep = 0;
        if (config_.enable_prefix_cache && reason != FinishReason::Error &&
            reason != FinishReason::Aborted) {
            keep = kv_.retain(seq);
        } else {
            kv_.free(seq);
        }
        runner_.release_sequence(group->req_id, keep);
    }
    running_.erase(std::remove(running_.begin(), running_.end(), group), running_.end());
    if (!pending_step_) {
        reap_donors();
    }
    if (reason == FinishReason::Aborted) {
        metrics_.inc("llmopt_requests_total{status=\"cancelled\"}");
    } else if (reason == FinishReason::Error) {
        metrics_.inc("llmopt_requests_total{status=\"error\"}");
    } else {
        metrics_.inc("llmopt_requests_total{status=\"ok\"}");
    }
    metrics_.set("llmopt_running_sequences", static_cast<std::int64_t>(running_.size()));
    metrics_.set("llmopt_kv_free_blocks", kv_.num_free_blocks());
    if (!shutting_down_ && !pending_step_) {
        pump();
    }
}

void SchedulerCore::reap_donors() {
    for (const RequestId& donor : kv_.take_dropped_donors()) {
        runner_.release_sequence(donor, 0);
        metrics_.inc("llmopt_prefix_evictions_total");
    }
}

void SchedulerCore::reconcile(const Sequence& seq) {
    const std::int32_t backend_pos = runner_.seq_pos_max(seq.group_id);
    const std::int32_t expected = seq.num_computed - 1;
    if (backend_pos < 0 || expected < 0) {
        return;
    }
    if (backend_pos != expected) {
        metrics_.inc("llmopt_kv_reconcile_diffs_total");
    }
}

std::shared_ptr<SequenceGroup> SchedulerCore::find_running(const RequestId& req_id) const {
    for (const auto& group : running_) {
        if (group->req_id == req_id) {
            return group;
        }
    }
    return nullptr;
}

RequestId SchedulerCore::find_live_donor(const PrefixHit& hit) const {
    if (hit.blocks.empty()) {
        return {};
    }
    const std::int32_t block_id = hit.blocks.front().block_id;
    for (const auto& group : running_) {
        if (group->seq.num_computed < hit.hit_tokens) {
            continue;
        }
        for (const BlockRef& ref : group->seq.block_table) {
            if (ref.block_id == block_id) {
                return group->req_id;
            }
        }
    }
    return {};
}

std::int32_t SchedulerCore::running_count() const noexcept {
    return static_cast<std::int32_t>(running_.size());
}

std::int32_t SchedulerCore::waiting_count() const noexcept {
    return static_cast<std::int32_t>(waiting_.size());
}

} // namespace llmopt
