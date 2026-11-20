// protocol.cpp — C-01 请求 JSON 与 C-02 帧渲染；nlohmann 错误一律转 ErrCode。
#include "llmopt/protocol.hpp"

#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

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

ErrCode parse_body(const std::string& body, InferenceRequest& out, std::string& message) noexcept {
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
        if (parsed.contains("stream") && !parsed["stream"].is_boolean()) {
            message = "stream 必须是布尔";
            return ErrCode::InvalidJson;
        }
        out.stream = parsed.value("stream", false);
        out.model_echo = parsed.value("model", "");
        if (parsed.contains("id") && parsed["id"].is_string()) {
            out.req_id = parsed["id"].get<std::string>();
        } else {
            out.req_id = make_request_id();
        }
        const ErrCode sampling_code = parse_sampling(parsed, out.sampling, message);
        return sampling_code;
    } catch (const std::exception& exc) {
        message = std::string("解析异常: ") + exc.what();
        return ErrCode::InvalidJson;
    }
}

std::string render_frame(const InferenceRequest& req, const Delta& delta) {
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
    frame["choices"] = json::array();
    json choice;
    choice["index"] = 0;
    choice["text"] = delta.text;
    choice["token_id"] = delta.token_id;
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
    json body;
    body["id"] = req.req_id;
    body["object"] = "text_completion";
    body["choices"] = json::array();
    json choice;
    choice["index"] = 0;
    choice["text"] = text;
    choice["finish_reason"] = finish_reason_name(finish);
    body["choices"].push_back(choice);
    body["usage"] = {{"prompt_tokens", prompt_tokens},
                     {"completion_tokens", gen_tokens},
                     {"total_tokens", prompt_tokens + gen_tokens}};
    return body.dump();
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
