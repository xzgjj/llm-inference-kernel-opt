// slow_dump_test.cc — D2-10：慢请求 dump 的阈值、脱敏、文件名安全与保留策略。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/slow_dump.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path make_temp_dir() {
    const fs::path dir =
            fs::temp_directory_path() /
            ("llmopt-slow-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

std::string read_all(const fs::path& file) {
    std::ifstream in(file);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

llmopt::SlowRequestRecord make_record(const std::string& req_id, std::int64_t total_ms) {
    llmopt::SlowRequestRecord record;
    record.req_id = req_id;
    record.tenant = "t-a";
    record.prompt_tokens = 128;
    record.output_tokens = 32;
    record.preemptions = 1;
    record.finish = llmopt::FinishReason::Stopped;
    record.queue_wait_ms = 7;
    record.prefill_ms = 21;
    record.first_token_ms = 3;
    record.total_ms = total_ms;
    record.prompt_sha8 = "deadbeef";
    return record;
}

} // namespace

TEST_CASE("关闭或未超阈值时不落盘") {
    const fs::path dir = make_temp_dir();
    llmopt::SlowDumpWriter off(dir.string(), 0);
    CHECK_FALSE(off.enabled());
    CHECK_FALSE(off.maybe_dump(make_record("req-1", 5000)));

    llmopt::SlowDumpWriter on(dir.string(), 1000LL * 1'000'000);
    CHECK(on.enabled());
    CHECK_FALSE(on.maybe_dump(make_record("req-2", 999)));
    CHECK(fs::is_empty(dir));
    fs::remove_all(dir);
}

TEST_CASE("超阈值落一条脱敏 JSONL") {
    const fs::path dir = make_temp_dir();
    llmopt::SlowDumpWriter writer(dir.string(), 500LL * 1'000'000);
    CHECK(writer.maybe_dump(make_record("req-slow", 812)));

    const fs::path file = dir / "req-slow.jsonl";
    REQUIRE(fs::exists(file));
    const std::string text = read_all(file);
    CHECK(text.find("\"req_id\":\"req-slow\"") != std::string::npos);
    CHECK(text.find("\"total_ms\":812") != std::string::npos);
    CHECK(text.find("\"queue_wait_ms\":7") != std::string::npos);
    CHECK(text.find("\"prefill_ms\":21") != std::string::npos);
    CHECK(text.find("\"first_token_ms\":3") != std::string::npos);
    CHECK(text.find("\"prompt_sha8\":\"deadbeef\"") != std::string::npos);
    CHECK(text.find("\"prompt_redacted\":true") != std::string::npos);
    // 一行一条，末尾带换行，便于 append 与 grep。
    CHECK(text.size() > 2);
    CHECK(text.back() == '\n');
    CHECK(text.find('\n') == text.size() - 1);
    fs::remove_all(dir);
}

TEST_CASE("同一 req_id 追加而不是覆盖") {
    const fs::path dir = make_temp_dir();
    llmopt::SlowDumpWriter writer(dir.string(), 1LL * 1'000'000);
    CHECK(writer.maybe_dump(make_record("req-append", 600)));
    CHECK(writer.maybe_dump(make_record("req-append", 700)));
    const std::string text = read_all(dir / "req-append.jsonl");
    std::size_t lines = 0;
    for (char ch : text) {
        if (ch == '\n') {
            ++lines;
        }
    }
    CHECK(lines == 2);
    fs::remove_all(dir);
}

TEST_CASE("非法 req_id 不生成路径穿越文件") {
    const fs::path dir = make_temp_dir();
    llmopt::SlowDumpWriter writer(dir.string(), 1LL * 1'000'000);
    CHECK_FALSE(writer.maybe_dump(make_record("../escape", 900)));
    CHECK_FALSE(writer.maybe_dump(make_record("", 900)));
    CHECK_FALSE(writer.maybe_dump(make_record(std::string(120, 'a'), 900)));
    CHECK_FALSE(writer.maybe_dump(make_record("req ok", 900)));
    CHECK(fs::is_empty(dir));
    fs::remove_all(dir);
}

TEST_CASE("目录只保留最近 20 份") {
    const fs::path dir = make_temp_dir();
    llmopt::SlowDumpWriter writer(dir.string(), 1LL * 1'000'000);
    for (int index = 0; index < 25; ++index) {
        // 每次 mtime 拉开一点，保证排序稳定；文件名 zero-padded 便于人工核对。
        const std::string id =
                "req-" + std::string(3 - std::to_string(index).size(), '0') + std::to_string(index);
        CHECK(writer.maybe_dump(make_record(id, 600)));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
            ++files;
        }
    }
    CHECK(files == llmopt::SlowDumpWriter::kMaxFiles);
    fs::remove_all(dir);
}

TEST_CASE("prompt 指纹稳定、8 位十六进制且不含明文") {
    const std::int32_t ids[] = {101, 202, 303};
    const std::string first = llmopt::prompt_fingerprint(ids, 3);
    CHECK(first.size() == 8);
    for (char ch : first) {
        CHECK(std::isxdigit(static_cast<unsigned char>(ch)) != 0);
    }
    CHECK(first == llmopt::prompt_fingerprint(ids, 3));
    const std::int32_t other[] = {101, 202, 304};
    CHECK(first != llmopt::prompt_fingerprint(other, 3));
    CHECK(first.find("101") == std::string::npos);
}
