// SPDX-License-Identifier: GPL-2.0-or-later
// uncharted_android: see gow3_profiler.h.
//
// How a sample is taken: the sampler thread sends a real-time signal to one thread and waits.
// The handler records that thread's pc, sp, fp and lr and parks (spins) until the sampler is
// done; the sampler reads the parked thread's stack with process_vm_readv (which fails cleanly
// on an unmapped address, unlike a load in the handler) and follows the frame-pointer chain
// (every library of ours is built with frame pointers; arm64 keeps them by default). Then it
// releases the thread. A thread is parked for a few tens of microseconds per sample.
#include "gow3_profiler.h"

#ifndef _WIN32

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "common/thread.h"
#include "gow3_toggles.h"

namespace Gow3Profiler {
namespace {

// SIGRTMIN is a function call on Linux and Bionic (the first signals are the C library's).
int SamplingSignal() {
    static const int signal = SIGRTMIN + 4;
    return signal;
}
constexpr unsigned kMaxFrames = 48;

struct Config {
    double start = 30.0;
    double interval = 30.0;
    double duration = 6.0;
    unsigned hz = 1000;
};

// The parked thread's registers: one sample in flight at a time.
struct alignas(64) Park {
    std::atomic<uint32_t> state{0}; // 0 idle, 1 request for `tid`, 2 registers recorded, 3 released
    std::atomic<int> tid{0};
    uint64_t pc = 0, sp = 0, fp = 0, lr = 0;
};
Park g_park;

void Handler(int, siginfo_t*, void* context) {
    const int self = static_cast<int>(syscall(SYS_gettid));
    if (g_park.state.load(std::memory_order_acquire) != 1 || g_park.tid.load() != self) {
        return; // not for this thread, or a late delivery after the sampler gave up
    }
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__aarch64__)
    g_park.pc = uc->uc_mcontext.pc;
    g_park.sp = uc->uc_mcontext.sp;
    g_park.fp = uc->uc_mcontext.regs[29];
    g_park.lr = uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    g_park.pc = uc->uc_mcontext.gregs[REG_RIP];
    g_park.sp = uc->uc_mcontext.gregs[REG_RSP];
    g_park.fp = uc->uc_mcontext.gregs[REG_RBP];
    g_park.lr = 0;
#else
    g_park.pc = g_park.sp = g_park.fp = g_park.lr = 0;
#endif
    g_park.state.store(2, std::memory_order_release);
    // Parked until the sampler read the stack (bounded: a sampler that died must not hang us).
    for (unsigned spins = 0; g_park.state.load(std::memory_order_acquire) == 2; ++spins) {
        if (spins > 20000) {
            break;
        }
        if (spins > 64) {
            sched_yield();
        }
    }
    g_park.state.store(0, std::memory_order_release);
}

struct ThreadInfo {
    int tid;
    std::string name;
    bool hot;
};

// The threads that carry a frame (names as /proc shows them: 15 characters): the command
// processor, the draw recording thread, the Vulkan recording thread, the ring copier, the draw
// preparation workers, the copy threads and the game's job workers. The rest (submit threads,
// shader compilers, the game's I/O threads) is sampled at a tenth of the rate.
bool HotThread(const std::string& name) {
    static const char* const hot[] = {"shadPS4:GpuComm", "gow3:DrawRec", "gow3:VkRecorder",
                                      "gow3:RingCopy", "gow3:DrawPrep", "gow3:DrawScan",
                                      "gow3:Copy", "gow3:TexBind", "NdJobWorker"};
    for (const char* h : hot) {
        if (name.rfind(h, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<ThreadInfo> ListThreads(int self_tid) {
    std::vector<ThreadInfo> threads;
    DIR* dir = opendir("/proc/self/task");
    if (!dir) {
        return threads;
    }
    while (dirent* entry = readdir(dir)) {
        const int tid = std::atoi(entry->d_name);
        if (tid <= 0 || tid == self_tid) {
            continue;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        std::string name;
        if (FILE* f = std::fopen(path, "r")) {
            char buf[64] = {};
            if (std::fgets(buf, sizeof(buf), f)) {
                name = buf;
                while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) {
                    name.pop_back();
                }
            }
            std::fclose(f);
        }
        threads.push_back({tid, name, HotThread(name)});
    }
    closedir(dir);
    std::sort(threads.begin(), threads.end(),
              [](const ThreadInfo& a, const ThreadInfo& b) { return a.tid < b.tid; });
    return threads;
}

struct StackHash {
    size_t operator()(const std::vector<uint64_t>& v) const noexcept {
        size_t h = 1469598103934665603ull;
        for (uint64_t x : v) {
            h ^= x;
            h *= 1099511628211ull;
        }
        return h;
    }
};
using Stacks = std::unordered_map<std::vector<uint64_t>, uint32_t, StackHash>;

struct ThreadSamples {
    ThreadInfo info;
    uint32_t samples = 0, failed = 0;
    Stacks stacks;
};

// Reads [address, address + size) of this process; false when any of it is unmapped.
bool ReadMemory(uint64_t address, void* out, size_t size) {
    iovec local{out, size};
    iovec remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

// A return address as saved on the stack carries pointer-authentication bits above the
// address on arm64 CPUs with PAC (the NDK's default branch protection): XPACLRI strips them
// (a NOP on CPUs without PAC).
uint64_t StripPac(uint64_t address) {
#if defined(__aarch64__)
    register uint64_t x30 __asm__("x30") = address;
    __asm__ volatile("hint #7" : "+r"(x30)); // xpaclri
    return x30;
#else
    return address;
#endif
}

// The frame-pointer chain of the parked thread, leaf first.
void Walk(std::vector<uint64_t>& frames) {
    frames.clear();
    frames.push_back(g_park.pc);
    const uint64_t lr = StripPac(g_park.lr);
    if (lr && lr != g_park.pc) {
        frames.push_back(lr);
    }
    uint64_t fp = g_park.fp;
    const uint64_t sp = g_park.sp;
    // A chunk of the stack from the frame pointer up; frames above it fetch the next chunk.
    alignas(16) uint8_t chunk[2048];
    uint64_t chunk_start = 0, chunk_end = 0;
    while (frames.size() < kMaxFrames) {
        if (fp == 0 || (fp & 15) != 0 || fp < sp || fp - sp > (8u << 20)) {
            break;
        }
        if (fp < chunk_start || fp + 16 > chunk_end) {
            if (!ReadMemory(fp, chunk, sizeof(chunk))) {
                // The last frames before the mapping's end: one pair.
                if (!ReadMemory(fp, chunk, 16)) {
                    break;
                }
                chunk_start = fp;
                chunk_end = fp + 16;
            } else {
                chunk_start = fp;
                chunk_end = fp + sizeof(chunk);
            }
        }
        uint64_t next_fp, ret;
        std::memcpy(&next_fp, chunk + (fp - chunk_start), 8);
        std::memcpy(&ret, chunk + (fp - chunk_start) + 8, 8);
        ret = StripPac(ret);
        if (ret == 0) {
            break;
        }
        if (frames.size() == 2 && frames[1] == ret) {
            // The leaf had already saved lr: the same return address twice.
            frames.pop_back();
        }
        frames.push_back(ret);
        if (next_fp <= fp) {
            break;
        }
        fp = next_fp;
    }
}

// One sample of `tid`: false when the thread did not answer (gone, or stuck in the kernel).
bool Sample(int tid, std::vector<uint64_t>& frames) {
    g_park.tid.store(tid);
    g_park.state.store(1, std::memory_order_release);
    if (syscall(SYS_tgkill, getpid(), tid, SamplingSignal()) != 0) {
        g_park.state.store(0);
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    for (unsigned spins = 0; g_park.state.load(std::memory_order_acquire) != 2; ++spins) {
        if (spins > 64) {
            sched_yield();
        }
        if ((spins & 255) == 255 && std::chrono::steady_clock::now() > deadline) {
            // Not delivered yet: withdraw the request (a later delivery sees state 0 and
            // returns at once).
            uint32_t expected = 1;
            if (g_park.state.compare_exchange_strong(expected, 0)) {
                return false;
            }
            break; // it arrived at the last moment
        }
    }
    Walk(frames);
    g_park.state.store(3, std::memory_order_release);
    // The handler resets the state to 0 on its way out; wait for it, bounded.
    for (unsigned spins = 0; g_park.state.load(std::memory_order_acquire) != 0 && spins < 100000;
         ++spins) {
        if (spins > 64) {
            sched_yield();
        }
    }
    g_park.state.store(0);
    return true;
}

void WriteWindow(FILE* out, unsigned index, uint64_t started_ms, double seconds, uint64_t frames,
                 const std::vector<ThreadSamples>& threads) {
    std::fprintf(out, "W %u %llu %.3f %llu %.1f\n", index, static_cast<unsigned long long>(started_ms),
                 seconds, static_cast<unsigned long long>(frames),
                 seconds > 0 ? frames / seconds : 0.0);
    if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), maps)) {
            // Only mappings code can run from: the rest is noise for symbolization.
            if (std::strstr(line, " r-xp ") || std::strstr(line, " rwxp ")) {
                std::fprintf(out, "M %s", line);
            }
        }
        std::fclose(maps);
    }
    for (const auto& t : threads) {
        if (t.samples == 0 && t.failed == 0) {
            continue;
        }
        std::string name = t.info.name.empty() ? "?" : t.info.name;
        std::replace(name.begin(), name.end(), ' ', '_');
        std::fprintf(out, "T %d %s %u %u %s\n", t.info.tid, name.c_str(),
                     t.samples, t.failed, t.info.hot ? "hot" : "cold");
        std::vector<std::pair<const std::vector<uint64_t>*, uint32_t>> sorted;
        sorted.reserve(t.stacks.size());
        for (const auto& [stack, count] : t.stacks) {
            sorted.emplace_back(&stack, count);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [stack, count] : sorted) {
            std::fprintf(out, "S %u", count);
            for (uint64_t frame : *stack) {
                std::fprintf(out, " %llx", static_cast<unsigned long long>(frame));
            }
            std::fputc('\n', out);
        }
    }
    std::fflush(out);
}

void Run(Config config, std::string path) {
    Common::SetCurrentThreadName("gow3:profiler");
    const int self = static_cast<int>(syscall(SYS_gettid));
    FILE* out = std::fopen(path.c_str(), "w");
    if (!out) {
        std::printf("Profiler: cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(out, "# gow3 profile v1: W index start_ms seconds frames fps; M maps; "
                      "T tid name samples failed hot|cold; S count leaf..root\n");
    std::fflush(out);
    const auto boot = std::chrono::steady_clock::now();
    auto next = boot + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>(config.start));
    std::vector<uint64_t> frames;
    for (unsigned index = 0;; ++index) {
        std::this_thread::sleep_until(next);
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(config.interval));
        std::vector<ThreadSamples> threads;
        for (ThreadInfo& info : ListThreads(self)) {
            threads.emplace_back();
            threads.back().info = std::move(info);
        }
        const uint64_t flips0 = Gow3Stats::flips.load(std::memory_order_relaxed);
        const auto started = std::chrono::steady_clock::now();
        const uint64_t started_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(started - boot).count());
        const auto end = started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(config.duration));
        const auto period = std::chrono::nanoseconds(1000000000ull / std::max(1u, config.hz));
        auto tick_at = started;
        for (unsigned tick = 0; std::chrono::steady_clock::now() < end; ++tick) {
            for (auto& t : threads) {
                if (!t.info.hot && tick % 10 != 0) {
                    continue;
                }
                if (Sample(t.info.tid, frames)) {
                    ++t.samples;
                    ++t.stacks[frames];
                } else {
                    ++t.failed;
                }
            }
            tick_at += period;
            std::this_thread::sleep_until(tick_at);
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const uint64_t flips = Gow3Stats::flips.load(std::memory_order_relaxed) - flips0;
        WriteWindow(out, index, started_ms, seconds, flips, threads);
        uint64_t total = 0;
        for (const auto& t : threads) {
            total += t.samples;
        }
        std::printf("Profiler: window %u at %.0f s: %llu samples of %zu threads in %.1f s, %.1f FPS "
                    "-> %s\n",
                    index, started_ms / 1000.0, static_cast<unsigned long long>(total), threads.size(),
                    seconds, seconds > 0 ? flips / seconds : 0.0, path.c_str());
        std::fflush(stdout);
    }
}

} // namespace

