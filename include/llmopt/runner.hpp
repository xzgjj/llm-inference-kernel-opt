// runner.hpp — C-07 执行层接口：唯一允许写 KV 的线程调用 decode_step。
#ifndef LLMOPT_RUNNER_HPP
#define LLMOPT_RUNNER_HPP

#include "llmopt/batch.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace llmopt {

struct ModelConfig {
    std::string model_path;
    std::int32_t n_ctx = 2048;       // 单请求上下文上限（HTTP 准入口径）
    std::int32_t n_ctx_total = 2048; // 整机 KV 容量（默认等于单请求上限）
    std::int32_t n_batch = 2048;
    std::int32_t n_ubatch = 512;
    std::int32_t n_seq_max = 1;
    std::int32_t threads = 0;
    std::int32_t n_gpu_layers = 0;
};

class ModelRunner {
public:
    virtual ~ModelRunner() = default;

    virtual ErrCode load_model(const ModelConfig& config) = 0;
    virtual ErrCode add_sequence(const Sequence& seq) = 0;
    virtual RunnerStepOutput decode_step(const Batch& batch) = 0;
    // keep_tokens>0 表示保留 [0, keep_tokens) 的 KV 作为前缀 donor，其余释放。
    virtual void release_sequence(const RequestId& req_id, std::int32_t keep_tokens = 0) = 0;
    virtual void release() = 0;

    // M3 tokenizer 随模型词表走，避免二次加载权重。
    virtual ErrCode tokenize(const std::string& text, std::vector<std::int32_t>& ids) = 0;
    virtual std::string detokenize(std::int32_t token_id) = 0;
    virtual bool is_eos(std::int32_t token_id) const = 0;
    virtual std::int32_t n_ctx() const noexcept = 0;
    // 后端已写入该序列的最大位置；-1 表示空或后端不支持对账。
    virtual std::int32_t seq_pos_max(const RequestId& req_id) const {
        (void)req_id;
        return -1;
    }
    // 最近一次失败的详细信息（message/detail）；适配器在此保留第三方原始 cause。
    // 只用于日志与启动报错，不参与控制流。
    virtual Error last_error() const {
        return Error{};
    }
    // 是否允许从运行中的序列借前缀；跨 stream 后端只能从已截断的 donor 借。
    virtual bool supports_live_prefix() const noexcept {
        return false;
    }

    // 后端能否零拷贝共享已算好的前缀 KV；默认关闭，只有确认安全的实现才打开。
    virtual bool supports_prefix_share() const noexcept {
        return false;
    }
    virtual const char* backend_name() const noexcept = 0;
};

std::unique_ptr<ModelRunner> make_stub_runner();
// 仅 LLMOPT_ENABLE_LLAMA=ON 时由 runner_llama.cpp 提供。
std::unique_ptr<ModelRunner> make_llama_runner();

} // namespace llmopt

#endif // LLMOPT_RUNNER_HPP
