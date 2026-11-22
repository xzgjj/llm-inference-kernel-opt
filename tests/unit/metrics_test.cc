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
    // 直方图是独立 family：每个 HistId 各一组 TYPE/HELP，且不进 MetricId 表。
    CHECK(count_lines(text, "# HELP ") == families.size() + llmopt::kHistCount);
    CHECK(count_lines(text, "# TYPE ") == families.size() + llmopt::kHistCount);
    CHECK(text.find("# TYPE llmopt_requests_total counter") != std::string::npos);
    CHECK(text.find("# TYPE llmopt_queue_depth gauge") != std::string::npos);
    CHECK(text.find("# TYPE llmopt_latency_ttft_ms histogram") != std::string::npos);

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
    // 固定指标表之外还有一条静态构建信息（llmopt_build_info），它不属于 MetricId 表；
    // 每个直方图固定输出 bounds_count 个 le 桶 + +Inf + _sum + _count，基数不随样本变化。
    std::size_t expected = llmopt::kMetricCount + 1;
    for (std::size_t index = 0; index < llmopt::kHistCount; ++index) {
        expected += llmopt::kHistogramDefs[index].bounds_count + 3;
    }
    CHECK(sample_lines == expected);
    CHECK(text.find("llmopt_build_info{version=\"") != std::string::npos);
    CHECK(text.find("llmopt_vram_peak_mib NaN") != std::string::npos);
}

TEST_CASE("直方图定义有序、基数受限且不与既有序列撞名") {
    for (std::size_t index = 0; index < llmopt::kHistCount; ++index) {
        const llmopt::HistogramDef& def = llmopt::kHistogramDefs[index];
        REQUIRE(def.name != nullptr);
        REQUIRE(def.help != nullptr);
        REQUIRE(def.bounds != nullptr);
        CHECK(std::string(def.name).rfind("llmopt_", 0) == 0);
        CHECK(def.bounds_count > 0);
        CHECK(def.bounds_count <= llmopt::kHistMaxBuckets);
        for (std::size_t bound = 1; bound < def.bounds_count; ++bound) {
            // 升序，否则 accumulate 顺序与 le 语义都会错。
            CHECK(def.bounds[bound - 1] < def.bounds[bound]);
        }
        // 直方图会输出 <name>_sum/_count；这些名字若与 MetricId 表重合就是重复时间序列。
        const std::string sum_series = std::string(def.name) + "_sum";
        for (std::size_t metric = 0; metric < llmopt::kMetricCount; ++metric) {
            CHECK(std::string(llmopt::kMetricDefs[metric].series) != sum_series);
        }
    }
    CHECK(std::string(llmopt::kHistogramDefs[0].name) == "llmopt_latency_queue_wait_ms");
    CHECK(std::string(llmopt::kHistogramDefs[1].name) == "llmopt_latency_ttft_ms");
    CHECK(std::string(llmopt::kHistogramDefs[2].name) == "llmopt_latency_itl_ms");
}

TEST_CASE("直方图观测落桶、+Inf 收尾与脏样本丢弃") {
    llmopt::HistogramRegistry hist;
    const llmopt::HistId id = llmopt::HistId::QueueWaitMs;
    const std::size_t inf_slot = llmopt::kHistogramDefs[0].bounds_count;

    hist.observe_hist(id, 3.0);    // <= 5，落第 0 桶
    hist.observe_hist(id, 5.0);    // 恰在边界，按 le 语义仍落第 0 桶
    hist.observe_hist(id, 21.0);   // > 20 且 <= 40，落第 3 桶（bounds: 5/10/20/40）
    hist.observe_hist(id, 2000.0); // 超过最大边界，落 +Inf
    hist.observe_hist(id, -1.0);   // 时钟回拨/脏样本丢弃
    hist.observe_hist(id, std::nan(""));
    hist.observe_hist(id, INFINITY);

    CHECK(hist.bucket(id, 0) == 2);
    CHECK(hist.bucket(id, 1) == 0);
    CHECK(hist.bucket(id, 2) == 0);
    CHECK(hist.bucket(id, 3) == 1);
    CHECK(hist.bucket(id, inf_slot) == 1);
    CHECK(hist.count(id) == 4);
    CHECK(hist.sum(id) == doctest::Approx(3.0 + 5.0 + 21.0 + 2000.0));
    // 空直方图不产出样本，但桶槽位仍然存在。
    CHECK(hist.count(llmopt::HistId::ItlMs) == 0);

    std::string text;
    hist.render(text);
    CHECK(text.find("# TYPE llmopt_latency_queue_wait_ms histogram") != std::string::npos);
    // le 一律用整数写法，避免同一系列出现 "5" 与 "5.0" 两种标签。
    CHECK(text.find("le=\"5.0\"") == std::string::npos);
    CHECK(text.find("llmopt_latency_queue_wait_ms_bucket{le=\"5\"} 2") != std::string::npos);
    CHECK(text.find("llmopt_latency_queue_wait_ms_bucket{le=\"10\"} 2") != std::string::npos);
    CHECK(text.find("llmopt_latency_queue_wait_ms_bucket{le=\"20\"} 2") != std::string::npos);
    CHECK(text.find("llmopt_latency_queue_wait_ms_bucket{le=\"40\"} 3") != std::string::npos);
    CHECK(text.find("llmopt_latency_queue_wait_ms_bucket{le=\"+Inf\"} 4") != std::string::npos);
    CHECK(text.find("llmopt_latency_queue_wait_ms_sum 2029.0000") != std::string::npos);
    CHECK(text.find("llmopt_latency_queue_wait_ms_count 4") != std::string::npos);
}

