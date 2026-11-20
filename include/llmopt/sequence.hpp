// sequence.hpp — C-04 序列四态、进度与块表；状态只由 SchedulerCore 写。
#ifndef LLMOPT_SEQUENCE_HPP
#define LLMOPT_SEQUENCE_HPP

#include "llmopt/common.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace llmopt {

struct BlockRef {
    std::int32_t block_id = -1;
};

enum class SeqStatus : std::uint8_t { Waiting, Running, Swapped, Finished };

// C-04：num_computed 是已写入 KV 的 token 数，num_output 是已采样数。
struct Sequence {
    std::int32_t id = -1;
    GroupId group_id;
    std::int32_t prompt_len = 0;
    std::int32_t num_computed = 0;
    std::int32_t num_output = 0;
    SeqStatus status = SeqStatus::Waiting;
    FinishReason finish = FinishReason::None;
    std::vector<std::int32_t> prompt_token_ids;
    std::vector<std::int32_t> output_token_ids;
    std::vector<BlockRef> block_table;
    SamplingParams sampling;
    std::atomic<bool> cancel_requested{false};
};

inline bool legal_transition(SeqStatus from, SeqStatus to) noexcept {
    if (from == SeqStatus::Finished) {
        return false;
    }
    if (from == SeqStatus::Waiting && to == SeqStatus::Running) {
        return true;
    }
    if (from == SeqStatus::Running && (to == SeqStatus::Finished || to == SeqStatus::Swapped)) {
        return true;
    }
    if (from == SeqStatus::Running && to == SeqStatus::Waiting) {
        return true; // preempt 后的恢复入口，阶段 1 不可达但语义保留
    }
    if (from == SeqStatus::Swapped && to == SeqStatus::Waiting) {
        return true;
    }
    return false;
}

} // namespace llmopt

#endif // LLMOPT_SEQUENCE_HPP
