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
    std::int32_t n_ctx = 2048;
    std::int32_t n_batch = 2048;
    std::int32_t n_ubatch = 512;
    std::int32_t threads = 0;
    std::int32_t n_gpu_layers = 0;
};

class ModelRunner {
public:
    virtual ~ModelRunner() = default;

    virtual ErrCode load_model(const ModelConfig& config) = 0;
    virtual ErrCode add_sequence(const Sequence& seq) = 0;
    virtual RunnerStepOutput decode_step(const Batch& batch) = 0;
    virtual void release_sequence(const RequestId& req_id) = 0;
    virtual void release() = 0;

    // M3 tokenizer 随模型词表走，避免二次加载权重。
    virtual ErrCode tokenize(const std::string& text, std::vector<std::int32_t>& ids) = 0;
    virtual std::string detokenize(std::int32_t token_id) = 0;
    virtual bool is_eos(std::int32_t token_id) const = 0;
    virtual std::int32_t n_ctx() const noexcept = 0;
    virtual const char* backend_name() const noexcept = 0;
};

std::unique_ptr<ModelRunner> make_stub_runner();
// 仅 LLMOPT_ENABLE_LLAMA=ON 时由 runner_llama.cpp 提供。
std::unique_ptr<ModelRunner> make_llama_runner();

} // namespace llmopt

#endif // LLMOPT_RUNNER_HPP
