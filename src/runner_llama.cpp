// runner_llama.cpp — C-07 真实后端适配：多序列 batch、按输出序采样、seq_cp 前缀复用。
// 耦合点：cell 由 llama 内部 find_slot 决定；这里只用 seq_id+pos 间接表达逻辑块表。
#include "llama.h"
#include "llmopt/runner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llmopt {
namespace {

// C 句柄统一交给 unique_ptr：析构即释放，异常/早退路径不再手工配对 free。
struct ModelDeleter {
    void operator()(llama_model* handle) const noexcept {
        if (handle != nullptr) {
            llama_model_free(handle);
        }
    }
};
struct ContextDeleter {
    void operator()(llama_context* handle) const noexcept {
        if (handle != nullptr) {
            llama_free(handle);
        }
    }
};
struct BatchDeleter {
    void operator()(llama_batch_ext* handle) const noexcept {
        if (handle != nullptr) {
            llama_batch_ext_free(handle);
        }
    }
};
struct SamplerDeleter {
    void operator()(llama_sampler* handle) const noexcept {
        if (handle != nullptr) {
            llama_sampler_free(handle);
        }
    }
};
using ModelPtr = std::unique_ptr<llama_model, ModelDeleter>;
using ContextPtr = std::unique_ptr<llama_context, ContextDeleter>;
using BatchPtr = std::unique_ptr<llama_batch_ext, BatchDeleter>;
using SamplerPtr = std::unique_ptr<llama_sampler, SamplerDeleter>;

// 每个序列一个采样链与一个 llama seq 槽；retained 表示 KV 前缀仍被借用。
struct LlamaSeqState {
    SamplerPtr sampler;
    std::int32_t slot = -1;
    std::int32_t n_past = 0;
    bool retained = false;
};

// 进程级后端初始化只做一次；重复调用无害。
void ensure_backend() {
    static std::once_flag once;
    std::call_once(once, [] { llama_backend_init(); });
}

// 把 (0,1] 的 top_p/min_p 转成 llama 需要的 min_keep=1，避免空候选。
SamplerPtr build_chain(const SamplingParams& sampling, std::int32_t n_vocab) {
    SamplerPtr chain{llama_sampler_chain_init(llama_sampler_chain_default_params())};
    llama_sampler_chain_add(chain.get(), llama_sampler_init_penalties(
                                                 n_vocab, 64, sampling.repeat_penalty, 0.0F, 0.0F));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_top_k(sampling.top_k));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_top_p(sampling.top_p, 1));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_min_p(sampling.min_p, 1));
    llama_sampler_chain_add(chain.get(), llama_sampler_init_temp(sampling.temperature));
    const std::uint32_t seed =
            sampling.seed >= 0 ? static_cast<std::uint32_t>(sampling.seed) : LLAMA_DEFAULT_SEED;
    llama_sampler_chain_add(chain.get(), llama_sampler_init_dist(seed));
    return chain;
}

} // namespace

// 真后端：模型/上下文/批全部独占在执行线程，析构按反序释放。
class LlamaRunner final : public ModelRunner {
public:
    ~LlamaRunner() override {
        release();
    }

