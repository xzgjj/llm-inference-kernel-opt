// crash.cpp — 阶段 12 S5：崩溃信号处理器。
//
// 约束（async-signal-safety）：
// - 注册期（install）允许 std::filesystem / std::string 准备路径与目录；
// - 信号 handler 内只用 open/read/write/close/unlink 等裸系统调用与预格式化，
//   不 malloc、不 iostream、不加锁；用 std::atomic 标志防重入。
// - dump 落盘后恢复默认处置并 raise(sig) 重新投递，保留原始信号退出语义。
// - 任何失败只 fprintf(stderr, ...) 告警，不影响安装与启动。

#include "llmopt/crash.hpp"

#include "llmopt/version.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
// windows.h 必须先于 dbghelp.h（psdk_inc 依赖其基础类型与 CALLBACK 宏）。
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#else
#include <dirent.h>
#include <execinfo.h>
#include <signal.h>
#endif

namespace llmopt {
namespace {

// ---- 注册期准备好的只读状态（handler 内不触碰堆分配接口）----
std::atomic<bool> g_handler_active{false};
std::string g_log_path;  // <log_dir>/llmopt.log
std::string g_crash_dir; // <log_dir 父目录>/crash
char g_version[128] = "unknown";
char g_llama_revision[64] = "unknown";

// 1 MiB 静态 BSS 缓冲区：读日志尾部用，不占堆、不占 handler 栈。
char g_tail_buf[1 << 20];

#ifdef _WIN32
HANDLE g_process = nullptr;
#endif

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGABRT:
        return "SIGABRT";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
    default:
        return "SIGUNKNOWN";
    }
}

int write_all(int fd, const char* buf, int len) {
    int off = 0;
    while (off < len) {
        const int w = static_cast<int>(::write(fd, buf + off, len - off));
        if (w <= 0) {
            return -1;
        }
        off += w;
    }
    return 0;
}

void format_timestamp(char* out, std::size_t cap) {
    const std::time_t t = std::time(nullptr);
    struct tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::strftime(out, cap, "%Y%m%d-%H%M%S", &tm_buf);
}

// 从日志文件尾部读最多 cap 字节；任何失败都返回已读字节数并保证 fd 关闭。
ssize_t read_log_tail(const std::size_t cap) {
    const int lfd = ::open(g_log_path.c_str(), O_RDONLY);
    if (lfd < 0) {
        return -1;
    }
    struct stat st{};
    if (::fstat(lfd, &st) != 0 || st.st_size <= 0) {
        ::close(lfd);
        return 0;
    }
    const off_t size = st.st_size;
    const off_t want = size > static_cast<off_t>(cap) ? static_cast<off_t>(cap) : size;
    const off_t start_off = size - want;
    ssize_t got = 0;
#ifdef _WIN32
    if (::_lseek(lfd, start_off, SEEK_SET) == (off_t)-1) {
        ::close(lfd);
        return -1;
    }
    while (got < want) {
        const int r = ::read(lfd, g_tail_buf + got, static_cast<unsigned int>(want - got));
        if (r <= 0) {
            break;
        }
        got += r;
    }
#else
    got = ::pread(lfd, g_tail_buf, want, start_off);
    if (got < 0) {
        got = 0;
    }
#endif
    ::close(lfd);
    return got;
}

// 从尾部回数 100 个换行，返回第 100 行起点（不足 100 行则 0）。
off_t last_lines_start(ssize_t got) {
    off_t start = 0;
    int lines = 0;
    for (ssize_t i = got - 1; i >= 0; --i) {
        if (g_tail_buf[i] == '\n') {
            ++lines;
            if (lines == 100) {
                start = i + 1;
                break;
            }
        }
    }
    return start;
}

// 日志尾部：最多取缓冲区大小的末尾内容，保留最后 100 行。
void append_log_tail(int fd) {
    const char* header = "---- recent log tail (last 100 lines) ----\n";
    write_all(fd, header, static_cast<int>(std::strlen(header)));
    const ssize_t got = read_log_tail(sizeof(g_tail_buf));
    if (got < 0) {
        const char* missing = "(log 不可读，跳过)\n";
        write_all(fd, missing, static_cast<int>(std::strlen(missing)));
        return;
    }
    if (got == 0) {
        const char* empty = "(log 为空)\n";
        write_all(fd, empty, static_cast<int>(std::strlen(empty)));
        return;
    }
    const off_t start = last_lines_start(got);
    write_all(fd, g_tail_buf + start, static_cast<int>(got - start));
}

void append_stack_trace(int fd) {
    const char* header = "---- stack trace ----\n";
    write_all(fd, header, static_cast<int>(std::strlen(header)));
#ifdef _WIN32
    void* frames[64] = {};
    const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    char line[512];
    for (USHORT i = 0; i < count; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        // SYMBOL_INFO 内嵌定长名字缓冲区，避免 handler 内堆分配。
        struct {
            SYMBOL_INFO info;
            char name[256];
        } sym{};
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen = 256;
        DWORD64 disp = 0;
        const BOOL resolved =
                g_process != nullptr && SymFromAddr(g_process, addr, &disp, &sym.info);
        if (resolved) {
            const int len = std::snprintf(line, sizeof(line), "  #%02d 0x%llx %s+0x%llx\n", i,
                                          static_cast<unsigned long long>(addr), sym.info.Name,
                                          static_cast<unsigned long long>(disp));
            write_all(fd, line, len);
        } else {
            const int len = std::snprintf(line, sizeof(line), "  #%02d 0x%llx\n", i,
                                          static_cast<unsigned long long>(addr));
            write_all(fd, line, len);
        }
    }
#else
    void* frames[64] = {};
    const int count = backtrace(frames, 64);
    // backtrace_symbols_fd 内部不堆分配，直接把符号行写到 fd。
    backtrace_symbols_fd(frames, count, fd);
#endif
}

