// tenant.hpp — 静态 key → tenant + 配额的值对象；鉴权只在这里做摘要比对。
#ifndef LLMOPT_TENANT_HPP
#define LLMOPT_TENANT_HPP

#include "llmopt/auth.hpp"
#include "llmopt/common.hpp"
#include "llmopt/sha256.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace llmopt {

struct ApiKeyEntry {
    std::string id;
    std::string sha256; // 64 位小写十六进制，不落明文 key
    TenantQuota quota;
};

// 阶段 12 S4：不可变 key 快照。鉴权线程一次 atomic_load 拿到本地 shared_ptr，
// 此后只在快照上匹配，不再触碰可变 config；热加载成功后构造新快照一次 atomic_store
// 替换，旧快照由 shared_ptr 在最后一个在飞请求结束后回收。
struct KeySnapshot {
    std::vector<ApiKeyEntry> keys;
    std::uint64_t generation = 1;
};

// ServerConfig 内的 key 表句柄；读写只走自由函数 std::atomic_load / std::atomic_store，
// shared_ptr 本身可拷贝，ServerConfig 仍保持可值拷贝。
struct KeyTable {
    std::shared_ptr<const KeySnapshot> snapshot;

    // 自由函数形式的原子读写：shared_ptr 本身可拷贝，ServerConfig 仍保持可值拷贝。
    // C++20 起这些自由函数被标记废弃，但 std::atomic<shared_ptr> 不可拷贝、会破坏
    // ServerConfig 值语义；这里仅在调用点局部压制废弃告警，语义不变。
    [[nodiscard]] std::shared_ptr<const KeySnapshot> load() const noexcept {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        return std::atomic_load(&snapshot);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    }

    void store(std::shared_ptr<const KeySnapshot> next) noexcept {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        std::atomic_store(&snapshot, std::move(next));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    }

    [[nodiscard]] bool empty() const noexcept {
        const auto current = load();
        return !current || current->keys.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const auto current = load();
        return current ? current->keys.size() : 0U;
    }

    // 启动期一次性遍历用（装配阶段无并发）；返回快照内条目引用，空表返回静态空向量。
    [[nodiscard]] const std::vector<ApiKeyEntry>& initial_entries() const noexcept {
        static const std::vector<ApiKeyEntry> empty;
        const auto current = load();
        return current ? current->keys : empty;
    }
};

struct AuthDecision {
    bool ok = false;
    std::string tenant;
    TenantQuota quota;
};

// id 同时是 /metrics 标签，字符集从源头收敛，避免任意输入放大标签基数。
inline bool is_valid_tenant_id(const std::string& id) noexcept {
    if (id.empty() || id.size() > 32) {
        return false;
    }
    for (char ch : id) {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '-') {
            return false;
        }
    }
    return true;
}

namespace detail {

// 纯匹配逻辑：只在传入的本地条目上比对，不持有任何可变状态。
inline AuthDecision resolve_bearer_impl(const std::string& authorization,
                                        const std::vector<ApiKeyEntry>& keys,
                                        const TenantQuota& default_quota) {
    if (keys.empty()) {
        return AuthDecision{true, "default", default_quota};
    }
    constexpr const char* kPrefix = "Bearer ";
    if (authorization.rfind(kPrefix, 0) != 0) {
        return AuthDecision{};
    }
    const std::string digest = sha256_hex(authorization.substr(7));
    for (const ApiKeyEntry& entry : keys) {
        if (secure_equals(digest, entry.sha256)) {
            return AuthDecision{true, entry.id, entry.quota};
        }
    }
    return AuthDecision{};
}

} // namespace detail

// spec 只允许 "Bearer <token>"；表为空（或快照缺失）表示未启用鉴权，回落到 default 租户。
// 热路径入口：调用方先 atomic_load 拿到快照，整次请求只在这份本地快照上匹配。
inline AuthDecision resolve_bearer(const std::string& authorization,
                                   const std::shared_ptr<const KeySnapshot>& keys,
                                   const TenantQuota& default_quota) {
    if (!keys) {
        return AuthDecision{true, "default", default_quota};
    }
    return detail::resolve_bearer_impl(authorization, keys->keys, default_quota);
}

// 兼容重载：既有单测直接传 vector；内部包一层临时快照，行为与快照版一致。
// 模板 + SFINAE 仅接受 vector<ApiKeyEntry> 实参，避免 {} 与 shared_ptr 重载产生歧义。
template <typename T,
          typename = std::enable_if_t<std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>,
                                                     std::vector<ApiKeyEntry>>>>
inline AuthDecision resolve_bearer(const std::string& authorization, const T& keys,
                                   const TenantQuota& default_quota) {
    auto snapshot = std::make_shared<KeySnapshot>();
    snapshot->keys = keys;
    return resolve_bearer(authorization, snapshot, default_quota);
}

} // namespace llmopt

#endif // LLMOPT_TENANT_HPP
