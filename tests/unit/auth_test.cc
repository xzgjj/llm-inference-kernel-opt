// auth_test.cc — D6-2 Bearer 比对：空 token 放行、前缀/长度/内容严格匹配。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/auth.hpp"
#include "llmopt/tenant.hpp"

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

TEST_CASE("静态 key 解析为 tenant 与配额") {
    TenantQuota fallback;
    fallback.max_concurrency = 8;
    ApiKeyEntry entry;
    entry.id = "tenant-a";
    entry.sha256 = sha256_hex("lk_0123456789abcdef0123456789abcdef");
    entry.quota.max_concurrency = 2;
    entry.quota.kv_blocks = 16;
    std::vector<ApiKeyEntry> keys{entry};

    const AuthDecision hit =
            resolve_bearer("Bearer lk_0123456789abcdef0123456789abcdef", keys, fallback);
    CHECK(hit.ok);
    CHECK_EQ(hit.tenant, "tenant-a");
    CHECK_EQ(hit.quota.max_concurrency, 2);
    CHECK_EQ(hit.quota.kv_blocks, 16);
}

TEST_CASE("未知 key、缺 Bearer 与非 Bearer 一律拒绝") {
    TenantQuota fallback;
    ApiKeyEntry entry;
    entry.id = "tenant-a";
    entry.sha256 = sha256_hex("good");
    std::vector<ApiKeyEntry> keys{entry};

    CHECK_FALSE(resolve_bearer("", keys, fallback).ok);
    CHECK_FALSE(resolve_bearer("good", keys, fallback).ok);
    CHECK_FALSE(resolve_bearer("Basic good", keys, fallback).ok);
    CHECK_FALSE(resolve_bearer("Bearer bad", keys, fallback).ok);
}

TEST_CASE("未配置 key 时回落到 default 租户") {
    TenantQuota fallback;
    fallback.max_concurrency = 7;
    const AuthDecision decision = resolve_bearer("", {}, fallback);
    CHECK(decision.ok);
    CHECK_EQ(decision.tenant, "default");
    CHECK_EQ(decision.quota.max_concurrency, 7);
}

TEST_CASE("tenant id 字符集与长度") {
    CHECK(is_valid_tenant_id("tenant-a"));
    CHECK(is_valid_tenant_id("a1"));
    CHECK_FALSE(is_valid_tenant_id(""));
    CHECK_FALSE(is_valid_tenant_id("Tenant"));
    CHECK_FALSE(is_valid_tenant_id("tenant_a"));
    CHECK_FALSE(is_valid_tenant_id(std::string(33, 'a')));
}
