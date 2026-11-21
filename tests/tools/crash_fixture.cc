// crash_fixture.cc — 阶段 12 S5：崩溃注入 fixture（不登记 CTest，不依赖 llmopt_core）。
//
// 独立编译（msys2 ucrt64 g++）：
//   g++ -std=c++20 -I include src/crash.cpp tests/tools/crash_fixture.cc
//   -o build-b/crash_fixture.exe -ldbghelp
//
// 行为：安装 handler（log_dir=logs/server，dump 落 logs/crash/）→ raise(SIGSEGV)，
// 期望 handler 落盘 dump 后恢复默认处置，进程以 SEGV 语义退出。

#include "llmopt/crash.hpp"

#include <csignal>
#include <cstdio>

int main() {
    llmopt::install_crash_handler("logs/server");
    std::fprintf(stderr, "[fixture] handler installed, raising SIGSEGV\n");
    std::raise(SIGSEGV);
    std::fprintf(stderr, "[fixture] unexpected: survived SIGSEGV\n");
    return 0;
}
