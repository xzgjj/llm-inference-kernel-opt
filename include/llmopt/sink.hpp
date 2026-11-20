// sink.hpp — C-13 每请求一个有界回包队列；socket 写只发生在持连接线程。
#ifndef LLMOPT_SINK_HPP
#define LLMOPT_SINK_HPP

#include "llmopt/common.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

namespace llmopt {

class ResponseSink {
public:
    virtual ~ResponseSink() = default;
    // 非阻塞入队：已关闭或队列满立即返回 false（不等待）。调用方按背压/取消收尾。
    virtual bool push_frame(const Delta& delta) = 0;
    virtual void close() = 0;
    // 供 HTTP 线程按需取帧；closed 且队列空时返回 false。
    virtual bool pop_frame(Delta& out, int timeout_ms) = 0;
    virtual bool is_closed() const noexcept = 0;
    virtual void abort() noexcept = 0;
    // 溢出次数：队列满导致的写入失败，用于慢客户端计数。
    virtual std::uint64_t overflow_total() const noexcept = 0;
};

// 有界实现：容量满时立即失败并置关闭，绝不阻塞生产线程（R-02）。
class BoundedSink final : public ResponseSink {
public:
    explicit BoundedSink(std::size_t capacity = 256);
    bool push_frame(const Delta& delta) override;
    void close() override;
    bool pop_frame(Delta& out, int timeout_ms) override;
    bool is_closed() const noexcept override;
    void abort() noexcept override;
    std::uint64_t overflow_total() const noexcept override;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Delta> queue_;
    std::size_t capacity_;
    bool closed_ = false;
    std::atomic<std::uint64_t> overflow_{0};
};

} // namespace llmopt

#endif // LLMOPT_SINK_HPP
