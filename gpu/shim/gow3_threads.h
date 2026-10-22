// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <bit>
#include <windows.h>
#else
#include <cerrno>
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

/// uncharted_android: GOW3_BIG_CORES=1 keeps the calling thread (the renderer's stages) on the
/// cores with the highest clock. A phone's scheduler places threads by their utilization, and a
/// stage that is busy in short bursts between waits looks light to it: it may land on a little
/// core, where each burst takes about twice as long. The big cores are those whose maximum
/// frequency is at least 80% of the fastest core's (a Snapdragon 8 Gen 3: the prime core and
/// the five performance cores, not the two efficiency cores). Off by default; logs once.
inline void PreferBigCores(const char* who) {
    static const bool wanted = [] {
        const char* env = std::getenv("GOW3_BIG_CORES");
        return env && env[0] == '1';
    }();
    if (!wanted) {
        return;
    }
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
            std::printf("Big cores: renderer threads kept on cores %s (%.2f GHz and up, of %zu; "
                        "GOW3_BIG_CORES=1)\n",
                        list.c_str(), fastest * 0.8 / 1e6, cores.size());
        }
        return set;
    }();
    if (CPU_COUNT(&big) == 0) {
        return;
    }
    if (sched_setaffinity(0, sizeof(big), &big) != 0) {
        std::printf("Big cores: sched_setaffinity failed for %s (errno %d)\n", who, errno);
    }
}
#endif

} // namespace Gow3Threads
