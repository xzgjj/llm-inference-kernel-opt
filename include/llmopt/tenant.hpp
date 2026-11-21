// tenant.hpp — 静态 key → tenant + 配额的值对象；鉴权只在这里做摘要比对。
#ifndef LLMOPT_TENANT_HPP
#define LLMOPT_TENANT_HPP

#include "llmopt/auth.hpp"
#include "llmopt/common.hpp"
#include "llmopt/sha256.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace llmopt {

struct ApiKeyEntry {
    std::string id;
    std::string sha256; // 64 位小写十六进制，不落明文 key
    TenantQuota quota;
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

// spec 只允许 "Bearer <token>"；表为空表示未启用鉴权，回落到 default 租户。
inline AuthDecision resolve_bearer(const std::string& authorization,
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

} // namespace llmopt

#endif // LLMOPT_TENANT_HPP
