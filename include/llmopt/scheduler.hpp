// scheduler.hpp — C-12 有界 Mailbox、C-P2-13 双预算与单线程 SchedulerCore。
// 连续批处理：每 tick 把 running 的 prefill/decode 与待准入请求合成一个不可变 Batch。
#ifndef LLMOPT_SCHEDULER_HPP
#define LLMOPT_SCHEDULER_HPP

#include "llmopt/batch.hpp"
#include "llmopt/kv.hpp"
#include "llmopt/metrics.hpp"
#include "llmopt/runner.hpp"
#include "llmopt/sink.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llmopt {

struct SchedulerConfig {
    std::int32_t max_concurrency = 8;
    std::int32_t max_num_batched_tokens = 2048;
    std::int32_t queue_capacity = 32;
    bool enable_prefix_cache = true;
    // 单步 prefill 宽度；生产值限定 {32,64,128,256}，用例可传更小值。
    std::int32_t chunk_size = 128;
    // 公平性：默认关闭老化，只观测；阈值仅在 aging_enabled 时生效。
    bool fairness_aging_enabled = false;
    std::int64_t aging_threshold_ns = 30'000'000'000LL;
    std::int32_t max_consecutive_preemptions = 3;
    // 请求级总时限，从 enqueued_ns 起算；<=0 表示关闭（只给用例）。
    std::int64_t request_deadline_ns = 120'000'000'000LL;
    // 预留：抢占事件 CSV；空串表示不落盘。
    std::string preempt_trace_path{};
    // 步级 trace（JSONL）：只给离线 p99 复算用，空串关闭，避免热路径写盘。
    std::string step_trace_path{};
    // 单步 Completion 回投超时；<=0 关闭。超时只把请求判失败，不强杀执行线程。
    std::int64_t completion_timeout_ns = 30'000'000'000LL;
    // 批次策略：true=连续批（有空位立刻补，阶段 3 起默认）；false=静态批对照（一批跑空
    // 才准入下一批，用于隔离"槽位复用"这一项收益，只应出现在对照实验里）。
    bool continuous_batching = true;
};

// Chunked prefill 切块结果：n_tokens 为本步可算 token 数，is_last 标记末块需采样。
struct ChunkSlice {
    std::int32_t n_tokens = 0;
    bool is_last = false;
};

// 纯逻辑切块器：只按 prompt 进度与剩余预算取 min(chunk, left, budget)，不碰状态。
class ChunkPlanner {
public:
    explicit ChunkPlanner(std::int32_t chunk_size) noexcept;
    [[nodiscard]] ChunkSlice next(std::int32_t prompt_len, std::int32_t computed,
                                  std::int32_t budget_left) const noexcept;
    [[nodiscard]] std::int32_t chunk_size() const noexcept;

private:
    std::int32_t chunk_size_;
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
    RequestId req_id;     // 外部协议身份，只用于回包/取消/重复 ID 判定
    BackendId backend_id; // 内部后端序列键，服务端生成且永不复用
    std::vector<std::int32_t> prompt_ids;
    SamplingParams sampling;
    bool stream = false;
    std::shared_ptr<ResponseSink> sink;
    Sequence seq;
    bool admitted = false;
    BackendId prefix_source;
    std::int32_t prefix_tokens = 0;
    // 前缀借用只在首个 chunk 下发一次，后续 chunk 不能重复 seq_cp。
    bool prefix_applied = false;
    // 半个 UTF-8 字符先攒在这里，完整后再下发，避免 JSON 编码异常。
    std::string utf8_pending;
    // 抢占与公平性观测；admit_order 越大越新，对应 fcfs_tail。
    std::int64_t admit_order = 0;
    std::int32_t consecutive_preemptions = 0;
    std::int32_t preemptions_total = 0;
    bool recompute_active = false;
    // 准入时已按预算预留的首个 prefill 切片；append_running 直接消费，避免重复扣预算。
    bool has_reserved_slice = false;
    ChunkSlice reserved_slice;
    // 本 tick 是否已有步骤进入待提交批；用于阻止抢占已组批的序列。
    bool scheduled_this_pump = false;
    std::int64_t last_progress_ns = 0;
    std::int64_t enqueued_ns = 0;
    // 本次"在等待队列里排队"的起点：入队时取 enqueued_ns，被抢占回队时重置；
    // 只服务 queue_wait 直方图，不参与 deadline 与公平性判定。
    std::int64_t wait_started_ns = 0;
    // 上一个成立 token 的到达时刻；只用于纯 decode 步的 ITL 样本。
    std::int64_t last_token_ns = 0;
    // Completion 超时已向客户端推过错误：保持运行态等执行线程回投，避免提前释放 KV。
    bool completion_timed_out = false;
    // 阶段 11：租户身份与配额；live 覆盖 running+waiting+尚未被 Core 取走的提交。
    std::string tenant = "default";
    TenantQuota quota;
    bool tenant_live_reserved = false;
    bool tenant_running_slot = false;
};

