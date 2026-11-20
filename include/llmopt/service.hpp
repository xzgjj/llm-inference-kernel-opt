// service.hpp — C-14 服务生命周期：Starting → Ready → Draining → Stopped。
// 只维护状态，不做 IO；HTTP 层据此决定 /readyz 与是否继续准入。
#ifndef LLMOPT_SERVICE_HPP
#define LLMOPT_SERVICE_HPP

#include <atomic>
#include <cstdint>

namespace llmopt {

enum class ServiceState : std::uint8_t { Starting = 0, Ready, Draining, Stopped };

inline const char* service_state_name(ServiceState state) noexcept {
    switch (state) {
    case ServiceState::Starting:
        return "starting";
    case ServiceState::Ready:
        return "ready";
    case ServiceState::Draining:
        return "draining";
    case ServiceState::Stopped:
        return "stopped";
    }
    return "stopped";
}

// 合法迁移：Starting→Ready/Draining/Stopped；Ready→Draining；Draining→Stopped。
// 其余迁移一律拒绝，避免"停止后又被拉回 Ready"这类不可恢复的错序。
class ServiceController {
public:
    bool transit(ServiceState to) noexcept {
        ServiceState from = state_.load(std::memory_order_acquire);
        while (true) {
            if (!allowed(from, to)) {
                return false;
            }
            if (state_.compare_exchange_weak(from, to, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] ServiceState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    // 只有 Ready 接受新请求；Starting/Draining/Stopped 一律 503。
    [[nodiscard]] bool accepting() const noexcept {
        return state() == ServiceState::Ready;
    }

private:
    static bool allowed(ServiceState from, ServiceState to) noexcept {
        if (from == to) {
            return false;
        }
        switch (from) {
        case ServiceState::Starting:
            return to == ServiceState::Ready || to == ServiceState::Draining ||
                   to == ServiceState::Stopped;
        case ServiceState::Ready:
            return to == ServiceState::Draining || to == ServiceState::Stopped;
        case ServiceState::Draining:
            return to == ServiceState::Stopped;
        case ServiceState::Stopped:
            return false;
        }
        return false;
    }

    std::atomic<ServiceState> state_{ServiceState::Starting};
};

} // namespace llmopt

#endif // LLMOPT_SERVICE_HPP
