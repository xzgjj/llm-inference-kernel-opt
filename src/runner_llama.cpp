// runner_llama.cpp — C-07 真实后端适配：llama.cpp 上下文/批/采样链全部收在本文件。
// 耦合点：batch 无公开 cell 字段、无 cell 查询 getter；阶段 1 单序列，释放时清整段 KV。
#include "llama.h"
#include "llmopt/runner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llmopt {
namespace {

// 每个序列一个采样链：惩罚 → top-k → top-p → min-p → 温度 → dist（已冻结顺序）。
struct LlamaSeqState {
    llama_sampler* sampler = nullptr;
    std::int32_t n_past = 0;
};

// 进程级后端初始化只做一次；重复调用无害。
void ensure_backend() {
    static std::once_flag once;
    std::call_once(once, [] { llama_backend_init(); });
}

// 把 (0,1] 的 top_p/min_p 转成 llama 需要的 min_keep=1，避免空候选。
llama_sampler* build_chain(const SamplingParams& sampling, std::int32_t n_vocab) {
    auto* chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(
            chain, llama_sampler_init_penalties(n_vocab, 64, sampling.repeat_penalty, 0.0F, 0.0F));
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(sampling.top_k));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(sampling.top_p, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_min_p(sampling.min_p, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(sampling.temperature));
    const std::uint32_t seed =
            sampling.seed >= 0 ? static_cast<std::uint32_t>(sampling.seed) : LLAMA_DEFAULT_SEED;
    llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
    return chain;
}

} // namespace

// 真后端：模型/上下文/批全部独占在执行线程，析构按反序释放。
class LlamaRunner final : public ModelRunner {
public:
    ~LlamaRunner() override {
        release();
    }

