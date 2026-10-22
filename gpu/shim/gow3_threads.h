// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>
#include <string>
#ifdef _WIN32
#include <bit>
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <utility>
#include <vector>
#endif

namespace Gow3Threads {
#ifdef _WIN32
/// Hardware threads available to the process (its affinity mask).
inline unsigned Available() {
    DWORD_PTR process = 0, system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        return std::max(1, std::popcount(static_cast<unsigned long long>(process)));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs when cores are otherwise idle (lowest Windows priority).
inline void MakeBackground() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

/// Phones only (below).
inline void PreferBigCores(const char*) {}
inline void PreferBigCoresForProcess() {}
inline std::string CpuClocks() { return {}; }
#else

/// Hardware threads available to the process.
inline unsigned Available() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
}

/// uncharted_android: GOW3_BIG_CORES keeps threads on the cores with the highest clock. A
/// phone's scheduler places threads by their utilization, and a renderer stage that is busy in
/// short bursts between waits looks light to it: it may land on a little core, where each burst
/// takes about twice as long (Galaxy S24 Ultra, Uncharted 1: 7-12 us per draw on the command
/// thread without, 5-7 with; frames of 2,000 draws from 50-67 ms to the 30 FPS cap). The big
/// cores are those whose maximum frequency is at least 80% of the fastest core's (a Snapdragon
/// 8 Gen 3: the prime core and the five performance cores, not the two efficiency cores).
///   1: the renderer's threads (command processor, draw recording, Vulkan recording, ring copy)
///   2: the whole process: the main thread at GPU initialization, inherited by every thread
///      created from then on (the game's own included); the renderer's threads as with 1.
/// Logs the chosen cores once.
inline unsigned BigCoresLevel() {
    static const unsigned level = [] {
        const char* env = std::getenv("GOW3_BIG_CORES");
        return env && env[0] >= '1' && env[0] <= '9' ? unsigned(env[0] - '0') : 0u;
    }();
    return level;
}

/// The big cores' set; empty when there is no distinction (or no cpufreq information).
inline const cpu_set_t& BigCores() {
    static const cpu_set_t big = [] {
        cpu_set_t set;
        CPU_ZERO(&set);
        std::vector<std::pair<int, long>> cores; // (cpu, max kHz)
        for (int cpu = 0; cpu < 64; ++cpu) {
            char path[96];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
            FILE* f = std::fopen(path, "r");
            if (!f) {
                continue;
            }
            long khz = 0;
            if (std::fscanf(f, "%ld", &khz) == 1 && khz > 0) {
                cores.emplace_back(cpu, khz);
            }
            std::fclose(f);
        }
        long fastest = 0;
        for (const auto& [cpu, khz] : cores) {
            fastest = std::max(fastest, khz);
        }
        std::string list;
        for (const auto& [cpu, khz] : cores) {
            if (khz * 10 >= fastest * 8) {
                CPU_SET(cpu, &set);
                list += (list.empty() ? "" : ",") + std::to_string(cpu);
            }
        }
        if (CPU_COUNT(&set) == 0 || CPU_COUNT(&set) == static_cast<int>(cores.size())) {
            std::printf("Big cores: no distinction found among %zu cores (GOW3_BIG_CORES ignored)\n",
                        cores.size());
            CPU_ZERO(&set);
        } else {
            std::printf("Big cores: %s kept on cores %s (%.2f GHz and up, of %zu; GOW3_BIG_CORES=%u)\n",
                        BigCoresLevel() >= 2 ? "the whole process" : "renderer threads",
                        list.c_str(), fastest * 0.8 / 1e6, cores.size(), BigCoresLevel());
        }
        return set;
    }();
    return big;
}

/// Level 1 and up: the calling thread (one of the renderer's) stays on the big cores.
inline void PreferBigCores(const char* who) {
    if (BigCoresLevel() == 0) {
        return;
    }
    const cpu_set_t& big = BigCores();
    if (CPU_COUNT(&big) == 0) {
        return;
    }
    if (sched_setaffinity(0, sizeof(big), &big) != 0) {
        std::printf("Big cores: sched_setaffinity failed for %s (errno %d)\n", who, errno);
    }
}

/// Reads one number from a sysfs file; -1 when unreadable.
inline long ReadSysfsLong(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        return -1;
    }
    long value = -1;
    if (std::fscanf(f, "%ld", &value) != 1) {
        value = -1;
    }
    std::fclose(f);
    return value;
}

