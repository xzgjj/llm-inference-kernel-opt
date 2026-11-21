// scheduler.cpp — 单线程状态机 + 单线程执行；每 tick 合成连续批，状态只由 Core 写。
#include "llmopt/scheduler.hpp"

#include "llmopt/log.hpp"
#include "llmopt/text.hpp"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <new>

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

// 准入拒绝原因 → 冻结指标 ID；SequenceTooLong 归入 context_overflow（容量类拒绝）。
MetricId admission_metric_id(AllocFailReason reason) {
    switch (reason) {
    case AllocFailReason::NoFreeBlock:
        return MetricId::AdmissionNoFreeBlock;
    case AllocFailReason::BelowWatermark:
        return MetricId::AdmissionBelowWatermark;
    case AllocFailReason::ContextOverflow:
    case AllocFailReason::SequenceTooLong:
        return MetricId::AdmissionContextOverflow;
    default:
        return MetricId::Count;
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
    return push_impl(std::move(event), true);
}

// 完成/取消/关停事件不许丢：满了也入队，容量只对 AddRequest 生效。
bool SchedulerMailbox::push_critical(Event event) {
    return push_impl(std::move(event), false);
}

bool SchedulerMailbox::push_impl(Event event, bool bounded) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (bounded && static_cast<std::int32_t>(queue_.size()) >= capacity_) {
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
        RunnerStepOutput output;
        try {
            output = runner_.decode_step(batch);
        } catch (const std::exception& exc) {
            output = RunnerStepOutput{};
            output.error = make_error(ErrCode::ExecutionFailed, "执行线程异常", exc.what());
            logger()->error("executor exception: {}", exc.what());
        } catch (...) {
            output = RunnerStepOutput{};
            output.error = make_error(ErrCode::ExecutionFailed, "执行线程未知异常");
            logger()->error("executor unknown exception");
        }
        metrics_.inc(MetricId::ForwardSteps);
        // 即使 runner 提前失败也要逐条回投，避免 Core 永久等待 pending。
        // sampled 只按“需要采样”的步骤顺序排列，非末块 chunk 不占位。
        std::size_t sample_index = 0;
        for (std::size_t i = 0; i < batch.steps.size(); ++i) {
            const SequenceStep& step = batch.steps[i];
            Event event = make_event(EventKind::Completion, step.route.backend_id);
            event.completion.backend_id = step.route.backend_id;
            event.completion.seq_serial = step.kv.seq_id;
            event.completion.computed_tokens = step.pos_end;
            event.completion.sampled_expected = step.needs_sampling;
            event.completion.error = output.error;
            if (step.needs_sampling && sample_index < output.sampled.size()) {
                event.completion.sampled.push_back(output.sampled[sample_index++]);
            }
            // 关键通道：丢了这条就等于让 Core 永远等不到这一步。
            if (!mailbox_.push_critical(std::move(event))) {
                logger()->error("completion 回投失败 req={} serial={}", step.route.backend_id,
                                step.kv.seq_id);
            }
        }
    }
}

SchedulerCore::SchedulerCore(ModelRunner& runner, KVBlockManager& kv, MetricsRegistry& metrics,
                             SchedulerConfig config)
    : runner_(runner), kv_(kv), metrics_(metrics), config_(config), planner_(config.chunk_size),
      fairness_{0, config.fairness_aging_enabled, config.aging_threshold_ns,
                config.max_consecutive_preemptions},
      trace_(std::make_unique<PreemptTrace>(config.preempt_trace_path)),
      mailbox_(config.max_concurrency + config.queue_capacity),
      executor_(runner, mailbox_, metrics), started_ns_(now_ns()) {
    if (!config_.step_trace_path.empty()) {
        step_trace_.open(config_.step_trace_path, std::ios::out | std::ios::trunc);
        step_trace_open_ = step_trace_.is_open();
        if (!step_trace_open_) {
            logger()->warn("step trace 打不开，已关闭: {}", config_.step_trace_path);
        }
    }
}

SchedulerCore::~SchedulerCore() {
    shutdown();
}

