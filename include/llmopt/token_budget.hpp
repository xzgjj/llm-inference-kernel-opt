// token_budget.hpp — D2-12：租户 completion token 预算（自然小时 / 自然日固定窗口）。
//
// 契约（2026-11-22 已与用户核对）：
//   - key 文件 quota 新增 tokens_per_hour / tokens_per_day，缺省=不限（0）；
//   - 只计 completion tokens（prompt 不计费）；
//   - 窗口按 UTC 自然小时/自然日切分，固定窗口、不做滑动；
//   - 超限由 HTTP 层回 429 QUOTA_EXCEEDED，并给 Retry-After（到下个窗口的秒数）。
#ifndef LLMOPT_TOKEN_BUDGET_HPP
#define LLMOPT_TOKEN_BUDGET_HPP

#include "llmopt/common.hpp"
#include "llmopt/tenant.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace llmopt {

// 固定窗口 + 原子计数：写入在调度线程（finish 时一次性加），读取在 HTTP 线程。
class TenantTokenBudget {
public:
    TenantTokenBudget() = default;
    explicit TenantTokenBudget(const std::vector<std::string>& tenants);

    // 统一用 system_clock 的 epoch 秒：调度与 HTTP 两侧必须同一时间基准。
    static std::int64_t now_epoch_seconds() noexcept {
        return static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    }

    static std::int64_t hour_index(std::int64_t epoch_seconds) noexcept {
        return epoch_seconds / 3600;
    }
    static std::int64_t day_index(std::int64_t epoch_seconds) noexcept {
        return epoch_seconds / 86'400;
    }
    // 距离窗口结束的秒数，最小 1，保证 Retry-After 不会给 0。
    static std::int64_t seconds_to_next_hour(std::int64_t epoch_seconds) noexcept;
    static std::int64_t seconds_to_next_day(std::int64_t epoch_seconds) noexcept;

    // 只累加已生成 token；未知租户（未预注册）直接忽略，避免标签基数失控。
    void add(std::string_view tenant, std::int64_t tokens, std::int64_t epoch_seconds) noexcept;

    // 0 = 未超限；>0 = 需要等待的秒数（小时窗口优先，取两者较大值）。
    [[nodiscard]] std::int64_t retry_after(const std::string& tenant, const TenantQuota& quota,
                                           std::int64_t epoch_seconds) const noexcept;

    [[nodiscard]] std::int64_t hour_used(const std::string& tenant,
                                         std::int64_t epoch_seconds) const noexcept;
    [[nodiscard]] std::int64_t day_used(const std::string& tenant,
                                        std::int64_t epoch_seconds) const noexcept;

    // 监控口径：llmopt_tenant_tokens_used{tenant="…",window="hour|day"}。
    [[nodiscard]] std::string render(std::int64_t epoch_seconds) const;

private:
    struct Row {
        std::atomic<std::int64_t> hour_index{-1};
        std::atomic<std::int64_t> hour_used{0};
        std::atomic<std::int64_t> day_index{-1};
        std::atomic<std::int64_t> day_used{0};
    };

    [[nodiscard]] const Row* find(const std::string& tenant) const noexcept;
    [[nodiscard]] Row* find_mut(const std::string& tenant) noexcept;

    std::vector<std::unique_ptr<Row>> rows_;
    std::unordered_map<std::string, std::size_t> index_;
};

} // namespace llmopt

#endif // LLMOPT_TOKEN_BUDGET_HPP
