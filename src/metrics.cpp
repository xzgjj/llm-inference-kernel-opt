// metrics.cpp — 固定指标表 + 线程分片计数 + Prometheus 文本渲染（TYPE/HELP 全覆盖）。
// 热路径只碰原子数组；字符串解析、分片归并与格式化全部落在冷路径（/metrics 抓取时）。
#include "llmopt/metrics.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace llmopt {
namespace {

constexpr const char* kRequestsHelp = "请求总数，status 取 accepted/ok/error/cancelled/queue_full";
constexpr const char* kAdmissionHelp = "准入拒绝总数，reason 取 no_free_block/below_watermark/"
                                       "context_overflow/token_budget/queue";
constexpr const char* kPreemptHelp = "抢占总次数，reason 取 admission/chunk_plan";
constexpr const char* kGateHelp = "启动门控判定总数，verdict 取 ELIGIBLE/DEGRADED/BLOCKED";

} // namespace

// 顺序必须与 MetricId 完全一致；同 family 的标签变体紧邻，渲染只输出一次 HELP/TYPE。
const std::array<MetricDef, kMetricCount> kMetricDefs = {{
        {"llmopt_requests_total", "llmopt_requests_total{status=\"accepted\"}",
         MetricKind::CounterInt, kRequestsHelp},
        {"llmopt_requests_total", "llmopt_requests_total{status=\"ok\"}", MetricKind::CounterInt,
         kRequestsHelp},
        {"llmopt_requests_total", "llmopt_requests_total{status=\"error\"}", MetricKind::CounterInt,
         kRequestsHelp},
        {"llmopt_requests_total", "llmopt_requests_total{status=\"cancelled\"}",
         MetricKind::CounterInt, kRequestsHelp},
        {"llmopt_requests_total", "llmopt_requests_total{status=\"queue_full\"}",
         MetricKind::CounterInt, kRequestsHelp},
        {"llmopt_http_requests_total", "llmopt_http_requests_total", MetricKind::CounterInt,
         "进入补全端点的 HTTP 请求数（含解析失败与拒绝）"},
        {"llmopt_tokens_generated_total", "llmopt_tokens_generated_total", MetricKind::CounterInt,
         "已采样并下发的生成 token 总数"},
        {"llmopt_prefill_tokens_total", "llmopt_prefill_tokens_total", MetricKind::CounterInt,
         "已写入 KV 的 prefill token 总数（含 recompute 重放）"},
        {"llmopt_forward_steps_total", "llmopt_forward_steps_total", MetricKind::CounterInt,
         "执行线程完成的前向步数"},
        {"llmopt_queue_depth", "llmopt_queue_depth", MetricKind::GaugeInt,
         "deprecated：等待队列深度，等价 llmopt_requests_queued"},
        {"llmopt_running_sequences", "llmopt_running_sequences", MetricKind::GaugeInt,
         "deprecated：运行序列数，等价 llmopt_requests_running"},
        {"llmopt_requests_running", "llmopt_requests_running", MetricKind::GaugeInt,
         "当前在跑请求数"},
        {"llmopt_requests_running_peak", "llmopt_requests_running_peak", MetricKind::GaugeInt,
         "进程寿命内运行请求数峰值（K-6）"},
        {"llmopt_requests_queued", "llmopt_requests_queued", MetricKind::GaugeInt,
         "当前等待队列深度"},
        {"llmopt_kv_free_blocks", "llmopt_kv_free_blocks", MetricKind::GaugeInt, "空闲 KV 块数"},
        {"llmopt_kv_used_blocks", "llmopt_kv_used_blocks", MetricKind::GaugeInt,
         "已占用 KV 块数（含可淘汰的 cached donor）"},
        {"llmopt_kv_cached_blocks", "llmopt_kv_cached_blocks", MetricKind::GaugeInt,
         "仍被缓存前缀占用的块数（可淘汰）"},
        {"llmopt_kv_shared_blocks", "llmopt_kv_shared_blocks", MetricKind::GaugeInt,
         "引用计数大于 1 的共享块数"},
        {"llmopt_kv_watermark_blocks", "llmopt_kv_watermark_blocks", MetricKind::GaugeInt,
         "准入预留的空闲块水位"},
        {"llmopt_kv_held_blocks", "llmopt_kv_held_blocks", MetricKind::GaugeInt,
         "活跃占用块数 = used_blocks - cached_blocks（K-5）"},
        {"llmopt_kv_internal_frag_ratio", "llmopt_kv_internal_frag_ratio", MetricKind::GaugeFloat,
         "块表内部碎片率：1 - 已写 token 所需块 / 已分配块（含 max_tokens 预留）"},
        {"llmopt_scheduler_ns_total", "llmopt_scheduler_ns_total", MetricKind::CounterInt,
         "调度线程累计耗时（纳秒）"},
        {"llmopt_scheduler_steps_total", "llmopt_scheduler_steps_total", MetricKind::CounterInt,
         "调度步总数"},
        {"llmopt_schedule_step_ns_last", "llmopt_schedule_step_ns_last", MetricKind::GaugeInt,
         "最近一次调度步耗时（纳秒）；p99 由离线 step trace 复算"},
        {"llmopt_sched_cpu_ratio", "llmopt_sched_cpu_ratio", MetricKind::GaugeFloat,
         "调度耗时占进程墙钟比例（K-4）"},
        {"llmopt_batch_size_last", "llmopt_batch_size_last", MetricKind::GaugeInt,
         "最近一批的序列数"},
        {"llmopt_batch_size_peak", "llmopt_batch_size_peak", MetricKind::GaugeInt,
         "批次序列数峰值"},
        {"llmopt_batch_tokens_last", "llmopt_batch_tokens_last", MetricKind::GaugeInt,
         "最近一批的 token 数"},
        {"llmopt_batch_tokens_peak", "llmopt_batch_tokens_peak", MetricKind::GaugeInt,
         "批次 token 数峰值"},
        {"llmopt_batch_prefill_tokens_last", "llmopt_batch_prefill_tokens_last",
         MetricKind::GaugeInt, "最近一批的 prefill token 数"},
        {"llmopt_batch_decode_tokens_last", "llmopt_batch_decode_tokens_last", MetricKind::GaugeInt,
         "最近一批的 decode token 数"},
        {"llmopt_batch_prefill_seqs_last", "llmopt_batch_prefill_seqs_last", MetricKind::GaugeInt,
         "最近一批的 prefill 序列数"},
        {"llmopt_batch_decode_seqs_last", "llmopt_batch_decode_seqs_last", MetricKind::GaugeInt,
         "最近一批的 decode 序列数"},
        {"llmopt_chunk_prefill_steps_total", "llmopt_chunk_prefill_steps_total",
         MetricKind::CounterInt, "非末块 chunk 的步数"},
        {"llmopt_chunk_prefill_tokens_total", "llmopt_chunk_prefill_tokens_total",
         MetricKind::CounterInt, "非末块 chunk 写入的 token 数"},
        {"llmopt_chunk_deferred_total", "llmopt_chunk_deferred_total", MetricKind::CounterInt,
         "因 token 预算不足而延后的 prefill 次数"},
        {"llmopt_preemptions_total", "llmopt_preemptions_total{reason=\"admission\"}",
         MetricKind::CounterInt, kPreemptHelp},
        {"llmopt_preemptions_total", "llmopt_preemptions_total{reason=\"chunk_plan\"}",
         MetricKind::CounterInt, kPreemptHelp},
        {"llmopt_preempted_tokens_total", "llmopt_preempted_tokens_total", MetricKind::CounterInt,
         "被抢占请求丢弃并需重算的 token 总数"},
        {"llmopt_preempt_recompute_total", "llmopt_preempt_recompute_total", MetricKind::CounterInt,
         "走 recompute 恢复路径的抢占次数"},
        {"llmopt_preempt_recovered_total", "llmopt_preempt_recovered_total", MetricKind::CounterInt,
         "重算完成并恢复生成的请求数"},
        {"llmopt_preempt_cap_errors_total", "llmopt_preempt_cap_errors_total",
         MetricKind::CounterInt, "连续抢占超限而终止的请求数"},
        {"llmopt_preempt_stalls_total", "llmopt_preempt_stalls_total", MetricKind::CounterInt,
         "抢占试算未能释放足够块的次数"},
        {"llmopt_aging_exempt_total", "llmopt_aging_exempt_total", MetricKind::CounterInt,
         "因老化规则豁免、未参与 victim 竞争的组数"},
        {"llmopt_prefix_hits_total", "llmopt_prefix_hits_total", MetricKind::CounterInt,
         "前缀缓存命中次数"},
        {"llmopt_prefix_misses_total", "llmopt_prefix_misses_total", MetricKind::CounterInt,
         "前缀缓存未命中次数"},
        {"llmopt_prefix_hit_tokens_total", "llmopt_prefix_hit_tokens_total", MetricKind::CounterInt,
         "前缀命中的 token 总数"},
        {"llmopt_prefix_evictions_total", "llmopt_prefix_evictions_total", MetricKind::CounterInt,
         "被淘汰的缓存 donor 数"},
        {"llmopt_kv_reconcile_diffs_total", "llmopt_kv_reconcile_diffs_total",
         MetricKind::CounterInt, "块表与后端位置对账差异次数"},
        {"llmopt_sink_overflow_total", "llmopt_sink_overflow_total", MetricKind::CounterInt,
         "回包队列满导致的溢出次数（请求随即断开，不丢帧）"},
        {"llmopt_admission_rejected_total",
         "llmopt_admission_rejected_total{reason=\"no_free_block\"}", MetricKind::CounterInt,
         kAdmissionHelp},
        {"llmopt_admission_rejected_total",
         "llmopt_admission_rejected_total{reason=\"below_watermark\"}", MetricKind::CounterInt,
         kAdmissionHelp},
        {"llmopt_admission_rejected_total",
         "llmopt_admission_rejected_total{reason=\"context_overflow\"}", MetricKind::CounterInt,
         kAdmissionHelp},
        {"llmopt_admission_rejected_total",
         "llmopt_admission_rejected_total{reason=\"token_budget\"}", MetricKind::CounterInt,
         kAdmissionHelp},
        {"llmopt_admission_rejected_total", "llmopt_admission_rejected_total{reason=\"queue\"}",
         MetricKind::CounterInt, kAdmissionHelp},
        {"llmopt_ttft_ms_sum", "llmopt_ttft_ms_sum", MetricKind::CounterFloat,
         "首 token 延迟累计（毫秒，服务端从入队到首帧）"},
        {"llmopt_ttft_ms_count", "llmopt_ttft_ms_count", MetricKind::CounterInt,
         "首 token 延迟样本数（K-1）"},
        {"llmopt_itl_ms_sum", "llmopt_itl_ms_sum", MetricKind::CounterFloat,
         "纯 decode 步 token 间隔累计（毫秒）"},
        {"llmopt_itl_ms_count", "llmopt_itl_ms_count", MetricKind::CounterInt,
         "纯 decode 步 token 间隔样本数（K-2，不含 mixed 步）"},
        {"llmopt_decode_steps_total", "llmopt_decode_steps_total", MetricKind::CounterInt,
         "纯 decode 步数"},
        {"llmopt_mixed_steps_total", "llmopt_mixed_steps_total", MetricKind::CounterInt,
         "含 prefill 的 mixed 步数（不计入 K-2 分母）"},
        {"llmopt_tokens_per_sec", "llmopt_tokens_per_sec", MetricKind::GaugeFloat,
         "启动至今平均生成吞吐（tok/s，窗口值由离线聚合给出）"},
        {"llmopt_rss_mib", "llmopt_rss_mib", MetricKind::GaugeFloat, "当前进程 RSS（MiB）"},
        {"llmopt_rss_peak_mib", "llmopt_rss_peak_mib", MetricKind::GaugeFloat,
         "进程 RSS 峰值（MiB，K-8）"},
        {"llmopt_vram_mib", "llmopt_vram_mib", MetricKind::GaugeFloat,
         "当前进程 VRAM（MiB）；NaN 表示本平台采不到"},
        {"llmopt_vram_peak_mib", "llmopt_vram_peak_mib", MetricKind::GaugeFloat,
         "进程 VRAM 峰值（MiB，K-8）；NaN 表示采不到"},
        {"llmopt_vram_probe_ok", "llmopt_vram_probe_ok", MetricKind::GaugeInt,
         "VRAM 进程级采样可用性：1 可用 / 0 不可用"},
        {"llmopt_gate_verdict_total", "llmopt_gate_verdict_total{verdict=\"ELIGIBLE\"}",
         MetricKind::CounterInt, kGateHelp},
        {"llmopt_gate_verdict_total", "llmopt_gate_verdict_total{verdict=\"DEGRADED\"}",
         MetricKind::CounterInt, kGateHelp},
        {"llmopt_gate_verdict_total", "llmopt_gate_verdict_total{verdict=\"BLOCKED\"}",
         MetricKind::CounterInt, kGateHelp},
        {"llmopt_degraded", "llmopt_degraded", MetricKind::GaugeInt,
         "门控降级标记：1 表示 force_on 降并发放行 / 0 正常（K-9）"},
        {"llmopt_drain_timeout_total", "llmopt_drain_timeout_total", MetricKind::CounterInt,
         "排空超过 drain-deadline-seconds 的次数（超时后在飞请求强制中止）"},
        {"llmopt_rate_limited_total", "llmopt_rate_limited_total", MetricKind::CounterInt,
         "令牌桶拒绝的请求数（429，未进入队列）"},
}};

