// protocol_test.cc — C-01 解析与 C-02 帧/错误体的契约用例。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/common.hpp"
#include "llmopt/protocol.hpp"

#include <string>

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 拼一个最小请求体，便于逐项改字段。
std::string body_with(const std::string& extra) {
    return R"({"prompt":"hello","stream":false)" + extra + "}";
}

} // namespace

TEST_CASE("合法请求解析出 prompt 与采样默认值") {
    InferenceRequest req;
    std::string message;
    REQUIRE(parse_body(R"({"prompt":"hi"})", req, message) == ErrCode::Ok);
    CHECK(req.prompt == "hi");
    CHECK(req.stream == false);
    CHECK(req.sampling.max_tokens == 128);
    CHECK(req.sampling.temperature == doctest::Approx(0.7F));
    CHECK_FALSE(req.req_id.empty());
}

TEST_CASE("显式字段覆盖默认且 max_tokens 支持 n_predict 别名") {
    InferenceRequest req;
    std::string message;
    const std::string body =
            R"({"prompt":"hi","id":"req-x","stream":true,"n_predict":3,"seed":42,"top_k":10})";
    REQUIRE(parse_body(body, req, message) == ErrCode::Ok);
    CHECK(req.req_id == "req-x");
    CHECK(req.stream);
    CHECK(req.sampling.max_tokens == 3);
    CHECK(req.sampling.seed == 42);
    CHECK(req.sampling.top_k == 10);
}

