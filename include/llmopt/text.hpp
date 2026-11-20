// text.hpp — 流式文本的 UTF-8 安全切分；token piece 可能是半个汉字，不能直接进 JSON。
#ifndef LLMOPT_TEXT_HPP
#define LLMOPT_TEXT_HPP

#include <cstddef>
#include <string>

namespace llmopt {

// 返回 text 中最长完整 UTF-8 前缀的字节数；末尾残缺序列不计入。
std::size_t utf8_complete_prefix(const std::string& text) noexcept;

} // namespace llmopt

#endif // LLMOPT_TEXT_HPP