// 已生成 token 已写入 KV 的长度：prompt 之后只差“最后一个已采样、尚未回填”的 token。
inline std::int32_t context_tokens(const Sequence& seq) noexcept {
    return seq.prompt_len + std::max<std::int32_t>(0, seq.num_output - 1);
}

enum class PreemptTrigger : std::uint8_t { Admission = 0, ChunkPlan = 1 };
enum class RecoveryPath : std::uint8_t { Recompute = 0, Swap = 1 };

struct FairnessState {
    std::int64_t now_ns = 0;
    bool aging_enabled = false;
    std::int64_t aging_threshold_ns = 30'000'000'000LL;
    std::int32_t max_consecutive_preemptions = 3;
};

struct VictimWeights {
    double priority = 0.0;
    double progress = 0.0;
    double blocks = 0.0;
    double remaining = 0.0;
};

// fcfs_tail：默认选最晚准入的在跑序列；weighted 权重非零时按打分升序。
class VictimPolicy {
public:
    void configure(VictimWeights weights) noexcept;
    [[nodiscard]] std::vector<BackendId> select(const std::vector<SequenceGroup*>& groups,
                                                const FairnessState& fairness,
                                                std::int32_t* aging_exempt = nullptr) const;

private:
    VictimWeights weights_;
};

struct PreemptDecision {
    std::vector<BackendId> victims;
    // 与 victims 一一对应；共享块未归零时单个 victim 释放数可能小于块表长度。
    std::vector<std::int32_t> released_per_victim;
    std::int32_t blocks_released = 0;
    // 被老化规则豁免、未参与本次 victim 竞争的组数（仅观测）。
    std::int32_t aging_exempt = 0;
    bool retry_ok = false;
};

struct PreemptEvent {
    std::int64_t ts_ns = 0;
    PreemptTrigger trigger = PreemptTrigger::Admission;
    BackendId victim;
    std::int32_t free_before = 0;
    std::int32_t need_blocks = 0;
    std::int32_t blocks_released = 0;
    std::int32_t recompute_tokens = 0;
    RecoveryPath path = RecoveryPath::Recompute;
    double cost_ms = -1.0;
    bool cap_error = false;
};

// 抢占事件 CSV：字段固定，便于压力档回放与证据归档。
class PreemptTrace {
public:
    explicit PreemptTrace(const std::string& path);
    void record(const PreemptEvent& event);

private:
    std::ofstream file_;
    bool header_written_ = false;
};

// 纯代价模型：本机系数未标定时返回 -1，调用方不得据此选 swap。
class CostModel {
public:
    explicit CostModel(double recompute_ms_per_token = 0.0,
                       double swap_bytes_per_ms = 0.0) noexcept;
    [[nodiscard]] std::int32_t recompute_tokens(const SequenceGroup& group) const noexcept;
    [[nodiscard]] double recompute_ms(std::int32_t tokens) const noexcept;
    [[nodiscard]] std::int64_t swap_bytes(std::int32_t blocks,
                                          std::int32_t cap_tokens) const noexcept;
    [[nodiscard]] double swap_ms(std::int32_t blocks, std::int32_t cap_tokens) const noexcept;
    [[nodiscard]] RecoveryPath choose(bool swap_enabled, double margin) const noexcept;

private:
    double recompute_ms_per_token_;
    double swap_bytes_per_ms_;
};

// 组批前试算：只释放块，不碰序列状态；调用方随后 release/重置/回队首。
PreemptDecision try_preempt(const std::vector<SequenceGroup*>& groups, std::int32_t need_blocks,
                            KVBlockManager& kv, const VictimPolicy& policy,
                            const FairnessState& fairness);

enum class EventKind : std::uint8_t { AddRequest, Cancel, Completion, Shutdown };

