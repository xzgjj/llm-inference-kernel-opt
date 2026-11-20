// gguf.cpp — 只读 GGUF 元数据头，供启动 L2 估算层数/头数/维度。
// 不 mmap、不加载张量，未知类型按宽度跳过，解析失败返回 ok=false 并写明原因。
#include "llmopt/gguf.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace llmopt {
namespace {

// GGUF 元数据值类型编号，与 gguf 规范一致。
enum class GgufType : std::uint32_t {
    Uint8 = 0,
    Int8 = 1,
    Uint16 = 2,
    Int16 = 3,
    Uint32 = 4,
    Int32 = 5,
    Float32 = 6,
    Bool = 7,
    String = 8,
    Array = 9,
    Uint64 = 10,
    Int64 = 11,
    Float64 = 12
};

constexpr std::uint64_t kMaxStringBytes = 1ULL << 20;
constexpr std::uint64_t kMaxArrayCount = 1ULL << 20;
constexpr std::uint32_t kMaxKvPairs = 4096;

// 顺序读取器：只提供定长读、字符串读与跳过，任何越界都让 good() 变 false。
class Reader {
public:
    explicit Reader(const std::string& path) : stream_(path, std::ios::binary) {}

    // 流是否仍可读；解析循环用它判断提前结束。
    bool good() const {
        return stream_.good();
    }

    // 读 1/4/8 字节无符号整数与有符号 32 位、float、double。
    bool read_u8(std::uint8_t& out) {
        return read_raw(&out, sizeof(out));
    }
    bool read_u16(std::uint16_t& out) {
        return read_raw(&out, sizeof(out));
    }
    bool read_u32(std::uint32_t& out) {
        return read_raw(&out, sizeof(out));
    }
    bool read_u64(std::uint64_t& out) {
        return read_raw(&out, sizeof(out));
    }
    bool read_i32(std::int32_t& out) {
        return read_raw(&out, sizeof(out));
    }
    bool read_f32(float& out) {
        return read_raw(&out, sizeof(out));
    }
    bool read_f64(double& out) {
        return read_raw(&out, sizeof(out));
    }

    // 按原生宽度读一个算术标量；仅用于定长数值类型。
    template <typename T>
    bool read_value(T& out) {
        return read_raw(&out, sizeof(out));
    }

