// kv.cpp — 定长逻辑块分配；只记账 BlockRef，不分配物理 cell。
#include "llmopt/kv.hpp"

namespace llmopt {

KVBlockManager::KVBlockManager(KVConfig config) : config_(config) {
    if (config_.block_tokens <= 0) {
        config_.block_tokens = 16;
    }
    if (config_.num_blocks < 0) {
        config_.num_blocks = 0;
    }
    free_list_.reserve(static_cast<std::size_t>(config_.num_blocks));
    for (std::int32_t id = config_.num_blocks - 1; id >= 0; --id) {
        free_list_.push_back(id);
    }
}

std::optional<std::vector<BlockRef>> KVBlockManager::acquire(Sequence& seq, std::int32_t n_tokens,
                                                             std::int32_t lookahead) {
    if (n_tokens < 0 || lookahead < 0) {
        return std::nullopt;
    }
    const std::int32_t total = n_tokens + lookahead;
    const std::int32_t required = (total + config_.block_tokens - 1) / config_.block_tokens;
    const std::int32_t additional = required - static_cast<std::int32_t>(seq.block_table.size());
    if (additional <= 0) {
        return std::vector<BlockRef>{};
    }
    if (static_cast<std::int32_t>(free_list_.size()) < additional) {
        return std::nullopt;
    }
    std::vector<BlockRef> added;
    added.reserve(static_cast<std::size_t>(additional));
    for (std::int32_t i = 0; i < additional; ++i) {
        BlockRef ref{free_list_.back()};
        free_list_.pop_back();
        added.push_back(ref);
        seq.block_table.push_back(ref);
    }
    return added;
}

void KVBlockManager::free(Sequence& seq) {
    for (const BlockRef& ref : seq.block_table) {
        free_list_.push_back(ref.block_id);
    }
    seq.block_table.clear();
}

void KVBlockManager::touch(const std::vector<BlockRef>& blocks) noexcept {
    // 阶段 1 无共享块与 LRU；保留接口，阶段 2 在这里记访问序。
    (void)blocks;
}

std::int32_t KVBlockManager::num_free_blocks() const noexcept {
    return static_cast<std::int32_t>(free_list_.size());
}

std::int32_t KVBlockManager::num_total_blocks() const noexcept {
    return config_.num_blocks;
}

std::int32_t KVBlockManager::block_capacity_tokens() const noexcept {
    return config_.num_blocks * config_.block_tokens;
}

} // namespace llmopt
