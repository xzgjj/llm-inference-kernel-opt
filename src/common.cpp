// common.cpp — C-03 错误码到 HTTP 的唯一映射，以及枚举文本名。
#include "llmopt/common.hpp"

namespace llmopt {

int http_status(ErrCode code) noexcept {
    switch (code) {
    case ErrCode::Ok:
        return 200;
    case ErrCode::InvalidJson:
    case ErrCode::PromptTooLong:
    case ErrCode::ContextOverflow:
    case ErrCode::ConfigInvalid:
        return 400;
    case ErrCode::Unauthorized:
        return 401;
    case ErrCode::ModelNotLoaded:
    case ErrCode::Unavailable:
        return 503;
    case ErrCode::KvBelowWatermark:
    case ErrCode::QueueFull:
    case ErrCode::RateLimited:
        return 429;
    case ErrCode::KvInsufficient:
        return 507;
    case ErrCode::Cancelled:
        return 499;
    case ErrCode::ExecutionFailed:
        return 500;
    case ErrCode::NotSupported:
        return 404;
    case ErrCode::ResourceInsufficient:
        return 507;
    case ErrCode::Timeout:
        return 504;
    case ErrCode::UnsupportedField:
        return 400;
    case ErrCode::PayloadTooLarge:
        return 413;
    }
    return 500;
}

const char* error_code_name(ErrCode code) noexcept {
    switch (code) {
    case ErrCode::Ok:
        return "OK";
    case ErrCode::InvalidJson:
        return "INVALID_JSON";
    case ErrCode::ModelNotLoaded:
        return "MODEL_NOT_LOADED";
    case ErrCode::KvInsufficient:
        return "KV_INSUFFICIENT";
    case ErrCode::ContextOverflow:
        return "CONTEXT_OVERFLOW";
    case ErrCode::PromptTooLong:
        return "PROMPT_TOO_LONG";
    case ErrCode::Cancelled:
        return "REQUEST_CANCELLED";
    case ErrCode::ExecutionFailed:
        return "EXECUTION_FAILED";
    case ErrCode::QueueFull:
        return "QUEUE_FULL";
    case ErrCode::NotSupported:
        return "NOT_SUPPORTED";
    case ErrCode::ResourceInsufficient:
        return "RESOURCE_INSUFFICIENT";
    case ErrCode::Timeout:
        return "TIMEOUT";
    case ErrCode::UnsupportedField:
        return "UNSUPPORTED_FIELD";
    case ErrCode::Unavailable:
        return "SERVICE_UNAVAILABLE";
    case ErrCode::RateLimited:
        return "RATE_LIMITED";
    case ErrCode::Unauthorized:
        return "UNAUTHORIZED";
    case ErrCode::ConfigInvalid:
        return "CONFIG_INVALID";
    case ErrCode::KvBelowWatermark:
        return "KV_BELOW_WATERMARK";
    case ErrCode::PayloadTooLarge:
        return "PAYLOAD_TOO_LARGE";
    }
    return "EXECUTION_FAILED";
}

const char* finish_reason_name(FinishReason reason) noexcept {
    switch (reason) {
    case FinishReason::None:
        return "none";
    case FinishReason::Stopped:
        return "stop";
    case FinishReason::LengthCapped:
        return "length";
    case FinishReason::Aborted:
        return "abort";
    case FinishReason::Error:
        return "error";
    case FinishReason::Preempted:
        return "preempt";
    }
    return "error";
}

bool retryable(ErrCode code) noexcept {
    return code == ErrCode::ModelNotLoaded || code == ErrCode::KvBelowWatermark ||
           code == ErrCode::QueueFull || code == ErrCode::Timeout || code == ErrCode::Unavailable ||
           code == ErrCode::RateLimited;
}

} // namespace llmopt