MetricId find_metric(std::string_view series) noexcept {
    for (std::size_t index = 0; index < kMetricDefs.size(); ++index) {
        if (series == kMetricDefs[index].series) {
            return static_cast<MetricId>(index);
        }
    }
    return MetricId::Count;
}

const char* metric_kind_name(MetricKind kind) noexcept {
    switch (kind) {
    case MetricKind::CounterInt:
    case MetricKind::CounterFloat:
        return "counter";
    case MetricKind::GaugeInt:
    case MetricKind::GaugeFloat:
        return "gauge";
    }
    return "untyped";
}

MetricsRegistry::MetricsRegistry() = default;

// 线程分片：首次调用分配稳定的小整数；线程数超过分片数时复用分片，仍保持无锁与正确性。
std::size_t MetricsRegistry::shard() noexcept {
    static std::atomic<std::uint32_t> next{0};
    static thread_local const std::size_t index =
            static_cast<std::size_t>(next.fetch_add(1, std::memory_order_relaxed)) % kShards;
    return index;
}

std::atomic<std::int64_t>& MetricsRegistry::counter(MetricId id) noexcept {
    return slots_[static_cast<std::size_t>(id)].counts[shard()];
}

const std::atomic<std::int64_t>& MetricsRegistry::counter(MetricId id) const noexcept {
    return slots_[static_cast<std::size_t>(id)].counts[shard()];
}