    // 加载模型并建上下文；n_ctx/n_batch/n_ubatch 来自 CLI，n_gpu_layers=0 即 CPU。
    ErrCode load_model(const ModelConfig& config) override {
        ensure_backend();
        config_ = config;
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = config.n_gpu_layers;
        model_ = llama_model_load_from_file(config.model_path.c_str(), mparams);
        if (!model_) {
            return ErrCode::ModelNotLoaded;
        }
        vocab_ = llama_model_get_vocab(model_);
        n_vocab_ = static_cast<std::int32_t>(llama_vocab_n_tokens(vocab_));
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = static_cast<std::uint32_t>(config.n_ctx);
        cparams.n_batch = static_cast<std::uint32_t>(std::max(config.n_batch, 1));
        cparams.n_ubatch = static_cast<std::uint32_t>(
                std::min(std::max(config.n_ubatch, 1), static_cast<std::int32_t>(cparams.n_batch)));
        cparams.n_seq_max = 1;
        const std::int32_t threads =
                config.threads > 0 ? config.threads
                                   : static_cast<std::int32_t>(std::thread::hardware_concurrency());
        cparams.n_threads = threads;
        cparams.n_threads_batch = threads;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            llama_model_free(model_);
            model_ = nullptr;
            return ErrCode::ModelNotLoaded;
        }
        batch_ = llama_batch_ext_init(ctx_);
        if (!batch_) {
            release();
            return ErrCode::ModelNotLoaded;
        }
        n_ctx_ = config.n_ctx;
        return ErrCode::Ok;
    }

    // 为序列建独立采样链，并把 prompt 喂给惩罚统计以对齐 llama.cpp 口径。
    ErrCode add_sequence(const Sequence& seq) override {
        std::lock_guard<std::mutex> lock(mutex_);
        LlamaSeqState state;
        state.sampler = build_chain(seq.sampling, n_vocab_);
        for (std::int32_t id : seq.prompt_token_ids) {
            llama_sampler_accept(state.sampler, id);
        }
        states_[seq.group_id] = state;
        return ErrCode::Ok;
    }

    // 一步前向：prefill 全量入批，decode 单 token 入批；仅最后位置取 logits。
    RunnerStepOutput decode_step(const Batch& batch) override {
        RunnerStepOutput output;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ctx_ || !batch_) {
            output.err = ErrCode::ModelNotLoaded;
            return output;
        }
        llama_batch_ext_clear(batch_);
        std::int32_t token_index = 0;
        for (const NewRequestData& request : batch.new_reqs) {
            LlamaSeqState& state = states_[request.req_id];
            const std::int32_t size = static_cast<std::int32_t>(request.prefill_token_ids.size());
            for (std::int32_t i = 0; i < size; ++i) {
                const std::int32_t idx = add_token(request.prefill_token_ids[i], i);
                if (idx < 0) {
                    output.err = ErrCode::ExecutionFailed;
                    return output;
                }
                if (i == size - 1) {
                    (void)llama_batch_ext_set_output_logits(batch_, idx, true);
                }
                token_index = idx;
            }
            state.n_past = size;
            output.req_ids.push_back(request.req_id);
        }
        for (const CachedRequestData& request : batch.cached_reqs) {
            LlamaSeqState& state = states_[request.req_id];
            const std::int32_t idx = add_token(request.new_token_ids.front(), state.n_past);
            if (idx < 0) {
                output.err = ErrCode::ExecutionFailed;
                return output;
            }
            (void)llama_batch_ext_set_output_logits(batch_, idx, true);
            state.n_past += 1;
            token_index = idx;
            output.req_ids.push_back(request.req_id);
        }
        if (batch.total_tokens <= 0) {
            output.err = ErrCode::ExecutionFailed;
            return output;
        }
        if (llama_process(ctx_, LLAMA_PROCESS_TYPE_DECODE, batch_) != 0) {
            output.err = ErrCode::ExecutionFailed;
            return output;
        }
        (void)token_index;
        for (const RequestId& req_id : output.req_ids) {
            SampledToken token = sample(req_id);
            if (token.token_id < 0) {
                output.err = ErrCode::ExecutionFailed;
                return output;
            }
            output.sampled.push_back(token);
            output.num_nans.push_back(0);
        }
        output.err = ErrCode::Ok;
        return output;
    }

    // 释放序列：擦采样链并清空 KV；单序列阶段清整段是安全的。
    void release_sequence(const RequestId& req_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(req_id);
        if (it != states_.end()) {
            llama_sampler_free(it->second.sampler);
            states_.erase(it);
        }
        if (ctx_) {
            llama_memory_clear(llama_get_memory(ctx_), true);
        }
    }

    // 全量释放：采样链、批、上下文、模型、后端，顺序与创建相反。
    void release() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : states_) {
            llama_sampler_free(entry.second.sampler);
        }
        states_.clear();
        if (batch_) {
            llama_batch_ext_free(batch_);
            batch_ = nullptr;
        }
        if (ctx_) {
            llama_free(ctx_);
            ctx_ = nullptr;
        }
        if (model_) {
            llama_model_free(model_);
            model_ = nullptr;
        }
        if (backend_ready_) {
            llama_backend_free();
            backend_ready_ = false;
        }
    }

    // 分词：add_special=true 与 llama-server 默认一致；负返回值表示需要更大缓冲。
    ErrCode tokenize(const std::string& text, std::vector<std::int32_t>& ids) override {
        if (!vocab_) {
            return ErrCode::ModelNotLoaded;
        }
        std::vector<llama_token> buffer(text.size() + 16);
        int32_t count =
                llama_tokenize(vocab_, text.data(), static_cast<int32_t>(text.size()),
                               buffer.data(), static_cast<int32_t>(buffer.size()), true, false);
        if (count < 0) {
            buffer.resize(static_cast<std::size_t>(-count));
            count = llama_tokenize(vocab_, text.data(), static_cast<int32_t>(text.size()),
                                   buffer.data(), static_cast<int32_t>(buffer.size()), true, false);
        }
        if (count < 0) {
            return ErrCode::PromptTooLong;
        }
        ids.assign(buffer.begin(), buffer.begin() + count);
        return ErrCode::Ok;
    }

    // 反分词：负返回值给出所需字节数，扩容后重取。
    std::string detokenize(std::int32_t token_id) override {
        if (!vocab_) {
            return {};
        }
        std::string piece(8, '\0');
        int32_t length = llama_token_to_piece(vocab_, token_id, piece.data(),
                                              static_cast<int32_t>(piece.size()), 0, true);
        if (length < 0) {
            piece.resize(static_cast<std::size_t>(-length));
            length = llama_token_to_piece(vocab_, token_id, piece.data(),
                                          static_cast<std::int32_t>(piece.size()), 0, true);
        }
        if (length <= 0) {
            return {};
        }
        piece.resize(static_cast<std::size_t>(length));
        return piece;
    }

    // EOS 判定交给词表，避免把 </s> 当普通文本继续生成。
    bool is_eos(std::int32_t token_id) const override {
        return vocab_ ? token_id == llama_vocab_eos(vocab_) : false;
    }

    // 上下文长度取启动配置，供 HTTP 准入。
    std::int32_t n_ctx() const noexcept override {
        return n_ctx_;
    }

    // 后端名进入启动摘要。
    const char* backend_name() const noexcept override {
        return "llama.cpp";
    }

private:
    // 往批里加一个 token 并设位置；返回批内下标，负值即失败。
    std::int32_t add_token(std::int32_t token_id, std::int32_t pos) {
        const std::int32_t idx = llama_batch_ext_add_token(batch_, 0, token_id);
        if (idx < 0) {
            return idx;
        }
        const llama_pos position = pos;
        (void)llama_batch_ext_set_pos(batch_, idx, &position);
        return idx;
    }

    // 用序列自己的采样链取一个 token，并 accept 进历史；失败返回 -1。
    SampledToken sample(const RequestId& req_id) {
        auto it = states_.find(req_id);
        if (it == states_.end() || !it->second.sampler) {
            return SampledToken{};
        }
        const llama_token token = llama_sampler_sample(it->second.sampler, ctx_, -1);
        llama_sampler_accept(it->second.sampler, token);
        SampledToken out;
        out.token_id = token;
        out.logprob = 0.0F;
        return out;
    }

    ModelConfig config_;
    llama_model* model_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    llama_context* ctx_ = nullptr;
    llama_batch_ext* batch_ = nullptr;
    std::int32_t n_vocab_ = 0;
    std::int32_t n_ctx_ = 2048;
    bool backend_ready_ = true;
    std::mutex mutex_;
    std::map<RequestId, LlamaSeqState> states_;
};

// 工厂：只在 LLMOPT_ENABLE_LLAMA=ON 的构建里链接本文件。
std::unique_ptr<ModelRunner> make_llama_runner() {
    return std::make_unique<LlamaRunner>();
}

} // namespace llmopt
