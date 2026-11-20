// runner_raii_test.cc — 4-G0-7：C 句柄由 unique_ptr 持有，异常/早退/重复 release 都不泄漏。
// 只在 LLMOPT_ENABLE_LLAMA=ON 的构建里登记；有真模型时走完整 load→release→reload。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/runner.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace llmopt;

namespace {

ModelConfig config_for(const std::string& path) {
    ModelConfig config;
    config.model_path = path;
    config.n_ctx = 128;
    config.n_ctx_total = 128;
    config.n_batch = 128;
    config.n_ubatch = 64;
    config.n_seq_max = 1;
    config.threads = 1;
    config.n_gpu_layers = 0;
    return config;
}

} // namespace

TEST_CASE("未加载即析构：反复构造/销毁不泄漏") {
    for (int round = 0; round < 20; ++round) {
        auto runner = make_llama_runner();
        REQUIRE(runner != nullptr);
        runner->release();
    }
}

TEST_CASE("加载失败后 release 再加载：早退路径不持有半初始化句柄") {
    auto runner = make_llama_runner();
    REQUIRE(runner != nullptr);
    const ModelConfig bad = config_for("definitely-not-a-model.gguf");
    CHECK(runner->load_model(bad) == ErrCode::ModelNotLoaded);
    runner->release();
    CHECK(runner->load_model(bad) == ErrCode::ModelNotLoaded);
    runner->release();
    runner->release();
}

TEST_CASE("真模型 load->tokenize->release->reload") {
    const char* model = std::getenv("LLMOPT_TEST_MODEL");
    if (model == nullptr || model[0] == '\0') {
        return; // 未提供模型时只验证句柄生命周期。
    }
    auto runner = make_llama_runner();
    REQUIRE(runner != nullptr);
    const ModelConfig config = config_for(model);
    const ErrCode loaded = runner->load_model(config);
    REQUIRE(loaded == ErrCode::Ok);
    std::vector<std::int32_t> ids;
    CHECK(runner->tokenize("hello", ids) == ErrCode::Ok);
    CHECK_FALSE(ids.empty());
    runner->release();
    REQUIRE(runner->load_model(config) == ErrCode::Ok);
    runner->release();
}