ErrCode SchedulerCore::submit(InferenceRequest req, std::vector<std::int32_t> prompt_ids,
                              std::shared_ptr<ResponseSink> sink) {
    if (!try_reserve_admission()) {
        metrics_.inc(MetricId::AdmissionQueue);
        metrics_.inc(MetricId::RequestsQueueFull);
        return ErrCode::QueueFull;
    }
    bool id_reserved = false;
    try {
        auto group = std::make_shared<SequenceGroup>();
        group->req_id = req.req_id;
        group->prompt_ids = std::move(prompt_ids);
        group->sampling = req.sampling;
        group->stream = req.stream;
        group->sink = std::move(sink);
        group->seq.id = g_seq_id.fetch_add(1, std::memory_order_relaxed);
        group->backend_id = "seq-" + std::to_string(group->seq.id);
        group->seq.backend_id = group->backend_id;
        group->seq.prompt_len = static_cast<std::int32_t>(group->prompt_ids.size());
        group->seq.prompt_token_ids = group->prompt_ids;
        group->seq.sampling = group->sampling;
        group->enqueued_ns = now_ns();
        group->last_progress_ns = group->enqueued_ns;
        if (!reserve_request_id(group->req_id)) {
            release_admission();
            metrics_.inc(MetricId::RequestsError);
            return ErrCode::InvalidJson;
        }
        id_reserved = true;
        Event event = make_event(EventKind::AddRequest, group->req_id);
        event.group = std::move(group);
        if (!mailbox_.push(std::move(event))) {
            // event 已 move 走，无法再取 group；按入队前保存的 id 释放。
            release_request_id(req.req_id);
            id_reserved = false;
            release_admission();
            metrics_.inc(MetricId::AdmissionQueue);
            metrics_.inc(MetricId::RequestsQueueFull);
            return ErrCode::QueueFull;
        }
        return ErrCode::Ok;
    } catch (const std::bad_alloc&) {
        // 只有真正插进集合后才回滚；异常可能发生在 reserve 之前。
        if (id_reserved) {
            release_request_id(req.req_id);
        }
        release_admission();
        metrics_.inc(MetricId::RequestsError);
        logger()->error("submit 内存分配失败");
        return ErrCode::ResourceInsufficient;
    } catch (const std::exception& exc) {
        if (id_reserved) {
            release_request_id(req.req_id);
        }
        release_admission();
        metrics_.inc(MetricId::RequestsError);
        logger()->error("submit 异常: {}", exc.what());
        return ErrCode::ExecutionFailed;
    }
}

void SchedulerCore::cancel(const RequestId& req_id) noexcept {
    Event event = make_event(EventKind::Cancel, req_id);
    (void)mailbox_.push_critical(std::move(event));
}

void SchedulerCore::shutdown() noexcept {
    Event event = make_event(EventKind::Shutdown, "");
    (void)mailbox_.push_critical(std::move(event));
}

bool SchedulerCore::accepting() const noexcept {
    const std::int32_t live = running_depth_.load(std::memory_order_relaxed) +
                              waiting_depth_.load(std::memory_order_relaxed) +
                              inflight_admissions_.load(std::memory_order_relaxed);
    return live < admission_capacity();
}

std::int32_t SchedulerCore::queue_capacity() const noexcept {
    return config_.queue_capacity;
}

std::int32_t SchedulerCore::concurrency_limit() const noexcept {
    return config_.max_concurrency;
}

std::int32_t SchedulerCore::admission_capacity() const noexcept {
    return config_.max_concurrency + config_.queue_capacity;
}

bool SchedulerCore::try_reserve_admission() noexcept {
    const std::int32_t live = running_depth_.load(std::memory_order_relaxed) +
                              waiting_depth_.load(std::memory_order_relaxed) +
                              inflight_admissions_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (live > admission_capacity()) {
        release_admission();
        return false;
    }
    return true;
}

void SchedulerCore::release_admission() noexcept {
    inflight_admissions_.fetch_sub(1, std::memory_order_acq_rel);
}

