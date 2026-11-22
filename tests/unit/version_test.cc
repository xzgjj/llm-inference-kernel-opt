// version_test.cc — D2-08：版本号唯一来源、semver 形状与 build_info 同源。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/version.hpp"

#include <string>

namespace {

bool is_semver(std::string_view text) {
    // 只接受 major.minor.patch，每段必须是非负整数，不允许多余字符。
    std::size_t dots = 0;
    std::size_t digits = 0;
    for (char ch : text) {
        if (ch == '.') {
            if (digits == 0) {
                return false;
            }
            ++dots;
            digits = 0;
        } else if (ch >= '0' && ch <= '9') {
            ++digits;
        } else {
            return false;
        }
    }
    return dots == 2 && digits > 0;
}

} // namespace

TEST_CASE("版本号是 semver 且与发布版一致") {
    CHECK(llmopt::kVersion == "0.13.0");
    CHECK(is_semver(llmopt::kVersion));
    // 编译期宏与常量必须同一个来源，否则 --version 与日志会各说各话。
    CHECK(std::string(llmopt::kVersion) == std::string(LLMOPT_VERSION));
}

TEST_CASE("llama revision 只有 unknown 或固定 tag 两种合法值") {
    CHECK(!llmopt::kLlamaRevision.empty());
    // stub-only 构建不接 llama.cpp，宏保持 unknown；接了真后端就必须是 deps.cmake 的期望 tag。
    const bool expected = llmopt::kLlamaRevision == "b12557";
    const bool stub_only = llmopt::kLlamaRevision == "unknown";
    if (!expected) {
        CHECK(stub_only);
    }
}

TEST_CASE("semver 校验器拒绝脏版本") {
    CHECK_FALSE(is_semver("0.13"));
    CHECK_FALSE(is_semver("0.13.0-dev"));
    CHECK_FALSE(is_semver("v0.13.0"));
    CHECK_FALSE(is_semver("0..0"));
    CHECK(is_semver("1.2.3"));
}
