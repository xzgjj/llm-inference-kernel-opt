// rate_limit.hpp — C-15 原子令牌桶：无锁、不参与调度预算。
#ifndef LLMOPT_RATE_LIMIT_HPP
#define LLMOPT_RATE_LIMIT_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace llmopt {

// rps<=0 表示关闭限流，try_acquire 恒真且不做任何原子写。
// 时间戳与令牌各自 CAS：极端并发下可能少扣/多扣一个令牌，但不会死锁，也不进热路径锁。
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
        double tokens = tokens_.load(std::memory_order_relaxed);
        while (true) {
            const std::int64_t last = last_ns_.load(std::memory_order_relaxed);
            const double refill =
                    static_cast<double>(std::max<std::int64_t>(now - last, 0)) * rps_ / 1e9;
            const double available = std::min(burst_, tokens + refill);
            if (available < 1.0) {
                tokens_.store(available, std::memory_order_relaxed);
                last_ns_.store(now, std::memory_order_relaxed);
                return false;
            }
            if (tokens_.compare_exchange_weak(tokens, available - 1.0, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                last_ns_.store(now, std::memory_order_relaxed);
                return true;
            }
        }
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
    std::atomic<double> tokens_;
    std::atomic<std::int64_t> last_ns_;
};

} // namespace llmopt

#endif // LLMOPT_RATE_LIMIT_HPP
