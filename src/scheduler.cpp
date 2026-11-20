// scheduler.cpp — 单线程状态机 + 单线程执行；每 tick 合成连续批，状态只由 Core 写。
#include "llmopt/scheduler.hpp"

#include "llmopt/text.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>

namespace llmopt {
namespace {

std::atomic<std::int32_t> g_seq_id{0};

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

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

// 切块器只做纯计算：chunk_size 非法时收敛到 1，生产值由配置层校验。
ChunkPlanner::ChunkPlanner(std::int32_t chunk_size) noexcept
    : chunk_size_(chunk_size > 0 ? chunk_size : 1) {}

ChunkSlice ChunkPlanner::next(std::int32_t prompt_len, std::int32_t computed,
                              std::int32_t budget_left) const noexcept {
    ChunkSlice slice;
    const std::int32_t remaining = prompt_len - computed;
    if (remaining <= 0 || budget_left <= 0) {
        return slice;
    }
    slice.n_tokens = std::min(chunk_size_, std::min(remaining, budget_left));
    slice.is_last = computed + slice.n_tokens == prompt_len;
    return slice;
}

std::int32_t ChunkPlanner::chunk_size() const noexcept {
    return chunk_size_;
}

BatchComposition batch_composition(const Batch& batch) noexcept {
    BatchComposition composition;
    for (const SequenceStep& step : batch.steps) {
        if (step.phase == Phase::Decode) {
            composition.decode_tokens += step.n_tokens;
            ++composition.decode_seqs;
        } else {
            composition.prefill_tokens += step.n_tokens;
            ++composition.prefill_seqs;
        }
    }
    return composition;
}

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
        // sampled 只按“需要采样”的步骤顺序排列，非末块 chunk 不占位。
        std::size_t sample_index = 0;
        for (std::size_t i = 0; i < batch.steps.size(); ++i) {
            const SequenceStep& step = batch.steps[i];
            Event event = make_event(EventKind::Completion, step.route.req_id);
            event.completion.seq_id = step.route.req_id;
            event.completion.seq_serial = step.kv.seq_id;
            event.completion.computed_tokens = step.pos_end;
            event.completion.sampled_expected = step.needs_sampling;
            event.completion.err = output.err;
            if (step.needs_sampling && sample_index < output.sampled.size()) {
                event.completion.sampled.push_back(output.sampled[sample_index++]);
            }
            mailbox_.push(std::move(event));
        }
    }
}

SchedulerCore::SchedulerCore(ModelRunner& runner, KVBlockManager& kv, MetricsRegistry& metrics,
                             SchedulerConfig config)
    : runner_(runner), kv_(kv), metrics_(metrics), config_(config), planner_(config.chunk_size),
      fairness_{0, config.fairness_aging_enabled, config.aging_threshold_ns,
                config.max_consecutive_preemptions},
      trace_(std::make_unique<PreemptTrace>(config.preempt_trace_path)),
      mailbox_(config.queue_capacity), executor_(runner, mailbox_, metrics) {}

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
        group->enqueued_ns = now_ns();
        group->last_progress_ns = group->enqueued_ns;
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

// 组批前的 KV 预检：decode 尚未进批，victim 仍可释放；已被抢占过的候选不再反向抢占。
void SchedulerCore::preempt_for_waiting() {
    const std::int32_t slots = std::max<std::int32_t>(
            0, config_.max_concurrency - static_cast<std::int32_t>(running_.size()));
    std::vector<std::shared_ptr<SequenceGroup>> candidates;
    for (const auto& group : waiting_) {
        if (static_cast<std::int32_t>(candidates.size()) >= slots) {
            break;
        }
        candidates.push_back(group);
    }
    for (const auto& candidate : candidates) {
        if (candidate->consecutive_preemptions > 0) {
            continue;
        }
        const std::int32_t total_tokens =
                candidate->seq.prompt_len + candidate->seq.sampling.max_tokens;
        if (total_tokens > kv_.block_capacity_tokens()) {
            continue; // 交给准入路径报 ContextOverflow
        }
        const std::int32_t need = kv_.blocks_for_tokens(total_tokens) + kv_.watermark_blocks();
        if (kv_.num_free_blocks() >= need) {
            continue;
        }
        FairnessState fairness = fairness_;
        fairness.now_ns = now_ns();
        const std::int32_t free_before = kv_.num_free_blocks();
        const PreemptDecision decision =
                try_preempt(running_ptrs(), need, kv_, victim_policy_, fairness);
        apply_preemption(decision, PreemptTrigger::Admission, free_before, need);
    }
}