std::atomic<double>& MetricsRegistry::float_counter(MetricId id) noexcept {
    return slots_[static_cast<std::size_t>(id)].floats;
}

void MetricsRegistry::inc(MetricId id, std::int64_t delta) noexcept {
    counter(id).fetch_add(delta, std::memory_order_relaxed);
}

void MetricsRegistry::add_float(MetricId id, double delta) noexcept {
    std::atomic<double>& target = float_counter(id);
    double expected = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(expected, expected + delta, std::memory_order_relaxed)) {
    }
}

void MetricsRegistry::set(MetricId id, std::int64_t value) noexcept {
    slots_[static_cast<std::size_t>(id)].gauge.store(value, std::memory_order_relaxed);
}

void MetricsRegistry::set_float(MetricId id, double value) noexcept {
    slots_[static_cast<std::size_t>(id)].gauge_float.store(value, std::memory_order_relaxed);
}

std::int64_t MetricsRegistry::get(MetricId id) const noexcept {
    if (id == MetricId::Count) {
        return 0;
    }
    const Slot& slot = slots_[static_cast<std::size_t>(id)];
    switch (kMetricDefs[static_cast<std::size_t>(id)].kind) {
    case MetricKind::CounterInt: {
        std::int64_t total = 0;
        for (const auto& part : slot.counts) {
            total += part.load(std::memory_order_relaxed);
        }
        return total;
    }
    case MetricKind::CounterFloat:
        return static_cast<std::int64_t>(slot.floats.load(std::memory_order_relaxed));
    case MetricKind::GaugeInt:
        return slot.gauge.load(std::memory_order_relaxed);
    case MetricKind::GaugeFloat:
        return static_cast<std::int64_t>(slot.gauge_float.load(std::memory_order_relaxed));
    }
    return 0;
}

