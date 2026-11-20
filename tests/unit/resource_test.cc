// resource_test.cc — K-8 进程级采样契约：RSS 必有、峰值单调、VRAM unknown 语义、start/stop 幂等。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/metrics.hpp"
#include "llmopt/resource.hpp"

#include <chrono>
#include <cmath>
#include <thread>

using namespace llmopt;

TEST_CASE("read_now 在无 GPU 依赖下也能拿到正的 RSS") {
    const ResourceReading reading = ResourceSampler::read_now(false);
    CHECK(reading.rss_mib > 0.0);
    CHECK(std::isfinite(reading.rss_mib));
    CHECK_FALSE(reading.vram_known);
}

TEST_CASE("sample_once 发布 RSS 且峰值只增不减") {
    MetricsRegistry metrics;
    ResourceSampler sampler(metrics, std::chrono::milliseconds(20));
    double previous_peak = 0.0;
    for (int round = 0; round < 20; ++round) {
        const ResourceReading reading = sampler.sample_once();
        CHECK(reading.rss_mib > 0.0);
        const double rss = metrics.get_float(MetricId::RssMib);
        const double peak = metrics.get_float(MetricId::RssPeakMib);
        CHECK(std::isfinite(rss));
        CHECK(std::isfinite(peak));
        CHECK(peak >= rss - 1e-9);
        CHECK(peak >= previous_peak);
        previous_peak = peak;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

TEST_CASE("VRAM 未知时写 NaN 且 probe_ok=0，已知时三者自洽") {
    MetricsRegistry metrics;
    ResourceSampler sampler(metrics, std::chrono::milliseconds(20));
    const ResourceReading reading = sampler.sample_once();
    const double vram = metrics.get_float(MetricId::VramMib);
    const double peak = metrics.get_float(MetricId::VramPeakMib);
    const std::int64_t probe = metrics.get(MetricId::VramProbeOk);
    if (reading.vram_known) {
        CHECK(probe == 1);
        CHECK(std::isfinite(vram));
        CHECK(std::isfinite(peak));
        CHECK(vram >= 0.0);
    } else {
        CHECK(probe == 0);
        CHECK(std::isnan(vram));
        CHECK(std::isnan(peak));
    }
}

TEST_CASE("start/stop 幂等，后台线程停止后 gauge 仍可读") {
    MetricsRegistry metrics;
    ResourceSampler sampler(metrics, std::chrono::milliseconds(10));
    sampler.start();
    sampler.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    sampler.stop();
    sampler.stop();
    CHECK(std::isfinite(metrics.get_float(MetricId::RssMib)));
    CHECK(metrics.get_float(MetricId::RssMib) > 0.0);
}

TEST_CASE("析构时未显式 stop 也能安全收尾") {
    MetricsRegistry metrics;
    {
        ResourceSampler sampler(metrics, std::chrono::milliseconds(10));
        sampler.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(metrics.get_float(MetricId::RssMib) > 0.0);
}
