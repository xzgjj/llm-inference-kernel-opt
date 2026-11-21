// metrics.hpp — M9 在线指标：固定 MetricId 表 + 线程分片计数，热路径零锁零分配。
// 在线只出 counter/gauge（含 sum/count）；分位数只在聚合侧由逐请求样本计算（R-06）。
#ifndef LLMOPT_METRICS_HPP
#define LLMOPT_METRICS_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llmopt {

// 指标种类决定渲染格式与读写路径：counter 分片累加，gauge 单点覆盖。
enum class MetricKind : std::uint8_t { CounterInt, CounterFloat, GaugeInt, GaugeFloat };

// 冻结的指标 ID：顺序与 kMetricDefs 一一对应，新增只能追加在 Count 之前。
enum class MetricId : std::uint16_t {
    RequestsAccepted = 0,
    RequestsOk,
    RequestsError,
    RequestsCancelled,
    RequestsQueueFull,
    HttpRequests,
    TokensGenerated,
    PrefillTokens,
    ForwardSteps,
    QueueDepth,
    RunningSequences,
    RequestsRunning,
    RequestsRunningPeak,
    RequestsQueued,
    KvFreeBlocks,
    KvUsedBlocks,
    KvUsedPeak,
    KvCachedBlocks,
    KvSharedBlocks,
    KvWatermarkBlocks,
    KvHeldBlocks,
    KvInternalFragRatio,
    SchedulerNsTotal,
    SchedulerStepsTotal,
    ScheduleStepNsLast,
    SchedCpuRatio,
    BatchSizeLast,
    BatchSizePeak,
    BatchTokensLast,
    BatchTokensPeak,
    BatchPrefillTokensLast,
    BatchDecodeTokensLast,
    BatchPrefillSeqsLast,
    BatchDecodeSeqsLast,
    ChunkPrefillSteps,
    ChunkPrefillTokens,
    ChunkDeferred,
    PreemptionsAdmission,
    PreemptionsChunkPlan,
    PreemptedTokens,
    PreemptRecompute,
    PreemptRecovered,
    PreemptCapErrors,
    PreemptStalls,
    AgingExempt,
    PrefixHits,
    PrefixMisses,
    PrefixHitTokens,
    PrefixEvictions,
    KvReconcileDiffs,
    SinkOverflow,
    AdmissionNoFreeBlock,
    AdmissionBelowWatermark,
    AdmissionContextOverflow,
    AdmissionTokenBudget,
    AdmissionQueue,
    TtftMsSum,
    TtftMsCount,
    ItlMsSum,
    ItlMsCount,
    DecodeStepsTotal,
    MixedStepsTotal,
    TokensPerSec,
    RssMib,
    RssPeakMib,
    VramMib,
    VramPeakMib,
    VramProbeOk,
    VramDeviceMib,
    GateEligible,
    GateDegraded,
    GateBlocked,
    Degraded,
    DrainTimeoutTotal,
    RateLimitedTotal,
    CompletionTimeoutsTotal,
    Count
};

constexpr std::size_t kMetricCount = static_cast<std::size_t>(MetricId::Count);

// 指标定义：family 用于 HELP/TYPE 去重，series 是含固定标签的完整序列名。
struct MetricDef {
    const char* family;
    const char* series;
    MetricKind kind;
    const char* help;
};

// 表按 family 分组连续排列；渲染只在 family 变化时输出一次 HELP/TYPE。
extern const std::array<MetricDef, kMetricCount> kMetricDefs;

// 快照样本：聚合侧只读，不在热路径调用。
struct MetricSample {
    std::string name;
    double value = 0.0;
    MetricKind kind = MetricKind::GaugeInt;
};

struct MetricsSnapshot {
    std::vector<MetricSample> samples;
};

namespace metrics {
using Labels = std::vector<std::pair<std::string, std::string>>;
}

// 指标写侧接口：引擎热路径用 MetricId 快路径，字符串 API 留给冷路径与测试。
class IMetricsSink {
public:
    virtual ~IMetricsSink() = default;
    virtual void inc_counter(const std::string& name, std::uint64_t delta = 1,
                             const metrics::Labels& labels = {}) = 0;
    virtual void set_gauge(const std::string& name, double value,
                           const metrics::Labels& labels = {}) = 0;
    virtual void observe_sample(const std::string& name, double value,
                                const metrics::Labels& labels = {}) = 0;
    virtual MetricsSnapshot snapshot() const = 0;
    virtual std::string expose_text() const = 0;
};

// 分片计数 + 单点 gauge：同一 MetricId 的计数器按线程分片累加，渲染时归并。
class MetricsRegistry final : public IMetricsSink {
public:
    static constexpr std::size_t kShards = 8;

    MetricsRegistry();

    // 热路径：只做原子加/存，无锁、无分配。
    void inc(MetricId id, std::int64_t delta = 1) noexcept;
    void add_float(MetricId id, double delta) noexcept;
    void set(MetricId id, std::int64_t value) noexcept;
    void set_float(MetricId id, double value) noexcept;

    [[nodiscard]] std::int64_t get(MetricId id) const noexcept;
    [[nodiscard]] double get_float(MetricId id) const noexcept;
    // 冷路径/测试用：按完整序列名取值，未知名字返回 0。
    [[nodiscard]] std::int64_t get(std::string_view series) const noexcept;
    [[nodiscard]] double get_float(std::string_view series) const noexcept;

    // 档间清零；token 累计 counter 由调用方决定是否保留（reset_policy）。
    void reset_all() noexcept;
    void reset_counters() noexcept;

    // IMetricsSink：字符串键统一在此解析成 MetricId，找不到即忽略（不新增基数）。
    void inc_counter(const std::string& name, std::uint64_t delta = 1,
                     const metrics::Labels& labels = {}) override;
    void set_gauge(const std::string& name, double value,
                   const metrics::Labels& labels = {}) override;
    void observe_sample(const std::string& name, double value,
                        const metrics::Labels& labels = {}) override;
    [[nodiscard]] MetricsSnapshot snapshot() const override;
    [[nodiscard]] std::string expose_text() const override;

    // 与 expose_text 等价，保留历史调用名。
    [[nodiscard]] std::string render() const;

private:
    struct Slot {
        std::array<std::atomic<std::int64_t>, kShards> counts{};
        std::atomic<double> floats{0.0};
        std::atomic<std::int64_t> gauge{0};
        std::atomic<double> gauge_float{0.0};
    };

    [[nodiscard]] static std::size_t shard() noexcept;
    [[nodiscard]] std::atomic<std::int64_t>& counter(MetricId id) noexcept;
    [[nodiscard]] const std::atomic<std::int64_t>& counter(MetricId id) const noexcept;
    [[nodiscard]] std::atomic<double>& float_counter(MetricId id) noexcept;

    std::array<Slot, kMetricCount> slots_{};
};

// 完整序列名 → ID；未登记返回 MetricId::Count，供冷路径与测试共用。
[[nodiscard]] MetricId find_metric(std::string_view series) noexcept;
[[nodiscard]] const char* metric_kind_name(MetricKind kind) noexcept;

} // namespace llmopt

#endif // LLMOPT_METRICS_HPP
