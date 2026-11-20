// runner_stub.cpp — 确定性假后端：字节级分词 + 种子派生 token，供无 GPU 单测与 CI。
// 不模拟真实语义，只保证同种子逐 token 可复现、边界与真后端一致。
#include "llmopt/runner.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace llmopt {
namespace {

// FNV-1a：把 prompt token 序列压成稳定哈希，作为缺省种子。
std::uint64_t hash_tokens(const std::vector<std::int32_t>& ids) {
    std::uint64_t value = 1469598103934665603ULL;
    for (std::int32_t id : ids) {
        value ^= static_cast<std::uint64_t>(id + 1);
        value *= 1099511628211ULL;
    }
    return value;
}

// 假后端单序列状态：已写 KV 的 token 数与派生种子。
struct StubState {
    std::int32_t n_past = 0;
    std::uint64_t seed = 0;
};

} // namespace

// 假后端实现：分词按 UTF-8 字节，采样按 (seed, step) 派生可打印 ASCII。
class StubRunner final : public ModelRunner {
public:
    // 记录配置；n_ctx 由 --ctx-tokens 决定，供 HTTP 准入复用。
    ErrCode load_model(const ModelConfig& config) override {
        config_ = config;
        n_ctx_ = config.n_ctx;
        return ErrCode::Ok;
    }

    // 登记序列并重置 n_past；同一 req_id 重复登记视为覆盖。
    ErrCode add_sequence(const Sequence& seq) override {
        std::lock_guard<std::mutex> lock(mutex_);
        StubState state;
        state.seed = seq.sampling.seed >= 0 ? static_cast<std::uint64_t>(seq.sampling.seed)
                                            : hash_tokens(seq.prompt_token_ids);
        states_[seq.group_id] = state;
        return ErrCode::Ok;
    }

    // 执行一步：prefill 写入 n_past，decode 前进一格，随后派生一个 token。
    RunnerStepOutput decode_step(const Batch& batch) override {
        RunnerStepOutput output;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const NewRequestData& request : batch.new_reqs) {
            StubState& state = states_[request.req_id];
            state.n_past = static_cast<std::int32_t>(request.prefill_token_ids.size());
            output.req_ids.push_back(request.req_id);
            output.sampled.push_back(sample_token(state));
            output.num_nans.push_back(0);
        }
        for (const CachedRequestData& request : batch.cached_reqs) {
            StubState& state = states_[request.req_id];
            state.n_past += static_cast<std::int32_t>(request.new_token_ids.size());
            output.req_ids.push_back(request.req_id);
            output.sampled.push_back(sample_token(state));
            output.num_nans.push_back(0);
        }
        output.err = ErrCode::Ok;
        return output;
    }

    // 释放序列状态；真后端在这里清 KV，假后端只擦状态。
    void release_sequence(const RequestId& req_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.erase(req_id);
    }

    // 无模型资源可释放，保持接口对称。
    void release() override {}

    // 字节级分词：每个 UTF-8 字节一个 token（256+byte），保证往返无损。
    ErrCode tokenize(const std::string& text, std::vector<std::int32_t>& ids) override {
        ids.clear();
        ids.reserve(text.size());
        for (unsigned char byte : text) {
            ids.push_back(256 + static_cast<std::int32_t>(byte));
        }
        return ErrCode::Ok;
    }

    // 逆变换：只反解 256..511 的字节 token，其余返回空串。
    std::string detokenize(std::int32_t token_id) override {
        if (token_id < 256 || token_id > 511) {
            return {};
        }
        return std::string(1, static_cast<char>(token_id - 256));
    }

    // 假后端不产生 EOS，结束只由 max_tokens 或取消触发。
    bool is_eos(std::int32_t token_id) const override {
        (void)token_id;
        return false;
    }

    // 上下文长度直接取配置值。
    std::int32_t n_ctx() const noexcept override {
        return n_ctx_;
    }

    // 后端名进入启动摘要，避免把假后端误当真实推理。
    const char* backend_name() const noexcept override {
        return "stub";
    }

private:
    // 采样公式：种子 ⊕ 步数 → 可打印 ASCII；同种子同序列完全可复现。
    static SampledToken sample_token(StubState& state) {
        const std::uint64_t mixed = state.seed + static_cast<std::uint64_t>(state.n_past) * 131ULL;
        SampledToken token;
        token.token_id = 256 + 32 + static_cast<std::int32_t>(mixed % 94ULL);
        token.logprob = -0.01F;
        return token;
    }

    ModelConfig config_;
    std::int32_t n_ctx_ = 2048;
    std::mutex mutex_;
    std::map<RequestId, StubState> states_;
};

// 工厂：main 在 backend=stub 且显式放行时调用。
std::unique_ptr<ModelRunner> make_stub_runner() {
    return std::make_unique<StubRunner>();
}

} // namespace llmopt
