// kv.hpp — C-06 逻辑块分配：只产 BlockRef，不碰显存/cell。
#ifndef LLMOPT_KV_HPP
#define LLMOPT_KV_HPP

#include "llmopt/sequence.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace llmopt {

struct KVConfig {
    std::int32_t block_tokens = 16;
    std::int32_t num_blocks = 256;
};

// 块表是唯一分配决策者；物理 cell 由后端映射，禁止反向回写。
class KVBlockManager {
public:
    explicit KVBlockManager(KVConfig config);

    // 按 n_tokens + lookahead 补足块表；不足返回 nullopt 且不改动序列。
    std::optional<std::vector<BlockRef>> acquire(Sequence& seq, std::int32_t n_tokens,
                                                 std::int32_t lookahead = 0);
    void free(Sequence& seq);
    void touch(const std::vector<BlockRef>& blocks) noexcept;

    std::int32_t num_free_blocks() const noexcept;
    std::int32_t num_total_blocks() const noexcept;
    std::int32_t block_capacity_tokens() const noexcept;

private:
    std::vector<std::int32_t> free_list_;
    KVConfig config_;
};

} // namespace llmopt

#endif // LLMOPT_KV_HPP