double MetricsRegistry::get_float(MetricId id) const noexcept {
    if (id == MetricId::Count) {
        return 0.0;
    }
    const Slot& slot = slots_[static_cast<std::size_t>(id)];
    switch (kMetricDefs[static_cast<std::size_t>(id)].kind) {
    case MetricKind::CounterInt:
        return static_cast<double>(get(id));
    case MetricKind::CounterFloat:
        return slot.floats.load(std::memory_order_relaxed);
    case MetricKind::GaugeInt:
        return static_cast<double>(slot.gauge.load(std::memory_order_relaxed));
    case MetricKind::GaugeFloat:
        return slot.gauge_float.load(std::memory_order_relaxed);
    }
    return 0.0;
}

std::int64_t MetricsRegistry::get(std::string_view series) const noexcept {
    const MetricId id = find_metric(series);
    if (id != MetricId::Count) {
        return get(id);
    }
    // 冷路径便利：允许用 family 名读取全部标签变体之和（测试/脚本对账用）。
    std::int64_t total = 0;
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        if (std::string_view(kMetricDefs[index].family) == series) {
            total += get(static_cast<MetricId>(index));
        }
    }
    return total;
}

double MetricsRegistry::get_float(std::string_view series) const noexcept {
    const MetricId id = find_metric(series);
    if (id != MetricId::Count) {
        return get_float(id);
    }
    double total = 0.0;
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        if (std::string_view(kMetricDefs[index].family) == series) {
            total += get_float(static_cast<MetricId>(index));
        }
    }
    return total;
}