// 预算内补新请求；跳过本 tick 刚被抢占的 victim，让触发抢占的请求先进入。
void SchedulerCore::admit_waiting(TokenBudget& budget) {
    while (!waiting_.empty()) {
        if (static_cast<std::int32_t>(running_.size()) >= config_.max_concurrency) {
            break;
        }
        if (budget.tokens_remain() < 1) {
            metrics_.inc("llmopt_admission_rejected_total{reason=\"token_budget\"}");
            break;
        }
        std::size_t index = waiting_.size();
        for (std::size_t i = 0; i < waiting_.size(); ++i) {
            if (!(preempted_this_pump_ && waiting_[i]->consecutive_preemptions > 0)) {
                index = i;
                break;
            }
        }
        if (index >= waiting_.size()) {
            break;
        }
        auto group = waiting_[index];
        // 先摘出候选：admit 内的抢占会把 victim push_front，不能再用原下标定位。
        waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(index));
        metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
        if (!admit(group, budget)) {
            // 终态组不得塞回队列；仍可重试的组按原位置插回。
            if (group->seq.status != SeqStatus::Finished && !group->admitted) {
                waiting_.insert(waiting_.begin() + static_cast<std::ptrdiff_t>(index), group);
            }
            metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
            break;
        }
    }
}

// 批次只有在这里才变为 pending；提交失败清掉剩余计数，避免执行线程空等。
bool SchedulerCore::submit_scheduled(Batch& batch, std::chrono::steady_clock::time_point started) {
    if (batch.steps.empty()) {
        return false;
    }
    pending_serials_.clear();
    for (const SequenceStep& step : batch.steps) {
        pending_serials_.push_back(step.kv.seq_id);
    }
    pending_remaining_ = static_cast<std::int32_t>(batch.steps.size());
    record_schedule_step(started, pending_remaining_, batch.total_tokens, batch);
    if (!executor_.submit(std::move(batch))) {
        pending_remaining_ = 0;
        return false;
    }
    pending_step_ = true;
    return true;
}

void SchedulerCore::pump() {
    if (pending_step_ || shutting_down_ || pumping_) {
        return;
    }
    struct PumpGuard {
        bool& flag;
        explicit PumpGuard(bool& value) : flag(value) {
            flag = true;
        }
        ~PumpGuard() {
            flag = false;
        }
    } guard(pumping_);
    const auto started = std::chrono::steady_clock::now();
    // 上一 tick 淘汰的 donor 此刻才能真正释放：本批若还要借前缀，必须等复制完成。
    reap_donors();
    Batch batch;
    TokenBudget budget(config_.max_concurrency, config_.max_num_batched_tokens);
    preempted_this_pump_ = false;
    for (const auto& group : running_) {
        group->has_reserved_slice = false; // 上一 tick 的预留必须已经消费
        group->scheduled_this_pump = false;
    }
    // 1) 预算内先为等待队列做一次 KV 预检；2) decode 保位；3) 准入；4) 补 prefill。
    preempt_for_waiting();
    append_decodes(batch, budget);
    admit_waiting(budget);
    append_prefills(batch, budget);
    if (!submit_scheduled(batch, started)) {
        return;
    }
    metrics_.set("llmopt_running_sequences", static_cast<std::int64_t>(running_.size()));
    metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
    publish_kv_usage();
}

// 块表快照统一从这里发布，准入、收尾与关停看到的 used/cached/held 口径一致。
void SchedulerCore::publish_kv_usage() {
    const KVUsage usage = kv_.usage();
    metrics_.set("llmopt_kv_used_blocks", usage.used_blocks);
    metrics_.set("llmopt_kv_cached_blocks", usage.cached_blocks);
    metrics_.set("llmopt_kv_shared_blocks", usage.shared_blocks);
    metrics_.set("llmopt_kv_free_blocks", usage.free_blocks);
    metrics_.set("llmopt_kv_watermark_blocks", usage.watermark_blocks);
    metrics_.set("llmopt_kv_held_blocks", usage.used_blocks - usage.cached_blocks);
}