struct Event {
    EventKind kind = EventKind::AddRequest;
    RequestId seq_id;
    std::shared_ptr<SequenceGroup> group;
    Completion completion;
};

// 有界 MPMC：满即背压；只有入队与出队，不做状态判断。
// push_critical 走无损通道（Completion/Cancel/Shutdown）：丢一条完成事件 = 调度器永久挂死。
class SchedulerMailbox {
public:
    explicit SchedulerMailbox(std::int32_t capacity);
    bool push(Event event);
    bool push_critical(Event event);
    bool try_push(Event event);
    bool wait_pop(Event& out);
    bool wait_pop_for(Event& out, int timeout_ms);
    bool try_pop(Event& out);
    std::int32_t size() const noexcept;

private:
    bool push_impl(Event event, bool bounded);

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
    // 入口准入判定：HTTP 线程读镜像值，达到队列容量即回 429，不再先进队后报错。
    bool accepting() const noexcept;
    std::int32_t queue_capacity() const noexcept;
    // HTTP 工作线程池按活跃序列上限绑定，不把等待队列折算成并行 handler。
    std::int32_t concurrency_limit() const noexcept;
    // 准入预算 = max_concurrency + queue_capacity；含尚未被 Core 取走的在飞提交。
    std::int32_t admission_capacity() const noexcept;
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
    // /metrics 抓取：tenant 计数快照；kv_used 逐个租户扫描块表。
    [[nodiscard]] std::vector<TenantStat> tenant_stats() const;

private:
    enum class DeliverResult : std::uint8_t { Continue, Finished, Failed };

    void handle(Event& event);
    void handle_add_request(Event& event);
    void record_token_metrics(const std::shared_ptr<SequenceGroup>& group, std::int64_t token_ns,
                              std::int32_t previous, bool first_token);
    void pump();
    void expire_deadlines();
    // 单步 Completion 丢失兜底：只清 pending 并让请求失败，不触碰执行线程。
    void expire_completion_timeouts();
    void preempt_for_waiting();
    void admit_waiting(TokenBudget& budget);
    bool submit_scheduled(Batch& batch, std::chrono::steady_clock::time_point started);
    void publish_kv_usage();
    void record_schedule_step(std::chrono::steady_clock::time_point started, std::int32_t steps,
                              std::int32_t tokens, const Batch& batch);
    // 准入预留：先占坑再投递，避免突发并发同时看到旧镜像而集体越界。
    bool try_reserve_admission() noexcept;
    void release_admission() noexcept;
    // 租户 live 计数跨 HTTP/Core 线程：一把短锁保护 map，计数本身在原子里做加减。
    // 返回 ErrCode：Ok / QuotaExceeded / ResourceInsufficient（分配失败不得留空 slot）。
    ErrCode reserve_tenant_live(const std::string& tenant, const TenantQuota& quota);
    void release_tenant_live(const std::string& tenant) noexcept;
    [[nodiscard]] bool tenant_slot_available(const std::string& tenant,
                                             const TenantQuota& quota) const;
    void tenant_running_acquire(const std::string& tenant);
    void tenant_running_release(const std::string& tenant);
    // 外部 Request ID 只在 submit/finish 冷路径加锁；同一活跃 ID 不得二次入队。
    bool reserve_request_id(const RequestId& req_id);
    void release_request_id(const RequestId& req_id);
    bool admit(const std::shared_ptr<SequenceGroup>& group, TokenBudget& budget);
    std::int32_t apply_prefix_hit(const std::shared_ptr<SequenceGroup>& group);
    bool acquire_blocks(const std::shared_ptr<SequenceGroup>& group, std::int32_t total_tokens,
                        std::int32_t hit_tokens);
    bool append_decode(Batch& batch, const std::shared_ptr<SequenceGroup>& group,
                       TokenBudget& budget);
    bool append_prefill_chunk(Batch& batch, const std::shared_ptr<SequenceGroup>& group,
                              TokenBudget& budget);
    void append_decodes(Batch& batch, TokenBudget& budget);
    void append_prefills(Batch& batch, TokenBudget& budget);
    void apply_completion(const Completion& completion);
    void advance_chunk(const std::shared_ptr<SequenceGroup>& group, const Completion& completion);
    void conclude_step(bool batch_done);
    DeliverResult deliver(const std::shared_ptr<SequenceGroup>& group, const SampledToken& token);
    void finish(const std::shared_ptr<SequenceGroup>& group, FinishReason reason,
                const Error& error = {}, bool final_frame_pushed = false);
    void reject_over_capacity(const std::shared_ptr<SequenceGroup>& group);
    // 线程兜底：异常/关停时把在飞请求全部按同一 Error 收尾，保证客户端拿到终态。
    void abort_all(const Error& error);
    void apply_preemption(const PreemptDecision& decision, PreemptTrigger trigger,
                          std::int32_t free_before, std::int32_t need_blocks);
    std::vector<SequenceGroup*> running_ptrs() const;
    void reap_donors();
    void reconcile(const Sequence& seq);
    std::shared_ptr<SequenceGroup> find_running(const RequestId& req_id) const;
    std::shared_ptr<SequenceGroup> find_running_by_backend(const BackendId& backend_id) const;
    std::shared_ptr<SequenceGroup> find_running_by_serial(std::int32_t serial) const;
    BackendId find_live_donor(const PrefixHit& hit) const;

