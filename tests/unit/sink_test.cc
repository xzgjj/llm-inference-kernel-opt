// sink_test.cc — C-13 修订：回包队列非阻塞，满即失败并关闭，不拖住生产线程。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/sink.hpp"

#include <chrono>
#include <cstdint>

namespace {

llmopt::Delta text_delta(const char* text) {
    llmopt::Delta delta;
    delta.text = text;
    return delta;
}

// 用毫秒比较而不是 tick 计数：steady_clock 的 period 在 MSVC 下是 100ns，
// 拿 count() 直接比常量会把 5 秒当成 50 毫秒。
std::chrono::milliseconds elapsed_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
}

} // namespace

TEST_CASE("队列满时立即失败并置关闭，耗时有上限") {
    llmopt::BoundedSink sink(2);
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(sink.push_frame(text_delta("a")));
    REQUIRE(sink.push_frame(text_delta("b")));
    CHECK_FALSE(sink.push_frame(text_delta("c")));
    CHECK(elapsed_ms(started) < std::chrono::milliseconds(50));
    CHECK(sink.is_closed());
    CHECK(sink.overflow_total() == 1);
}

TEST_CASE("关闭后 pop 立即返回，不阻塞") {
    llmopt::BoundedSink sink(4);
    sink.close();
    llmopt::Delta delta;
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(sink.pop_frame(delta, 500));
    CHECK(elapsed_ms(started) < std::chrono::milliseconds(100));
}

TEST_CASE("abort 清空队列并统计溢出") {
    llmopt::BoundedSink sink(1);
    REQUIRE(sink.push_frame(text_delta("x")));
    CHECK_FALSE(sink.push_frame(text_delta("y")));
    CHECK(sink.overflow_total() == 1);
    sink.abort();
    CHECK(sink.is_closed());
}

TEST_CASE("关闭后的写入不计入溢出，溢出只描述队列满") {
    llmopt::BoundedSink sink(1);
    sink.close();
    CHECK_FALSE(sink.push_frame(text_delta("late")));
    CHECK(sink.overflow_total() == 0);
}