void SchedulerCore::record_schedule_step(std::chrono::steady_clock::time_point started,
                                         std::int32_t steps, std::int32_t tokens,
                                         const Batch& batch) {
    const BatchComposition composition = batch_composition(batch);
    const std::int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
    scheduler_ns_total_ += ns;
    ++scheduler_steps_total_;
    schedule_samples_.push_back(ns);
    if (schedule_samples_.size() > 256) {
        schedule_samples_.erase(schedule_samples_.begin());
    }
    batch_size_peak_ = std::max(batch_size_peak_, steps);
    batch_tokens_peak_ = std::max(batch_tokens_peak_, tokens);
    metrics_.set("llmopt_scheduler_ns_total", scheduler_ns_total_);
    metrics_.set("llmopt_scheduler_steps_total", scheduler_steps_total_);
    metrics_.set("llmopt_batch_size_last", steps);
    metrics_.set("llmopt_batch_size_peak", batch_size_peak_);
    metrics_.set("llmopt_batch_tokens_last", tokens);
    metrics_.set("llmopt_batch_tokens_peak", batch_tokens_peak_);
    metrics_.set("llmopt_batch_prefill_tokens_last", composition.prefill_tokens);
    metrics_.set("llmopt_batch_decode_tokens_last", composition.decode_tokens);
    metrics_.set("llmopt_batch_prefill_seqs_last", composition.prefill_seqs);
    metrics_.set("llmopt_batch_decode_seqs_last", composition.decode_seqs);
    std::vector<std::int64_t> sorted = schedule_samples_;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = sorted.empty() ? 0 : (sorted.size() - 1) * 99 / 100;
    metrics_.set("llmopt_schedule_step_p99_ns",
                 sorted.empty() ? 0 : sorted[static_cast<std::size_t>(index)]);
}

// 前缀命中优先用块表 donor；后端支持 live prefix 时再找在飞序列。返回命中 token 数。
std::int32_t SchedulerCore::apply_prefix_hit(const std::shared_ptr<SequenceGroup>& group) {
    Sequence& seq = group->seq;
    if (!config_.enable_prefix_cache || seq.num_computed != 0 || group->recompute_active) {
        return 0;
    }
    const PrefixHit hit = kv_.lookup_prefix(seq);
    RequestId donor = hit.donor;
    if (donor.empty() && hit.hit_tokens > 0 && runner_.supports_live_prefix()) {
        donor = find_live_donor(hit);
    }
    if (hit.hit_tokens > 0 && !donor.empty()) {
        const std::int32_t hit_tokens = kv_.attach_prefix(seq, hit);
        if (hit_tokens > 0) {
            seq.num_computed = hit_tokens;
            group->prefix_source = donor;
            group->prefix_tokens = hit_tokens;
            metrics_.inc("llmopt_prefix_hits_total");
            metrics_.inc("llmopt_prefix_hit_tokens_total", hit_tokens);
        }
        return hit_tokens;
    }
    metrics_.inc("llmopt_prefix_misses_total");
    return 0;
}

// 按全序列长度取块；不足时在组批前选 victim 重试。终态由调用方决定是否 finish。
bool SchedulerCore::acquire_blocks(const std::shared_ptr<SequenceGroup>& group,
                                   std::int32_t total_tokens, std::int32_t hit_tokens) {
    Sequence& seq = group->seq;
    AllocResult alloc = kv_.acquire(seq, total_tokens, 0, true);
    if (!alloc.ok() && hit_tokens > 0) {
        kv_.free(seq);
        seq.num_computed = 0;
        group->prefix_source.clear();
        group->prefix_tokens = 0;
    }
    if (!alloc.ok() &&
        (alloc.reason == AllocFailReason::NoFreeBlock ||
         alloc.reason == AllocFailReason::BelowWatermark) &&
        !running_.empty() && group->consecutive_preemptions == 0) {
        // 组批前试算：先释放 donor，再按 fcfs_tail 选 victim；不动候选块表。
        const std::int32_t need = kv_.blocks_for_tokens(total_tokens) + kv_.watermark_blocks();
        const std::int32_t free_before = kv_.num_free_blocks();
        FairnessState fairness = fairness_;
        fairness.now_ns = now_ns();
        const PreemptDecision decision =
                try_preempt(running_ptrs(), need, kv_, victim_policy_, fairness);
        apply_preemption(decision, PreemptTrigger::Admission, free_before, need);
        if (decision.retry_ok) {
            alloc = kv_.acquire(seq, total_tokens, 0, true);
        } else {
            metrics_.inc("llmopt_preempt_stalls_total");
        }
    }
    if (alloc.ok()) {
        return true;
    }
    metrics_.inc(std::string("llmopt_admission_rejected_total{reason=\"") +
                 alloc_reason_name(alloc.reason) + "\"}");
    if (alloc.reason == AllocFailReason::ContextOverflow) {
        finish(group, FinishReason::Error);
    }
    return false;
}

