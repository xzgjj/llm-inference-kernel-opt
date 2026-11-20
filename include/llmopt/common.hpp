// common.hpp — 全局标识、错误码、采样参数与结束原因；只放跨模块共享的值类型。
#ifndef LLMOPT_COMMON_HPP
#define LLMOPT_COMMON_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llmopt {

using RequestId = std::string;
using GroupId = std::string;

// C-03：HTTP 映射在 protocol.cpp 一处维护。
enum class ErrCode : std::uint8_t {
    Ok = 0,
    InvalidJson,
    ModelNotLoaded,
    KvInsufficient,
    ContextOverflow,
    PromptTooLong,
    Cancelled,
    ExecutionFailed,
    QueueFull,
    NotSupported,
    ResourceInsufficient,
};

enum class FinishReason : std::uint8_t {
    None = 0,
    Stopped,
    LengthCapped,
    Aborted,
    Error,
    Preempted,
};

// C-01：请求侧采样参数；seed=-1 表示由服务端派生固定种子。
struct SamplingParams {
    std::int32_t max_tokens = 128;
    std::int32_t top_k = 40;
    float temperature = 0.7F;
    float top_p = 0.95F;
    float min_p = 0.05F;
    float repeat_penalty = 1.0F;
    std::int64_t seed = -1;
};

struct SampledToken {
    std::int32_t token_id = -1;
    float logprob = 0.0F;
};

// C-09 的 logits 视图：只在一次 decode_step 内有效，禁止留存。
struct LogitsView {
    const float* data = nullptr;
    std::int32_t vocab_size = 0;
};

// C-01
struct InferenceRequest {
    RequestId req_id;
    std::string prompt;
    bool stream = false;
    SamplingParams sampling;
    std::string model_echo;
};

// C-02
struct Delta {
    std::int32_t token_id = -1;
    std::string text;
    std::optional<FinishReason> finish;
};

int http_status(ErrCode code) noexcept;
const char* error_code_name(ErrCode code) noexcept;
const char* finish_reason_name(FinishReason reason) noexcept;
bool retryable(ErrCode code) noexcept;

} // namespace llmopt

#endif // LLMOPT_COMMON_HPP
