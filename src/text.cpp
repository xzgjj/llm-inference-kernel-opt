// text.cpp — UTF-8 完整性扫描：只放行可安全编码为 JSON 的完整前缀。
#include "llmopt/text.hpp"

#include <cstdint>

namespace llmopt {
namespace {

// 按首字节判断后续需要的续字节数；非法首字节返回 -1。
std::int32_t continuation_count(unsigned char byte) noexcept {
    if ((byte & 0x80U) == 0) {
        return 0;
    }
    if ((byte & 0xE0U) == 0xC0U) {
        return 1;
    }
    if ((byte & 0xF0U) == 0xE0U) {
        return 2;
    }
    if ((byte & 0xF8U) == 0xF0U) {
        return 3;
    }
    return -1;
}

} // namespace

std::size_t utf8_complete_prefix(const std::string& text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const std::int32_t need = continuation_count(static_cast<unsigned char>(text[index]));
        if (need < 0) {
            return index;
        }
        if (index + static_cast<std::size_t>(need) >= text.size()) {
            return index;
        }
        for (std::int32_t offset = 1; offset <= need; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return index;
            }
        }
        index += static_cast<std::size_t>(need) + 1;
    }
    return index;
}

} // namespace llmopt
