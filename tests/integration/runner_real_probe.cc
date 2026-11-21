// runner_real_probe.cc — 真模型 load→tokenize→release→reload 的进程级探针。
// 只在显式提供 LLMOPT_TEST_MODEL 时由 test_runner_real.py 调用；不做早退。
#include "llmopt/runner.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

llmopt::ModelConfig config_for(const std::string& path) {
    llmopt::ModelConfig config;
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

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::string model = argv[1];
    auto runner = llmopt::make_llama_runner();
    if (runner == nullptr) {
        return 3;
    }
    const llmopt::ModelConfig config = config_for(model);
    if (runner->load_model(config) != llmopt::ErrCode::Ok) {
        return 4;
    }
    std::vector<std::int32_t> ids;
    if (runner->tokenize("hello", ids) != llmopt::ErrCode::Ok || ids.empty()) {
        return 5;
    }
    runner->release();
    std::vector<std::int32_t> after_release;
    if (runner->tokenize("hello", after_release) != llmopt::ErrCode::ModelNotLoaded) {
        return 6;
    }
    if (!runner->detokenize(1).empty()) {
        return 7;
    }
    if (runner->is_eos(1)) {
        return 8;
    }
    if (runner->load_model(config) != llmopt::ErrCode::Ok) {
        return 9;
    }
    runner->release();
    return 0;
}