TEST_CASE("reset_counters 与 reset_all 一并清空直方图") {
    llmopt::MetricsRegistry metrics;
    metrics.observe_hist(llmopt::HistId::TtftMs, 120.0);
    metrics.observe_hist(llmopt::HistId::QueueWaitMs, 30.0);
    metrics.observe_hist(llmopt::HistId::QueueWaitMs, 70.0);
    metrics.set(llmopt::MetricId::QueueDepth, 3);
    CHECK(metrics.histograms().count(llmopt::HistId::QueueWaitMs) == 2);

    metrics.reset_counters();
    CHECK(metrics.histograms().count(llmopt::HistId::QueueWaitMs) == 0);
    CHECK(metrics.histograms().count(llmopt::HistId::TtftMs) == 0);
    CHECK(metrics.get(llmopt::MetricId::QueueDepth) == 3); // reset_counters 不动 gauge

    metrics.observe_hist(llmopt::HistId::ItlMs, 12.0);
    metrics.reset_all();
    CHECK(metrics.histograms().count(llmopt::HistId::ItlMs) == 0);
    CHECK(metrics.get(llmopt::MetricId::QueueDepth) == 0);
    const std::string text = metrics.render();
    CHECK(text.find("llmopt_latency_itl_ms_bucket{le=\"+Inf\"} 0") != std::string::npos);
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

TEST_CASE("TenantMetrics 只渲染配置内租户，计数与配额拒绝按标签区分") {
    llmopt::TenantMetrics metrics({"tenant-a", "tenant-b"});
    CHECK(metrics.render({}).find("tenant=\"tenant-a\"") != std::string::npos);
    CHECK(metrics.render({}).find("tenant=\"tenant-z\"") == std::string::npos);

    metrics.inc_request("tenant-a", "accepted");
    metrics.inc_request("tenant-a", "ok");
    metrics.inc_request("tenant-a", "error");
    metrics.inc_rejected("tenant-a", "rate_limit");
    metrics.inc_rejected("tenant-b", "queue");
    metrics.inc_rejected("tenant-b", "quota");
    // 未知租户/未知标签不新增基数，也不写坏数据。
    metrics.inc_request("tenant-z", "ok");
    metrics.inc_rejected("tenant-b", "unknown");
    std::vector<llmopt::TenantStat> stats;
    stats.push_back({"tenant-a", 1, 1, 0, 2});
    stats.push_back({"tenant-b", 3, 1, 2, 4});
    const std::string text = metrics.render(stats);
    CHECK(text.find("llmopt_tenant_requests_total{tenant=\"tenant-a\",status=\"accepted\"} 1") !=
          std::string::npos);
    CHECK(text.find("llmopt_tenant_requests_total{tenant=\"tenant-a\",status=\"ok\"} 1") !=
          std::string::npos);
    CHECK(text.find("llmopt_tenant_requests_total{tenant=\"tenant-a\",status=\"error\"} 1") !=
          std::string::npos);
    CHECK(text.find("llmopt_tenant_quota_rejected_total{tenant=\"tenant-a\",reason=\"rate_limit\"} "
                    "1") != std::string::npos);
    CHECK(text.find("llmopt_tenant_quota_rejected_total{tenant=\"tenant-b\",reason=\"queue\"} 1") !=
          std::string::npos);
    CHECK(text.find("llmopt_tenant_quota_rejected_total{tenant=\"tenant-b\",reason=\"quota\"} 1") !=
          std::string::npos);
    CHECK(text.find("llmopt_tenant_inflight{tenant=\"tenant-b\"} 3") != std::string::npos);
    CHECK(text.find("llmopt_tenant_queue_depth{tenant=\"tenant-b\"} 2") != std::string::npos);
    CHECK(text.find("llmopt_tenant_kv_used_blocks{tenant=\"tenant-a\"} 2") != std::string::npos);
    CHECK(text.find("tenant=\"tenant-z\"") == std::string::npos);
    CHECK(text.find("reason=\"unknown\"") == std::string::npos);
    CHECK(text.find("# TYPE llmopt_tenant_inflight gauge") != std::string::npos);
}