    // 读 GGUF 字符串：uint64 长度 + 原始字节；超长视为损坏。
    bool read_string(std::string& out) {
        std::uint64_t size = 0;
        if (!read_u64(size) || size > kMaxStringBytes) {
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        return size == 0 || read_raw(out.data(), out.size());
    }

    // 跳过指定字节数；用于不需要值的字符串与数组元素。
    bool skip_bytes(std::uint64_t count) {
        stream_.seekg(static_cast<std::streamoff>(count), std::ios::cur);
        return stream_.good();
    }

private:
    // 原始读：gcount 必须等于请求长度，短读即失败。
    bool read_raw(void* data, std::size_t size) {
        stream_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(stream_.gcount()) == size;
    }

    std::ifstream stream_;
};

// 读定长数值并按 u32 截断；维度字段不会超出 u32 语义。
template <typename T>
bool store_scalar(Reader& reader, std::uint32_t& out) {
    T value{};
    if (!reader.read_value(value)) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

// 读一个标量并转成 u32；只接受整数与浮点类型，bool/string 不参与维度统计。
bool read_scalar_u32(Reader& reader, GgufType type, std::uint32_t& out) {
    switch (type) {
    case GgufType::Uint8:
    case GgufType::Int8:
        return store_scalar<std::uint8_t>(reader, out);
    case GgufType::Uint16:
    case GgufType::Int16:
        return store_scalar<std::uint16_t>(reader, out);
    case GgufType::Uint32:
    case GgufType::Int32:
        return store_scalar<std::uint32_t>(reader, out);
    case GgufType::Uint64:
    case GgufType::Int64:
        return store_scalar<std::uint64_t>(reader, out);
    case GgufType::Float32:
        return store_scalar<float>(reader, out);
    case GgufType::Float64:
        return store_scalar<double>(reader, out);
    default:
        return false;
    }
}

// 消费一个值：标量写入 scalar，数组逐元素递归并取最大值写入 array_max。
bool consume_value(Reader& reader, GgufType type, std::uint32_t& scalar, std::uint32_t& array_max,
                   int depth) {
    if (depth > 3) {
        return false;
    }
    if (type == GgufType::String) {
        std::string ignored;
        return reader.read_string(ignored);
    }
    if (type == GgufType::Bool) {
        std::uint8_t flag = 0;
        return reader.read_u8(flag);
    }
    if (type == GgufType::Array) {
        std::uint32_t element_type = 0;
        std::uint64_t count = 0;
        if (!reader.read_u32(element_type) || !reader.read_u64(count) || count > kMaxArrayCount) {
            return false;
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            std::uint32_t value = 0;
            std::uint32_t nested = 0;
            if (!consume_value(reader, static_cast<GgufType>(element_type), value, nested,
                               depth + 1)) {
                return false;
            }
            array_max = value > array_max ? value : array_max;
            array_max = nested > array_max ? nested : array_max;
        }
        return true;
    }
    return read_scalar_u32(reader, type, scalar);
}

// 按 key 的架构前缀与字段后缀写入；Qwen 系用 `qwen2.*`，旧模型用 `llama.*`。
void assign_field(const std::string& key, std::uint32_t scalar, std::uint32_t array_max,
                  GgufMeta& meta) {
    const std::size_t dot = key.find('.');
    const std::string suffix = dot == std::string::npos ? key : key.substr(dot + 1);
    const std::uint32_t value = scalar != 0 ? scalar : array_max;
    if (suffix == "block_count") {
        meta.block_count = static_cast<std::int32_t>(value);
    } else if (suffix == "attention.head_count") {
        meta.head_count = static_cast<std::int32_t>(value);
    } else if (suffix == "attention.head_count_kv") {
        meta.head_count_kv = static_cast<std::int32_t>(value);
    } else if (suffix == "embedding_length") {
        meta.embedding_length = static_cast<std::int32_t>(value);
    } else if (suffix == "attention.key_length") {
        meta.key_length = static_cast<std::int32_t>(value);
    } else if (suffix == "attention.value_length") {
        meta.value_length = static_cast<std::int32_t>(value);
    } else if (suffix == "context_length") {
        meta.context_length = static_cast<std::int32_t>(value);
    }
}

// 解析全部 KV 对；只保留估算所需字段，其余按类型跳过。
bool parse_pairs(Reader& reader, std::uint64_t kv_count, GgufMeta& meta) {
    if (kv_count > kMaxKvPairs) {
        meta.error = "KV 对数量异常";
        return false;
    }
    for (std::uint64_t index = 0; index < kv_count; ++index) {
        std::string key;
        std::uint32_t type = 0;
        if (!reader.read_string(key) || !reader.read_u32(type)) {
            meta.error = "KV 头读取失败";
            return false;
        }
        std::uint32_t scalar = 0;
        std::uint32_t array_max = 0;
        if (key == "general.architecture" && type == static_cast<std::uint32_t>(GgufType::String)) {
            if (!reader.read_string(meta.architecture)) {
                meta.error = "general.architecture 读取失败";
                return false;
            }
            continue;
        }
        if (!consume_value(reader, static_cast<GgufType>(type), scalar, array_max, 0)) {
            meta.error = "KV 值读取失败: " + key;
            return false;
        }
        assign_field(key, scalar, array_max, meta);
    }
    return true;
}

} // namespace

// 打开文件读 magic/version/计数，再解析 KV；缺关键字段时给出可读原因。
GgufMeta read_gguf_meta(const std::string& path) {
    GgufMeta meta;
    try {
        meta.file_bytes = static_cast<std::int64_t>(std::filesystem::file_size(path));
    } catch (const std::filesystem::filesystem_error& exc) {
        meta.error = std::string("读文件大小失败: ") + exc.what();
        return meta;
    }
    Reader reader(path);
    if (!reader.good()) {
        meta.error = "无法打开模型文件";
        return meta;
    }
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t tensor_count = 0;
    std::uint64_t kv_count = 0;
    if (!reader.read_u32(magic) || !reader.read_u32(version) || !reader.read_u64(tensor_count) ||
        !reader.read_u64(kv_count)) {
        meta.error = "GGUF 头读取失败";
        return meta;
    }
    if (magic != 0x46554747U) {
        meta.error = "magic 不是 GGUF";
        return meta;
    }
    if (version < 2) {
        meta.error = "仅支持 GGUF v2+";
        return meta;
    }
    (void)tensor_count;
    if (!parse_pairs(reader, kv_count, meta)) {
        return meta;
    }
    if (meta.block_count <= 0 || meta.head_count <= 0 || meta.embedding_length <= 0) {
        meta.error = "缺少 block_count/head_count/embedding_length";
        return meta;
    }
    if (meta.head_count_kv <= 0) {
        meta.head_count_kv = meta.head_count;
    }
    if (meta.key_length <= 0) {
        meta.key_length = meta.embedding_length / meta.head_count;
    }
    if (meta.value_length <= 0) {
        meta.value_length = meta.key_length;
    }
    meta.ok = true;
    return meta;
}

// KV 估算：层数 × ctx × kv 头数 × (k 维 + v 维) × 2 字节（f16）。
std::int64_t estimate_kv_bytes(const GgufMeta& meta, std::int32_t ctx_tokens) {
    if (!meta.ok || ctx_tokens <= 0) {
        return 0;
    }
    const std::int64_t per_token = static_cast<std::int64_t>(meta.block_count) *
                                   meta.head_count_kv * (meta.key_length + meta.value_length) * 2;
    return per_token * ctx_tokens;
}

} // namespace llmopt