bool SchedulerCore::admit(const std::shared_ptr<SequenceGroup>& group, TokenBudget& budget) {
    Sequence& seq = group->seq;
    const std::int32_t hit_tokens = apply_prefix_hit(group);
    const std::int32_t total_tokens = seq.prompt_len + seq.sampling.max_tokens;
    if (!acquire_blocks(group, total_tokens, hit_tokens)) {
        return false;
    }
    if (runner_.add_sequence(seq) != ErrCode::Ok) {
        kv_.free(seq);
        finish(group, FinishReason::Error);
        return false;
    }
    const ChunkSlice slice =
            planner_.next(context_tokens(seq), seq.num_computed, budget.tokens_remain());
    if (slice.n_tokens <= 0) {
        metrics_.inc("llmopt_chunk_deferred_total");
        kv_.free(seq);
        runner_.release_sequence(group->req_id, 0);
        finish(group, FinishReason::Error);
        return false;
    }
    group->admitted = true;
    group->admit_order = ++admit_seq_;
    group->last_progress_ns = now_ns();
    group->has_reserved_slice = false;
    seq.status = SeqStatus::Running;
    running_.push_back(group);
    // 序列槽与本步首个 chunk 的 token 预算一起记账；append_prefills 消费预留切片。
    group->reserved_slice = slice;
    group->has_reserved_slice = true;
    budget.commit(1, slice.n_tokens);
    return true;
}

// 被抢请求四步：释放块（try_preempt 已做）→ 清零进度 → Swapped/回队首 → 释放后端槽。
void SchedulerCore::apply_preemption(const PreemptDecision& decision, PreemptTrigger trigger,
                                     std::int32_t free_before, std::int32_t need_blocks) {
    for (std::size_t index = 0; index < decision.victims.size(); ++index) {
        const RequestId& req_id = decision.victims[index];
        auto group = find_running(req_id);
        if (!group) {
            continue;
        }
        Sequence& seq = group->seq;
        const std::int32_t recompute_tokens = context_tokens(seq);
        runner_.release_sequence(req_id, 0);
        group->admitted = false;
        seq.num_computed = 0;
        seq.id += 1; // 旧 serial 的完成事件一律 stale
        seq.status = SeqStatus::Swapped;
        group->prefix_source.clear();
        group->prefix_tokens = 0;
        group->prefix_applied = false;
        group->recompute_active = true;
        ++group->consecutive_preemptions;
        ++group->preemptions_total;
        preempted_this_pump_ = true;
        running_.erase(std::remove(running_.begin(), running_.end(), group), running_.end());

        const std::int32_t released = index < decision.released_per_victim.size()
                                              ? decision.released_per_victim[index]
                                              : 0;
        const bool cap_error =
                group->consecutive_preemptions >= config_.max_consecutive_preemptions;
        PreemptEvent event;
        event.ts_ns = now_ns();
        event.trigger = trigger;
        event.victim = req_id;
        event.free_before = free_before;
        event.need_blocks = need_blocks;
        event.blocks_released = released;
        event.recompute_tokens = recompute_tokens;
        event.path = cost_model_.choose(false, 1.0);
        event.cost_ms = cost_model_.recompute_ms(recompute_tokens);
        event.cap_error = cap_error;
        trace_->record(event);

        metrics_.inc("llmopt_preemptions_total");
        metrics_.inc("llmopt_preempted_tokens_total", recompute_tokens);
        metrics_.inc("llmopt_preempt_recompute_total");
        if (cap_error) {
            metrics_.inc("llmopt_preempt_cap_errors_total");
            metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
            finish(group, FinishReason::Error);
            continue;
        }
        waiting_.push_front(group);
        metrics_.set("llmopt_queue_depth", static_cast<std::int64_t>(waiting_.size()));
    }
    metrics_.set("llmopt_running_sequences", static_cast<std::int64_t>(running_.size()));
    if (decision.aging_exempt > 0) {
        metrics_.inc("llmopt_aging_exempt_total", decision.aging_exempt);
    }
    publish_kv_usage();
}

