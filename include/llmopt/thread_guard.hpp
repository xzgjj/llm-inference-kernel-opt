// thread_guard.hpp — 线程 join 的 RAII 守卫：异常展开路径也必须先通知停止再 join。
// DEBUG-04：joinable 的 std::thread 在栈展开时析构会 std::terminate，
// 因此凡是"启动后可能抛异常"的线程都用这里包一层。
#ifndef LLMOPT_THREAD_GUARD_HPP
#define LLMOPT_THREAD_GUARD_HPP

#include <functional>
#include <thread>
#include <utility>

namespace llmopt {

class ScopedThread {
public:
    ScopedThread(std::function<void()> body, std::function<void()> stop)
        : stop_(std::move(stop)), thread_(std::move(body)) {}
    ~ScopedThread() {
        stop_and_join();
    }
    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;

    // 幂等：正常收尾与异常展开都会走到这里，第二次调用直接返回。
    void stop_and_join() noexcept {
        if (!thread_.joinable()) {
            return;
        }
        if (stop_) {
            stop_();
        }
        thread_.join();
    }

private:
    std::function<void()> stop_;
    std::thread thread_;
};

} // namespace llmopt

#endif // LLMOPT_THREAD_GUARD_HPP
