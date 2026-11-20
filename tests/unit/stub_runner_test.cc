// stub_runner_test.cc — 假后端的确定性、字节级往返与边界契约。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/runner.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 组装 prefill 批次，只填 stub 需要的字段。
Batch prefill_batch(const RequestId& id, const std::vector<std::int32_t>& tokens) {
    Batch batch;
    NewRequestData data;
    data.req_id = id;
    data.prefill_token_ids = tokens;
    batch.new_reqs.push_back(std::move(data));
    batch.total_tokens = static_cast<std::int32_t>(tokens.size());
    return batch;
}

// 取第一个采样 token，省去重复下标。
std::int32_t first_token(const RunnerStepOutput& output) {
    REQUIRE(output.sampled.size() == 1);
    return output.sampled.front().token_id;
}

} // namespace

TEST_CASE("分词按 UTF-8 字节且可逆") {
    auto runner = make_stub_runner();
    std::vector<std::int32_t> ids;
    REQUIRE(runner->tokenize("AB", ids) == ErrCode::Ok);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 256 + 65);
    CHECK(ids[1] == 256 + 66);
    CHECK(runner->detokenize(ids[0]) == "A");
    CHECK(runner->detokenize(1).empty());
}

TEST_CASE("同种子同 prompt 输出逐 token 可复现") {
    auto runner = make_stub_runner();
    ModelConfig config;
    config.n_ctx = 512;
    REQUIRE(runner->load_model(config) == ErrCode::Ok);
    CHECK(runner->n_ctx() == 512);
    CHECK(std::string(runner->backend_name()) == "stub");

    Sequence seq;
    seq.group_id = "req-det";
    seq.sampling.seed = 7;
    seq.prompt_token_ids = {256 + 104, 256 + 105};
    REQUIRE(runner->add_sequence(seq) == ErrCode::Ok);

    const std::int32_t first =
            first_token(runner->decode_step(prefill_batch("req-det", {256 + 104, 256 + 105})));

    auto again = make_stub_runner();
    REQUIRE(again->load_model(config) == ErrCode::Ok);
    REQUIRE(again->add_sequence(seq) == ErrCode::Ok);
    CHECK(first_token(again->decode_step(prefill_batch("req-det", {256 + 104, 256 + 105}))) ==
          first);

    Sequence other;
    other.group_id = seq.group_id;
    other.sampling.seed = 8;
    other.prompt_token_ids = seq.prompt_token_ids;
    REQUIRE(again->add_sequence(other) == ErrCode::Ok);
    CHECK(first_token(again->decode_step(prefill_batch("req-det", {256 + 104, 256 + 105}))) !=
          first);
}

TEST_CASE("decode 步推进 n_past，释放后状态被清除") {
    auto runner = make_stub_runner();
    ModelConfig config;
    REQUIRE(runner->load_model(config) == ErrCode::Ok);
    Sequence seq;
    seq.group_id = "req-step";
    seq.sampling.seed = 1;
    REQUIRE(runner->add_sequence(seq) == ErrCode::Ok);
    RunnerStepOutput prefill = runner->decode_step(prefill_batch("req-step", {1, 2, 3}));
    CHECK(prefill.err == ErrCode::Ok);

    Batch decode;
    CachedRequestData cached;
    cached.req_id = "req-step";
    cached.new_token_ids = {4};
    decode.cached_reqs.push_back(cached);
    decode.total_tokens = 1;
    CHECK(runner->decode_step(decode).err == ErrCode::Ok);
    runner->release_sequence("req-step");
    CHECK(runner->is_eos(0) == false);
}
