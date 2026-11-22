// slow_dump.cpp — D2-10：慢请求落盘与目录保留策略。
#include "llmopt/slow_dump.hpp"

#include "llmopt/sha256.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string_view>
#include <system_error>
#include <vector>

namespace llmopt {
namespace {

using json = nlohmann::json;

// 文件名只接受 [A-Za-z0-9._-]：req_id 来自 HTTP 头/客户端，不能直接当路径用。
bool is_safe_id(std::string_view id) {
    if (id.empty() || id.size() > 96) {
        return false;
    }
    for (char ch : id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string prompt_fingerprint(const std::int32_t* ids, std::size_t count) {
    std::string joined;
    joined.reserve(count * 6);
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            joined.push_back(',');
        }
        joined += std::to_string(ids[index]);
    }
    return sha256_hex(joined).substr(0, 8);
}

SlowDumpWriter::SlowDumpWriter(std::string dir, std::int64_t threshold_ns)
    : dir_(std::move(dir)), threshold_ns_(threshold_ns) {
    if (threshold_ns_ <= 0) {
        threshold_ns_ = 0;
    }
}

bool SlowDumpWriter::maybe_dump(const SlowRequestRecord& record) const {
    if (!enabled() || record.total_ms * 1'000'000LL < threshold_ns_) {
        return false;
    }
    if (!is_safe_id(record.req_id)) {
        spdlog::warn("慢请求 dump 跳过：req_id 含非法字符 req_id={}", record.req_id);
        return false;
    }
    const std::filesystem::path root(dir_);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        spdlog::warn("慢请求 dump 建目录失败 dir={} error={}", dir_, error.message());
        return false;
    }
    const std::filesystem::path file = root / (record.req_id + ".jsonl");
    const std::int64_t existing =
            std::filesystem::exists(file, error)
                    ? static_cast<std::int64_t>(std::filesystem::file_size(file))
                    : 0;
    if (existing >= kMaxFileBytes) {
        spdlog::warn("慢请求 dump 跳过：单文件已达上限 file={} bytes={}", file.string(), existing);
        return false;
    }
    const json line = {{"req_id", record.req_id},
                       {"tenant", record.tenant},
                       {"finish", finish_reason_name(record.finish)},
                       {"prompt_tokens", record.prompt_tokens},
                       {"output_tokens", record.output_tokens},
                       {"preemptions", record.preemptions},
                       {"queue_wait_ms", record.queue_wait_ms},
                       {"prefill_ms", record.prefill_ms},
                       {"first_token_ms", record.first_token_ms},
                       {"total_ms", record.total_ms},
                       {"prompt_sha8", record.prompt_sha8},
                       {"prompt_redacted", true}};
    std::ofstream out(file, std::ios::app);
    if (!out) {
        spdlog::warn("慢请求 dump 打开失败 file={}", file.string());
        return false;
    }
    out << line.dump() << '\n';
    out.close();
    prune();
    return true;
}

void SlowDumpWriter::prune() const {
    std::error_code error;
    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == ".jsonl") {
            entries.push_back(entry);
        }
    }
    if (error) {
        return;
    }
    if (entries.size() <= kMaxFiles) {
        return;
    }
    // 新的在前，超出的删掉；mtime 相同的按文件名兜底，保证顺序稳定。
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        const auto left_time = std::filesystem::last_write_time(left.path());
        const auto right_time = std::filesystem::last_write_time(right.path());
        if (left_time != right_time) {
            return left_time > right_time;
        }
        return left.path().filename().string() > right.path().filename().string();
    });
    for (std::size_t index = kMaxFiles; index < entries.size(); ++index) {
        std::filesystem::remove(entries[index].path(), error);
        if (error) {
            spdlog::warn("慢请求 dump 清理失败 file={} error={}", entries[index].path().string(),
                         error.message());
            return;
        }
    }
}

} // namespace llmopt