void Start() {
    const char* env = std::getenv("GOW3_PROFILE");
    if (!env || !env[0] || env[0] == '0') {
        return;
    }
    Config config;
    if (std::strcmp(env, "1") != 0) {
        unsigned hz = config.hz;
        if (std::sscanf(env, "%lf,%lf,%lf,%u", &config.start, &config.interval, &config.duration,
                        &hz) < 3) {
            std::printf("Profiler: bad GOW3_PROFILE=%s (1, or start,interval,duration[,hz] in seconds)\n",
                        env);
            return;
        }
        config.hz = std::clamp(hz, 50u, 4000u);
        config.start = std::max(1.0, config.start);
        config.interval = std::max(config.duration + 1.0, config.interval);
        config.duration = std::clamp(config.duration, 0.5, 60.0);
    }
    const char* dir = std::getenv("GOW3_LOG_DIR");
    std::string path = std::string(dir && dir[0] ? dir : ".") + "/profile.txt";
    struct sigaction sa{};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SamplingSignal(), &sa, nullptr) != 0) {
        std::printf("Profiler: cannot install the sampling signal\n");
        return;
    }
    std::printf("Profiler: sampling from %.0f s, every %.0f s for %.1f s at %u Hz, to %s\n",
                config.start, config.interval, config.duration, config.hz, path.c_str());
    std::thread([config, path] { Run(config, path); }).detach();
}

} // namespace Gow3Profiler

#else
namespace Gow3Profiler {
void Start() {}
} // namespace Gow3Profiler
#endif
