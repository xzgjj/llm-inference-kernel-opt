// protocol.hpp — C-01 请求解析与 C-02 SSE/JSON 帧渲染；可独立单测。
#ifndef LLMOPT_PROTOCOL_HPP
#define LLMOPT_PROTOCOL_HPP

#include "llmopt/common.hpp"

#include <optional>
#include <string>

namespace llmopt {

// 解析 `/v1/completions` 与 `/completion` 共用的请求体。
ErrCode parse_body(const std::string& body, InferenceRequest& out, std::string& message) noexcept;

// 请求缺省时生成唯一 id；格式 req-<12 hex>，不依赖随机源。
std::string make_request_id();

// 增量帧与终止帧；字节序列见 SPEC-1 C-02。
std::string render_frame(const InferenceRequest& req, const Delta& delta);
std::string render_done_frame() noexcept;

// 非流式响应：text 为拼接结果，finish 为结束原因，usage 为计数。
std::string render_completion_body(const InferenceRequest& req, const std::string& text,
                                   FinishReason finish, int prompt_tokens, int gen_tokens);

// 错误体：{"error":{"code","message","retryable"}}；Error 版本是唯一生产路径。
std::string render_error_body(const Error& error);
std::string render_error_body(ErrCode code, const std::string& message);

} // namespace llmopt

#endif // LLMOPT_PROTOCOL_HPP
