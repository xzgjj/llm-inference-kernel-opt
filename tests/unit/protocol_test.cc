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
    CHECK(parse_body(R"({"prompt":""})", req, message) == ErrCode::PromptTooLong);
    CHECK(parse_body(R"({"prompt":1})", req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(body_with(R"(,"max_tokens":0)"), req, message) == ErrCode::InvalidJson);
    CHECK(parse_body(body_with(R"(,"temperature":3,"top_p":1,"min_p":0)"), req, message) ==
          ErrCode::InvalidJson);
    CHECK(parse_body(body_with(R"(,"stream":1)"), req, message) == ErrCode::InvalidJson);
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

TEST_CASE("错误体映射状态码、名称与可重试性") {
    CHECK(http_status(ErrCode::InvalidJson) == 400);
    CHECK(http_status(ErrCode::KvInsufficient) == 429);
    CHECK(http_status(ErrCode::QueueFull) == 429);
    CHECK(http_status(ErrCode::ContextOverflow) == 400);
    CHECK(http_status(ErrCode::NotSupported) == 404);
    CHECK(std::string(error_code_name(ErrCode::KvInsufficient)) == "KV_INSUFFICIENT");
    CHECK(retryable(ErrCode::QueueFull));
    CHECK_FALSE(retryable(ErrCode::InvalidJson));
    const std::string body = render_error_body(ErrCode::QueueFull, "busy");
    CHECK(body.find("\"retryable\":true") != std::string::npos);
}
