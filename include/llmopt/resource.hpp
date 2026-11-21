// resource.hpp — K-8 进程级资源采样：RSS 必有，VRAM 按 pid 查 nvidia-smi，采不到写 NaN。
// 采样线程与 /metrics 解耦：只写 gauge，不参与调度；构造即停，start 才起线程。
#ifndef LLMOPT_RESOURCE_HPP
#define LLMOPT_RESOURCE_HPP

#include "llmopt/metrics.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace llmopt {

struct ResourceReading {
    double rss_mib = 0.0;
    double vram_mib = -1.0; // 负数表示 unknown；对外转 NaN。
    bool vram_known = false;
};

// 进程级采样器：默认 500 ms 一次；VRAM 每 4 次采样探一次，避免频繁起 nvidia-smi。
class ResourceSampler {
public:
    explicit ResourceSampler(MetricsRegistry& metrics,
                             std::chrono::milliseconds interval = std::chrono::milliseconds(500));
    ~ResourceSampler();

    ResourceSampler(const ResourceSampler&) = delete;
    ResourceSampler& operator=(const ResourceSampler&) = delete;

    void start();
    void stop();

    // 同步采一次并发布 gauge；测试与启动基线都走这里。
    ResourceReading sample_once();
    // probe_vram=false 供单测跳过外部进程，避免依赖 GPU 工具。
    static ResourceReading read_now(bool probe_vram = true);

private:
    void loop();
    void publish(const ResourceReading& reading);
    bool probe_vram(double& out_mib);

    MetricsRegistry& metrics_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool running_ = false;
    bool stopping_ = false;
    std::int32_t vram_tick_ = 0;
    double vram_cache_mib_ = -1.0;
    bool vram_cached_known_ = false;
    // 峰值字段：仅 start() 同步首报与采样线程 loop 写入；std::thread 构造提供 happens-before，
    // stop() join 后才析构；若新增第三处写入口，必须先改原子或纳入 mutex_ 保护。
    double rss_peak_mib_ = 0.0;
    double vram_peak_mib_ = -1.0;
};

} // namespace llmopt

#endif // LLMOPT_RESOURCE_HPP
