// rate_limit_test.cc — C-15 原子令牌桶：关闭语义、突发容量与按时间回填。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/rate_limit.hpp"

#include <chrono>
#include <thread>

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