// 定长文件名拷贝：截断到 259 字节并补 NUL，避免 strncpy 的 -Wstringop-truncation。
void copy_name(char* dst, const char* src) {
    const std::size_t len = std::strlen(src);
    const std::size_t n = len < 259 ? len : 259;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

// 收集 crash-*.txt 文件名到定长缓冲；返回条数（最多 64）。
int collect_dump_names(const char* dir, char (*names)[260]) {
    int count = 0;
#ifdef _WIN32
    char pattern[512];
    std::snprintf(pattern, sizeof(pattern), "%s\\crash-*.txt", dir);
    WIN32_FIND_DATAA fdata{};
    HANDLE h = FindFirstFileA(pattern, &fdata);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (count < 64) {
            copy_name(names[count], fdata.cFileName);
            ++count;
        }
    } while (FindNextFileA(h, &fdata) && count < 64);
    FindClose(h);
#else
    DIR* d = ::opendir(dir);
    if (d == nullptr) {
        return 0;
    }
    while (dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, "crash-", 6) != 0) {
            continue;
        }
        if (count < 64) {
            copy_name(names[count], e->d_name);
            ++count;
        }
    }
    ::closedir(d);
#endif
    return count;
}

// 保留最近 2 份 dump：文件名时间戳前缀保证字典序≈时间序；超出 2 份删最旧。
void prune_dumps(const char* dir) {
    constexpr int kMaxKeep = 2;
    constexpr std::size_t kNameCap = 260;
    char names[64][kNameCap] = {};
    const int count = collect_dump_names(dir, names);

    // 简单插入排序：字典序降序（新的在前）。
    for (int i = 1; i < count; ++i) {
        char tmp[kNameCap];
        copy_name(tmp, names[i]);
        int j = i - 1;
        while (j >= 0 && std::strcmp(names[j], tmp) < 0) {
            copy_name(names[j + 1], names[j]);
            --j;
        }
        copy_name(names[j + 1], tmp);
    }

    for (int i = kMaxKeep; i < count; ++i) {
        char full[512];
#ifdef _WIN32
        std::snprintf(full, sizeof(full), "%s\\%s", dir, names[i]);
        ::remove(full);
#else
        std::snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
        ::unlink(full);
#endif
    }
}

void crash_handler(int sig) {
    bool expected = false;
    if (!g_handler_active.compare_exchange_strong(expected, true)) {
        return; // 重入：直接走默认处置，不再重复落盘。
    }

    char ts[32];
    format_timestamp(ts, sizeof(ts));
    char path[512];
    const int pid =
#ifdef _WIN32
            static_cast<int>(GetCurrentProcessId());
#else
            ::getpid();
#endif
    std::snprintf(path, sizeof(path), "%s/crash-%s-%d.txt", g_crash_dir.c_str(), ts, pid);

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "[crash] dump 打开失败 path=%s errno=%d\n", path, errno);
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }

    char line[384];
    int len = std::snprintf(line, sizeof(line), "signal=%s pid=%d dump=%s\n", signal_name(sig), pid,
                            path);
    write_all(fd, line, len);
    len = std::snprintf(line, sizeof(line), "version=%s llama_revision=%s\n", g_version,
                        g_llama_revision);
    write_all(fd, line, len);
    len = std::snprintf(line, sizeof(line), "shutdown_state=not-tracked-by-crash-handler\n");
    write_all(fd, line, len);

    append_stack_trace(fd);
    append_log_tail(fd);
    ::close(fd);

    prune_dumps(g_crash_dir.c_str());

    // 恢复默认处置并重新投递，保留原始信号退出码/语义。
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

void install_crash_handler(const std::string& log_dir) {
    g_log_path = log_dir + "/llmopt.log";
    const auto slash = log_dir.find_last_of("/\\");
    const std::string parent = slash == std::string::npos ? "." : log_dir.substr(0, slash);
    g_crash_dir = parent + "/crash";

    std::error_code ec;
    std::filesystem::create_directories(g_crash_dir, ec);
    if (ec) {
        std::fprintf(stderr, "[crash] dump 目录创建失败 dir=%s ec=%s\n", g_crash_dir.c_str(),
                     ec.message().c_str());
    }

    std::snprintf(g_version, sizeof(g_version), "%.*s", static_cast<int>(kVersion.size()),
                  kVersion.data());
    std::snprintf(g_llama_revision, sizeof(g_llama_revision), "%.*s",
                  static_cast<int>(kLlamaRevision.size()), kLlamaRevision.data());

#ifdef _WIN32
    g_process = GetCurrentProcess();
    SymInitialize(g_process, nullptr, TRUE);
#endif

    const int signals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL};
    for (int sig : signals) {
        if (std::signal(sig, crash_handler) == SIG_ERR) {
            std::fprintf(stderr, "[crash] signal 注册失败 sig=%d errno=%d\n", sig, errno);
        }
    }
    std::fprintf(stderr, "[crash] handler installed; dump dir=%s\n", g_crash_dir.c_str());
}

} // namespace llmopt
