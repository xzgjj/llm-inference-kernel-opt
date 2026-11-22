// slow_dump.hpp — D2-10 慢请求落盘：一条请求一个 JSONL，只写计数与哈希前缀。
//
// 脱敏边界（写死）：不写 prompt 原文、不写生成文本、不写 key；
// prompt 只落 token 数与 token 序列的 sha256 前 8 位，便于"同 prompt"聚类而不可还原。
#ifndef LLMOPT_SLOW_DUMP_HPP
#define LLMOPT_SLOW_DUMP_HPP

#include "llmopt/common.hpp"

#include <cstdint>
#include <string>

namespace llmopt {

// 四段计时与生命周期计数；全为整数毫秒/个数，便于 grep 与 jq。
struct SlowRequestRecord {
    std::string req_id;
    std::string tenant = "default";
    std::int32_t prompt_tokens = 0;
    std::int32_t output_tokens = 0;
    std::int32_t preemptions = 0;
    FinishReason finish = FinishReason::Stopped;
    std::int64_t queue_wait_ms = 0;  // 入队 → 准入
    std::int64_t prefill_ms = 0;     // 准入 → prompt 计算完成
    std::int64_t first_token_ms = 0; // prompt 完成 → 首 token 回投
    std::int64_t total_ms = 0;       // 入队 → 收尾
    std::string prompt_sha8;         // prompt token 序列 sha256 前 8 位
};

// 目录约定：<dump_dir>/<req_id>.jsonl；保留最近 20 份，单文件上限 8 MB。
class SlowDumpWriter {
public:
    static constexpr std::size_t kMaxFiles = 20;
    static constexpr std::int64_t kMaxFileBytes = 8 * 1024 * 1024;

    // threshold_ns<=0 或 dir 为空 = 关闭：maybe_dump 只判不写。
    SlowDumpWriter(std::string dir, std::int64_t threshold_ns);
    SlowDumpWriter() = default;

    [[nodiscard]] bool enabled() const noexcept {
        return threshold_ns_ > 0 && !dir_.empty();
    }
    [[nodiscard]] std::int64_t threshold_ns() const noexcept {
        return threshold_ns_;
    }
    [[nodiscard]] const std::string& dir() const noexcept {
        return dir_;
    }

    // 超过阈值才落盘；返回是否真的写了文件。写失败只记 warning，不影响请求收尾。
    bool maybe_dump(const SlowRequestRecord& record) const;

private:
    void prune() const;

    std::string dir_;
    std::int64_t threshold_ns_ = 0;
};

// 供调用方与测试共用：prompt token 序列 → 前 8 位 sha256（不含明文）。
[[nodiscard]] std::string prompt_fingerprint(const std::int32_t* ids, std::size_t count);

} // namespace llmopt

#endif // LLMOPT_SLOW_DUMP_HPP
