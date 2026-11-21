// resource.cpp — 进程级 RSS/VRAM 采样：RSS 走 OS API，VRAM 按 pid 读 nvidia-smi。
// 采不到 VRAM 时写 NaN + probe_ok=0；绝不把整卡占用冒充成本进程占用（4-G0-3）。
#include "llmopt/resource.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#ifdef _WIN32
// clang-format off
// psapi.h 依赖 windows.h 先定义 DWORD/HANDLE，顺序不能按字母排。
#include <windows.h>
#include <psapi.h>
// clang-format on
#else
#include <unistd.h>
#endif

namespace llmopt {
namespace {

constexpr double kMib = 1024.0 * 1024.0;
constexpr std::int32_t kVramProbeEvery = 4;

std::int32_t current_pid() {
#ifdef _WIN32
    return static_cast<std::int32_t>(GetCurrentProcessId());
#else
    return static_cast<std::int32_t>(getpid());
#endif
}

bool read_rss(double& out_mib) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return false;
    }
    out_mib = static_cast<double>(counters.WorkingSetSize) / kMib;
    return true;
#else
    FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return false;
    }
    long size_pages = 0;
    long resident_pages = 0;
    const bool parsed = std::fscanf(file, "%ld %ld", &size_pages, &resident_pages) == 2;
    std::fclose(file);
    if (!parsed) {
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    out_mib = static_cast<double>(resident_pages) * static_cast<double>(page_size) / kMib;
    return true;
#endif
}

// 从 "1234, 567" 里取 pid 与 MiB；pid 不匹配的行直接跳过。
bool parse_compute_apps(const std::string& text, std::int32_t pid, double& out_mib) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = text.find('\n', pos);
        const std::string line =
                text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        const std::size_t comma = line.find(',');
        if (comma == std::string::npos) {
            continue;
        }
        const std::int32_t line_pid = std::atoi(line.substr(0, comma).c_str());
        if (line_pid != pid) {
            continue;
        }
        const double mib = std::atof(line.substr(comma + 1).c_str());
        if (mib > 0.0) {
            out_mib = mib;
            return true;
        }
    }
    return false;
}

FILE* open_pipe(const char* command) {
#ifdef _WIN32
    return _popen(command, "r");
#else
    return popen(command, "r");
#endif
}

int close_pipe(FILE* pipe) {
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

bool probe_vram_impl(double& out_mib) {
#ifdef _WIN32
    const char* command =
            "nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>NUL";
#else
    const char* command =
            "nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits "
            "2>/dev/null";
#endif
    FILE* pipe = open_pipe(command);
    if (pipe == nullptr) {
        return false;
    }
    std::string text;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        text += buffer;
    }
    const int status = close_pipe(pipe);
    if (status != 0) {
        return false;
    }
    return parse_compute_apps(text, current_pid(), out_mib);
}

bool probe_vram_device_impl(double& out_mib) {
#ifdef _WIN32
    const char* command = "nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>NUL";
#else
    const char* command =
            "nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null";
#endif
    FILE* pipe = open_pipe(command);
    if (pipe == nullptr) {
        return false;
    }
    char buffer[64] = {0};
    const bool got = std::fgets(buffer, sizeof(buffer), pipe) != nullptr;
    const int status = close_pipe(pipe);
    if (!got || status != 0) {
        return false;
    }
    const double value = std::atof(buffer);
    if (value <= 0.0) {
        return false;
    }
    out_mib = value;
    return true;
}

} // namespace

ResourceSampler::ResourceSampler(MetricsRegistry& metrics, std::chrono::milliseconds interval)
    : metrics_(metrics),
      interval_(interval > std::chrono::milliseconds(0) ? interval
                                                        : std::chrono::milliseconds(500)) {}

ResourceSampler::~ResourceSampler() {
    stop();
}

void ResourceSampler::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    stopping_ = false;
    running_ = true;
    // 先同步发布一次 RSS：避免 /metrics 在采样线程首次调度前读到 0，也固定启动基线。
    publish(read_now(false));
    thread_ = std::thread([this] { loop(); });
}

void ResourceSampler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void ResourceSampler::loop() {
    while (true) {
        sample_once();
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, interval_, [this] { return stopping_; })) {
            return;
        }
    }
}

ResourceReading ResourceSampler::read_now(bool probe_vram) {
    ResourceReading reading;
    double rss = 0.0;
    if (read_rss(rss)) {
        reading.rss_mib = rss;
    }
    if (probe_vram && probe_vram_impl(reading.vram_mib)) {
        reading.vram_known = true;
    }
    return reading;
}

bool ResourceSampler::probe_vram(double& out_mib) {
    return probe_vram_impl(out_mib);
}

void ResourceSampler::publish(const ResourceReading& reading) {
    rss_peak_mib_ = std::max(rss_peak_mib_, reading.rss_mib);
    metrics_.set_float(MetricId::RssMib, reading.rss_mib);
    metrics_.set_float(MetricId::RssPeakMib, rss_peak_mib_);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if (reading.vram_known) {
        vram_peak_mib_ = std::max(vram_peak_mib_, reading.vram_mib);
        metrics_.set_float(MetricId::VramMib, reading.vram_mib);
        metrics_.set_float(MetricId::VramPeakMib, vram_peak_mib_);
        metrics_.set(MetricId::VramProbeOk, 1);
    } else {
        metrics_.set_float(MetricId::VramMib, nan);
        metrics_.set_float(MetricId::VramPeakMib, vram_peak_mib_ > 0.0 ? vram_peak_mib_ : nan);
        metrics_.set(MetricId::VramProbeOk, 0);
        // 进程级采不到时（WSL 的 pid 命名空间），补一条整卡设备级序列，明确不等价于本进程。
        static std::int32_t device_tick = 0;
        if (++device_tick % 4 == 1) {
            double device_mib = 0.0;
            if (probe_vram_device_impl(device_mib)) {
                metrics_.set_float(MetricId::VramDeviceMib, device_mib);
            } else {
                metrics_.set_float(MetricId::VramDeviceMib, nan);
            }
        }
    }
}

ResourceReading ResourceSampler::sample_once() {
    ResourceReading reading = read_now(false);
    // VRAM 探测较贵：每 4 个采样周期一次，其余复用缓存值。
    if (++vram_tick_ % kVramProbeEvery == 1) {
        double probed = -1.0;
        vram_cached_known_ = probe_vram(probed);
        vram_cache_mib_ = vram_cached_known_ ? probed : -1.0;
    }
    reading.vram_mib = vram_cache_mib_;
    reading.vram_known = vram_cached_known_;
    publish(reading);
    return reading;
}

} // namespace llmopt
