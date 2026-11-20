// metrics.cpp — counter/gauge 文本暴露；排序输出便于逐字对比与缓存。
#include "llmopt/metrics.hpp"

#include <sstream>

namespace llmopt {

// 预置 0 值序列：/metrics 从启动第一秒就有完整指标面，抓取端无需等待首个请求。
MetricsRegistry::MetricsRegistry() {
    for (const char* status : {"accepted", "ok", "error", "cancelled", "queue_full"}) {
        values_["llmopt_requests_total{status=\"" + std::string(status) + "\"}"] = 0;
    }
    values_["llmopt_tokens_generated_total"] = 0;
    values_["llmopt_prefill_tokens_total"] = 0;
    values_["llmopt_forward_steps_total"] = 0;
    values_["llmopt_queue_depth"] = 0;
    values_["llmopt_running_sequences"] = 0;
    values_["llmopt_kv_free_blocks"] = 0;
    values_["llmopt_kv_used_blocks"] = 0;
    values_["llmopt_kv_cached_blocks"] = 0;
    values_["llmopt_kv_shared_blocks"] = 0;
    values_["llmopt_kv_watermark_blocks"] = 0;
    values_["llmopt_kv_held_blocks"] = 0;
    values_["llmopt_scheduler_ns_total"] = 0;
    values_["llmopt_scheduler_steps_total"] = 0;
    values_["llmopt_schedule_step_p99_ns"] = 0;
    values_["llmopt_batch_size_last"] = 0;
    values_["llmopt_batch_size_peak"] = 0;
    values_["llmopt_batch_tokens_last"] = 0;
    values_["llmopt_batch_tokens_peak"] = 0;
    values_["llmopt_batch_prefill_tokens_last"] = 0;
    values_["llmopt_batch_decode_tokens_last"] = 0;
    values_["llmopt_batch_prefill_seqs_last"] = 0;
    values_["llmopt_batch_decode_seqs_last"] = 0;
    values_["llmopt_chunk_prefill_steps_total"] = 0;
    values_["llmopt_chunk_prefill_tokens_total"] = 0;
    values_["llmopt_chunk_deferred_total"] = 0;
    values_["llmopt_preemptions_total"] = 0;
    values_["llmopt_preempted_tokens_total"] = 0;
    values_["llmopt_preempt_recompute_total"] = 0;
    values_["llmopt_preempt_recovered_total"] = 0;
    values_["llmopt_preempt_cap_errors_total"] = 0;
    values_["llmopt_preempt_stalls_total"] = 0;
    values_["llmopt_aging_exempt_total"] = 0;
    values_["llmopt_prefix_hits_total"] = 0;
    values_["llmopt_prefix_misses_total"] = 0;
    values_["llmopt_prefix_hit_tokens_total"] = 0;
    values_["llmopt_prefix_evictions_total"] = 0;
    values_["llmopt_kv_reconcile_diffs_total"] = 0;
    for (const char* reason :
         {"no_free_block", "below_watermark", "context_overflow", "token_budget", "queue"}) {
        values_["llmopt_admission_rejected_total{reason=\"" + std::string(reason) + "\"}"] = 0;
    }
}

void MetricsRegistry::inc(const std::string& name, std::int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[name] += delta;
}

void MetricsRegistry::set(const std::string& name, std::int64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[name] = value;
}

std::int64_t MetricsRegistry::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(name);
    return it == values_.end() ? 0 : it->second;
}

std::string MetricsRegistry::render() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    for (const auto& [name, value] : values_) {
        out << name << ' ' << value << '\n';
    }
    return out.str();
}

} // namespace llmopt