    ModelRunner& runner_;
    KVBlockManager& kv_;
    MetricsRegistry& metrics_;
    SchedulerConfig config_;
    ChunkPlanner planner_;
    VictimPolicy victim_policy_;
    CostModel cost_model_;
    FairnessState fairness_;
    std::unique_ptr<PreemptTrace> trace_;
    std::int64_t admit_seq_ = 0;
    SchedulerMailbox mailbox_;
    StepExecutor executor_;
    std::deque<std::shared_ptr<SequenceGroup>> waiting_;
    std::vector<std::shared_ptr<SequenceGroup>> running_;
    std::vector<std::int32_t> pending_serials_;
    std::int32_t pending_remaining_ = 0;
    bool pending_step_ = false;
    // 本批提交时刻；Completion 超时判定只读 Core 线程时钟，避免执行线程写状态。
    std::int64_t pending_since_ns_ = 0;
    // 本批是否为纯 decode（无 prefill token）；只有这种步的 token 间隔进 K-2。
    bool pending_pure_decode_ = false;
    bool pumping_ = false;
    bool preempted_this_pump_ = false;
    // 关停/兜底标志：Core 线程写，HTTP 线程在 accepting() 只读；用原子保证跨线程可见。
    std::atomic<bool> shutting_down_{false};
    // 队列深度镜像：只有 Core 线程写，HTTP 线程只读，用于入口准入。
    std::atomic<std::int32_t> waiting_depth_{0};
    std::atomic<std::int32_t> running_depth_{0};
    // 已受理、尚未被 Core 处理的在飞提交数；准入预留的唯一原子锚点。
    std::atomic<std::int32_t> inflight_admissions_{0};
    // 活跃外部 ID 集合：仅在 submit/finish 冷路径访问，只判协议层重复，不参与后端槽位键。
    std::mutex active_ids_mutex_;
    std::unordered_set<RequestId> active_ids_;
    struct TenantCounter {
        std::atomic<std::int32_t> live{0};
    };
    mutable std::mutex tenant_mutex_;
    std::unordered_map<std::string, std::shared_ptr<TenantCounter>> tenant_counters_;
    // 租户 KV 块用量快照：只有 Core 线程能扫块表（publish_kv_usage），
    // HTTP /metrics 线程只读这份不可变拷贝，避免跨线程遍历 KV（DEBUG-01）。
    mutable std::mutex tenant_kv_mutex_;
    std::unordered_map<std::string, std::int32_t> tenant_kv_used_;
    // Core 线程私有的 running 计数；admit/finish 是唯一写点。
    std::unordered_map<std::string, std::int32_t> tenant_running_;
    std::int64_t started_ns_ = 0;
    std::int32_t running_peak_ = 0;
    // KV 占用峰值：Core 线程私有，仅随 publish_kv_usage() 的快照刷新；新增收尾出口须走该路径，
    // 否则峰值可能低于真实占用。
    std::int32_t kv_used_peak_ = 0;
    std::int32_t batch_size_peak_ = 0;
    std::int32_t batch_tokens_peak_ = 0;
    // 步级 trace：可选，默认关闭；开启时每步一行 JSONL，供离线复算 p99。
    std::ofstream step_trace_;
    bool step_trace_open_ = false;
};

} // namespace llmopt

#endif // LLMOPT_SCHEDULER_HPP
