// crash.hpp — 阶段 12 S5：崩溃信号捕获与 dump 落盘。
//
// install_crash_handler 注册 SIGSEGV/SIGABRT/SIGFPE/SIGILL；
// dump 落到 <log_dir 父目录>/crash/crash-<YYYYmmdd-HHMMSS>-<pid>.txt
// （log_dir=logs/server → logs/crash/），只保留最近 2 份。
// 安装失败只告警 stderr，不影响服务启动。
#ifndef LLMOPT_CRASH_HPP
#define LLMOPT_CRASH_HPP

#include <string>

namespace llmopt {

// 安装崩溃处理器；log_dir 与服务运行日志同目录（llmopt.log 位于其下）。
void install_crash_handler(const std::string& log_dir);

} // namespace llmopt

#endif // LLMOPT_CRASH_HPP
