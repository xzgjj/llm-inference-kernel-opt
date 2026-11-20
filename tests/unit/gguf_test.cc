// gguf_test.cc — 只读元数据解析与 KV 估算；用合成头避免依赖真实权重。
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "llmopt/gguf.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace llmopt; // NOLINT：用例短名与契约同名

namespace {

// 小端写入器：GGUF 头字段按规范顺序追加。
struct Writer {
    std::vector<std::uint8_t> bytes;

    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void str(const std::string& text) {
        u64(text.size());
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
};

// 追加一个 u32 类型的 KV 对。
void add_u32(Writer& writer, const std::string& key, std::uint32_t value) {
    writer.str(key);
    writer.u32(4);
    writer.u32(value);
}

// 追加一个字符串类型的 KV 对。
void add_string(Writer& writer, const std::string& key, const std::string& value) {
    writer.str(key);
    writer.u32(8);
    writer.str(value);
}

// 写临时文件并返回路径；用例结束由调用方删除。
std::string write_file(const std::vector<std::uint8_t>& bytes, const std::string& name) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return path;
}

// 合成最小可用 GGUF：架构 qwen2、2 层、4 头、KV 头 2、隐层 128。
std::vector<std::uint8_t> valid_header() {
    Writer writer;
    writer.u32(0x46554747U);
    writer.u32(3);
    writer.u64(0);
    writer.u64(5);
    add_string(writer, "general.architecture", "qwen2");
    add_u32(writer, "qwen2.block_count", 2);
    add_u32(writer, "qwen2.attention.head_count", 4);
    add_u32(writer, "qwen2.attention.head_count_kv", 2);
    add_u32(writer, "qwen2.embedding_length", 128);
    return writer.bytes;
}

} // namespace

TEST_CASE("解析关键字段并推导 key/value 维度") {
    const std::string path = write_file(valid_header(), "llmopt_gguf_ok.gguf");
    const GgufMeta meta = read_gguf_meta(path);
    CHECK(meta.ok);
    CHECK(meta.architecture == "qwen2");
    CHECK(meta.block_count == 2);
    CHECK(meta.head_count == 4);
    CHECK(meta.head_count_kv == 2);
    CHECK(meta.embedding_length == 128);
    CHECK(meta.key_length == 32);
    CHECK(meta.value_length == 32);
    CHECK(meta.file_bytes == static_cast<std::int64_t>(valid_header().size()));
    std::filesystem::remove(path);
}

TEST_CASE("KV 估算按层×ctx×kv 头×(k+v)×2 字节") {
    const std::string path = write_file(valid_header(), "llmopt_gguf_kv.gguf");
    const GgufMeta meta = read_gguf_meta(path);
    const std::int64_t per_token = 2LL * 2 * (32 + 32) * 2;
    CHECK(estimate_kv_bytes(meta, 10) == per_token * 10);
    CHECK(estimate_kv_bytes(meta, 2048) == per_token * 2048);
    CHECK(estimate_kv_bytes(meta, 0) == 0);
    std::filesystem::remove(path);
}

TEST_CASE("magic 错误、版本过低、截断与缺字段都给出原因") {
    std::vector<std::uint8_t> bad_magic = valid_header();
    bad_magic[0] = 0;
    const std::string path_bad = write_file(bad_magic, "llmopt_gguf_bad.gguf");
    const GgufMeta bad = read_gguf_meta(path_bad);
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("magic") != std::string::npos);
    std::filesystem::remove(path_bad);

    const std::string path_trunc = write_file({}, "llmopt_gguf_trunc.gguf");
    const GgufMeta trunc = read_gguf_meta(path_trunc);
    CHECK_FALSE(trunc.ok);
    std::filesystem::remove(path_trunc);

    Writer missing;
    missing.u32(0x46554747U);
    missing.u32(3);
    missing.u64(0);
    missing.u64(1);
    add_u32(missing, "llama.block_count", 2);
    const std::string path_missing = write_file(missing.bytes, "llmopt_gguf_missing.gguf");
    const GgufMeta incomplete = read_gguf_meta(path_missing);
    CHECK_FALSE(incomplete.ok);
    CHECK(incomplete.error.find("缺少") != std::string::npos);
    std::filesystem::remove(path_missing);

    const GgufMeta absent = read_gguf_meta("Z:/definitely/not/here.gguf");
    CHECK_FALSE(absent.ok);
}
