// runner_stub.cpp — 确定性假后端：多序列 n_past 记账 + 前缀借用校验，供无 GPU 单测与 CI。
// 不模拟真实语义，只保证同种子逐 token 可复现、边界与真后端一致。
#include "llmopt/runner.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace llmopt {
namespace {

// 测试钩子：仅 stub 后端读取 LLMOPT_TEST_STUB_DELAY_MS，用来稳定复现准入/背压竞态。
// 生产后端不经过这里；默认 0，不改变任何现有行为。
std::int32_t stub_step_delay_ms() {
    const char* text = std::getenv("LLMOPT_TEST_STUB_DELAY_MS");
    if (text == nullptr) {
        return 0;
    }
    const int value = std::atoi(text);
    if (value <= 0) {
        return 0;
    }
    return value > 1000 ? 1000 : value;
}

// FNV-1a：把 prompt token 序列压成稳定哈希，作为缺省种子。
std::uint64_t hash_tokens(const std::vector<std::int32_t>& ids) {
    std::uint64_t value = 1469598103934665603ULL;
    for (std::int32_t id : ids) {
        value ^= static_cast<std::uint64_t>(id + 1);
        value *= 1099511628211ULL;
    }
    return value;
}

// 假后端单序列状态：已写 KV 的 token 数与派生种子；留置序列保留 n_past。
struct StubState {
    std::int32_t n_past = 0;
    std::uint64_t seed = 0;
    bool retained = false;
};

} // namespace

// 假后端实现：分词按 UTF-8 字节，采样按 (seed, step) 派生可打印 ASCII。
class StubRunner final : public ModelRunner {
public:
    // 记录配置；n_ctx 由 --ctx-tokens 决定，供 HTTP 准入复用。
    ErrCode load_model(const ModelConfig& config) override {
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

    // 执行一步：按步骤投影推进 n_past；前缀借用必须存在 donor，否则整批失败。
    RunnerStepOutput decode_step(const Batch& batch) override {
        if (const std::int32_t delay = stub_step_delay_ms(); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        RunnerStepOutput output;
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t new_index = 0;
        std::size_t cached_index = 0;
        for (const SequenceStep& step : batch.steps) {
            if (step.phase == Phase::Decode) {
                if (cached_index >= batch.cached_reqs.size()) {
                    return failed(output, make_error(ErrCode::ExecutionFailed, "decode 数据缺失",
                                                     "op=decode_step cached_index=" +
                                                             std::to_string(cached_index)));
                }
                const CachedRequestData& data = batch.cached_reqs[cached_index++];
                StubState& state = states_[data.req_id];
                state.n_past = step.pos_end;
                emit(state, output);
                continue;
            }
            if (new_index >= batch.new_reqs.size()) {
                return failed(output,
                              make_error(ErrCode::ExecutionFailed, "prefill 数据缺失",
                                         "op=decode_step new_index=" + std::to_string(new_index)));
            }
            const NewRequestData& data = batch.new_reqs[new_index++];
            auto donor = states_.find(data.prefix_source);
            if (data.prefix_tokens > 0 && (data.prefix_source.empty() || donor == states_.end())) {
                return failed(output,
                              make_error(ErrCode::ExecutionFailed, "前缀 donor 不存在",
                                         "op=prefix_borrow source=" + data.prefix_source +
                                                 " tokens=" + std::to_string(data.prefix_tokens)));
            }
            StubState& state = states_[data.req_id];
            state.n_past = step.pos_end;
            // 非末块 chunk 只推进 KV，不采样；末块才产出首个 token。
            if (step.needs_sampling) {
                emit(state, output);
            }
        }
        return output;
    }

    // 释放序列：keep_tokens>0 时把 n_past 截到前缀长度并保留为 donor。
    void release_sequence(const RequestId& req_id, std::int32_t keep_tokens = 0) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(req_id);
        if (it == states_.end()) {
            return;
        }
        if (keep_tokens > 0) {
            it->second.n_past = keep_tokens;
            it->second.retained = true;
            return;
        }
        states_.erase(it);
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

    // 对账锚点：返回后端已写入的最大位置。
    std::int32_t seq_pos_max(const RequestId& req_id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(req_id);
        if (it == states_.end() || it->second.n_past <= 0) {
            return -1;
        }
        return it->second.n_past - 1;
    }

    // 假后端没有物理 cell 限制，可直接从运行中的序列借前缀。
    bool supports_live_prefix() const noexcept override {
        return true;
    }

    // stub 用 token 数组模拟 KV，前缀共享是纯内存拷贝，不存在 stream 约束。
    bool supports_prefix_share() const noexcept override {
        return true;
    }

    // 后端名进入启动摘要，避免把假后端误当真实推理。
    const char* backend_name() const noexcept override {
        return "stub";
    }

private:
    // 整批失败：清空已累积结果，避免按步回投时把半批 token 当成成功。
    static RunnerStepOutput failed(RunnerStepOutput output, Error error) {
        output.req_ids.clear();
        output.sampled.clear();
        output.num_nans.clear();
        output.error = std::move(error);
        return output;
    }

    // 采样公式：种子 ⊕ 步数 → 可打印 ASCII；同种子同序列完全可复现。
    static void emit(StubState& state, RunnerStepOutput& output) {
        const std::uint64_t mixed = state.seed + static_cast<std::uint64_t>(state.n_past) * 131ULL;
        SampledToken token;
        token.token_id = 256 + 32 + static_cast<std::int32_t>(mixed % 94ULL);
        token.logprob = -0.01F;
        output.sampled.push_back(token);
        output.num_nans.push_back(0);
    }

    std::int32_t n_ctx_ = 2048;
    mutable std::mutex mutex_;
    std::map<RequestId, StubState> states_;
};

// 工厂：main 在 backend=stub 且显式放行时调用。
std::unique_ptr<ModelRunner> make_stub_runner() {
    return std::make_unique<StubRunner>();
}

} // namespace llmopt
