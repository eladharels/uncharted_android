// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 R.9.12 (GOW3_RING_COPY_THREAD=1): stage A (the GPU command thread) spends about a
// quarter of its time copying constants from guest memory into the constant ring
// (vk_constant_ring.h, ~2 GB/s in the streets). With this on, stage A only queues each copy
// here and a copy thread runs them in order. Before stage B runs a packet it waits until the
// copies queued before that packet are done, so stage B and the GPU see the same bytes as
// before. Guest memory writes stage A makes itself (MemSemaphore signal, DumpConstRam) first
// wait for every queued copy (Rasterizer::WaitRingCopies); writes handed to stage B
// (RunInOrder) run after their packet's wait, and other packets drain stage B first.

#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include "common/rdtsc.h"
#include "gow3_threads.h"
#include "common/futex_wait.h"
#include "common/spin_wait.h"

#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class RingCopier {
public:
    /// Copies `size` bytes of guest memory at `address` to `dst` (ring offset `offset`).
    using CopyFn = void (*)(void* context, VAddr address, u8* dst, u64 size, u64 offset);

    RingCopier(CopyFn fn_, void* context_) : fn{fn_}, context{context_} {
        jobs = std::make_unique<Job[]>(Capacity);
        // R.9.14: GOW3_RING_COPY_THREADS=N (1-4) splits the copies over N threads, copy j on
        // thread j % N: a packet waits for less (each thread copies its share in order).
        if (const char* env = std::getenv("GOW3_RING_COPY_THREADS")) {
            num_threads = std::clamp(std::atoi(env), 1, int(MaxThreads));
        }
        // ucndc R.11 item 2 (Fable): GOW3_RING_COPY_STEAL=1 - copies are claimed from a shared
        // queue; whoever waits for a copy (stage B, stage A) runs it itself, and the copy thread
        // sleeps until GOW3_RING_COPY_WAKE_N copies (default 256) piled up. One copy thread only.
        if (const char* env = std::getenv("GOW3_RING_COPY_STEAL"); env && env[0] == '1') {
            if (num_threads == 1) {
                steal = true;
                finished = std::make_unique<std::atomic<u64>[]>(Capacity);
                if (const char* n = std::getenv("GOW3_RING_COPY_WAKE_N"); n && n[0]) {
                    wake_n = std::max<u64>(1, std::strtoull(n, nullptr, 10));
                }
                std::printf("Ring copy: waiters run the copies they wait for; the copy thread "
                            "wakes every %llu copies\n",
                            static_cast<unsigned long long>(wake_n));
            } else {
                std::printf("Ring copy: GOW3_RING_COPY_STEAL needs one copy thread, ignored\n");
            }
        }
        for (u32 i = 0; i < num_threads; ++i) {
            threads[i] = std::jthread([this, i](std::stop_token stop) {
                if (steal) {
                    RunSteal(stop);
                } else {
                    Run(stop, i);
                }
            });
        }
    }

    ~RingCopier() {
        for (u32 i = 0; i < num_threads; ++i) {
            threads[i].request_stop();
        }
        wake.fetch_add(1, std::memory_order_seq_cst);
        Common::FutexWakeAll(wake);
    }

    static bool Wanted() {
        static const bool on = [] {
            const char* env = std::getenv("GOW3_RING_COPY_THREAD");
            return env && env[0] == '1';
        }();
        return on;
    }

    /// Stage A: queues a copy.
    void Push(VAddr address, u8* dst, u64 size, u64 offset) {
        if (steal) {
            if (head >= Capacity) {
                WaitDone(head - Capacity + 1);
            }
            jobs[head % Capacity] = {address, dst, size, offset};
            ++head;
            enqueued.store(head, std::memory_order_seq_cst);
            if (sleeping.load(std::memory_order_seq_cst) != 0 &&
                head - sleep_at.load(std::memory_order_seq_cst) >= wake_n &&
                !woken.exchange(true, std::memory_order_seq_cst)) {
                // One wake per sleep: later pushes see woken until the thread sleeps again.
                notifies.fetch_add(1, std::memory_order_relaxed);
                wake.fetch_add(1, std::memory_order_seq_cst);
                Common::FutexWakeAll(wake);
            }
            return;
        }
        while (head >= Capacity && !IsDone(head - Capacity + 1)) {
            COMMON_CPU_RELAX();
        }
        jobs[head % Capacity] = {address, dst, size, offset};
        ++head;
        enqueued.store(head, std::memory_order_seq_cst);
        if (sleeping.load(std::memory_order_seq_cst) != 0) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            Common::FutexWakeAll(wake);
        }
    }

    /// The number of copies queued so far (a packet's sequence number).
    [[nodiscard]] u64 Enqueued() const noexcept {
        return enqueued.load(std::memory_order_acquire);
    }

    /// True once the first `seq` queued copies are done.
    [[nodiscard]] bool IsDone(u64 seq) const noexcept {
        if (steal) {
            return done_prefix.load(std::memory_order_acquire) >= seq;
        }
        for (u32 i = 0; i < num_threads; ++i) {
            // Copies below seq that thread i runs: i, i + N, ...
            const u64 count = seq > i ? (seq - i + num_threads - 1) / num_threads : 0;
            if (done[i].value.load(std::memory_order_acquire) < count) {
                return false;
            }
        }
        return true;
    }

    /// Any thread: waits until the first `seq` queued copies are done.
    void WaitDone(u64 seq, bool stage_b = false) {
        if (IsDone(seq)) {
            return;
        }
        const u64 start = Common::RawRDTSC();
        if (steal) {
            // Run the copies this wait needs; when they are all claimed, wait for the others.
            u64 copied = 0;
            u64 copy_cycles = 0;
            Common::SpinWait backoff;
            while (!IsDone(seq)) {
                const u64 c0 = Common::RawRDTSC();
                if (const u64 n = Claim(seq); n != NoJob) {
                    RunJob(n);
                    copy_cycles += Common::RawRDTSC() - c0;
                    ++copied;
                    continue;
                }
                backoff();
            }
            (stage_b ? b_copies : a_copies).fetch_add(copied, std::memory_order_relaxed);
            (stage_b ? b_copy_cycles : a_copy_cycles)
                .fetch_add(copy_cycles, std::memory_order_relaxed);
            (stage_b ? b_wait_cycles : wait_cycles)
                .fetch_add(Common::RawRDTSC() - start, std::memory_order_relaxed);
            (stage_b ? b_waits : waits).fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Common::SpinWait backoff;
        while (!IsDone(seq)) {
            backoff();
        }
        (stage_b ? b_wait_cycles : wait_cycles).fetch_add(Common::RawRDTSC() - start, std::memory_order_relaxed);
        (stage_b ? b_waits : waits).fetch_add(1, std::memory_order_relaxed);
    }

    /// Any thread: waits for every copy queued so far.
    void WaitAll() {
        WaitDone(Enqueued());
    }

    /// Statistics: cycles spent waiting for copies (and how many waits), cycles copying.
    std::atomic<u64> wait_cycles{0}, waits{0}, busy_cycles{0};
    std::atomic<u64> b_wait_cycles{0}, b_waits{0}; ///< stage B's share
    /// GOW3_RING_COPY_STEAL: copies run by the copy thread / stage B / the others, the cycles
    /// stage B and the others spent copying (part of their wait), sleeps and wakes.
    std::atomic<u64> thread_copies{0}, b_copies{0}, a_copies{0};
    std::atomic<u64> b_copy_cycles{0}, a_copy_cycles{0}, sleeps{0}, notifies{0};

private:
    struct Job {
        VAddr address;
        u8* dst;
        u64 size;
        u64 offset;
    };
    static constexpr u64 Capacity = 1ull << 16;
    static constexpr u32 MaxThreads = 4;

    static u32 SpinMicros() {
        static const u32 micros = [] {
            const char* env = std::getenv("GOW3_RING_COPY_SPIN_US");
            const u32 value = env && env[0] ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 200;
            if (value != 200) {
                std::printf("Ring copy: the copy threads spin %u us before sleeping\n", value);
            }
            return value;
        }();
        return micros;
    }

    static constexpr u64 NoJob = ~0ull;

    /// GOW3_RING_COPY_STEAL: claims the next unclaimed copy below `limit` (and below what was
    /// queued), or returns NoJob.
    u64 Claim(u64 limit) {
        u64 n = claim.load(std::memory_order_seq_cst);
        while (true) {
            const u64 end = std::min(limit, enqueued.load(std::memory_order_seq_cst));
            if (n >= end) {
                return NoJob;
            }
            if (claim.compare_exchange_weak(n, n + 1, std::memory_order_seq_cst)) {
                return n;
            }
        }
    }

    /// GOW3_RING_COPY_STEAL: runs claimed copy `n`, then moves done_prefix past every copy
    /// finished in order. A copy finished out of order is picked up either by the thread that
    /// moves the prefix up to it or by its own thread re-reading the prefix (both seq_cst).
    void RunJob(u64 n) {
        const Job& job = jobs[n % Capacity];
        fn(context, job.address, job.dst, job.size, job.offset);
        finished[n % Capacity].store(n + 1, std::memory_order_seq_cst);
        u64 p = done_prefix.load(std::memory_order_seq_cst);
        while (finished[p % Capacity].load(std::memory_order_seq_cst) == p + 1) {
            if (done_prefix.compare_exchange_weak(p, p + 1, std::memory_order_seq_cst)) {
                ++p;
            }
        }
    }

    /// GOW3_RING_COPY_STEAL: one log line every ~5 s (from the copy thread, when it is awake):
    /// who ran the copies; the "waited for it" of the draw pipe line includes the copying.
    std::chrono::steady_clock::time_point ReportSteal() {
        const auto now = std::chrono::steady_clock::now();
        const u64 tsc = Common::RawRDTSC();
        const double seconds = std::chrono::duration<double>(now - report_last).count();
        const double cycles = double(tsc - report_tsc);
        const u64 t = thread_copies.load(std::memory_order_relaxed);
        const u64 bc = b_copies.load(std::memory_order_relaxed);
        const u64 ac = a_copies.load(std::memory_order_relaxed);
        const u64 bcyc = b_copy_cycles.load(std::memory_order_relaxed);
        const u64 acyc = a_copy_cycles.load(std::memory_order_relaxed);
        const u64 tcyc = busy_cycles.load(std::memory_order_relaxed);
        const u64 sl = sleeps.load(std::memory_order_relaxed);
        const u64 no = notifies.load(std::memory_order_relaxed);
        if (report_tsc != 0 && seconds > 0.0) {
            std::printf("Ring copy steal: copies/s by the thread %.0f, by stage B %.0f, by others "
                        "%.0f; thread copying %.1f%%, stage B copying %.1f%%, others copying "
                        "%.1f%%; sleeps %.0f/s, wakes %.0f/s\n",
                        (t - r_thread) / seconds, (bc - r_b) / seconds, (ac - r_a) / seconds,
                        100.0 * (tcyc - r_tcyc) / cycles, 100.0 * (bcyc - r_bcyc) / cycles,
                        100.0 * (acyc - r_acyc) / cycles, (sl - r_sleeps) / seconds,
                        (no - r_notifies) / seconds);
        }
        r_thread = t;
        r_b = bc;
        r_a = ac;
        r_bcyc = bcyc;
        r_acyc = acyc;
        r_tcyc = tcyc;
        r_sleeps = sl;
        r_notifies = no;
        report_last = now;
        report_tsc = tsc;
        return now + std::chrono::seconds(5);
    }

    /// GOW3_RING_COPY_STEAL: the copy thread drains in the background and sleeps for real.
    void RunSteal(std::stop_token stop) {
        Common::SetCurrentThreadName("gow3:RingCopy0");
        Gow3Threads::PreferBigCores("gow3:RingCopy0");
        // The waiters run the copies they need, so by default the thread sleeps right after draining.
        const u32 spin_us = std::getenv("GOW3_RING_COPY_SPIN_US") ? SpinMicros() : 0;
        auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stop.stop_requested()) {
            if (std::chrono::steady_clock::now() >= report_at) {
                report_at = ReportSteal();
            }
            const u64 start = Common::RawRDTSC();
            if (const u64 n = Claim(NoJob); n != NoJob) {
                RunJob(n);
                u64 count = 1;
                for (u64 m; (m = Claim(NoJob)) != NoJob; ++count) {
                    RunJob(m);
                }
                thread_copies.fetch_add(count, std::memory_order_relaxed);
                busy_cycles.fetch_add(Common::RawRDTSC() - start, std::memory_order_relaxed);
                continue;
            }
            const auto spin_until =
                std::chrono::steady_clock::now() + std::chrono::microseconds(spin_us);
            bool found = false;
            for (u32 spins = 1; spin_us != 0 && !stop.stop_requested(); ++spins) {
                COMMON_CPU_RELAX();
                if (claim.load(std::memory_order_seq_cst) <
                    enqueued.load(std::memory_order_seq_cst)) {
                    found = true;
                    break;
                }
                if (!(spins & 31) && std::chrono::steady_clock::now() >= spin_until) {
                    break;
                }
            }
            if (found || stop.stop_requested()) {
                continue;
            }
            const u32 seen = wake.load(std::memory_order_seq_cst);
            sleep_at.store(claim.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
            woken.store(false, std::memory_order_seq_cst);
            sleeping.fetch_add(1, std::memory_order_seq_cst);
            // Unclaimed copies are left to the waiters until wake_n of them pile up.
            if (enqueued.load(std::memory_order_seq_cst) - sleep_at.load(std::memory_order_seq_cst) <
                    wake_n &&
                !stop.stop_requested()) {
                sleeps.fetch_add(1, std::memory_order_relaxed);
                Common::FutexWait(wake, seen); // uncharted_android: no polling
            }
            sleeping.fetch_sub(1, std::memory_order_seq_cst);
        }
    }

    /// Thread `index` runs copies index, index + N, ...
    void Run(std::stop_token stop, u32 index) {
        Common::SetCurrentThreadName(("gow3:RingCopy" + std::to_string(index)).c_str());
        const u64 step = num_threads;
        u64 at = index;
        u64 count = 0;
        while (true) {
            u64 available = enqueued.load(std::memory_order_acquire);
            if (available <= at) {
                // Copies come every few microseconds while a frame is decoded: spin, and
                // sleep only after a longer pause (as the draw pipe does). ucndc R.11 (Fable):
                // GOW3_RING_COPY_SPIN_US sets that pause (default 200 us); with copies queued
                // about every 0.6 us the thread rarely sleeps inside a frame and holds a core.
                const auto spin_until =
                    std::chrono::steady_clock::now() + std::chrono::microseconds(SpinMicros());
                const u32 check = SpinMicros() < 50 ? 31 : 255; // short spins check the clock sooner
                for (u32 spins = 1; available <= at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    COMMON_CPU_RELAX();
                    if (!(spins & check) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.fetch_add(1, std::memory_order_seq_cst);
                        if (enqueued.load(std::memory_order_seq_cst) <= at &&
                            !stop.stop_requested()) {
                            Common::FutexWait(wake, seen); // uncharted_android: no polling
                        }
                        sleeping.fetch_sub(1, std::memory_order_relaxed);
                    }
                    available = enqueued.load(std::memory_order_acquire);
                }
            }
            const u64 start = Common::RawRDTSC();
            while (at < available) {
                const Job& job = jobs[at % Capacity];
                fn(context, job.address, job.dst, job.size, job.offset);
                at += step;
                done[index].value.store(++count, std::memory_order_release);
            }
            busy_cycles.fetch_add(Common::RawRDTSC() - start, std::memory_order_relaxed);
        }
    }

    CopyFn fn;
    void* context;
    std::unique_ptr<Job[]> jobs;
    u64 head = 0; ///< stage A
    u32 num_threads = 1;
    alignas(64) std::atomic<u64> enqueued{0};
    struct alignas(64) Done {
        std::atomic<u64> value{0}; ///< copies done by one thread
    };
    std::array<Done, MaxThreads> done{};
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<u32> sleeping{0};
    // GOW3_RING_COPY_STEAL
    bool steal = false;
    u64 wake_n = 256;
    std::unique_ptr<std::atomic<u64>[]> finished; ///< finished[n % Capacity] = n + 1 when done
    alignas(64) std::atomic<u64> claim{0};
    alignas(64) std::atomic<u64> done_prefix{0};
    alignas(64) std::atomic<u64> sleep_at{0};
    std::atomic<bool> woken{false}; ///< a producer already woke the sleeping copy thread
    // ReportSteal state (copy thread only)
    std::chrono::steady_clock::time_point report_last{};
    u64 report_tsc = 0;
    u64 r_thread = 0, r_b = 0, r_a = 0, r_bcyc = 0, r_acyc = 0, r_tcyc = 0, r_sleeps = 0,
        r_notifies = 0;
    std::array<std::jthread, MaxThreads> threads;
};

} // namespace Vulkan
