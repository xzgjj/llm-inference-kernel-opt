// sink.cpp — C-13 有界回包队列；唯一写 socket 的仍是持连接的 HTTP 线程。
#include "llmopt/sink.hpp"

#include <chrono>

namespace llmopt {

BoundedSink::BoundedSink(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

// 非阻塞入队：满即关闭并返回 false，由 Core 立即收尾，不拖住调度/执行线程。
bool BoundedSink::push_frame(const Delta& delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return false;
    }
    if (queue_.size() >= capacity_) {
        closed_ = true;
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    queue_.push_back(delta);
    cv_.notify_all();
    return true;
}

void BoundedSink::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

bool BoundedSink::pop_frame(Delta& out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool BoundedSink::is_closed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

void BoundedSink::abort() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        queue_.clear();
    }
    cv_.notify_all();
}

std::uint64_t BoundedSink::overflow_total() const noexcept {
    return overflow_.load(std::memory_order_relaxed);
}

} // namespace llmopt
