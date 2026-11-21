// metrics_test.cc — M9 指标表完整性、分片归并、TYPE/HELP 与 KPI 契约。
// 4-G0-1：热路径只做原子操作；这里用多线程累加验证分片不会丢计数。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/metrics.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

std::size_t count_lines(const std::string& text, const std::string& prefix) {
    std::size_t total = 0;
    std::size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
        ++total;
        pos += prefix.size();
    }
    return total;
}

} // namespace

TEST_CASE("指标表完整且序列名唯一") {
    std::set<std::string> series;
    std::set<std::string> families;
    for (std::size_t index = 0; index < llmopt::kMetricCount; ++index) {
        const llmopt::MetricDef& def = llmopt::kMetricDefs[index];
        REQUIRE(def.family != nullptr);
        REQUIRE(def.series != nullptr);
        REQUIRE(def.help != nullptr);
        CHECK(std::string(def.family).size() > 0);
        CHECK(std::string(def.series).size() > 0);
        CHECK(std::string(def.help).size() > 0);
        CHECK(series.insert(def.series).second);
        families.insert(def.family);
        // series 必须以 family 开头，否则 Prometheus 聚合会错位。
        CHECK(std::string(def.series).rfind(def.family, 0) == 0);
    }
    CHECK(series.size() == llmopt::kMetricCount);
    CHECK(families.size() < llmopt::kMetricCount);
}

TEST_CASE("KPI 冻结名可解析") {
    CHECK(llmopt::find_metric("llmopt_ttft_ms_sum") == llmopt::MetricId::TtftMsSum);
    CHECK(llmopt::find_metric("llmopt_itl_ms_count") == llmopt::MetricId::ItlMsCount);
    CHECK(llmopt::find_metric("llmopt_kv_internal_frag_ratio") ==
          llmopt::MetricId::KvInternalFragRatio);
    CHECK(llmopt::find_metric("llmopt_kv_used_blocks_peak") == llmopt::MetricId::KvUsedPeak);
    CHECK(llmopt::find_metric("llmopt_requests_running_peak") ==
          llmopt::MetricId::RequestsRunningPeak);
    CHECK(llmopt::find_metric("llmopt_preemptions_total{reason=\"admission\"}") ==
          llmopt::MetricId::PreemptionsAdmission);
    CHECK(llmopt::find_metric("llmopt_rss_peak_mib") == llmopt::MetricId::RssPeakMib);
    CHECK(llmopt::find_metric("llmopt_gate_verdict_total{verdict=\"ELIGIBLE\"}") ==
          llmopt::MetricId::GateEligible);
    CHECK(llmopt::find_metric("llmopt_not_registered") == llmopt::MetricId::Count);
}