void MetricsRegistry::reset_all() noexcept {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        for (auto& part : slots_[index].counts) {
            part.store(0, std::memory_order_relaxed);
        }
        slots_[index].floats.store(0.0, std::memory_order_relaxed);
        slots_[index].gauge.store(0, std::memory_order_relaxed);
        slots_[index].gauge_float.store(0.0, std::memory_order_relaxed);
    }
}

void MetricsRegistry::reset_counters() noexcept {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (kMetricDefs[index].kind == MetricKind::CounterInt ||
            kMetricDefs[index].kind == MetricKind::CounterFloat) {
            for (auto& part : slots_[index].counts) {
                part.store(0, std::memory_order_relaxed);
            }
            slots_[index].floats.store(0.0, std::memory_order_relaxed);
        }
    }
}

// 字符串键只用于冷路径：拼出带标签的完整序列名后查表，未登记的名字直接忽略。
void MetricsRegistry::inc_counter(const std::string& name, std::uint64_t delta,
                                  const metrics::Labels& labels) {
    std::string series = name;
    if (!labels.empty()) {
        series += '{';
        for (std::size_t index = 0; index < labels.size(); ++index) {
            if (index != 0) {
                series += ',';
            }
            series += labels[index].first + "=\"" + labels[index].second + "\"";
        }
        series += '}';
    }
    const MetricId id = find_metric(series);
    if (id == MetricId::Count) {
        return;
    }
    if (kMetricDefs[static_cast<std::size_t>(id)].kind == MetricKind::CounterFloat) {
        add_float(id, static_cast<double>(delta));
    } else {
        inc(id, static_cast<std::int64_t>(delta));
    }
}

void MetricsRegistry::set_gauge(const std::string& name, double value,
                                const metrics::Labels& labels) {
    std::string series = name;
    if (!labels.empty()) {
        series += '{';
        for (std::size_t index = 0; index < labels.size(); ++index) {
            if (index != 0) {
                series += ',';
            }
            series += labels[index].first + "=\"" + labels[index].second + "\"";
        }
        series += '}';
    }
    const MetricId id = find_metric(series);
    if (id == MetricId::Count) {
        return;
    }
    if (kMetricDefs[static_cast<std::size_t>(id)].kind == MetricKind::GaugeFloat) {
        set_float(id, value);
    } else {
        set(id, static_cast<std::int64_t>(value));
    }
}

// 在线不算分位数：observe_sample 只把样本并入已登记的 <name>_sum/_count 对。
void MetricsRegistry::observe_sample(const std::string& name, double value,
                                     const metrics::Labels& labels) {
    inc_counter(name + "_count", 1, labels);
    const std::string sum_name = name + "_sum";
    const MetricId sum_id = find_metric(sum_name);
    if (sum_id != MetricId::Count &&
        kMetricDefs[static_cast<std::size_t>(sum_id)].kind == MetricKind::CounterFloat) {
        add_float(sum_id, value);
    }
}

MetricsSnapshot MetricsRegistry::snapshot() const {
    MetricsSnapshot out;
    out.samples.reserve(kMetricCount);
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        MetricSample sample;
        sample.name = kMetricDefs[index].series;
        sample.value = get_float(static_cast<MetricId>(index));
        sample.kind = kMetricDefs[index].kind;
        out.samples.push_back(std::move(sample));
    }
    return out;
}

namespace {

// 浮点渲染：NaN 原样输出供 Prometheus 解析，其余保留 4 位小数。
std::string format_value(MetricKind kind, double value) {
    char buffer[64];
    if (kind == MetricKind::CounterInt || kind == MetricKind::GaugeInt) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return buffer;
    }
    if (std::isnan(value)) {
        return "NaN";
    }
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    return buffer;
}

} // namespace

std::string MetricsRegistry::expose_text() const {
    std::string out;
    out.reserve(kMetricCount * 96);
    const char* current_family = nullptr;
    for (std::size_t index = 0; index < kMetricCount; ++index) {
        const MetricDef& def = kMetricDefs[index];
        if (current_family == nullptr || std::strcmp(current_family, def.family) != 0) {
            current_family = def.family;
            out += "# HELP " + std::string(def.family) + " " + std::string(def.help) + "\n";
            out += "# TYPE " + std::string(def.family) + " " + metric_kind_name(def.kind) + "\n";
        }
        out += std::string(def.series) + " " +
               format_value(def.kind, get_float(static_cast<MetricId>(index))) + "\n";
    }
    return out;
}

std::string MetricsRegistry::render() const {
    return expose_text();
}

} // namespace llmopt
