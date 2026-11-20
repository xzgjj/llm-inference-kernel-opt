// gate.cpp — M11 L1 能力快照与 L2 三态判据；探测失败 fail-closed，不崩不放行。
#include "llmopt/gate.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/sysinfo.h>
#endif

namespace llmopt {
namespace {

// 执行只读探测命令并取首行；失败返回空串，调用方按缺失字段处理。
std::string run_capture(const char* command) {
#if defined(_WIN32)
    FILE* pipe = _popen(command, "r");
#else
    FILE* pipe = popen(command, "r");
#endif
    if (!pipe) {
        return {};
    }
    std::string output;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output.append(buffer.data());
        if (output.find('\n') != std::string::npos) {
            break;
        }
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

// 读系统内存：Linux 用 /proc/meminfo，Windows 用 GlobalMemoryStatusEx。
void fill_ram(CapabilitySnapshot& snapshot) {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        snapshot.ram_total_bytes = static_cast<std::int64_t>(status.ullTotalPhys);
        snapshot.ram_available_bytes = static_cast<std::int64_t>(status.ullAvailPhys);
    }
#else
    FILE* file = std::fopen("/proc/meminfo", "r");
    if (!file) {
        return;
    }
    std::int64_t total_kb = 0;
    std::int64_t available_kb = 0;
    std::array<char, 256> line{};
    while (std::fgets(line.data(), static_cast<int>(line.size()), file) != nullptr) {
        if (std::strncmp(line.data(), "MemTotal:", 9) == 0) {
            total_kb = std::atoll(line.data() + 9);
        } else if (std::strncmp(line.data(), "MemAvailable:", 13) == 0) {
            available_kb = std::atoll(line.data() + 13);
        }
    }
    std::fclose(file);
    snapshot.ram_total_bytes = total_kb * 1024;
    snapshot.ram_available_bytes = available_kb * 1024;
#endif
}

// 读 GPU 显存；无 nvidia-smi 或解析失败时保持 0，不阻断 CPU 路径。
void fill_gpu(CapabilitySnapshot& snapshot) {
    const std::string output = run_capture("nvidia-smi --query-gpu=memory.total,memory.free "
                                           "--format=csv,noheader,nounits 2>/dev/null");
    unsigned long long total_mib = 0;
    unsigned long long free_mib = 0;
    if (std::sscanf(output.c_str(), "%llu, %llu", &total_mib, &free_mib) != 2) {
        return;
    }
    snapshot.gpu_present = true;
    snapshot.vram_total_bytes = static_cast<std::int64_t>(total_mib) * 1024 * 1024;
    snapshot.vram_free_bytes = static_cast<std::int64_t>(free_mib) * 1024 * 1024;
}

// 判断是否运行在 WSL2：/proc/version 带 Microsoft 标记。
bool detect_wsl() {
#if defined(_WIN32)
    return false;
#else
    const std::string version = run_capture("cat /proc/version 2>/dev/null");
    return version.find("icrosoft") != std::string::npos;
#endif
}

} // namespace

// L1：采集 CPU/内存/显存/WSL 标记；缺失项写入 missing_fields 供 L2 fail-closed。
CapabilitySnapshot probe_capabilities() {
    CapabilitySnapshot snapshot;
    snapshot.logical_cores = static_cast<std::int32_t>(std::thread::hardware_concurrency());
    snapshot.is_wsl = detect_wsl();
    fill_ram(snapshot);
    fill_gpu(snapshot);
    if (snapshot.ram_total_bytes <= 0) {
        snapshot.missing_fields.push_back("ram.total");
    }
    if (snapshot.ram_available_bytes <= 0) {
        snapshot.missing_fields.push_back("ram.available");
    }
    return snapshot;
}

// L2+L3：硬超预算恒拒绝；软超预算按 off/auto/force_on 三档处置。
GateResult evaluate_gate(const CapabilitySnapshot& snapshot, const EstBudget& budget, GateMode mode,
                         bool allow_force_on) {
    GateResult result;
    result.budget = budget;
    if (!snapshot.missing_fields.empty() || budget.model_bytes <= 0 || budget.kv_bytes <= 0) {
        result.decision = GateDecision::Blocked;
        result.allow_start = false;
        result.reasons.push_back("能力快照或模型元数据缺失");
        for (const std::string& field : snapshot.missing_fields) {
            result.reasons.push_back("missing:" + field);
        }
        return result;
    }
    const std::int64_t free_bytes = (snapshot.gpu_present && snapshot.vram_free_bytes > 0)
                                            ? snapshot.vram_free_bytes
                                            : snapshot.ram_available_bytes;
    result.budget.free_bytes = free_bytes;
    if (budget.required_bytes > free_bytes) {
        result.decision = GateDecision::Blocked;
        result.allow_start = false;
        result.reasons.push_back("required_bytes 超过可用内存，拒绝启动");
        return result;
    }
    const double soft_limit = static_cast<double>(free_bytes) * budget.safety_ratio;
    if (static_cast<double>(budget.required_bytes) <= soft_limit) {
        result.decision = GateDecision::Eligible;
        return result;
    }
    result.decision = GateDecision::Degraded;
    result.reasons.push_back("required_bytes 超过 free×safety_ratio");
    if (mode == GateMode::Auto) {
        result.allow_start = false;
        result.reasons.push_back("mode=auto 在软超预算时拒绝启动");
    } else if (mode == GateMode::ForceOn) {
        if (!allow_force_on) {
            result.allow_start = false;
            result.reasons.push_back("force_on 需显式 --allow-force-on");
        } else {
            result.downgrade_concurrency = true;
            result.reasons.push_back("FORCE_ON 横幅：已降并发，风险自负");
        }
    } else {
        result.reasons.push_back("mode=off 仅记录，不拦截");
    }
    return result;
}

// 文本名，供启动摘要与测试断言使用。
const char* gate_decision_name(GateDecision decision) noexcept {
    switch (decision) {
    case GateDecision::Eligible:
        return "ELIGIBLE";
    case GateDecision::Degraded:
        return "DEGRADED";
    case GateDecision::Blocked:
        return "BLOCKED";
    }
    return "BLOCKED";
}

// 文本名，供启动摘要打印。
const char* gate_mode_name(GateMode mode) noexcept {
    switch (mode) {
    case GateMode::Off:
        return "off";
    case GateMode::Auto:
        return "auto";
    case GateMode::ForceOn:
        return "force_on";
    }
    return "off";
}

} // namespace llmopt
