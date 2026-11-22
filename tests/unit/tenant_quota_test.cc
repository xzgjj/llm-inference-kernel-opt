// tenant_quota_test.cc — D2-12：key 文件 token 预算解析 + 自然窗口计数与 429 判定。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/config.hpp"
#include "llmopt/token_budget.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string write_key_file(const std::string& name, const std::string& body) {
    const fs::path path = fs::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    out.close();
    return path.string();
}

llmopt::ErrCode load(const std::vector<std::string>& args, llmopt::ServerConfig& cfg,
                     std::string& message) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(const_cast<char*>("llmopt_server")); // argv[0] 是程序名，不是参数
    for (const std::string& item : args) {
        argv.push_back(const_cast<char*>(item.c_str()));
    }
    return llmopt::load_config(static_cast<int>(argv.size()), argv.data(), cfg, message);
}

} // namespace

TEST_CASE("quota 缺省时 token 预算不限") {
    const std::string path = write_key_file("llmopt_quota_default.json",
                                            R"({"version":1,"keys":[{"id":"tenant-a","sha256":")" +
                                                    std::string(64, 'a') + R"("}]})");
    llmopt::ServerConfig cfg;
    std::string message;
    INFO(message);
    REQUIRE(load({"--model", "m.gguf", "--api-keys-file", path}, cfg, message) ==
            llmopt::ErrCode::Ok);
    const auto snapshot = cfg.api_keys.load();
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->keys.size() == 1);
    CHECK(snapshot->keys[0].quota.tokens_per_hour == 0);
    CHECK(snapshot->keys[0].quota.tokens_per_day == 0);
    fs::remove(path);
}

TEST_CASE("quota 解析 tokens_per_hour 与 tokens_per_day") {
    const std::string path = write_key_file(
            "llmopt_quota_tokens.json",
            R"({"version":1,"keys":[{"id":"tenant-a","sha256":")" + std::string(64, 'b') +
                    R"(","quota":{"tokens_per_hour":120,"tokens_per_day":2400}}]})");
    llmopt::ServerConfig cfg;
    std::string message;
    INFO(message);
    REQUIRE(load({"--model", "m.gguf", "--api-keys-file", path}, cfg, message) ==
            llmopt::ErrCode::Ok);
    const auto snapshot = cfg.api_keys.load();
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->keys[0].quota.tokens_per_hour == 120);
    CHECK(snapshot->keys[0].quota.tokens_per_day == 2400);
    fs::remove(path);
}

TEST_CASE("quota 未知键仍被拒绝") {
    const std::string path = write_key_file("llmopt_quota_unknown.json",
                                            R"({"version":1,"keys":[{"id":"tenant-a","sha256":")" +
                                                    std::string(64, 'c') +
                                                    R"(","quota":{"tokens_per_minute":60}}]})");
    llmopt::ServerConfig cfg;
    std::string message;
    CHECK(load({"--model", "m.gguf", "--api-keys-file", path}, cfg, message) ==
          llmopt::ErrCode::ConfigInvalid);
    fs::remove(path);
}

TEST_CASE("quota 负值与非整数被拒绝") {
    for (const std::string body : {R"({"tokens_per_hour":-1})", R"({"tokens_per_day":1.5})"}) {
        const std::string path =
                write_key_file("llmopt_quota_negative.json",
                               R"({"version":1,"keys":[{"id":"tenant-a","sha256":")" +
                                       std::string(64, 'd') + R"(","quota":)" + body + R"(}]})");
        llmopt::ServerConfig cfg;
        std::string message;
        CHECK(load({"--model", "m.gguf", "--api-keys-file", path}, cfg, message) ==
              llmopt::ErrCode::ConfigInvalid);
        fs::remove(path);
    }
}

TEST_CASE("小时窗口用满即拒绝，并给出到下个窗口的等待秒数") {
    llmopt::TenantTokenBudget budget({"tenant-a", "tenant-b"});
    llmopt::TenantQuota quota;
    quota.tokens_per_hour = 100;
    const std::int64_t now = 1'000'000; // 任意固定时刻
    budget.add("tenant-a", 60, now);
    CHECK(budget.hour_used("tenant-a", now) == 60);
    CHECK(budget.retry_after("tenant-a", quota, now) == 0);

    budget.add("tenant-a", 40, now);
    CHECK(budget.hour_used("tenant-a", now) == 100);
    const std::int64_t wait = budget.retry_after("tenant-a", quota, now);
    CHECK(wait > 0);
    CHECK(wait <= 3600);
    // 未超限的邻居不受影响。
    CHECK(budget.retry_after("tenant-b", quota, now) == 0);
}

TEST_CASE("跨小时/跨日窗口自动清零") {
    llmopt::TenantTokenBudget budget({"tenant-a"});
    llmopt::TenantQuota quota;
    quota.tokens_per_hour = 10;
    quota.tokens_per_day = 15;
    const std::int64_t hour0 = 1'000'000 - (1'000'000 % 3600);
    budget.add("tenant-a", 10, hour0 + 3599);
    CHECK(budget.retry_after("tenant-a", quota, hour0 + 3599) > 0);

    const std::int64_t next_hour = hour0 + 3600;
    CHECK(budget.hour_used("tenant-a", next_hour) == 0);
    // 日预算跨小时不清零：剩下 5 个 token 仍可用。
    CHECK(budget.day_used("tenant-a", next_hour) == 10);
    budget.add("tenant-a", 5, next_hour);
    CHECK(budget.retry_after("tenant-a", quota, next_hour) > 0);

    const std::int64_t next_day = (next_hour / 86'400 + 1) * 86'400;
    CHECK(budget.day_used("tenant-a", next_day) == 0);
    CHECK(budget.hour_used("tenant-a", next_day) == 0);
    CHECK(budget.retry_after("tenant-a", quota, next_day) == 0);
}

TEST_CASE("未知租户不计数也不渲染，避免标签基数扩散") {
    llmopt::TenantTokenBudget budget({"tenant-a"});
    budget.add("tenant-z", 5, 1'000'000);
    CHECK(budget.hour_used("tenant-z", 1'000'000) == 0);
    const std::string text = budget.render(1'000'000);
    CHECK(text.find("tenant=\"tenant-a\"") != std::string::npos);
    CHECK(text.find("tenant-z") == std::string::npos);
    CHECK(text.find("# TYPE llmopt_tenant_tokens_used gauge") != std::string::npos);
    CHECK(text.find("window=\"hour\"") != std::string::npos);
    CHECK(text.find("window=\"day\"") != std::string::npos);
}

TEST_CASE("预算为 0 表示不限") {
    llmopt::TenantTokenBudget budget({"tenant-a"});
    llmopt::TenantQuota quota; // 全 0
    budget.add("tenant-a", 1'000'000, 1'000'000);
    CHECK(budget.retry_after("tenant-a", quota, 1'000'000) == 0);
}
