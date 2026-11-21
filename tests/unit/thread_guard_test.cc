// thread_guard_test.cc — DEBUG-04：异常展开路径必须先通知停止再 join，且 stop_and_join 幂等。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/thread_guard.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>

TEST_CASE("异常展开时 ScopedThread 先停止再 join，不触发 terminate") {
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    bool caught = false;
    try {
        llmopt::ScopedThread guard(
                [&stop, &finished] {
                    while (!stop.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    finished.store(true, std::memory_order_release);
                },
                [&stop] { stop.store(true, std::memory_order_release); });
        throw std::runtime_error("装配阶段异常");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
    CHECK(finished.load(std::memory_order_acquire));
}

TEST_CASE("stop_and_join 幂等：第二次调用不重复 join") {
    std::atomic<bool> stop{false};
    std::atomic<std::int32_t> stops{0};
    llmopt::ScopedThread guard(
            [&stop] {
                while (!stop.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            },
            [&stop, &stops] {
                stops.fetch_add(1, std::memory_order_relaxed);
                stop.store(true, std::memory_order_release);
            });
    guard.stop_and_join();
    guard.stop_and_join();
    CHECK_EQ(stops.load(std::memory_order_relaxed), 1);
}
