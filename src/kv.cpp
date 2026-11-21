// kv.cpp — 逻辑块表实现：ref 计数、满块哈希链、donor 留置与 LRU 淘汰。
// 这里不分配显存；块 id 只作为后端位置的逻辑锚点，物理 cell 由 find_slot 决定。
#include "llmopt/kv.hpp"

#include <algorithm>

namespace llmopt {
namespace {

constexpr std::uint64_t kHashPrime = 1099511628211ULL;

// 末尾标识：让空 parent 与真实哈希区分开。
constexpr std::uint64_t kHashSalt = 0x9E3779B97F4A7C15ULL;

} // namespace

KVBlockManager::KVBlockManager(KVConfig config) : config_(config) {
    if (config_.block_tokens <= 0) {
        config_.block_tokens = 16;
    }
    if (config_.num_blocks < 0) {
        config_.num_blocks = 0;
    }
    if (config_.max_donors < 0) {
        config_.max_donors = 0;
    }
    // 前缀缓存至少要能留下一个 donor，否则 retain 的返回 token 与后端保留不一致。
    if (config_.enable_prefix_cache && config_.max_donors < 1) {
        config_.max_donors = 1;
    }
    blocks_.assign(static_cast<std::size_t>(config_.num_blocks), BlockState{});
    free_list_.reserve(static_cast<std::size_t>(config_.num_blocks));
    for (std::int32_t id = 0; id < config_.num_blocks; ++id) {
        free_list_.push_back(id);
        blocks_[static_cast<std::size_t>(id)].listed_free = true;
    }
}

std::int32_t KVBlockManager::blocks_for(std::int32_t tokens) const noexcept {
    return (tokens + config_.block_tokens - 1) / config_.block_tokens;
}

std::uint64_t KVBlockManager::chain_hash(std::uint64_t parent,
                                         const std::vector<std::int32_t>& tokens,
                                         std::int32_t index) const noexcept {
    const std::int32_t begin = index * config_.block_tokens;
    std::uint64_t value = parent * kHashPrime ^ kHashSalt;
    for (std::int32_t offset = 0; offset < config_.block_tokens; ++offset) {
        value ^= static_cast<std::uint64_t>(tokens[static_cast<std::size_t>(begin) +
                                                   static_cast<std::size_t>(offset)]) +
                 1ULL;
        value *= kHashPrime;
    }
    return value == 0 ? 1 : value;
}

void KVBlockManager::push_free(std::int32_t id) {
    BlockState& block = blocks_[static_cast<std::size_t>(id)];
    block.listed_free = true;
    free_list_.push_back(id);
}

void KVBlockManager::forget_hash(std::int32_t id) {
    BlockState& block = blocks_[static_cast<std::size_t>(id)];
    if (block.hash == 0) {
        return;
    }
    auto it = hash_index_.find(block.hash);
    if (it != hash_index_.end() && it->second == id) {
        hash_index_.erase(it);
    }
    block.hash = 0;
    block.verified = false;
}

void KVBlockManager::register_hash(std::int32_t id, std::uint64_t hash) {
    BlockState& block = blocks_[static_cast<std::size_t>(id)];
    block.hash = hash;
    auto it = hash_index_.find(hash);
    if (it == hash_index_.end()) {
        hash_index_.emplace(hash, id);
    }
}

void KVBlockManager::release_block(std::int32_t id) {
    BlockState& block = blocks_[static_cast<std::size_t>(id)];
    if (block.ref_cnt <= 0) {
        return;
    }
    block.ref_cnt -= 1;
    ref_total_ -= 1;
    if (block.ref_cnt == 0) {
        block.owner.clear();
        forget_hash(id);
        push_free(id);
    }
}

void KVBlockManager::adopt_block(std::int32_t id, const std::string& owner) {
    BlockState& block = blocks_[static_cast<std::size_t>(id)];
    block.owner = owner;
    if (donors_.find(owner) == donors_.end()) {
        donor_order_.push_back(owner);
    }
    donors_[owner].push_back(id);
}

void KVBlockManager::evict_donor(const BackendId& donor) {
    auto it = donors_.find(donor);
    if (it == donors_.end()) {
        return;
    }
    // donor 可能正是 donor_order_ 里的元素（ensure_free 传 front()），下面 erase 会让它悬空，
    // 后续 push_back 必须用副本；ASan 下这里的 heap-use-after-free 是真实缺陷，不是误报。
    const BackendId dropped_id = donor;
    for (std::int32_t id : it->second) {
        BlockState& block = blocks_[static_cast<std::size_t>(id)];
        if (block.owner == donor) {
            block.owner.clear();
            forget_hash(id);
        }
        release_block(id);
    }
    donors_.erase(it);
    donor_order_.erase(std::remove(donor_order_.begin(), donor_order_.end(), donor),
                       donor_order_.end());
    dropped_.push_back(dropped_id);
}

bool KVBlockManager::ensure_free(std::int32_t need) {
    while (static_cast<std::int32_t>(free_list_.size()) < need && !donor_order_.empty()) {
        evict_donor(donor_order_.front());
    }
    return static_cast<std::int32_t>(free_list_.size()) >= need;
}

AllocResult KVBlockManager::acquire(Sequence& seq, std::int32_t n_tokens, std::int32_t lookahead,
                                    bool reserve_watermark) {
    AllocResult result;
    if (n_tokens < 0 || lookahead < 0) {
        result.reason = AllocFailReason::ContextOverflow;
        return result;
    }
    if (n_tokens > block_capacity_tokens() || lookahead > block_capacity_tokens() - n_tokens) {
        result.reason = AllocFailReason::ContextOverflow;
        return result;
    }
    const std::int32_t required = blocks_for(n_tokens + lookahead);
    const std::int32_t additional = required - static_cast<std::int32_t>(seq.block_table.size());
    if (additional <= 0) {
        return result;
    }
    const std::int32_t need = additional + (reserve_watermark ? watermark_blocks() : 0);
    if (!ensure_free(need)) {
        const std::int32_t free_blocks = static_cast<std::int32_t>(free_list_.size());
        result.reason = free_blocks >= additional ? AllocFailReason::BelowWatermark
                                                  : AllocFailReason::NoFreeBlock;
        return result;
    }
    for (std::int32_t i = 0; i < additional; ++i) {
        const std::int32_t id = free_list_.back();
        free_list_.pop_back();
        BlockState& block = blocks_[static_cast<std::size_t>(id)];
        block.listed_free = false;
        block.ref_cnt = 1;
        block.hash = 0;
        block.verified = false;
        block.owner.clear();
        ref_total_ += 1;
        seq.block_table.push_back(BlockRef{id});
        result.added.push_back(BlockRef{id});
        const std::int32_t index = static_cast<std::int32_t>(seq.block_table.size()) - 1;
        const bool covers = config_.enable_prefix_cache &&
                            (index + 1) * config_.block_tokens <= seq.prompt_len &&
                            static_cast<std::int32_t>(seq.prompt_token_ids.size()) >=
                                    (index + 1) * config_.block_tokens;
        if (!covers) {
            continue;
        }
        const std::uint64_t parent =
                index == 0 ? 0
                           : blocks_[static_cast<std::size_t>(
                                             seq.block_table[static_cast<std::size_t>(index - 1)]
                                                     .block_id)]
                                     .hash;
        register_hash(id, chain_hash(parent, seq.prompt_token_ids, index));
    }
    return result;
}

PrefixHit KVBlockManager::lookup_prefix(const Sequence& seq) const {
    PrefixHit hit;
    if (!config_.enable_prefix_cache || seq.prompt_len < config_.block_tokens + 1) {
        return hit;
    }
    const std::int32_t max_blocks = (seq.prompt_len - 1) / config_.block_tokens;
    std::uint64_t parent = 0;
    for (std::int32_t index = 0; index < max_blocks; ++index) {
        const std::uint64_t hash = chain_hash(parent, seq.prompt_token_ids, index);
        auto it = hash_index_.find(hash);
        if (it == hash_index_.end()) {
            break;
        }
        const BlockState& block = blocks_[static_cast<std::size_t>(it->second)];
        if (block.hash != hash || !block.verified || block.ref_cnt <= 0) {
            break;
        }
        if (hit.blocks.empty() && !block.owner.empty()) {
            hit.donor = block.owner;
        }
        hit.blocks.push_back(BlockRef{it->second});
        hit.hit_tokens += config_.block_tokens;
        parent = hash;
    }
    return hit;
}

std::int32_t KVBlockManager::attach_prefix(Sequence& seq, const PrefixHit& hit) {
    if (hit.blocks.empty() || hit.hit_tokens <= 0) {
        return 0;
    }
    for (const BlockRef& ref : hit.blocks) {
        BlockState& block = blocks_[static_cast<std::size_t>(ref.block_id)];
        if (!block.verified || block.ref_cnt <= 0) {
            return 0;
        }
        block.ref_cnt += 1;
        ref_total_ += 1;
        seq.block_table.push_back(ref);
    }
    return hit.hit_tokens;
}

void KVBlockManager::free(Sequence& seq) {
    for (const BlockRef& ref : seq.block_table) {
        BlockState& block = blocks_[static_cast<std::size_t>(ref.block_id)];
        if (block.ref_cnt > 1 && block.owner == seq.backend_id) {
            block.owner.clear();
            forget_hash(ref.block_id);
        }
        release_block(ref.block_id);
    }
    seq.block_table.clear();
}

std::int32_t KVBlockManager::releasable_blocks(const Sequence& seq) const {
    std::int32_t count = 0;
    for (const BlockRef& ref : seq.block_table) {
        const BlockState& block = blocks_[static_cast<std::size_t>(ref.block_id)];
        if (block.ref_cnt == 1) {
            ++count;
        }
    }
    return count;
}

std::int32_t KVBlockManager::retain(Sequence& seq) {
    if (!config_.enable_prefix_cache) {
        free(seq);
        return 0;
    }
    const std::int32_t full_blocks = seq.prompt_len / config_.block_tokens;
    std::int32_t keep_tokens = 0;
    std::int32_t index = 0;
    for (const BlockRef& ref : seq.block_table) {
        BlockState& block = blocks_[static_cast<std::size_t>(ref.block_id)];
        const bool cacheable = index < full_blocks && block.hash != 0 && block.verified;
        if (!cacheable) {
            release_block(ref.block_id);
            ++index;
            continue;
        }
        if (block.owner == seq.backend_id) {
            ++index;
            keep_tokens += config_.block_tokens;
            continue;
        }
        if (block.owner.empty()) {
            adopt_block(ref.block_id, seq.backend_id);
            keep_tokens += config_.block_tokens;
        } else {
            // 内容已由别的 donor 持有，本序列无需再保留一份。
            release_block(ref.block_id);
        }
        ++index;
    }
    seq.block_table.clear();
    while (config_.max_donors >= 0 &&
           static_cast<std::int32_t>(donors_.size()) > config_.max_donors &&
           !donor_order_.empty()) {
        evict_donor(donor_order_.front());
    }
    return keep_tokens;
}

void KVBlockManager::mark_written(const Sequence& seq, std::int32_t computed_tokens) {
    std::int32_t index = 0;
    for (const BlockRef& ref : seq.block_table) {
        BlockState& block = blocks_[static_cast<std::size_t>(ref.block_id)];
        const std::int32_t block_end = (index + 1) * config_.block_tokens;
        if (block.hash != 0 && block_end <= computed_tokens) {
            block.verified = true;
            register_hash(ref.block_id, block.hash);
        }
        ++index;
    }
}

std::vector<BackendId> KVBlockManager::flush_cache() {
    while (!donor_order_.empty()) {
        evict_donor(donor_order_.front());
    }
    for (std::int32_t id = 0; id < config_.num_blocks; ++id) {
        forget_hash(id);
    }
    return take_dropped_donors();
}

std::vector<BackendId> KVBlockManager::take_dropped_donors() {
    std::vector<BackendId> out;
    out.swap(dropped_);
    return out;
}

std::int32_t KVBlockManager::num_free_blocks() const noexcept {
    return static_cast<std::int32_t>(free_list_.size());
}

std::int32_t KVBlockManager::num_total_blocks() const noexcept {
    return config_.num_blocks;
}

std::int32_t KVBlockManager::blocks_for_tokens(std::int32_t tokens) const noexcept {
    if (tokens <= 0) {
        return 0;
    }
    return blocks_for(tokens);
}

std::int32_t KVBlockManager::block_capacity_tokens() const noexcept {
    return config_.num_blocks * config_.block_tokens;
}

std::int32_t KVBlockManager::watermark_blocks() const noexcept {
    if (config_.watermark_ratio <= 0.0) {
        return 0;
    }
    const double blocks = static_cast<double>(config_.num_blocks) * config_.watermark_ratio;
    return static_cast<std::int32_t>(blocks) + 1;
}

KVUsage KVBlockManager::usage() const {
    KVUsage usage;
    usage.total_blocks = config_.num_blocks;
    usage.free_blocks = static_cast<std::int32_t>(free_list_.size());
    usage.watermark_blocks = watermark_blocks();
    for (const BlockState& block : blocks_) {
        if (block.ref_cnt <= 0) {
            continue;
        }
        ++usage.used_blocks;
        if (block.ref_cnt > 1) {
            ++usage.shared_blocks;
        }
        if (!block.owner.empty()) {
            ++usage.cached_blocks;
        }
    }
    return usage;
}

bool KVBlockManager::check_invariants(std::string& why) const {
    std::int32_t ref_total = 0;
    std::int32_t free_count = 0;
    for (std::int32_t id = 0; id < config_.num_blocks; ++id) {
        const BlockState& block = blocks_[static_cast<std::size_t>(id)];
        ref_total += block.ref_cnt;
        if (block.ref_cnt < 0) {
            why = "block " + std::to_string(id) + " ref<0";
            return false;
        }
        if (block.listed_free) {
            ++free_count;
            if (block.ref_cnt != 0) {
                why = "free block " + std::to_string(id) + " still referenced";
                return false;
            }
        } else if (block.ref_cnt == 0) {
            why = "used block " + std::to_string(id) + " has zero ref";
            return false;
        }
    }
    if (free_count != static_cast<std::int32_t>(free_list_.size())) {
        why = "free list size mismatch";
        return false;
    }
    if (ref_total != ref_total_) {
        why = "ref total " + std::to_string(ref_total_) + " != " + std::to_string(ref_total);
        return false;
    }
    for (const auto& [hash, id] : hash_index_) {
        const BlockState& block = blocks_[static_cast<std::size_t>(id)];
        if (block.hash != hash || block.ref_cnt <= 0) {
            why = "hash index points to invalid block " + std::to_string(id);
            return false;
        }
    }
    for (const auto& [donor, ids] : donors_) {
        for (std::int32_t id : ids) {
            if (blocks_[static_cast<std::size_t>(id)].owner != donor) {
                why = "donor " + donor + " lost block " + std::to_string(id);
                return false;
            }
        }
    }
    if (donors_.size() != donor_order_.size()) {
        why = "donor order size mismatch";
        return false;
    }
    return true;
}

} // namespace llmopt