/// uncharted_android: the cores' current clocks in GHz ("1.7 1.7 1.5 1.5 1.5 1.6 1.6 1.6"),
/// and, when a core's allowed maximum (scaling_max_freq) sits below its hardware maximum,
/// those caps: the phone's thermal management lowering the clocks under sustained load. The
/// GPU's clock the same way (kgsl) where readable. The process's CPU use since the last call
/// (user and kernel time over wall time, in cores) says how much heat the game makes. For the
/// 5-second statistics. Empty where cpufreq is not readable.
inline std::string CpuClocks() {
    std::string cur, caps, hw;
    bool capped = false;
    for (int cpu = 0; cpu < 64; ++cpu) {
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",
                      cpu);
        const long khz = ReadSysfsLong(path);
        if (khz < 0 && cpu == 0) {
            return {};
        }
        if (khz < 0) {
            break;
        }
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
                      cpu);
        const long cap = ReadSysfsLong(path);
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",
                      cpu);
        const long max = ReadSysfsLong(path);
        char text[32];
        std::snprintf(text, sizeof(text), "%s%.1f", cur.empty() ? "" : " ", khz / 1e6);
        cur += text;
        std::snprintf(text, sizeof(text), "%s%.1f", caps.empty() ? "" : " ", cap / 1e6);
        caps += text;
        std::snprintf(text, sizeof(text), "%s%.1f", hw.empty() ? "" : " ", max / 1e6);
        hw += text;
        capped |= cap > 0 && max > 0 && cap < max;
    }
    std::string line = "CPU clocks (GHz): " + cur;
    if (capped) {
        line += "; capped to " + caps + " of " + hw;
    }
    const long gpu = ReadSysfsLong("/sys/class/kgsl/kgsl-3d0/gpuclk");
    const long gpu_max = ReadSysfsLong("/sys/class/kgsl/kgsl-3d0/max_gpuclk");
    if (gpu > 0) {
        char text[64];
        std::snprintf(text, sizeof(text), "; GPU %.0f MHz", gpu / 1e6);
        line += text;
        if (gpu_max > 0) {
            std::snprintf(text, sizeof(text), " of %.0f", gpu_max / 1e6);
            line += text;
        }
    }
    static std::chrono::steady_clock::time_point last_time{};
    static double last_user = 0, last_sys = 0;
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        const auto now = std::chrono::steady_clock::now();
        const double user = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6;
        const double sys = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
        if (last_time != std::chrono::steady_clock::time_point{}) {
            const double wall = std::chrono::duration<double>(now - last_time).count();
            if (wall > 0.5) {
                char text[96];
                std::snprintf(text, sizeof(text), "; process CPU %.1f cores (user %.1f, kernel %.1f)",
                              (user - last_user + sys - last_sys) / wall, (user - last_user) / wall,
                              (sys - last_sys) / wall);
                line += text;
            }
        }
        last_time = now;
        last_user = user;
        last_sys = sys;
    }
    return line;
}

/// Level 2: the calling thread (the process's main thread, at GPU initialization) and, by
/// inheritance, every thread it creates from now on.
inline void PreferBigCoresForProcess() {
    if (BigCoresLevel() >= 2) {
        PreferBigCores("the main thread");
    } else if (BigCoresLevel() == 1) {
        (void)BigCores(); // the log line at start-up rather than at the first renderer thread
    }
}
#endif

} // namespace Gow3Threads
