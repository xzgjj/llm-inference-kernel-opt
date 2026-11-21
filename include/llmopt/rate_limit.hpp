// rate_limit.hpp — C-15 令牌桶：单锁保护 tokens/last_ns，桶状态不允许撕裂。
#ifndef LLMOPT_RATE_LIMIT_HPP
#define LLMOPT_RATE_LIMIT_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace llmopt {

// rps<=0 表示关闭限流，try_acquire 恒真且不进入锁/不做时间读取。
// 弱项：单把 mutex 串行化入口判定；锁内只做算术，不参与调度预算。
class RateLimiter {
public:
    RateLimiter(double rps, std::int32_t burst) noexcept
        : rps_(rps), burst_(static_cast<double>(std::max(burst, 1))),
          tokens_(static_cast<double>(std::max(burst, 1))), last_ns_(now_ns()) {}

    bool try_acquire() noexcept {
        if (rps_ <= 0.0) {
            return true;
        }
        const std::int64_t now = now_ns();
        std::lock_guard<std::mutex> lock(mutex_);
        // 两个字段只在这把锁内读写，回填量与扣减值来自同一个一致快照。
        const double refill =
                static_cast<double>(std::max<std::int64_t>(now - last_ns_, 0)) * rps_ / 1e9;
        tokens_ = std::min(burst_, tokens_ + refill);
        last_ns_ = now;
        if (tokens_ < 1.0) {
            return false;
        }
        tokens_ -= 1.0;
        return true;
    }

    [[nodiscard]] double rps() const noexcept {
        return rps_;
    }

private:
    static std::int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
    }

    double rps_;
    double burst_;
    std::mutex mutex_;
    double tokens_;
    std::int64_t last_ns_;
};

} // namespace llmopt

#endif // LLMOPT_RATE_LIMIT_HPP
