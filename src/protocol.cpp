// protocol.cpp — C-01 请求 JSON 与 C-02 帧渲染；nlohmann 错误一律转 ErrCode。
#include "llmopt/protocol.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

namespace llmopt {
namespace {

using json = nlohmann::json;

std::atomic<std::uint64_t> g_request_seq{0};

bool read_int(const json& body, const char* key, std::int64_t min, std::int64_t max,
              std::int64_t& out) {
    auto it = body.find(key);
    if (it == body.end()) {
        return true;
    }
    if (!it->is_number_integer()) {
        return false;
    }
    const std::int64_t value = it->get<std::int64_t>();
    if (value < min || value > max) {
        return false;
    }
    out = value;
    return true;
}

bool read_float(const json& body, const char* key, double min, double max, float& out) {
    auto it = body.find(key);
    if (it == body.end()) {
        return true;
    }
    if (!it->is_number()) {
        return false;
    }
    const double value = it->get<double>();
    if (value < min || value > max) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

// D6-1：兼容面之外的能力一律显式拒绝，避免"静默忽略后行为与 OpenAI 不一致"。
ErrCode parse_sampling(const json& body, SamplingParams& sampling, std::string& message);

ErrCode reject_unsupported(const json& body, std::string& message) {
    static const char* const kUnsupported[] = {
            "tools",      "tool_choice",  "functions",           "function_call",
            "logprobs",   "top_logprobs", "logit_bias",          "response_format",
            "modalities", "audio",        "parallel_tool_calls",
    };
    for (const char* key : kUnsupported) {
        if (body.contains(key)) {
            message = std::string("unsupported_field: ") + key;
            return ErrCode::UnsupportedField;
        }
    }
    return ErrCode::Ok;
}

bool parse_stop(const json& body, std::vector<std::string>& out, std::string& message) {
    auto it = body.find("stop");
    if (it == body.end() || it->is_null()) {
        return true;
    }
    std::vector<std::string> stops;
    if (it->is_string()) {
        stops.push_back(it->get<std::string>());
    } else if (it->is_array()) {
        if (it->size() > 4) {
            message = "stop 最多 4 条";
            return false;
        }
        for (const json& item : *it) {
            if (!item.is_string()) {
                message = "stop 必须是字符串或字符串数组";
                return false;
            }
            stops.push_back(item.get<std::string>());
        }
    } else {
        message = "stop 必须是字符串或字符串数组";
        return false;
    }
    for (const std::string& stop : stops) {
        if (stop.empty() || stop.size() > 32) {
            message = "stop 每条长度必须在 [1, 32]";
            return false;
        }
    }
    out = std::move(stops);
    return true;
}

ErrCode parse_common(const json& body, InferenceRequest& out, std::string& message) {
    const ErrCode unsupported = reject_unsupported(body, message);
    if (unsupported != ErrCode::Ok) {
        return unsupported;
    }
    if (body.contains("stream") && !body["stream"].is_boolean()) {
        message = "stream 必须是布尔";
        return ErrCode::InvalidJson;
    }
    out.stream = body.value("stream", false);
    out.model_echo = body.value("model", "");
    if (body.contains("id") && body["id"].is_string()) {
        out.req_id = body["id"].get<std::string>();
    } else {
        out.req_id = make_request_id();
    }
    if (const ErrCode id_code = validate_request_id(out.req_id, message); id_code != ErrCode::Ok) {
        return id_code;
    }
    std::int64_t n = out.n;
    if (!read_int(body, "n", 1, 4, n)) {
        message = "n 必须是整数且在 [1, 4]";
        return ErrCode::InvalidJson;
    }
    out.n = static_cast<std::int32_t>(n);
    if (out.stream && out.n > 1) {
        message = "unsupported_field: stream 与 n>1 暂不支持组合";
        return ErrCode::UnsupportedField;
    }
    if (!parse_stop(body, out.stop, message)) {
        return ErrCode::InvalidJson;
    }
    return parse_sampling(body, out.sampling, message);
}

// chat content 允许纯文本数组（常见 SDK 形态），出现图片等非文本分片即拒绝。
ErrCode parse_message_content(const json& item, std::string& out, std::string& message) {
    auto content = item.find("content");
    if (content == item.end()) {
        message = "messages[].content 缺失";
        return ErrCode::InvalidJson;
    }
    if (content->is_string()) {
        out = content->get<std::string>();
        return ErrCode::Ok;
    }
    if (!content->is_array()) {
        message = "messages[].content 必须是字符串或文本分片数组";
        return ErrCode::InvalidJson;
    }
    std::string text;
    for (const json& part : *content) {
        if (!part.is_object() || part.value("type", "") != "text" || !part.contains("text")) {
            message = "unsupported_field: messages[].content 分片（仅支持 type=text）";
            return ErrCode::UnsupportedField;
        }
        const json& text_field = part["text"];
        if (!text_field.is_string()) {
            message = "messages[].content[].text 必须是字符串";
            return ErrCode::InvalidJson;
        }
        text += text_field.get<std::string>();
    }
    out = std::move(text);
    return ErrCode::Ok;
}

ErrCode parse_sampling(const json& body, SamplingParams& sampling, std::string& message) {
    std::int64_t max_tokens = sampling.max_tokens;
    if (body.contains("max_tokens")) {
        if (!read_int(body, "max_tokens", 1, 4096, max_tokens)) {
            message = "max_tokens 必须是整数且在 [1, 4096]";
            return ErrCode::InvalidJson;
        }
    } else if (body.contains("n_predict")) {
        if (!read_int(body, "n_predict", 1, 4096, max_tokens)) {
            message = "n_predict 必须是整数且在 [1, 4096]";
            return ErrCode::InvalidJson;
        }
    }
    sampling.max_tokens = static_cast<std::int32_t>(max_tokens);

    std::int64_t top_k = sampling.top_k;
    if (!read_int(body, "top_k", 0, 256, top_k)) {
        message = "top_k 必须是整数且在 [0, 256]";
        return ErrCode::InvalidJson;
    }
    sampling.top_k = static_cast<std::int32_t>(top_k);

    std::int64_t seed = sampling.seed;
    if (!read_int(body, "seed", -1, 1'000'000'000, seed)) {
        message = "seed 必须是整数且在 [-1, 1000000000]";
        return ErrCode::InvalidJson;
    }
    sampling.seed = seed;
    if (!read_float(body, "temperature", 0.0, 2.0, sampling.temperature)) {
        message = "temperature 必须是数字且在 [0, 2]";
        return ErrCode::InvalidJson;
    }
    if (!read_float(body, "top_p", 0.0, 1.0, sampling.top_p)) {
        message = "top_p 必须是数字且在 [0, 1]";
        return ErrCode::InvalidJson;
    }
    if (!read_float(body, "min_p", 0.0, 1.0, sampling.min_p)) {
        message = "min_p 必须是数字且在 [0, 1]";
        return ErrCode::InvalidJson;
    }
    if (!read_float(body, "repeat_penalty", 0.5, 2.0, sampling.repeat_penalty)) {
        message = "repeat_penalty 必须是数字且在 [0.5, 2]";
        return ErrCode::InvalidJson;
    }
    return ErrCode::Ok;
}

ErrCode parse_prompt(const json& body, InferenceRequest& out, std::string& message) {
    auto it = body.find("prompt");
    if (it == body.end() || !it->is_string()) {
        message = "prompt 必须是字符串";
        return ErrCode::InvalidJson;
    }
    out.prompt = it->get<std::string>();
    if (out.prompt.empty()) {
        message = "prompt 不能为空";
        return ErrCode::InvalidJson;
    }
    return ErrCode::Ok;
}

} // namespace

std::string make_request_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto seq = g_request_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "req-%08llx%04llx",
                  static_cast<unsigned long long>(now & 0xffffffffULL),
                  static_cast<unsigned long long>(seq & 0xffffULL));
    return std::string(buffer);
}

ErrCode validate_request_id(std::string_view id, std::string& message) {
    if (id.empty() || id.size() > 64) {
        message = "id 长度必须在 [1, 64]";
        return ErrCode::InvalidJson;
    }
    for (const char ch : id) {
        const bool allowed = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                             (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == ':' ||
                             ch == '-';
        if (!allowed) {
            message = "id 仅允许 [A-Za-z0-9._:-]";
            return ErrCode::InvalidJson;
        }
    }
    return ErrCode::Ok;
}

ErrCode parse_body(const std::string& body, InferenceRequest& out, std::string& message,
                   std::string* detail) noexcept {
    try {
        const json parsed = json::parse(body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            message = "请求体不是合法 JSON 对象";
            return ErrCode::InvalidJson;
        }
        const ErrCode prompt_code = parse_prompt(parsed, out, message);
        if (prompt_code != ErrCode::Ok) {
            return prompt_code;
        }
        out.chat = false;
        return parse_common(parsed, out, message);
    } catch (const std::exception& exc) {
        // 第三方异常原文只进 detail（日志），message 给字段级可行动信息，不外露库名。
        message = "请求体 JSON 非法，请检查字段类型与结构";
        if (detail != nullptr) {
            *detail = exc.what();
        }
        return ErrCode::InvalidJson;
    }
}

ErrCode parse_chat_body(const std::string& body, InferenceRequest& out, std::string& message,
                        std::string* detail) noexcept {
    try {
        const json parsed = json::parse(body, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            message = "请求体不是合法 JSON 对象";
            return ErrCode::InvalidJson;
        }
        const ErrCode unsupported = reject_unsupported(parsed, message);
        if (unsupported != ErrCode::Ok) {
            return unsupported;
        }
        auto it = parsed.find("messages");
        if (it == parsed.end() || !it->is_array() || it->empty()) {
            message = "messages 必须是非空数组";
            return ErrCode::InvalidJson;
        }
        std::vector<ChatMessage> messages;
        messages.reserve(it->size());
        for (const json& item : *it) {
            if (!item.is_object()) {
                message = "messages[] 必须是对象";
                return ErrCode::InvalidJson;
            }
            ChatMessage message_item;
            message_item.role = item.value("role", "");
            if (message_item.role != "system" && message_item.role != "user" &&
                message_item.role != "assistant") {
                message = "unsupported_field: messages[].role（仅支持 system/user/assistant）";
                return ErrCode::UnsupportedField;
            }
            const ErrCode content_code = parse_message_content(item, message_item.content, message);
            if (content_code != ErrCode::Ok) {
                return content_code;
            }
            messages.push_back(std::move(message_item));
        }
        out.messages = std::move(messages);
        out.chat = true;
        out.prompt.clear();
        return parse_common(parsed, out, message);
    } catch (const std::exception& exc) {
        // 第三方异常原文只进 detail（日志），message 给字段级可行动信息，不外露库名。
        message = "请求体 JSON 非法，请检查字段类型与结构";
        if (detail != nullptr) {
            *detail = exc.what();
        }
        return ErrCode::InvalidJson;
    }
}

std::string render_frame(const InferenceRequest& req, const Delta& delta) {
    return render_frame(req, delta, 0);
}

std::string render_frame(const InferenceRequest& req, const Delta& delta, std::int32_t index) {
    // 错误帧独立成形：客户端在 [DONE] 之前就能拿到 code/message/retryable。
    if (delta.error && !delta.error->ok()) {
        json frame;
        frame["id"] = req.req_id;
        const std::string message = delta.error->message.empty()
                                            ? std::string(error_code_name(delta.error->code))
                                            : delta.error->message;
        frame["error"] = {{"code", error_code_name(delta.error->code)},
                          {"message", message},
                          {"retryable", retryable(delta.error->code)}};
        return "data: " + frame.dump() + "\n\n";
    }
    json frame;
    frame["id"] = req.req_id;
    frame["model"] = req.model_echo;
    frame["object"] = req.chat ? "chat.completion.chunk" : "text_completion";
    frame["choices"] = json::array();
    json choice;
    choice["index"] = index;
    if (req.chat) {
        choice["delta"] = {{"content", delta.text}};
    } else {
        choice["text"] = delta.text;
        choice["token_id"] = delta.token_id;
    }
    choice["finish_reason"] =
            delta.finish ? json(finish_reason_name(*delta.finish)) : json(nullptr);
    frame["choices"].push_back(choice);
    return "data: " + frame.dump() + "\n\n";
}

std::string render_done_frame() noexcept {
    return "data: [DONE]\n\n";
}

std::string render_completion_body(const InferenceRequest& req, const std::string& text,
                                   FinishReason finish, int prompt_tokens, int gen_tokens) {
    return render_body(req, {text}, {finish}, prompt_tokens, {gen_tokens});
}

std::string render_body(const InferenceRequest& req, const std::vector<std::string>& texts,
                        const std::vector<FinishReason>& finishes, int prompt_tokens,
                        const std::vector<int>& gen_tokens) {
    json body;
    body["id"] = req.req_id;
    body["model"] = req.model_echo;
    body["object"] = req.chat ? "chat.completion" : "text_completion";
    body["choices"] = json::array();
    int total_completion = 0;
    for (std::size_t index = 0; index < texts.size(); ++index) {
        json choice;
        choice["index"] = static_cast<std::int32_t>(index);
        if (req.chat) {
            choice["message"] = {{"role", "assistant"}, {"content", texts[index]}};
        } else {
            choice["text"] = texts[index];
        }
        choice["finish_reason"] =
                finish_reason_name(index < finishes.size() ? finishes[index] : FinishReason::Error);
        body["choices"].push_back(choice);
        total_completion += index < gen_tokens.size() ? gen_tokens[index] : 0;
    }
    body["usage"] = {{"prompt_tokens", prompt_tokens},
                     {"completion_tokens", total_completion},
                     {"total_tokens", prompt_tokens + total_completion}};
    return body.dump();
}

std::string render_models_body(const std::string& model_name) {
    json body;
    body["object"] = "list";
    body["data"] = json::array();
    body["data"].push_back({{"id", model_name.empty() ? "llmopt" : model_name},
                            {"object", "model"},
                            {"owned_by", "llmopt"}});
    return body.dump();
}

StopFilter::StopFilter(const std::vector<std::string>& stops) : stops_(stops) {}

bool StopFilter::empty() const noexcept {
    return stops_.empty();
}

std::string StopFilter::feed(const std::string& text, bool* hit) {
    *hit = false;
    if (stops_.empty()) {
        return text;
    }
    pending_ += text;
    std::size_t earliest = std::string::npos;
    for (const std::string& stop : stops_) {
        const std::size_t pos = pending_.find(stop);
        if (pos != std::string::npos && (earliest == std::string::npos || pos < earliest)) {
            earliest = pos;
        }
    }
    if (earliest != std::string::npos) {
        std::string out = pending_.substr(0, earliest);
        pending_.clear();
        *hit = true;
        return out;
    }
    // 暂存"可能是 stop 前缀"的最长尾部，其余立即下发，避免流式被整段卡住。
    std::size_t hold = 0;
    for (const std::string& stop : stops_) {
        const std::size_t max = std::min(stop.size() - 1, pending_.size());
        for (std::size_t len = max; len > hold; --len) {
            if (pending_.compare(pending_.size() - len, len, stop, 0, len) == 0) {
                hold = len;
                break;
            }
        }
    }
    std::string out = pending_.substr(0, pending_.size() - hold);
    pending_.erase(0, pending_.size() - hold);
    return out;
}

std::string StopFilter::flush() {
    std::string out;
    out.swap(pending_);
    return out;
}

std::string render_error_body(const Error& error) {
    const std::string message =
            error.message.empty() ? std::string(error_code_name(error.code)) : error.message;
    return render_error_body(error.code, message);
}

std::string render_error_body(ErrCode code, const std::string& message) {
    json body;
    body["error"] = {
            {"code", error_code_name(code)}, {"message", message}, {"retryable", retryable(code)}};
    return body.dump();
}

} // namespace llmopt
