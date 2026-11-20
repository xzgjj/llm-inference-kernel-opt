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

struct RunnerInput {
    std::vector<std::int32_t> token_ids;
    std::vector<std::int32_t> pos_ids;
    std::vector<RequestId> seq_ids;
};

struct RunnerStepOutput {
    std::vector<RequestId> req_ids;
    std::vector<SampledToken> sampled;
    std::vector<std::int32_t> num_nans;
    ErrCode err = ErrCode::Ok;
};

struct Completion {
    RequestId seq_id;
    // 内部序号：同一 req_id 被复用或请求已收尾时，用它丢弃过期完成事件。
    std::int32_t seq_serial = -1;
    std::vector<SampledToken> sampled;
    ErrCode err = ErrCode::Ok;
};

} // namespace llmopt

#endif // LLMOPT_BATCH_HPP
