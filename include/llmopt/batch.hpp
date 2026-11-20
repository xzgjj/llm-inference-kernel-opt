// batch.hpp — C-05 批次值对象与 C-11 SequenceStep 投影；跨线程只传值。
#ifndef LLMOPT_BATCH_HPP
#define LLMOPT_BATCH_HPP

#include "llmopt/common.hpp"
#include "llmopt/sequence.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace llmopt {

struct NewRequestData {
    RequestId req_id;
    std::vector<std::int32_t> prefill_token_ids;
    std::vector<BlockRef> block_ids;
    SamplingParams sampling;
    // C-P2-13：命中的前缀从该序列零拷贝借用，prefill 只补 prefix_tokens 之后的部分。
    RequestId prefix_source;
    std::int32_t prefix_tokens = 0;
};

struct CachedRequestData {
    RequestId req_id;
    std::vector<std::int32_t> new_token_ids;
    std::vector<BlockRef> new_block_ids;
};

enum class Phase : std::uint8_t { PrefillChunk, PrefillLast, Decode };

struct KVPen {
    std::int32_t seq_id = -1;
    std::int32_t slot = -1;
};

struct Route {
    RequestId req_id;
    std::int32_t output_slot = 0;
};

struct SequenceStep {
    RequestId seq_id;
    Phase phase = Phase::Decode;
    std::int32_t pos_begin = 0;
    std::int32_t pos_end = 0;
    std::int32_t n_tokens = 0;
    KVPen kv;
    bool needs_sampling = false;
    std::int32_t sample_pos = 0;
    Route route;
};

struct Batch {
    std::vector<NewRequestData> new_reqs;
    std::vector<CachedRequestData> cached_reqs;
    std::vector<RequestId> preempted_ids;
    std::vector<RequestId> finished_ids;
    std::vector<SequenceStep> steps;
    std::int32_t total_tokens = 0;
};

// 批次构成快照：把 prefill/decode 的 token 与序列数从 steps 中一次抽出，供指标与日志复用。
struct BatchComposition {
    std::int32_t prefill_tokens = 0;
    std::int32_t decode_tokens = 0;
    std::int32_t prefill_seqs = 0;
    std::int32_t decode_seqs = 0;
    [[nodiscard]] std::int32_t steps() const noexcept {
        return prefill_seqs + decode_seqs;
    }
};

[[nodiscard]] BatchComposition batch_composition(const Batch& batch) noexcept;

struct RunnerInput {
    std::vector<std::int32_t> token_ids;
    std::vector<std::int32_t> pos_ids;
    std::vector<RequestId> seq_ids;
};

struct RunnerStepOutput {
    std::vector<RequestId> req_ids;
    std::vector<SampledToken> sampled;
    std::vector<std::int32_t> num_nans;
    // 失败原因随结果一起回投：整批失败时 sampled 为空，error 必为非 Ok。
    Error error;
};

struct Completion {
    RequestId seq_id;
    // 内部序号：同一 req_id 被复用或请求已收尾时，用它丢弃过期完成事件。
    std::int32_t seq_serial = -1;
    // 本步写入后的 KV token 总数，回写与对账都以它为准。
    std::int32_t computed_tokens = 0;
    std::vector<SampledToken> sampled;
    // 非末块 chunk 只推进预填、不采样；false 表示 sampled 允许为空。
    bool sampled_expected = true;
    // 后端失败原因；Ok 表示本步成功。回投时整批共用同一份 Error。
    Error error;
};

} // namespace llmopt

#endif // LLMOPT_BATCH_HPP
