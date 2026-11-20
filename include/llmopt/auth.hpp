// auth.hpp — D6-2 可选单 token Bearer：只比对固定 header，不做租户/配额。
#ifndef LLMOPT_AUTH_HPP
#define LLMOPT_AUTH_HPP

#include <cstddef>
#include <string>

namespace llmopt {

// 定长比较：不因首个不同字节提前返回，避免用响应时间猜 token。
inline bool secure_equals(const std::string& left, const std::string& right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        diff |= static_cast<unsigned char>(left[index] ^ right[index]);
    }
    return diff == 0;
}

// header 形如 "Bearer <token>"；token 为空表示未配置鉴权，恒放行。
inline bool bearer_allowed(const std::string& authorization, const std::string& token) noexcept {
    if (token.empty()) {
        return true;
    }
    constexpr const char* kPrefix = "Bearer ";
    if (authorization.rfind(kPrefix, 0) != 0) {
        return false;
    }
    return secure_equals(authorization.substr(7), token);
}

} // namespace llmopt

#endif // LLMOPT_AUTH_HPP