TEST_CASE("多线程分片计数归并后不丢样本") {
    llmopt::MetricsRegistry metrics;
    constexpr std::size_t kThreads = 8;
    constexpr std::int64_t kPerThread = 1000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t index = 0; index < kThreads; ++index) {
        workers.emplace_back([&metrics] {
            for (std::int64_t i = 0; i < 1000; ++i) {
                metrics.inc(llmopt::MetricId::ForwardSteps);
            }
            metrics.add_float(llmopt::MetricId::TtftMsSum, 1.5);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(metrics.get(llmopt::MetricId::ForwardSteps) ==
          static_cast<std::int64_t>(kThreads) * kPerThread);
    CHECK(metrics.get_float(llmopt::MetricId::TtftMsSum) ==
          doctest::Approx(static_cast<double>(kThreads) * 1.5));
    CHECK(std::atomic<std::int64_t>::is_always_lock_free);
}

TEST_CASE("gauge 覆盖写与浮点 gauge") {
    llmopt::MetricsRegistry metrics;
    metrics.set(llmopt::MetricId::RequestsRunning, 3);
    metrics.set(llmopt::MetricId::RequestsRunning, 1);
    CHECK(metrics.get("llmopt_requests_running") == 1);
    metrics.set_float(llmopt::MetricId::KvInternalFragRatio, 0.125);
    CHECK(metrics.get_float("llmopt_kv_internal_frag_ratio") == doctest::Approx(0.125));
    metrics.set_float(llmopt::MetricId::KvInternalFragRatio, 1.75);
    CHECK(metrics.get_float("llmopt_kv_internal_frag_ratio") == doctest::Approx(1.75));
}

TEST_CASE("字符串标签 API 只命中已登记序列") {
    llmopt::MetricsRegistry metrics;
    metrics.inc_counter("llmopt_requests_total", 1, {{"status", "ok"}});
    metrics.inc_counter("llmopt_requests_total", 1, {{"status", "queue_full"}});
    metrics.inc_counter("llmopt_requests_total", 1, {{"status", "unknown"}});
    CHECK(metrics.get("llmopt_requests_total{status=\"ok\"}") == 1);
    CHECK(metrics.get("llmopt_requests_total{status=\"queue_full\"}") == 1);
    CHECK(metrics.get("llmopt_requests_total") == 2);
    metrics.set_gauge("llmopt_queue_depth", 4);
    CHECK(metrics.get("llmopt_queue_depth") == 4);
    metrics.set_gauge("llmopt_missing_metric", 4);
    CHECK(metrics.get("llmopt_missing_metric") == 0);
}

TEST_CASE("observe_sample 落 sum/count 对") {
    llmopt::MetricsRegistry metrics;
    metrics.observe_sample("llmopt_ttft_ms", 10.0);
    metrics.observe_sample("llmopt_ttft_ms", 20.0);
    CHECK(metrics.get("llmopt_ttft_ms_count") == 2);
    CHECK(metrics.get_float("llmopt_ttft_ms_sum") == doctest::Approx(30.0));
}

TEST_CASE("expose_text 每个 family 恰好一组 TYPE/HELP") {
    llmopt::MetricsRegistry metrics;
    metrics.inc(llmopt::MetricId::RequestsOk);
    metrics.inc(llmopt::MetricId::RequestsQueueFull);
    metrics.set_float(llmopt::MetricId::VramPeakMib, std::nan(""));
    const std::string text = metrics.render();
    REQUIRE(!text.empty());

    std::set<std::string> families;
    for (std::size_t index = 0; index < llmopt::kMetricCount; ++index) {
        families.insert(llmopt::kMetricDefs[index].family);
    }
    CHECK(count_lines(text, "# HELP ") == families.size());
    CHECK(count_lines(text, "# TYPE ") == families.size());
    CHECK(text.find("# TYPE llmopt_requests_total counter") != std::string::npos);
    CHECK(text.find("# TYPE llmopt_queue_depth gauge") != std::string::npos);

    std::size_t sample_lines = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        const std::string line = text.substr(pos, end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        ++sample_lines;
        const std::size_t space = line.find(' ');
        REQUIRE(space != std::string::npos);
        REQUIRE(line.find(' ') == line.rfind(' '));
        const std::string value = line.substr(space + 1);
        if (value != "NaN") {
            CHECK(std::strtod(value.c_str(), nullptr) >= 0.0);
        }
    }
    CHECK(sample_lines == llmopt::kMetricCount);
    CHECK(text.find("llmopt_vram_peak_mib NaN") != std::string::npos);
}

TEST_CASE("snapshot 覆盖全部指标且数值与 get 一致") {
    llmopt::MetricsRegistry metrics;
    metrics.inc(llmopt::MetricId::RequestsAccepted, 2);
    metrics.set(llmopt::MetricId::QueueDepth, 5);
    const llmopt::MetricsSnapshot snapshot = metrics.snapshot();
    CHECK(snapshot.samples.size() == llmopt::kMetricCount);
    CHECK(snapshot.samples[static_cast<std::size_t>(llmopt::MetricId::RequestsAccepted)].value ==
          2.0);
    CHECK(snapshot.samples[static_cast<std::size_t>(llmopt::MetricId::QueueDepth)].value == 5.0);
}

TEST_CASE("reset_counters 保留 gauge，reset_all 清零") {
    llmopt::MetricsRegistry metrics;
    metrics.inc(llmopt::MetricId::TokensGenerated, 7);
    metrics.set(llmopt::MetricId::QueueDepth, 3);
    metrics.reset_counters();
    CHECK(metrics.get(llmopt::MetricId::TokensGenerated) == 0);
    CHECK(metrics.get(llmopt::MetricId::QueueDepth) == 3);
    metrics.reset_all();
    CHECK(metrics.get(llmopt::MetricId::QueueDepth) == 0);
}
