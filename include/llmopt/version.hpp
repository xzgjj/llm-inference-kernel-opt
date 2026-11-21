// version.hpp — 版本号唯一事实来源：CMake project(VERSION) → LLMOPT_VERSION 宏。
// 任何出口（--version、启动日志、/metrics build_info）都读这里，不再各自硬编码。
#ifndef LLMOPT_VERSION_HPP
#define LLMOPT_VERSION_HPP

#include <string_view>

#ifndef LLMOPT_VERSION
#define LLMOPT_VERSION "0.0.0-dev"
#endif

#ifndef LLMOPT_LLAMA_REVISION
#define LLMOPT_LLAMA_REVISION "unknown"
#endif

namespace llmopt {

inline constexpr std::string_view kVersion = LLMOPT_VERSION;
inline constexpr std::string_view kLlamaRevision = LLMOPT_LLAMA_REVISION;

} // namespace llmopt

#endif // LLMOPT_VERSION_HPP
