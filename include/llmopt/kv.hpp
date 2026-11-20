// kv.hpp — C-06/C-P2-13 逻辑块表：引用计数、块哈希前缀命中、watermark 与缓存留置。
// 块表是唯一分配决策者；物理 cell 仍由后端 find_slot 决定，这里只做逻辑映射与对账锚点。
#ifndef LLMOPT_KV_HPP
#define LLMOPT_KV_HPP

#include "llmopt/sequence.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace llmopt {

// acquire 失败原因；调度据此决定留队、拒绝或终态错误。
enum class AllocFailReason : std::uint8_t {
    None = 0,
    NoFreeBlock,
    BelowWatermark,
    ContextOverflow,
    SequenceTooLong,
};

struct KVConfig {
    std::int32_t block_tokens = 16;
    std::int32_t num_blocks = 256;
    double watermark_ratio = 0.05;
    bool enable_prefix_cache = true;
    std::int32_t max_donors = 4;
};

// 前缀命中只返回连续满块；hit_tokens 一定按 block_tokens 对齐且小于 prompt_len。
struct PrefixHit {
    std::vector<BlockRef> blocks;
    std::int32_t hit_tokens = 0;
    // 提供这些块 KV 的序列：运行中的 seq_id 或留置 donor；空表示不可物理借用。
    RequestId donor;
};

struct AllocResult {
    std::vector<BlockRef> added;
    AllocFailReason reason = AllocFailReason::None;

    bool ok() const noexcept {
        return reason == AllocFailReason::None;
    }
};

struct KVUsage {
    std::int32_t total_blocks = 0;
    std::int32_t used_blocks = 0;
    std::int32_t cached_blocks = 0;
    std::int32_t shared_blocks = 0;
    std::int32_t free_blocks = 0;
    std::int32_t watermark_blocks = 0;
};

// 逻辑块表：ref_cnt 区分独占/共享；满块哈希链支持前缀零拷贝复用。
class KVBlockManager {
public:
    explicit KVBlockManager(KVConfig config);

    // 补足块表到 ceil((n_tokens+lookahead)/block)；不足时返回原因且不改动序列。
    // reserve_watermark 只用于准入，decode 增量扩展不预留。
    AllocResult acquire(Sequence& seq, std::int32_t n_tokens, std::int32_t lookahead = 0,
                        bool reserve_watermark = false);
    // 查询前缀命中，不改变任何状态；调用方用 attach_prefix 挂载。
    PrefixHit lookup_prefix(const Sequence& seq) const;
    // 把命中块挂到序列并 ref++；返回实际命中 token 数。
    std::int32_t attach_prefix(Sequence& seq, const PrefixHit& hit);
    // 完全释放序列：逐块 ref--，归零回空闲链。
    void free(Sequence& seq);
    // 结束序列时留置满块内容：返回保留 token 数（0 表示未留置），seq.block_table 清空。
    std::int32_t retain(Sequence& seq);
    // 一步写出后确认哈希可用，避免未算完的块被命中。
    void mark_written(const Sequence& seq, std::int32_t computed_tokens);
    // 释放全部留置缓存（关停/测试收口用），返回需后端释放的序列 id。
    std::vector<RequestId> flush_cache();
    // 取走因淘汰而需后端释放的 cache donor。
    std::vector<RequestId> take_dropped_donors();

    std::int32_t num_free_blocks() const noexcept;
    std::int32_t num_total_blocks() const noexcept;
    std::int32_t block_capacity_tokens() const noexcept;
    std::int32_t watermark_blocks() const noexcept;
    KVUsage usage() const;

    // 对账：ref 总和、空闲链、哈希索引三者必须自洽。
    bool check_invariants(std::string& why) const;

private:
    struct BlockState {
        std::int32_t ref_cnt = 0;
        std::uint64_t hash = 0;
        bool verified = false;
        RequestId owner;
        bool listed_free = true;
    };

    std::int32_t blocks_for(std::int32_t tokens) const noexcept;
    std::uint64_t chain_hash(std::uint64_t parent, const std::vector<std::int32_t>& tokens,
                             std::int32_t index) const noexcept;
    void release_block(std::int32_t id);
    void push_free(std::int32_t id);
    bool ensure_free(std::int32_t need);
    void evict_donor(const RequestId& donor);
    void adopt_block(std::int32_t id, const std::string& owner);
    void forget_hash(std::int32_t id);
    void register_hash(std::int32_t id, std::uint64_t hash);

    KVConfig config_;
    std::vector<BlockState> blocks_;
    std::vector<std::int32_t> free_list_;
    std::unordered_map<std::uint64_t, std::int32_t> hash_index_;
    std::map<RequestId, std::vector<std::int32_t>> donors_;
    std::vector<RequestId> donor_order_;
    std::vector<RequestId> dropped_;
    std::int32_t ref_total_ = 0;
};

} // namespace llmopt

#endif // LLMOPT_KV_HPP