    // 加载模型并建上下文；n_ctx_total 是整机 KV 容量，n_seq_max 容纳运行与留置序列。
    ErrCode load_model(const ModelConfig& config) override {
        ensure_backend();
        config_ = config;
        n_gpu_layers_ = config.n_gpu_layers;
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = config.n_gpu_layers;
        model_.reset(llama_model_load_from_file(config.model_path.c_str(), mparams));
        if (!model_) {
            // 第三方只回空指针：在这里翻成带阶段的领域错误，原始信息进 detail。
            last_error_ = make_error(
                    ErrCode::ModelNotLoaded, "模型加载失败（文件不可读或格式不支持）",
                    "op=load_model file=" +
                            std::filesystem::path(config.model_path).filename().string());
            return ErrCode::ModelNotLoaded;
        }
        vocab_ = llama_model_get_vocab(model_.get());
        n_vocab_ = static_cast<std::int32_t>(llama_vocab_n_tokens(vocab_));
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = static_cast<std::uint32_t>(std::max(config.n_ctx_total, config.n_ctx));
        cparams.n_batch = static_cast<std::uint32_t>(std::max(config.n_batch, 1));
        cparams.n_ubatch = static_cast<std::uint32_t>(
                std::min(std::max(config.n_ubatch, 1), static_cast<std::int32_t>(cparams.n_batch)));
        cparams.n_seq_max = static_cast<std::uint32_t>(std::max(config.n_seq_max, 1));
        const std::int32_t threads =
                config.threads > 0 ? config.threads
                                   : static_cast<std::int32_t>(std::thread::hardware_concurrency());
        cparams.n_threads = threads;
        cparams.n_threads_batch = threads;
        ctx_.reset(llama_init_from_model(model_.get(), cparams));
        if (!ctx_) {
            model_.reset();
            last_error_ = make_error(ErrCode::ModelNotLoaded, "上下文创建失败（显存/内存不足）",
                                     "op=init_context n_ctx=" + std::to_string(cparams.n_ctx) +
                                             " n_seq_max=" + std::to_string(cparams.n_seq_max));
            return ErrCode::ModelNotLoaded;
        }
        batch_.reset(llama_batch_ext_init(ctx_.get()));
        if (!batch_) {
            release();
            last_error_ =
                    make_error(ErrCode::ResourceInsufficient, "批缓冲创建失败",
                               "op=batch_ext_init n_batch=" + std::to_string(cparams.n_batch));
            return ErrCode::ResourceInsufficient;
        }
        slots_.clear();
        for (std::int32_t slot = cparams.n_seq_max - 1; slot >= 0; --slot) {
            slots_.push_back(slot);
        }
        n_ctx_ = config.n_ctx;
        return ErrCode::Ok;
    }

