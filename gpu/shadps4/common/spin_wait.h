// SPDX-FileCopyrightText: Copyright 2026 uncharted_android contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <thread>
#include "common/arch.h"
#include "common/types.h"

namespace Common {

/// Backoff for a wait loop that polls a condition another thread sets: call it once per failed
/// poll.
///
/// x86-64 keeps the PC port's loop: 4096 PAUSE hints (about 150 us), then sched_yield.
///
/// uncharted_android: elsewhere the hint is far cheaper (arm64 YIELD is about one cycle), so the
/// same loop reached sched_yield after a few microseconds and spun on it for the rest of the
/// wait. On a Snapdragon 8 Gen 3 that was 25-50 ms of kernel time per frame in the GPU thread
/// alone and thousands of context switches, taken from the recording thread and the game's own
/// threads. There the backoff spins for GOW3_SPIN_US (default 20 us), yields for GOW3_YIELD_US
/// (default 20 us), and then sleeps GOW3_WAIT_SLEEP_US (default 50 us) per poll.
class SpinWait {
public:
    /// `yield_only`: on x86-64, sched_yield from the first poll (loops that did so before).
    explicit SpinWait(bool yield_only = false) : x86_yield_only{yield_only} {}

    void operator()() {
#if defined(ARCH_X86_64)
        if (!x86_yield_only && spins++ < 4096) {
            COMMON_CPU_RELAX();
        } else {
            std::this_thread::yield();
        }
#else
        if (spins == 0) {
            start = Clock::now();
        }
        ++spins;
        if (phase == 0) {
            COMMON_CPU_RELAX();
            if ((spins & 63) != 0 || Clock::now() - start < SpinTime()) {
                return;
            }
            phase = 1;
            start = Clock::now();
            return;
        }
        if (phase == 1) {
            // Each yield is a syscall (kernel time, heat, on a phone); GOW3_YIELD_US bounds
            // the phase, 0 skips it.
            if (Clock::now() - start < YieldTime()) {
                std::this_thread::yield();
                return;
            }
            phase = 2;
        }
        std::this_thread::sleep_for(SleepTime());
#endif
    }

    void Reset() {
        spins = 0;
        phase = 0;
    }

private:
    using Clock = std::chrono::steady_clock;

    static std::chrono::microseconds EnvMicros(const char* name, int fallback) {
        const char* env = std::getenv(name);
        const int value = env ? std::atoi(env) : fallback;
        return std::chrono::microseconds(value < 0 ? 0 : value);
    }
    static std::chrono::microseconds SpinTime() {
        static const auto time = EnvMicros("GOW3_SPIN_US", 20);
        return time;
    }
    static std::chrono::microseconds SleepTime() {
        static const auto time = EnvMicros("GOW3_WAIT_SLEEP_US", 50);
        return time;
    }
    static std::chrono::microseconds YieldTime() {
        static const auto time = EnvMicros("GOW3_YIELD_US", 20);
        return time;
    }

    bool x86_yield_only;
    u32 spins = 0;
    u32 phase = 0;
    Clock::time_point start{};
};

} // namespace Common
