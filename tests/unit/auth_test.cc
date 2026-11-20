// auth_test.cc — D6-2 Bearer 比对：空 token 放行、前缀/长度/内容严格匹配。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/auth.hpp"

using namespace llmopt;

TEST_CASE("未配置 api-key 时不校验") {
    CHECK(bearer_allowed("", ""));
    CHECK(bearer_allowed("Bearer anything", ""));
}

TEST_CASE("Bearer 前缀与 token 必须严格匹配") {
    CHECK(bearer_allowed("Bearer secret", "secret"));
    CHECK_FALSE(bearer_allowed("secret", "secret"));
    CHECK_FALSE(bearer_allowed("Basic secret", "secret"));
    CHECK_FALSE(bearer_allowed("Bearer Secret", "secret"));
    CHECK_FALSE(bearer_allowed("Bearer secret ", "secret"));
    CHECK_FALSE(bearer_allowed("Bearer secre", "secret"));
    CHECK_FALSE(bearer_allowed("", "secret"));
}

TEST_CASE("secure_equals 长度不同直接失败、相同内容成功") {
    CHECK(secure_equals("abc", "abc"));
    CHECK_FALSE(secure_equals("abc", "ab"));
    CHECK_FALSE(secure_equals("abd", "abc"));
    CHECK(secure_equals("", ""));
}
