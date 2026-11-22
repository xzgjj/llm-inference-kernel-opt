// token_budget.cpp — D2-12：固定窗口的原子计数与超限判定。
#include "llmopt/token_budget.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>

namespace llmopt {
namespace {

// 切窗口用 exchange：同一时刻多个写者只让第一个把 used 归零，其余继续累加。
void roll_window(std::atomic<std::int64_t>& slot, std::atomic<std::int64_t>& used,
                 std::int64_t index) noexcept {
    std::int64_t current = slot.load(std::memory_order_relaxed);
    if (current == index) {
        return;
    }
    std::int64_t expected = current;
    if (slot.compare_exchange_strong(expected, index, std::memory_order_relaxed)) {
        used.store(0, std::memory_order_relaxed);
    }
}

} // namespace

TenantTokenBudget::TenantTokenBudget(const std::vector<std::string>& tenants) {
    for (const std::string& tenant : tenants) {
        if (index_.count(tenant) != 0) {
            continue;
        }
        index_.emplace(tenant, rows_.size());
        rows_.push_back(std::make_unique<Row>());
    }
}

std::int64_t TenantTokenBudget::seconds_to_next_hour(std::int64_t epoch_seconds) noexcept {
    const std::int64_t rem = epoch_seconds % 3600;
    return std::max<std::int64_t>(1, 3600 - (rem < 0 ? 0 : rem));
}

std::int64_t TenantTokenBudget::seconds_to_next_day(std::int64_t epoch_seconds) noexcept {
    const std::int64_t rem = epoch_seconds % 86'400;
    return std::max<std::int64_t>(1, 86'400 - (rem < 0 ? 0 : rem));
}

const TenantTokenBudget::Row* TenantTokenBudget::find(const std::string& tenant) const noexcept {
    const auto it = index_.find(tenant);
    if (it == index_.end()) {
        return nullptr;
    }
    return rows_[it->second].get();
}

TenantTokenBudget::Row* TenantTokenBudget::find_mut(const std::string& tenant) noexcept {
    const auto it = index_.find(tenant);
    if (it == index_.end()) {
        return nullptr;
    }
    return rows_[it->second].get();
}

void TenantTokenBudget::add(std::string_view tenant, std::int64_t tokens,
                            std::int64_t epoch_seconds) noexcept {
    if (tokens <= 0) {
        return;
    }
    Row* target = find_mut(std::string(tenant));
    if (target == nullptr) {
        return;
    }
    roll_window(target->hour_index, target->hour_used, hour_index(epoch_seconds));
    roll_window(target->day_index, target->day_used, day_index(epoch_seconds));
    target->hour_used.fetch_add(tokens, std::memory_order_relaxed);
    target->day_used.fetch_add(tokens, std::memory_order_relaxed);
}

std::int64_t TenantTokenBudget::retry_after(const std::string& tenant, const TenantQuota& quota,
                                            std::int64_t epoch_seconds) const noexcept {
    const Row* row = find(tenant);
    if (row == nullptr) {
        return 0;
    }
    std::int64_t wait = 0;
    if (quota.tokens_per_hour > 0) {
        const std::int64_t used = hour_used(tenant, epoch_seconds);
        if (used >= quota.tokens_per_hour) {
            wait = std::max(wait, seconds_to_next_hour(epoch_seconds));
        }
    }
    if (quota.tokens_per_day > 0) {
        const std::int64_t used = day_used(tenant, epoch_seconds);
        if (used >= quota.tokens_per_day) {
            wait = std::max(wait, seconds_to_next_day(epoch_seconds));
        }
    }
    return wait;
}

std::int64_t TenantTokenBudget::hour_used(const std::string& tenant,
                                          std::int64_t epoch_seconds) const noexcept {
    const Row* row = find(tenant);
    if (row == nullptr) {
        return 0;
    }
    if (row->hour_index.load(std::memory_order_relaxed) != hour_index(epoch_seconds)) {
        return 0; // 上个窗口的余额不进本窗口判定
    }
    return row->hour_used.load(std::memory_order_relaxed);
}

std::int64_t TenantTokenBudget::day_used(const std::string& tenant,
                                         std::int64_t epoch_seconds) const noexcept {
    const Row* row = find(tenant);
    if (row == nullptr) {
        return 0;
    }
    if (row->day_index.load(std::memory_order_relaxed) != day_index(epoch_seconds)) {
        return 0;
    }
    return row->day_used.load(std::memory_order_relaxed);
}

std::string TenantTokenBudget::render(std::int64_t epoch_seconds) const {
    if (rows_.empty()) {
        return {};
    }
    std::string out;
    out += "# HELP llmopt_tenant_tokens_used 租户当前窗口内已生成的 completion token 数\n";
    out += "# TYPE llmopt_tenant_tokens_used gauge\n";
    for (const auto& entry : index_) {
        const std::string& tenant = entry.first;
        out += "llmopt_tenant_tokens_used{tenant=\"" + tenant + "\",window=\"hour\"} " +
               std::to_string(hour_used(tenant, epoch_seconds)) + "\n";
        out += "llmopt_tenant_tokens_used{tenant=\"" + tenant + "\",window=\"day\"} " +
               std::to_string(day_used(tenant, epoch_seconds)) + "\n";
    }
    return out;
}

} // namespace llmopt