std::vector<SequenceGroup*> SchedulerCore::running_ptrs() const {
    std::vector<SequenceGroup*> out;
    out.reserve(running_.size());
    for (const auto& group : running_) {
        // 已进入待提交批（decode 或预留 prefill）的序列不能再作为 victim。
        if (group->has_reserved_slice || group->scheduled_this_pump) {
            continue;
        }
        out.push_back(group.get());
    }
    return out;
}
// decode 保位：在跑且 prefill 已完成的序列每步固定吃 1 token 预算。
bool SchedulerCore::append_decode(Batch& batch, const std::shared_ptr<SequenceGroup>& group,
                                  TokenBudget& budget) {
    const Sequence& seq = group->seq;
    if (seq.num_computed < context_tokens(seq) || seq.output_token_ids.empty()) {
        return false;
    }
    CachedRequestData data;
    data.req_id = group->req_id;
    data.new_token_ids.push_back(seq.output_token_ids.back());
    data.new_block_ids = seq.block_table;
    SequenceStep step;
    step.seq_id = group->req_id;
    step.route.req_id = group->req_id;
    step.kv = KVPen{seq.id, 0};
    step.phase = Phase::Decode;
    step.pos_begin = seq.num_computed;
    step.pos_end = seq.num_computed + 1;
    step.n_tokens = 1;
    step.needs_sampling = true;
    step.sample_pos = step.pos_begin;
    batch.cached_reqs.push_back(std::move(data));
    batch.total_tokens += step.n_tokens;
    batch.steps.push_back(step);
    budget.commit(0, 1);
    group->scheduled_this_pump = true;
    return true;
}

// 续 prefill：按 ChunkPlanner 取本步 chunk；非末块只推进 KV，不采样。
bool SchedulerCore::append_prefill_chunk(Batch& batch, const std::shared_ptr<SequenceGroup>& group,
                                         TokenBudget& budget) {
    Sequence& seq = group->seq;
    const std::int32_t target = context_tokens(seq);
    if (seq.num_computed >= target) {
        return false;
    }
    ChunkSlice slice;
    bool reserved = false;
    if (group->has_reserved_slice) {
        slice = group->reserved_slice;
        group->has_reserved_slice = false;
        reserved = true;
    } else {
        slice = planner_.next(target, seq.num_computed, budget.tokens_remain());
        if (slice.n_tokens <= 0) {
            metrics_.inc("llmopt_chunk_deferred_total");
            return false;
        }
        budget.commit(0, slice.n_tokens);
    }
    if (slice.n_tokens <= 0) {
        metrics_.inc("llmopt_chunk_deferred_total");
        return false;
    }
    NewRequestData data;
    data.req_id = group->req_id;
    data.prefill_token_ids.reserve(static_cast<std::size_t>(slice.n_tokens));
    for (std::int32_t offset = 0; offset < slice.n_tokens; ++offset) {
        const std::int32_t pos = seq.num_computed + offset;
        if (pos < seq.prompt_len) {
            data.prefill_token_ids.push_back(seq.prompt_token_ids[static_cast<std::size_t>(pos)]);
        } else {
            // recompute 重放已生成 token（最后一个已采样 token 不回填，留给下一次 decode）。
            data.prefill_token_ids.push_back(
                    seq.output_token_ids[static_cast<std::size_t>(pos - seq.prompt_len)]);
        }
    }
    data.block_ids = seq.block_table;
    data.sampling = seq.sampling;
    // 前缀只在首个 chunk 借一次，后续 chunk 直接续写同一 KV。
    if (!group->prefix_applied && group->prefix_tokens > 0) {
        data.prefix_source = group->prefix_source;
        data.prefix_tokens = group->prefix_tokens;
        group->prefix_applied = true;
    }
    SequenceStep step;
    step.seq_id = group->req_id;
    step.route.req_id = group->req_id;
    step.kv = KVPen{seq.id, 0};
    step.phase = slice.is_last ? Phase::PrefillLast : Phase::PrefillChunk;
    step.pos_begin = seq.num_computed;
    step.pos_end = seq.num_computed + slice.n_tokens;
    step.n_tokens = slice.n_tokens;
    // recompute 已有生成 token 时，末块只回填 KV，不再采样；否则末块产出首 token。
    step.needs_sampling = slice.is_last && !(group->recompute_active && seq.num_output > 0);
    step.sample_pos = step.pos_end - 1;
    batch.new_reqs.push_back(std::move(data));
    batch.total_tokens += step.n_tokens;
    batch.steps.push_back(step);
    if (!reserved) {
        budget.commit(0, slice.n_tokens);
    }
    group->scheduled_this_pump = true;
    return true;
}

