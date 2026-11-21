// sha256.hpp — 静态 key 落盘只用 SHA-256；无 OpenSSL 构建也必须可用。
#ifndef LLMOPT_SHA256_HPP
#define LLMOPT_SHA256_HPP

#include <string>
#include <string_view>

namespace llmopt {

// 返回 64 位小写十六进制；输入按字节处理，不做任何截断。
[[nodiscard]] std::string sha256_hex(std::string_view data);

} // namespace llmopt

#endif // LLMOPT_SHA256_HPP
