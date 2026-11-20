// gguf.hpp — 只读 GGUF 头，取启动估算所需的层数/头数/维度；不加载权重。
#ifndef LLMOPT_GGUF_HPP
#define LLMOPT_GGUF_HPP

#include <cstdint>
#include <string>

namespace llmopt {

struct GgufMeta {
    bool ok = false;
    std::int64_t file_bytes = 0;
    std::string architecture;
    std::int32_t block_count = 0;
    std::int32_t head_count = 0;
    std::int32_t head_count_kv = 0;
    std::int32_t embedding_length = 0;
    std::int32_t key_length = 0;
    std::int32_t value_length = 0;
    std::int32_t context_length = 0;
    std::string error;
};

// 只解析元数据，遇到未知类型按类型宽度跳过；失败时 ok=false 且 error 说明原因。
GgufMeta read_gguf_meta(const std::string& path);
// KV 字节估算：block_count × ctx × head_count_kv × (key+value) × 2 字节（f16）。
std::int64_t estimate_kv_bytes(const GgufMeta& meta, std::int32_t ctx_tokens);

} // namespace llmopt

#endif // LLMOPT_GGUF_HPP