// 第一阶段：decode 保位；已完成 prefill 的序列每步固定吃 1 token 预算。
void SchedulerCore::append_decodes(Batch& batch, TokenBudget& budget) {
    for (const auto& group : running_) {
        if (group->seq.num_computed >= context_tokens(group->seq)) {
            (void)append_decode(batch, group, budget);
        }
    }
}

// 第三阶段：所有未完成 prefill 的序列（含本 tick 新准入）统一补切块。
void SchedulerCore::append_prefills(Batch& batch, TokenBudget& budget) {
    for (const auto& group : running_) {
        if (group->scheduled_this_pump) {
            continue;
        }
        if (group->seq.num_computed < context_tokens(group->seq)) {
            (void)append_prefill_chunk(batch, group, budget);
        }
    }
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

// 一批结束后才推进下一 tick，避免在上一步完成回调里重入提交。
void SchedulerCore::conclude_step(bool batch_done) {
    if (!batch_done) {
        return;
    }
    pending_step_ = false;
    pump();
}

// 非末块 chunk：只推进 KV 进度，不产生输出 token，也不能走结束判定。
void SchedulerCore::advance_chunk(const std::shared_ptr<SequenceGroup>& group,
                                  const Completion& completion) {
    Sequence& seq = group->seq;
    const std::int32_t previous = seq.num_computed;
    seq.num_computed = completion.computed_tokens;
    if (previous < seq.prompt_len && seq.num_computed > previous) {
        const std::int32_t upper = std::min(seq.num_computed, seq.prompt_len);
        metrics_.inc("llmopt_prefill_tokens_total", upper - previous);
    }
    if (seq.num_computed < seq.prompt_len) {
        metrics_.inc("llmopt_chunk_prefill_steps_total");
        metrics_.inc("llmopt_chunk_prefill_tokens_total", seq.num_computed - previous);
    }
    kv_.mark_written(seq, seq.num_computed);
    reconcile(seq);
    group->last_progress_ns = now_ns();
    if (group->recompute_active && seq.num_computed >= context_tokens(seq)) {
        group->recompute_active = false;
        group->consecutive_preemptions = 0;
        metrics_.inc("llmopt_preempt_recovered_total");
    } else if (!group->recompute_active) {
        group->consecutive_preemptions = 0;
    }
    if (seq.cancel_requested.load() || shutting_down_) {
        finish(group, FinishReason::Aborted);
    }
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
        conclude_step(batch_done);
        return;
    }
    Sequence& seq = group->seq;
    if (completion.err != ErrCode::Ok) {
        finish(group, FinishReason::Error);
        conclude_step(batch_done);
        return;
    }
    if (!completion.sampled_expected) {
        advance_chunk(group, completion);
        conclude_step(batch_done);
        return;
    }
    if (completion.sampled.empty()) {
        finish(group, FinishReason::Error);
        conclude_step(batch_done);
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
    group->last_progress_ns = now_ns();
    if (group->recompute_active && seq.num_computed >= context_tokens(seq)) {
        group->recompute_active = false;
        group->consecutive_preemptions = 0;
        metrics_.inc("llmopt_preempt_recovered_total");
    } else if (!group->recompute_active) {
        group->consecutive_preemptions = 0;
    }
    if (deliver(group, completion.sampled.front()) == DeliverResult::Failed) {
        finish(group, FinishReason::Aborted);
    }
    conclude_step(batch_done);
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
    publish_kv_usage();
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