    // 为序列分配 llama seq 槽并建独立采样链；prompt 全量喂给惩罚统计。
    ErrCode add_sequence(const Sequence& seq) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slots_.empty()) {
            last_error_ = make_error(ErrCode::ResourceInsufficient, "后端序列槽位耗尽",
                                     "op=add_sequence active=" + std::to_string(states_.size()));
            return ErrCode::ResourceInsufficient;
        }
        LlamaSeqState state;
        state.slot = slots_.back();
        slots_.pop_back();
        state.sampler = build_chain(seq.sampling, n_vocab_);
        for (std::int32_t id : seq.prompt_token_ids) {
            llama_sampler_accept(state.sampler.get(), id);
        }
        for (std::int32_t id : seq.output_token_ids) {
            llama_sampler_accept(state.sampler.get(), id);
        }
        states_.insert_or_assign(seq.group_id, std::move(state));
        return ErrCode::Ok;
    }

    // 一步前向：按步骤投影逐序列加 token；每步只取最后位置 logits，再按输出序采样。
    RunnerStepOutput decode_step(const Batch& batch) override {
        RunnerStepOutput output;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ctx_ || !batch_) {
            output.error = make_error(ErrCode::ModelNotLoaded, "推理上下文未就绪",
                                      "op=decode_step ctx_or_batch_null");
            return output;
        }
        llama_batch_ext_clear(batch_.get());
        std::size_t new_index = 0;
        std::size_t cached_index = 0;
        std::vector<RequestId> ordered;
        std::vector<std::int32_t> output_indexes;
        std::vector<bool> need_sample;
        for (const SequenceStep& step : batch.steps) {
            const std::int32_t output_idx = step.phase == Phase::Decode
                                                    ? add_decode_step(step, batch, cached_index)
                                                    : add_prefill_step(step, batch, new_index);
            if (output_idx < 0) {
                return failed(
                        output,
                        make_error(ErrCode::ExecutionFailed, "批内步骤构建失败",
                                   "op=add_step req=" + step.route.req_id + " phase=" +
                                           (step.phase == Phase::Decode ? "decode" : "prefill")));
            }
            ordered.push_back(step.route.req_id);
            output_indexes.push_back(output_idx);
            need_sample.push_back(step.needs_sampling);
        }
        if (batch.total_tokens <= 0 || ordered.empty()) {
            return failed(output, make_error(ErrCode::ExecutionFailed, "空批次无法执行",
                                             "op=decode_step tokens=" +
                                                     std::to_string(batch.total_tokens)));
        }
        const int process_rc = llama_process(ctx_.get(), LLAMA_PROCESS_TYPE_DECODE, batch_.get());
        if (process_rc != 0) {
            return failed(output, from_backend_rc("llama.cpp", "process", process_rc));
        }
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            if (!need_sample[index]) {
                continue;
            }
            SampledToken token = sample(ordered[index], output_indexes[index]);
            if (token.token_id < 0) {
                return failed(output,
                              make_error(ErrCode::ExecutionFailed, "采样失败",
                                         "op=sample req=" + ordered[index] + " output_index=" +
                                                 std::to_string(output_indexes[index])));
            }
            output.req_ids.push_back(ordered[index]);
            output.sampled.push_back(token);
            output.num_nans.push_back(0);
        }
        output.error = Error{};
        return output;
    }

    // 释放序列：keep_tokens>0 时只删尾部，前缀留在 KV 中供后续 seq_cp。
    void release_sequence(const RequestId& req_id, std::int32_t keep_tokens = 0) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(req_id);
        if (it == states_.end()) {
            return;
        }
        if (it->second.sampler) {
            it->second.sampler.reset();
        }
        const std::int32_t slot = it->second.slot;
        if (keep_tokens > 0 && ctx_) {
            (void)llama_memory_seq_rm(llama_get_memory(ctx_.get()), slot, keep_tokens, -1);
            it->second.n_past = keep_tokens;
            it->second.retained = true;
            return;
        }
        if (ctx_) {
            (void)llama_memory_seq_rm(llama_get_memory(ctx_.get()), slot, -1, -1);
        }
        slots_.push_back(slot);
        states_.erase(it);
    }

    // 全量释放：按创建反序 reset 句柄；后端初始化是进程级的，不在此处 free。
    void release() override {
        std::lock_guard<std::mutex> lock(mutex_);
        states_.clear();
        slots_.clear();
        batch_.reset();
        ctx_.reset();
        model_.reset();
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

    // 单请求上下文上限进入 HTTP 准入。
    std::int32_t n_ctx() const noexcept override {
        return n_ctx_;
    }

    // 对账锚点：后端已写入该 llama seq 的最大位置。
    std::int32_t seq_pos_max(const RequestId& req_id) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(req_id);
        if (it == states_.end() || !ctx_) {
            return -1;
        }
        return static_cast<std::int32_t>(
                llama_memory_seq_pos_max(llama_get_memory(ctx_.get()), it->second.slot));
    }

    // 后端名进入启动摘要。
    const char* backend_name() const noexcept override {
        return "llama.cpp";
    }

    // 最近一次失败详情：main 用它给出可诊断的启动/运行报错。
    Error last_error() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

    // 只有 GPU offload 才走统一 KV stream；纯 CPU 的 seq_cp 是跨 stream 复制，
    // 会 reset 目标 cells 并与本批写入冲突（实测 ExecutionFailed）。
    bool supports_prefix_share() const noexcept override {
        return n_gpu_layers_ > 0;
    }

