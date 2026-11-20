// common.hpp — 全局标识、错误码、采样参数与结束原因；只放跨模块共享的值类型。
#ifndef LLMOPT_COMMON_HPP
#define LLMOPT_COMMON_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
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
    Timeout,
};

// 错误载体：跨层携带 code + 面向调用方的 message + 面向运维的 detail。
// 规则见 docs/错误面规范.md：detail 只进日志，不进 HTTP body；成功路径统一用 Error{}。
struct Error {
    ErrCode code = ErrCode::Ok;
    std::string message;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept {
        return code == ErrCode::Ok;
    }
    explicit operator bool() const noexcept {
        return ok();
    }
};

inline Error make_error(ErrCode code, std::string message, std::string detail = {}) {
    Error error;
    error.code = code;
    error.message = std::move(message);
    error.detail = std::move(detail);
    return error;
}

// 第三方返回值 → 领域 Error：原始码只留在 detail，禁止把裸 int 带出适配器。
inline Error from_backend_rc(const char* backend, const char* op, long long rc) {
    return make_error(ErrCode::ExecutionFailed, std::string(backend) + " 执行失败：" + op,
                      "op=" + std::string(op) + " rc=" + std::to_string(rc));
}

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
    // 非空表示本帧是错误帧：流式必须在 [DONE] 之前推给客户端，客户端据此终止。
    std::optional<Error> error;
};

int http_status(ErrCode code) noexcept;
const char* error_code_name(ErrCode code) noexcept;
const char* finish_reason_name(FinishReason reason) noexcept;
bool retryable(ErrCode code) noexcept;

} // namespace llmopt

#endif // LLMOPT_COMMON_HPP
