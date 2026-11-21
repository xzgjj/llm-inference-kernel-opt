// rate_limit_test.cc — C-15 令牌桶：关闭语义、突发容量、按时间回填与并发精确计数。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/rate_limit.hpp"

#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

using namespace llmopt;

TEST_CASE("rps<=0 表示关闭：任意并发都放行") {
    RateLimiter limiter(0.0, 1);
    for (int index = 0; index < 1000; ++index) {
        CHECK(limiter.try_acquire());
    }
}

TEST_CASE("突发容量耗尽后拒绝，按 rps 回填一个令牌") {
    RateLimiter limiter(1000.0, 2); // 1ms 回填一个
    CHECK(limiter.try_acquire());
    CHECK(limiter.try_acquire());
    CHECK_FALSE(limiter.try_acquire());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(limiter.try_acquire());
}

TEST_CASE("低速率下桶容量不会被回填超过 burst") {
    RateLimiter limiter(10.0, 1);
    CHECK(limiter.try_acquire());
    CHECK_FALSE(limiter.try_acquire());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(limiter.try_acquire());
    CHECK_FALSE(limiter.try_acquire()); // 即使再等也只补到 burst=1
    CHECK(limiter.rps() == doctest::Approx(10.0));
}

TEST_CASE("并发抢占 burst 时成功数精确等于容量，不漏扣不多扣") {
    // 速率极低，使并发窗口内回填可忽略；16 线程同时抢 8 个令牌，必须恰好 8 个成功。
    RateLimiter limiter(0.001, 8);
    constexpr int kThreads = 16;
    std::atomic<int> successes{0};
    std::latch start(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            if (limiter.try_acquire()) {
                successes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(successes.load(std::memory_order_relaxed) == 8);
}