private:
    // 整批失败：清空已完成采样，交给 Core 统一走 Error 收尾；原始原因随 Error 回投。
    static RunnerStepOutput failed(RunnerStepOutput output, Error error) {
        output.req_ids.clear();
        output.sampled.clear();
        output.num_nans.clear();
        output.error = std::move(error);
        return output;
    }

    // prefill：可选 seq_cp 借用前缀，再按 pos 逐 token 入批；返回末 token 的批内下标。
    std::int32_t add_prefill_step(const SequenceStep& step, const Batch& batch,
                                  std::size_t& index) {
        if (index >= batch.new_reqs.size()) {
            return -1;
        }
        const NewRequestData& data = batch.new_reqs[index++];
        auto it = states_.find(data.req_id);
        if (it == states_.end()) {
            return -1;
        }
        LlamaSeqState& state = it->second;
        if (data.prefix_tokens > 0) {
            auto donor = states_.find(data.prefix_source);
            if (donor == states_.end() || !ctx_ || !donor->second.retained) {
                return -1;
            }
            // 只从已截断到前缀的 donor 借：cross-stream 的 seq_cp 只支持整段复制。
            llama_memory_seq_cp(llama_get_memory(ctx_.get()), donor->second.slot, state.slot, -1,
                                -1);
            state.n_past = data.prefix_tokens;
        }
        const std::int32_t size = static_cast<std::int32_t>(data.prefill_token_ids.size());
        std::int32_t last_idx = -1;
        for (std::int32_t offset = 0; offset < size; ++offset) {
            last_idx =
                    add_token(step.pos_begin + offset, data.prefill_token_ids[offset], state.slot);
            if (last_idx < 0) {
                return -1;
            }
        }
        // 只有末块需要 logits；中间 chunk 省掉 output 采样与 logits 计算。
        if (last_idx >= 0 && step.needs_sampling) {
            (void)llama_batch_ext_set_output_logits(batch_.get(), last_idx, true);
        }
        state.n_past = step.pos_end;
        return last_idx;
    }

    // decode：单个新 token 入批并出 logits；返回该 token 的批内下标。
    std::int32_t add_decode_step(const SequenceStep& step, const Batch& batch, std::size_t& index) {
        if (index >= batch.cached_reqs.size()) {
            return -1;
        }
        const CachedRequestData& data = batch.cached_reqs[index++];
        auto it = states_.find(data.req_id);
        if (it == states_.end() || data.new_token_ids.empty()) {
            return -1;
        }
        const std::int32_t idx =
                add_token(step.pos_begin, data.new_token_ids.front(), it->second.slot);
        if (idx < 0) {
            return -1;
        }
        (void)llama_batch_ext_set_output_logits(batch_.get(), idx, true);
        it->second.n_past = step.pos_end;
        return idx;
    }

    // 往批里加一个 token 并设位置；返回批内下标，负值即失败。
    std::int32_t add_token(std::int32_t pos, std::int32_t token_id, std::int32_t slot) {
        const std::int32_t idx =
                llama_batch_ext_add_token(batch_.get(), static_cast<llama_seq_id>(slot), token_id);
        if (idx < 0) {
            return idx;
        }
        const llama_pos position = pos;
        (void)llama_batch_ext_set_pos(batch_.get(), idx, &position);
        return idx;
    }

    // 按批内输出下标取 token；下标必须与 set_output_logits 的位置一致。
    SampledToken sample(const RequestId& req_id, std::int32_t output_index) {
        auto it = states_.find(req_id);
        if (it == states_.end() || !it->second.sampler) {
            return SampledToken{};
        }
        const llama_token token =
                llama_sampler_sample(it->second.sampler.get(), ctx_.get(), output_index);
        if (token < 0) {
            return SampledToken{};
        }
        llama_sampler_accept(it->second.sampler.get(), token);
        SampledToken out;
        out.token_id = token;
        out.logprob = 0.0F;
        return out;
    }

    ModelConfig config_;
    ModelPtr model_;
    const llama_vocab* vocab_ = nullptr;
    ContextPtr ctx_;
    BatchPtr batch_;
    std::int32_t n_vocab_ = 0;
    std::int32_t n_ctx_ = 2048;
    std::int32_t n_gpu_layers_ = 0;
    Error last_error_;
    mutable std::mutex mutex_;
    std::vector<std::int32_t> slots_;
    std::map<RequestId, LlamaSeqState> states_;
};

// 工厂：只在 LLMOPT_ENABLE_LLAMA=ON 的构建里链接本文件。
std::unique_ptr<ModelRunner> make_llama_runner() {
    return std::make_unique<LlamaRunner>();
}

} // namespace llmopt