TEST_CASE("非法 JSON / 非对象 / 空 prompt / 越界采样全部拒绝") {
    InferenceRequest req;
    std::string message;
    CHECK(parse_body("not json", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body("[]", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(R"({"prompt":""})", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(R"({"prompt":1})", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(body_with(R"(,"max_tokens":0)"), req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(body_with(R"(,"temperature":3,"top_p":1,"min_p":0)"), req, message) ==
          ErrCode::InvalidJson);
    CHECK(parse_body(body_with(R"(,"stream":1)"), req, message) == ErrCode::InvalidJson);
}

TEST_CASE("错误信息能定位到具体字段与合法区间") {
    InferenceRequest req;
    std::string message;
    CHECK(parse_body(body_with(R"(,"max_tokens":0)"), req, message) == ErrCode::InvalidJson);
    CHECK(message.find("max_tokens") != std::string::npos);
    CHECK(message.find("[1, 4096]") != std::string::npos);
    CHECK(parse_body(body_with(R"(,"top_p":2)"), req, message) == ErrCode::InvalidJson);
    CHECK(message.find("top_p") != std::string::npos);
    CHECK(parse_body(body_with(R"(,"repeat_penalty":0.1)"), req, message) == ErrCode::InvalidJson);
    CHECK(message.find("repeat_penalty") != std::string::npos);
    CHECK(parse_body(body_with(R"(,"seed":1.5)"), req, message) == ErrCode::InvalidJson);
    CHECK(message.find("seed") != std::string::npos);
    // 空 prompt 是语义错误，不是上下文超限：客户端不应据此裁剪 prompt。
    CHECK(parse_body(R"({"prompt":""})", req, message) == ErrCode::InvalidJson);
    CHECK(message.find("prompt") != std::string::npos);
}

TEST_CASE("增量帧是单行 data: 前缀的 JSON 并以空行结束") {
    InferenceRequest req;
    req.req_id = "req-1";
    Delta delta;
    delta.token_id = 9;
    delta.text = "A";
    const std::string frame = render_frame(req, delta);
    REQUIRE(frame.rfind("data: ", 0) == 0);
    REQUIRE(frame.size() >= 2);
    CHECK(frame.substr(frame.size() - 2) == "\n\n");
    CHECK(frame.find("\"token_id\":9") != std::string::npos);
    CHECK(frame.find("\"finish_reason\":null") != std::string::npos);

    delta.finish = FinishReason::LengthCapped;
    CHECK(render_frame(req, delta).find("length") != std::string::npos);
}

TEST_CASE("终止帧固定 14 字节") {
    const std::string done = render_done_frame();
    CHECK(done == "data: [DONE]\n\n");
    CHECK(done.size() == 14);
}

TEST_CASE("非流式体带 usage 且 total 为两项之和") {
    InferenceRequest req;
    req.req_id = "req-2";
    const std::string body = render_completion_body(req, "abc", FinishReason::Stopped, 5, 3);
    CHECK(body.find("\"object\":\"text_completion\"") != std::string::npos);
    CHECK(body.find("\"finish_reason\":\"stop\"") != std::string::npos);
    CHECK(body.find("\"total_tokens\":8") != std::string::npos);
}

TEST_CASE("错误帧携带 code/message/retryable，detail 不外发") {
    InferenceRequest req;
    req.req_id = "req-err";
    Delta delta;
    delta.error = make_error(ErrCode::ExecutionFailed, "前向失败", "op=process rc=1");
    const std::string frame = render_frame(req, delta);
    CHECK(frame.rfind("data: ", 0) == 0);
    CHECK(frame.find("\"code\":\"EXECUTION_FAILED\"") != std::string::npos);
    CHECK(frame.find("\"message\":\"前向失败\"") != std::string::npos);
    CHECK(frame.find("\"retryable\":false") != std::string::npos);
    CHECK(frame.find("op=process") == std::string::npos);
}

TEST_CASE("Error 错误体只出 code/message/retryable") {
    const std::string body =
            render_error_body(make_error(ErrCode::QueueFull, "等待队列已满", "cap=32"));
    CHECK(body.find("\"code\":\"QUEUE_FULL\"") != std::string::npos);
    CHECK(body.find("等待队列已满") != std::string::npos);
    CHECK(body.find("\"retryable\":true") != std::string::npos);
    CHECK(body.find("cap=32") == std::string::npos);
}

TEST_CASE("超时映射 504 且可重试") {
    CHECK(http_status(ErrCode::Timeout) == 504);
    CHECK(std::string(error_code_name(ErrCode::Timeout)) == "TIMEOUT");
    CHECK(retryable(ErrCode::Timeout));
}

TEST_CASE("错误体映射状态码、名称与可重试性") {
    CHECK(http_status(ErrCode::InvalidJson) == 400);
    CHECK(http_status(ErrCode::KvInsufficient) == 429);
    CHECK(http_status(ErrCode::QueueFull) == 429);
    CHECK(http_status(ErrCode::ContextOverflow) == 400);
    CHECK(http_status(ErrCode::NotSupported) == 404);
    CHECK(http_status(ErrCode::UnsupportedField) == 400);
    CHECK(std::string(error_code_name(ErrCode::UnsupportedField)) == "unsupported_field");
    CHECK_FALSE(retryable(ErrCode::UnsupportedField));
    CHECK(std::string(error_code_name(ErrCode::KvInsufficient)) == "KV_INSUFFICIENT");
    CHECK(retryable(ErrCode::QueueFull));
    CHECK_FALSE(retryable(ErrCode::InvalidJson));
    const std::string body = render_error_body(ErrCode::QueueFull, "busy");
    CHECK(body.find("\"retryable\":true") != std::string::npos);
}

TEST_CASE("chat 请求解析出消息列表且不要求 prompt") {
    InferenceRequest req;
    std::string message;
    const std::string body = R"({"model":"m","messages":[{"role":"system","content":"s"},
        {"role":"user","content":[{"type":"text","text":"你"},{"type":"text","text":"好"}]}],
        "stream":false,"max_tokens":16,"seed":7})";
    REQUIRE(parse_chat_body(body, req, message) == ErrCode::Ok);
    REQUIRE(req.chat);
    REQUIRE(req.messages.size() == 2);
    CHECK(req.messages[0].role == "system");
    CHECK(req.messages[1].content == "你好");
    CHECK(req.prompt.empty());
    CHECK(req.sampling.seed == 7);
}

TEST_CASE("chat 越界字段与非法消息一律按 unsupported_field 拒绝") {
    InferenceRequest req;
    std::string message;
    CHECK(parse_chat_body(R"({"messages":[{"role":"user","content":"hi"}],"tools":[]})", req,
                          message) == ErrCode::UnsupportedField);
    CHECK(message.find("unsupported_field: tools") != std::string::npos);
    CHECK(parse_chat_body(R"({"messages":[{"role":"user","content":[{"type":"image_url"}]}]})", req,
                          message) == ErrCode::UnsupportedField);
    CHECK(parse_chat_body(R"({"messages":[{"role":"tool","content":"x"}]})", req, message) ==
          ErrCode::UnsupportedField);
    CHECK(parse_chat_body(R"({"messages":[]})", req, message) == ErrCode::InvalidJson);
    CHECK(parse_chat_body(R"({"messages":[{"role":"user"}]})", req, message) ==
          ErrCode::InvalidJson);
    CHECK(parse_chat_body("not json", req, message) == ErrCode::InvalidJson);
}

TEST_CASE("stop/n 解析边界：stop 最多 4 条，n 仅 1–4，流式 n>1 拒绝") {
    InferenceRequest req;
    std::string message;
    REQUIRE(parse_body(R"({"prompt":"hi","stop":["a","b"],"n":2})", req, message) == ErrCode::Ok);
    REQUIRE(req.stop.size() == 2);
    CHECK(req.n == 2);
    CHECK(parse_body(R"({"prompt":"hi","stop":""})", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(R"({"prompt":"hi","stop":["a","b","c","d","e"]})", req, message) ==
          ErrCode::InvalidJson);
    CHECK(parse_body(R"({"prompt":"hi","n":0})", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(R"({"prompt":"hi","n":5})", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(R"({"prompt":"hi","stream":true,"n":2})", req, message) ==
          ErrCode::UnsupportedField);
}

TEST_CASE("StopFilter 跨帧命中并扣住可能的前缀") {
    StopFilter filter({"END"});
    bool hit = false;
    CHECK(filter.feed("AB", &hit) == "AB");
    CHECK_FALSE(hit);
    CHECK(filter.feed("C", &hit) == "C");
    CHECK_FALSE(hit);
    CHECK(filter.feed("E", &hit).empty());
    CHECK(filter.feed("NDtail", &hit).empty());
    CHECK(hit);
    CHECK(filter.flush().empty());
}

TEST_CASE("StopFilter 无命中时 flush 下发暂存尾部") {
    StopFilter filter({"END"});
    bool hit = false;
    CHECK(filter.feed("hello E", &hit) == "hello ");
    CHECK_FALSE(hit);
    CHECK(filter.flush() == "E");
}

TEST_CASE("chat 帧/非流式体/models 列表符合 OpenAI 形状") {
    InferenceRequest req;
    req.req_id = "req-chat";
    req.chat = true;
    Delta delta;
    delta.text = "hi";
    delta.finish = FinishReason::Stopped;
    const std::string frame = render_frame(req, delta);
    CHECK(frame.find("\"object\":\"chat.completion.chunk\"") != std::string::npos);
    CHECK(frame.find("\"delta\":{\"content\":\"hi\"}") != std::string::npos);

    const std::string body = render_body(
            req, {"a", "b"}, {FinishReason::Stopped, FinishReason::LengthCapped}, 4, {1, 2});
    CHECK(body.find("\"object\":\"chat.completion\"") != std::string::npos);
    CHECK(body.find("\"role\":\"assistant\"") != std::string::npos);
    CHECK(body.find("\"index\":1") != std::string::npos);
    CHECK(body.find("\"completion_tokens\":3") != std::string::npos);

    const std::string models = render_models_body("qwen2.5-0.5b");
    CHECK(models.find("\"object\":\"list\"") != std::string::npos);
    CHECK(models.find("\"id\":\"qwen2.5-0.5b\"") != std::string::npos);
}
