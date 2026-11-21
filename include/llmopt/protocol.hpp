// protocol.hpp — C-01 请求解析与 C-02 SSE/JSON 帧渲染；可独立单测。
#ifndef LLMOPT_PROTOCOL_HPP
#define LLMOPT_PROTOCOL_HPP

#include "llmopt/common.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llmopt {

// 解析 `/v1/completions` 与 `/completion` 共用的请求体。
ErrCode parse_body(const std::string& body, InferenceRequest& out, std::string& message) noexcept;

// 解析 `/v1/chat/completions`：messages 只做结构校验，模板渲染在提交前完成。
ErrCode parse_chat_body(const std::string& body, InferenceRequest& out,
                        std::string& message) noexcept;

// 请求缺省时生成唯一 id；格式 req-<12 hex>，不依赖随机源。
std::string make_request_id();

// 外部 Request ID 的协议边界：1–64 字节，仅 [A-Za-z0-9._:-]，禁止空白与控制字符。
ErrCode validate_request_id(std::string_view id, std::string& message);

// 增量帧与终止帧；字节序列见 SPEC-1 C-02。chat 入口输出 delta.content。
std::string render_frame(const InferenceRequest& req, const Delta& delta);
std::string render_frame(const InferenceRequest& req, const Delta& delta, std::int32_t index);
std::string render_done_frame() noexcept;

// 非流式响应：texts 按 choice.index 排列，finishes/usage 与之一一对应。
std::string render_completion_body(const InferenceRequest& req, const std::string& text,
                                   FinishReason finish, int prompt_tokens, int gen_tokens);
std::string render_body(const InferenceRequest& req, const std::vector<std::string>& texts,
                        const std::vector<FinishReason>& finishes, int prompt_tokens,
                        const std::vector<int>& gen_tokens);
std::string render_models_body(const std::string& model_name);

// stop 截断：流式跨帧命中需要暂存可能构成 stop 前缀的尾部，非流式最后 flush。
class StopFilter {
public:
    explicit StopFilter(const std::vector<std::string>& stops);
    [[nodiscard]] bool empty() const noexcept;
    // 返回可安全下发的前缀；命中时置 *hit，并把命中处之后全部丢弃。
    std::string feed(const std::string& text, bool* hit);
    // 生成自然结束时输出暂存的尾部。
    std::string flush();

private:
    std::vector<std::string> stops_;
    std::string pending_;
};

// 错误体：{"error":{"code","message","retryable"}}；Error 版本是唯一生产路径。
std::string render_error_body(const Error& error);
std::string render_error_body(ErrCode code, const std::string& message);

} // namespace llmopt

#endif // LLMOPT_PROTOCOL_HPP
