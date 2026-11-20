// sink.hpp — C-13 每请求一个有界回包队列；socket 写只发生在持连接线程。
#ifndef LLMOPT_SINK_HPP
#define LLMOPT_SINK_HPP

#include "llmopt/common.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace llmopt {

class ResponseSink {
public:
    virtual ~ResponseSink() = default;
    // 有界入队；关闭或等待超时返回 false，调用方据此判为背压/取消。
    virtual bool push_frame(const Delta& delta) = 0;
    virtual void close() = 0;
    // 供 HTTP 线程按需取帧；closed 且队列空时返回 false。
    virtual bool pop_frame(Delta& out, int timeout_ms) = 0;
    virtual bool is_closed() const noexcept = 0;
    virtual void abort() noexcept = 0;
};

// 有界实现：容量满时 push 等待至多 2s，超时返回 false 让 Core 判背压。
class BoundedSink final : public ResponseSink {
public:
    explicit BoundedSink(std::size_t capacity = 256);
    bool push_frame(const Delta& delta) override;
    void close() override;
    bool pop_frame(Delta& out, int timeout_ms) override;
    bool is_closed() const noexcept override;
    void abort() noexcept override;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Delta> queue_;
    std::size_t capacity_;
    bool closed_ = false;
};

} // namespace llmopt

#endif // LLMOPT_SINK_HPP
