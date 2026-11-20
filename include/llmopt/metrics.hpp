// metrics.hpp — 在线只出 counter/gauge，分位数离线算（ADR-08）。
#ifndef LLMOPT_METRICS_HPP
#define LLMOPT_METRICS_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace llmopt {

class MetricsRegistry {
public:
    // 构造即预置核心计数器/仪表为 0，保证 /metrics 首屏完整。
    MetricsRegistry();
    void inc(const std::string& name, std::int64_t delta = 1);
    void set(const std::string& name, std::int64_t value);
    std::int64_t get(const std::string& name) const;
    std::string render() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::int64_t> values_;
};

} // namespace llmopt

#endif // LLMOPT_METRICS_HPP