bool SchedulerCore::reserve_request_id(const RequestId& req_id) {
    if (req_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(active_ids_mutex_);
    return active_ids_.insert(req_id).second;
}

void SchedulerCore::release_request_id(const RequestId& req_id) {
    std::lock_guard<std::mutex> lock(active_ids_mutex_);
    active_ids_.erase(req_id);
}

// handle 的兜底拒绝单独成函数，避免单函数超出行数闸门（CI-COD-003）。
void SchedulerCore::reject_over_capacity(const std::shared_ptr<SequenceGroup>& group) {
    metrics_.inc(MetricId::AdmissionQueue);
    metrics_.inc(MetricId::RequestsQueueFull);
    finish(group, FinishReason::Error,
           make_error(ErrCode::QueueFull, "等待队列已满，请退避重试",
                      "depth=" + std::to_string(waiting_.size() + running_.size()) +
                              " cap=" + std::to_string(admission_capacity())));
}

void SchedulerCore::run() {
    while (true) {
        try {
            (void)run_once(50);
        } catch (const std::exception& exc) {
            logger()->error("scheduler exception: {}", exc.what());
            abort_all(make_error(ErrCode::ExecutionFailed, "调度线程异常", exc.what()));
            break;
        } catch (...) {
            logger()->error("scheduler unknown exception");
            abort_all(make_error(ErrCode::ExecutionFailed, "调度线程未知异常"));
            break;
        }
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
    const bool got_event = mailbox_.wait_pop_for(event, wait_ms);
    if (got_event) {
        handle(event);
        Event extra;
        while (mailbox_.try_pop(extra)) {
            handle(extra);
        }
    }
    // 即使没有事件也要推进：请求级 deadline 只能在 Core 线程判定，不能等下一个事件。
    pump();
    // 深度只在 Core 线程写；HTTP 准入读镜像，避免跨线程读 deque。
    waiting_depth_.store(static_cast<std::int32_t>(waiting_.size()), std::memory_order_relaxed);
    running_depth_.store(static_cast<std::int32_t>(running_.size()), std::memory_order_relaxed);
    return got_event;
}

// AddRequest 单独成函数：准入预留兑现、队列兜底拒绝与深度镜像都在这里收口。
void SchedulerCore::handle_add_request(Event& event) {
    auto group = std::move(event.group);
    metrics_.inc(MetricId::RequestsAccepted);
    // 预算按 running+waiting 总量判定；只看 waiting 会在“有空位可立即准入”时误拒。
    if (static_cast<std::int32_t>(waiting_.size() + running_.size()) >= admission_capacity()) {
        // 正常情况下入口已按 accepting() 拒绝；这里是 mailbox 与队列之间的兜底。
        release_admission();
        reject_over_capacity(group);
        return;
    }
    waiting_.push_back(std::move(group));
    // 先更新深度镜像再释放预留，避免窗口期内 HTTP 读到偏低的占用而超发。
    waiting_depth_.store(static_cast<std::int32_t>(waiting_.size()), std::memory_order_relaxed);
    metrics_.set(MetricId::QueueDepth, static_cast<std::int64_t>(waiting_.size()));
    metrics_.set(MetricId::RequestsQueued, static_cast<std::int64_t>(waiting_.size()));
    release_admission();
}

void SchedulerCore::handle(Event& event) {
    switch (event.kind) {
    case EventKind::AddRequest:
        handle_add_request(event);
        break;
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
                waiting_depth_.store(static_cast<std::int32_t>(waiting_.size()),
                                     std::memory_order_relaxed);
                metrics_.set(MetricId::QueueDepth, static_cast<std::int64_t>(waiting_.size()));
                metrics_.set(MetricId::RequestsQueued, static_cast<std::int64_t>(waiting_.size()));
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
        metrics_.set(MetricId::QueueDepth, 0);
        metrics_.set(MetricId::RequestsQueued, 0);
        waiting_depth_.store(0, std::memory_order_relaxed);
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
            metrics_.inc(MetricId::AdmissionTokenBudget);
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
        waiting_depth_.store(static_cast<std::int32_t>(waiting_.size()), std::memory_order_relaxed);
        metrics_.set(MetricId::QueueDepth, static_cast<std::int64_t>(waiting_.size()));
        metrics_.set(MetricId::RequestsQueued, static_cast<std::int64_t>(waiting_.size()));
        if (!admit(group, budget)) {
            // 终态组不得塞回队列；仍可重试的组按原位置插回。
            if (group->seq.status != SeqStatus::Finished && !group->admitted) {
                waiting_.insert(waiting_.begin() + static_cast<std::ptrdiff_t>(index), group);
            }
            waiting_depth_.store(static_cast<std::int32_t>(waiting_.size()),
                                 std::memory_order_relaxed);
            metrics_.set(MetricId::QueueDepth, static_cast<std::int64_t>(waiting_.size()));
            metrics_.set(MetricId::RequestsQueued, static_cast<std::int64_t>(waiting_.size()));
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

// 请求级 deadline：从提交入队起算，流式与非流式一致；到期的运行/排队请求都收终态。
// 只在 Core 线程且无 pending 批时调用，finish 可安全释放后端与块表。
void SchedulerCore::expire_deadlines() {
    if (config_.request_deadline_ns <= 0) {
        return;
    }
    const std::int64_t now = now_ns();
    const auto expired = [&](const std::shared_ptr<SequenceGroup>& group) {
        return group->enqueued_ns > 0 && now - group->enqueued_ns >= config_.request_deadline_ns;
    };
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (!expired(*it)) {
            ++it;
            continue;
        }
        auto group = *it;
        it = waiting_.erase(it);
        finish(group, FinishReason::Error,
               make_error(ErrCode::Timeout, "请求超过总时限",
                          "req=" + group->req_id + " deadline_s=" +
                                  std::to_string(config_.request_deadline_ns / 1'000'000'000)));
    }
    std::vector<std::shared_ptr<SequenceGroup>> running_copy = running_;
    for (const auto& group : running_copy) {
        if (expired(group)) {
            finish(group, FinishReason::Error,
                   make_error(ErrCode::Timeout, "请求超过总时限",
                              "req=" + group->req_id + " deadline_s=" +
                                      std::to_string(config_.request_deadline_ns / 1'000'000'000)));
        }
    }
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
    // 请求级总时限优先于组批：到期的请求不再消耗预算，直接按 Timeout 收尾。
    expire_deadlines();
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
    const std::int32_t live = static_cast<std::int32_t>(running_.size());
    running_peak_ = std::max(running_peak_, live);
    metrics_.set(MetricId::RunningSequences, live);
    metrics_.set(MetricId::RequestsRunning, live);
    metrics_.set(MetricId::RequestsRunningPeak, running_peak_);
    metrics_.set(MetricId::QueueDepth, static_cast<std::int64_t>(waiting_.size()));
    metrics_.set(MetricId::RequestsQueued, static_cast<std::int64_t>(waiting_.size()));
    publish_kv_usage();
}

// 块表快照统一从这里发布，准入、收尾与关停看到的 used/cached/held 口径一致。
// 内部碎片按块计：1 - (运行序列所需块 + cached donor 块) / 已占用块；含 max_tokens 预留。
void SchedulerCore::publish_kv_usage() {
    const KVUsage usage = kv_.usage();
    metrics_.set(MetricId::KvUsedBlocks, usage.used_blocks);
    metrics_.set(MetricId::KvCachedBlocks, usage.cached_blocks);
    metrics_.set(MetricId::KvSharedBlocks, usage.shared_blocks);
    metrics_.set(MetricId::KvFreeBlocks, usage.free_blocks);
    metrics_.set(MetricId::KvWatermarkBlocks, usage.watermark_blocks);
    metrics_.set(MetricId::KvHeldBlocks, usage.used_blocks - usage.cached_blocks);
    std::int32_t needed_blocks = usage.cached_blocks;
    for (const auto& group : running_) {
        needed_blocks += kv_.blocks_for_tokens(context_tokens(group->seq));
    }
    double frag_ratio = 0.0;
    if (usage.used_blocks > 0) {
        frag_ratio =
                1.0 - static_cast<double>(needed_blocks) / static_cast<double>(usage.used_blocks);
        frag_ratio = std::clamp(frag_ratio, 0.0, 1.0);
    }
    metrics_.set_float(MetricId::KvInternalFragRatio, frag_ratio);
}

void SchedulerCore::record_schedule_step(std::chrono::steady_clock::time_point started,
                                         std::int32_t steps, std::int32_t tokens,
                                         const Batch& batch) {
    const BatchComposition composition = batch_composition(batch);
    const std::int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
    const std::int64_t now = now_ns();
    metrics_.inc(MetricId::SchedulerNsTotal, ns);
    metrics_.inc(MetricId::SchedulerStepsTotal);
    metrics_.set(MetricId::ScheduleStepNsLast, ns);
    batch_size_peak_ = std::max(batch_size_peak_, steps);
    batch_tokens_peak_ = std::max(batch_tokens_peak_, tokens);
    metrics_.set(MetricId::BatchSizeLast, steps);
    metrics_.set(MetricId::BatchSizePeak, batch_size_peak_);
    metrics_.set(MetricId::BatchTokensLast, tokens);
    metrics_.set(MetricId::BatchTokensPeak, batch_tokens_peak_);
    metrics_.set(MetricId::BatchPrefillTokensLast, composition.prefill_tokens);
    metrics_.set(MetricId::BatchDecodeTokensLast, composition.decode_tokens);
    metrics_.set(MetricId::BatchPrefillSeqsLast, composition.prefill_seqs);
    metrics_.set(MetricId::BatchDecodeSeqsLast, composition.decode_seqs);
    // K-2 只统计纯 decode 步；mixed 步单独计数，作为分母污染程度的显式说明。
    pending_pure_decode_ = composition.prefill_tokens == 0 && composition.decode_tokens > 0;
    if (pending_pure_decode_) {
        metrics_.inc(MetricId::DecodeStepsTotal);
    }
    if (composition.prefill_tokens > 0 && composition.decode_tokens > 0) {
        metrics_.inc(MetricId::MixedStepsTotal);
    }
    const std::int64_t elapsed = std::max<std::int64_t>(1, now - started_ns_);
    metrics_.set_float(MetricId::SchedCpuRatio,
                       static_cast<double>(metrics_.get(MetricId::SchedulerNsTotal)) /
                               static_cast<double>(elapsed));
    const std::int64_t generated = metrics_.get(MetricId::TokensGenerated);
    metrics_.set_float(MetricId::TokensPerSec,
                       static_cast<double>(generated) * 1e9 / static_cast<double>(elapsed));
    if (step_trace_open_) {
        step_trace_ << "{\"ts_ns\":" << now << ",\"steps\":" << steps << ",\"tokens\":" << tokens
                    << ",\"prefill_tokens\":" << composition.prefill_tokens
                    << ",\"decode_tokens\":" << composition.decode_tokens << ",\"ns\":" << ns
                    << "}\n";
    }
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
            metrics_.inc(MetricId::PrefixHits);
            metrics_.inc(MetricId::PrefixHitTokens, hit_tokens);
        }
        return hit_tokens;
    }
    metrics_.inc(MetricId::PrefixMisses);
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
            metrics_.inc(MetricId::PreemptStalls);
        }
    }
    if (alloc.ok()) {
        return true;
    }
    const MetricId reject_id = admission_metric_id(alloc.reason);
    if (reject_id != MetricId::Count) {
        metrics_.inc(reject_id);
    }
    if (alloc.reason == AllocFailReason::ContextOverflow) {
        finish(group, FinishReason::Error,
               make_error(ErrCode::ContextOverflow, "prompt+max_tokens 超过 KV 块容量",
                          "tokens=" + std::to_string(total_tokens) +
                                  " capacity=" + std::to_string(kv_.block_capacity_tokens())));
    } else if (running_.empty()) {
        // 无 victim 的 KV 不足是终态；临时低于 watermark 仍给客户端 429 重试语义。
        const ErrCode code = alloc.reason == AllocFailReason::BelowWatermark
                                     ? ErrCode::KvBelowWatermark
                                     : ErrCode::KvInsufficient;
        finish(group, FinishReason::Error,
               make_error(code,
                          code == ErrCode::KvBelowWatermark ? "KV 低于安全水位，请稍后重试"
                                                            : "KV 块不足且无可等待释放的序列",
                          "free=" + std::to_string(kv_.num_free_blocks()) +
                                  " need=" + std::to_string(kv_.blocks_for_tokens(total_tokens))));
    }
    return false;
}

bool SchedulerCore::admit(const std::shared_ptr<SequenceGroup>& group, TokenBudget& budget) {
    Sequence& seq = group->seq;
    // 外部 id 只做协议身份；同名前缀 donor 属于另一内部 backend_id，不会撞新序列。
    const std::int32_t hit_tokens = apply_prefix_hit(group);
    const std::int32_t total_tokens = seq.prompt_len + seq.sampling.max_tokens;
    if (!acquire_blocks(group, total_tokens, hit_tokens)) {
        return false;
    }
    if (runner_.add_sequence(seq) != ErrCode::Ok) {
        kv_.free(seq);
        finish(group, FinishReason::Error,
               make_error(ErrCode::ResourceInsufficient, "后端无法登记序列（槽位可能耗尽）",
                          "req=" + group->req_id));
        return false;
    }
    const ChunkSlice slice =
            planner_.next(context_tokens(seq), seq.num_computed, budget.tokens_remain());
    if (slice.n_tokens <= 0) {
        metrics_.inc(MetricId::ChunkDeferred);
        kv_.free(seq);
        runner_.release_sequence(group->backend_id, 0);
        finish(group, FinishReason::Error,
               make_error(ErrCode::ExecutionFailed, "token 预算不足，无法生成首个切片",
                          "budget_left=" + std::to_string(budget.tokens_remain())));
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
        const BackendId& victim_backend = decision.victims[index];
        auto group = find_running_by_backend(victim_backend);
        if (!group) {
            continue;
        }
        Sequence& seq = group->seq;
        const std::int32_t recompute_tokens = context_tokens(seq);
        runner_.release_sequence(victim_backend, 0);
        group->admitted = false;
        seq.num_computed = 0;
        seq.id = g_seq_id.fetch_add(1, std::memory_order_relaxed); // 旧 serial 完成事件一律 stale
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
        event.victim = victim_backend;
        event.free_before = free_before;
        event.need_blocks = need_blocks;
        event.blocks_released = released;
        event.recompute_tokens = recompute_tokens;
        event.path = cost_model_.choose(false, 1.0);
        event.cost_ms = cost_model_.recompute_ms(recompute_tokens);
        event.cap_error = cap_error;
        trace_->record(event);

        if (trigger == PreemptTrigger::Admission) {
            metrics_.inc(MetricId::PreemptionsAdmission);
        } else {
            metrics_.inc(MetricId::PreemptionsChunkPlan);
        }
        metrics_.inc(MetricId::PreemptedTokens, recompute_tokens);
        metrics_.inc(MetricId::PreemptRecompute);
        if (cap_error) {
            metrics_.inc(MetricId::PreemptCapErrors);
            metrics_.set(MetricId::QueueDepth, static_cast<std::int64_t>(waiting_.size()));
            metrics_.set(MetricId::RequestsQueued, static_cast<std::int64_t>(waiting_.size()));
            finish(group, FinishReason::Error,
                   make_error(
                           ErrCode::ExecutionFailed, "连续抢占次数超限，请求终止",
                           "consecutive=" + std::to_string(group->consecutive_preemptions) +
                                   " cap=" + std::to_string(config_.max_consecutive_preemptions)));
            continue;
        }
        waiting_.push_front(group);
        metrics_.set(MetricId::QueueDepth, static_cast<std::int64_t>(waiting_.size()));
        metrics_.set(MetricId::RequestsQueued, static_cast<std::int64_t>(waiting_.size()));
    }
    metrics_.set(MetricId::RunningSequences, static_cast<std::int64_t>(running_.size()));
    metrics_.set(MetricId::RequestsRunning, static_cast<std::int64_t>(running_.size()));
    if (decision.aging_exempt > 0) {
        metrics_.inc(MetricId::AgingExempt, decision.aging_exempt);
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
    // decode 只在预算允许时占位；调用 cannot 依赖配置校验兜底。
    if (!budget.can_add(0, 1)) {
        return false;
    }
    CachedRequestData data;
    data.backend_id = group->backend_id;
    data.new_token_ids.push_back(seq.output_token_ids.back());
    data.new_block_ids = seq.block_table;
    SequenceStep step;
    step.backend_id = group->backend_id;
    step.route.backend_id = group->backend_id;
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
            metrics_.inc(MetricId::ChunkDeferred);
            return false;
        }
    }
    if (slice.n_tokens <= 0) {
        metrics_.inc(MetricId::ChunkDeferred);
        return false;
    }
    NewRequestData data;
    data.backend_id = group->backend_id;
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
    step.backend_id = group->backend_id;
    step.route.backend_id = group->backend_id;
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
        data.backend_id = seq.backend_id;
        data.prefill_token_ids = seq.prompt_token_ids;
        data.block_ids = seq.block_table;
        data.sampling = seq.sampling;
        step.phase = Phase::PrefillLast;
        step.pos_end = seq.prompt_len;
        batch.total_tokens = seq.prompt_len;
        batch.new_reqs.push_back(std::move(data));
    } else {
        CachedRequestData data;
        data.backend_id = seq.backend_id;
        data.new_token_ids.push_back(seq.output_token_ids.back());
        step.phase = Phase::Decode;
        step.pos_end = seq.num_computed + 1;
        batch.total_tokens = 1;
        batch.cached_reqs.push_back(std::move(data));
    }
    step.backend_id = seq.backend_id;
    step.pos_begin = seq.num_computed;
    step.n_tokens = batch.total_tokens;
    step.needs_sampling = true;
    step.sample_pos = step.pos_end - 1;
    step.route.backend_id = seq.backend_id;
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
        metrics_.inc(MetricId::PrefillTokens, upper - previous);
    }
    if (seq.num_computed < seq.prompt_len) {
        metrics_.inc(MetricId::ChunkPrefillSteps);
        metrics_.inc(MetricId::ChunkPrefillTokens, seq.num_computed - previous);
    }
    kv_.mark_written(seq, seq.num_computed);
    reconcile(seq);
    group->last_progress_ns = now_ns();
    if (group->recompute_active && seq.num_computed >= context_tokens(seq)) {
        group->recompute_active = false;
        group->consecutive_preemptions = 0;
        metrics_.inc(MetricId::PreemptRecovered);
    } else if (!group->recompute_active) {
        group->consecutive_preemptions = 0;
    }
    if (seq.cancel_requested.load() || shutting_down_) {
        finish(group, FinishReason::Aborted);
    }
}

// K-1：服务端 TTFT 从入队（HTTP 受理）到首 token；K-2 只取纯 decode 步间隔。
void SchedulerCore::record_token_metrics(const std::shared_ptr<SequenceGroup>& group,
                                         std::int64_t token_ns, std::int32_t previous,
                                         bool first_token) {
    Sequence& seq = group->seq;
    if (first_token) {
        const double ttft_ms = static_cast<double>(token_ns - group->enqueued_ns) / 1e6;
        metrics_.add_float(MetricId::TtftMsSum, ttft_ms);
        metrics_.inc(MetricId::TtftMsCount);
    } else if (pending_pure_decode_) {
        const double itl_ms = static_cast<double>(token_ns - group->last_token_ns) / 1e6;
        metrics_.add_float(MetricId::ItlMsSum, itl_ms);
        metrics_.inc(MetricId::ItlMsCount);
    }
    group->last_token_ns = token_ns;
    if (previous < seq.prompt_len && seq.num_computed >= seq.prompt_len) {
        metrics_.inc(MetricId::PrefillTokens, seq.prompt_len - previous);
    }
}

void SchedulerCore::apply_completion(const Completion& completion) {
    auto it = std::find(pending_serials_.begin(), pending_serials_.end(), completion.seq_serial);
    if (it == pending_serials_.end()) {
        return;
    }
    pending_serials_.erase(it);
    pending_remaining_ -= 1;
    auto group = find_running_by_serial(completion.seq_serial);
    const bool batch_done = pending_remaining_ == 0;
    if (!group || group->backend_id != completion.backend_id) {
        conclude_step(batch_done);
        return;
    }
    Sequence& seq = group->seq;
    if (!completion.error.ok()) {
        finish(group, FinishReason::Error, completion.error);
        conclude_step(batch_done);
        return;
    }
    if (!completion.sampled_expected) {
        advance_chunk(group, completion);
        conclude_step(batch_done);
        return;
    }
    if (completion.sampled.empty()) {
        finish(group, FinishReason::Error,
               make_error(ErrCode::ExecutionFailed, "后端返回空采样结果",
                          "req=" + group->req_id +
                                  " pos=" + std::to_string(completion.computed_tokens)));
        conclude_step(batch_done);
        return;
    }
    const std::int32_t previous = seq.num_computed;
    const std::int64_t token_ns = now_ns();
    const bool first_token = seq.num_output == 0;
    seq.num_computed = completion.computed_tokens;
    seq.num_output += 1;
    seq.output_token_ids.push_back(completion.sampled.front().token_id);
    kv_.mark_written(seq, seq.num_computed);
    metrics_.inc(MetricId::TokensGenerated);
    record_token_metrics(group, token_ns, previous, first_token);
    reconcile(seq);
    group->last_progress_ns = token_ns;
    if (group->recompute_active && seq.num_computed >= context_tokens(seq)) {
        group->recompute_active = false;
        group->consecutive_preemptions = 0;
        metrics_.inc(MetricId::PreemptRecovered);
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
    if (!group->sink) {
        return DeliverResult::Failed;
    }
    // 溢出次数只由 sink 记账，调度侧按增量上报，避免“关闭”与“队列满”混成一个口径。
    const std::uint64_t overflow_before = group->sink->overflow_total();
    if (!group->sink->push_frame(delta)) {
        const std::uint64_t overflow_after = group->sink->overflow_total();
        if (overflow_after > overflow_before) {
            const std::int64_t dropped =
                    static_cast<std::int64_t>(overflow_after - overflow_before);
            metrics_.inc(MetricId::SinkOverflow, dropped);
            logger()->warn("sink 溢出 req={} overflow={} frames_dropped={}", group->req_id,
                           overflow_after, dropped);
        }
        return DeliverResult::Failed;
    }
    if (reason == FinishReason::None) {
        return DeliverResult::Continue;
    }
    finish(group, reason, Error{}, true);
    return DeliverResult::Finished;
}

void SchedulerCore::finish(const std::shared_ptr<SequenceGroup>& group, FinishReason reason,
                           const Error& error, bool final_frame_pushed) {
    if (!group) {
        return;
    }
    Sequence& seq = group->seq;
    if (seq.status == SeqStatus::Finished) {
        return;
    }
    release_request_id(group->req_id);
    seq.finish = reason;
    seq.status = SeqStatus::Finished;
    if (!final_frame_pushed && group->sink) {
        // 先推错误帧再推终止帧：流式客户端能拿到 code/message，而不是空的正常结束。
        if (reason == FinishReason::Error && !error.ok()) {
            Delta error_delta;
            error_delta.error = error;
            (void)group->sink->push_frame(error_delta);
        }
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
        runner_.release_sequence(group->backend_id, keep);
    }
    running_.erase(std::remove(running_.begin(), running_.end(), group), running_.end());
    if (!pending_step_) {
        reap_donors();
    }
    if (reason == FinishReason::Aborted) {
        metrics_.inc(MetricId::RequestsCancelled);
    } else if (reason == FinishReason::Error) {
        metrics_.inc(MetricId::RequestsError);
        const ErrCode code = error.ok() ? ErrCode::ExecutionFailed : error.code;
        logger()->warn("request failed req={} code={} retryable={} msg={} detail={}", group->req_id,
                       error_code_name(code), retryable(code), error.message, error.detail);
    } else {
        metrics_.inc(MetricId::RequestsOk);
    }
    metrics_.set(MetricId::RunningSequences, static_cast<std::int64_t>(running_.size()));
    metrics_.set(MetricId::RequestsRunning, static_cast<std::int64_t>(running_.size()));
    publish_kv_usage();
    if (!shutting_down_ && !pending_step_) {
        pump();
    }
}

void SchedulerCore::reap_donors() {
    for (const RequestId& donor : kv_.take_dropped_donors()) {
        runner_.release_sequence(donor, 0);
        metrics_.inc(MetricId::PrefixEvictions);
    }
}

void SchedulerCore::reconcile(const Sequence& seq) {
    const std::int32_t backend_pos = runner_.seq_pos_max(seq.backend_id);
    const std::int32_t expected = seq.num_computed - 1;
    if (backend_pos < 0 || expected < 0) {
        return;
    }
    if (backend_pos != expected) {
        metrics_.inc(MetricId::KvReconcileDiffs);
        logger()->warn("kv 对账差异 req={} backend_pos={} expected={}", seq.backend_id, backend_pos,
                       expected);
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

std::shared_ptr<SequenceGroup>
SchedulerCore::find_running_by_backend(const BackendId& backend_id) const {
    for (const auto& group : running_) {
        if (group->backend_id == backend_id) {
            return group;
        }
    }
    return nullptr;
}

// Completion 以内部 serial 定位，避免外部 id 被复用时错投到同名新请求。
std::shared_ptr<SequenceGroup> SchedulerCore::find_running_by_serial(std::int32_t serial) const {
    for (const auto& group : running_) {
        if (group->seq.id == serial) {
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
                return group->backend_id;
            }
        }
    }
    return {};
}

std::int32_t SchedulerCore::running_count() const noexcept {
    return running_depth_.load(std::memory_order_relaxed);
}

std::int32_t SchedulerCore::waiting_count() const noexcept {
    return waiting_depth_.load(std::memory_order_relaxed);
}

// 线程兜底：同一份 Error 分发给所有在飞与排队请求，客户端一定拿到终态。
void SchedulerCore::abort_all(const Error& error) {
    shutting_down_ = true;
    std::vector<std::shared_ptr<SequenceGroup>> running_copy;
    running_copy.swap(running_);
    for (const auto& group : running_copy) {
        finish(group, FinishReason::Error, error);
    }
    while (!waiting_.empty()) {
        auto group = waiting_.front();
        waiting_.pop_front();
        finish(group, FinishReason::Error, error);
    }
    pending_serials_.clear();
    pending_remaining_ = 0;
    pending_step_ = false;
    waiting_depth_.store(0, std::memory_order_relaxed);
    running_depth_.store(0, std::memory_order_relaxed);
    // 仍在 mailbox 里的提交不会再被处理，预留名额必须一并归零。
    inflight_admissions_.store(0, std::memory_order_relaxed);
    metrics_.set(MetricId::QueueDepth, 0);
    metrics_.set(MetricId::RequestsQueued, 0);
    metrics_.set(MetricId::RunningSequences, 0);
    metrics_.set(MetricId::RequestsRunning, 0);
    publish_kv_usage();
}

} // namespace llmopt
