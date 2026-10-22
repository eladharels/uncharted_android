// SPDX-License-Identifier: GPL-2.0-or-later
// uncharted_android: waits on a 32-bit word straight through the futex (Linux, Android). The
// std::atomic::wait of libc++ polls first: it spins for 4 us, then calls sched_yield in a loop
// until 64 us have passed, and only then sleeps in the futex. The renderer's stages wait on each
// other thousands of times per second, so on a phone those yields were a core's worth of
// kernel time, heat, and the thermal cap on the clocks that followed. A waiter here spins a
// few hundred iterations (sub-microsecond on arm64) and sleeps; the waker pays one syscall only
// when someone sleeps (the callers' `waiting` flags). Elsewhere (Windows), std::atomic's own.

#pragma once

#include <atomic>
#include <climits>
#include "common/types.h"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Common {

/// Sleeps while `word == expected` (returns on a wake, a change, or spuriously: loop).
inline void FutexWait(std::atomic<u32>& word, u32 expected) {
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<u32*>(&word), FUTEX_WAIT_PRIVATE, expected, nullptr,
            nullptr, 0);
#else
    word.wait(expected, std::memory_order_seq_cst);
#endif
}

inline void FutexWakeOne(std::atomic<u32>& word) {
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<u32*>(&word), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#else
    word.notify_one();
#endif
}

inline void FutexWakeAll(std::atomic<u32>& word) {
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<u32*>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr,
            nullptr, 0);
#else
    word.notify_all();
#endif
}

/// A 64-bit counter others wait on to reach a value: the waiters sleep on a 32-bit generation
/// word the producer bumps while waiters are registered (Dekker: a waiter registers before
/// re-checking the counter, the producer stores the counter before reading the registration,
/// all seq_cst, so a store after the check is always followed by a wake).
struct CounterWait {
    std::atomic<u32> generation{0};
    std::atomic<u32> waiters{0};

    /// Producer side, after the counter was stored.
    void Signal() {
        if (waiters.load(std::memory_order_seq_cst) != 0) {
            generation.fetch_add(1, std::memory_order_seq_cst);
            FutexWakeAll(generation);
        }
    }

    /// Waiter side: sleeps until `done()` holds (re-checked after every wake).
    template <typename Done>
    void Wait(Done&& done) {
        waiters.fetch_add(1, std::memory_order_seq_cst);
        while (true) {
            const u32 seen = generation.load(std::memory_order_seq_cst);
            if (done()) {
                break;
            }
            FutexWait(generation, seen);
            if (done()) {
                break;
            }
        }
        waiters.fetch_sub(1, std::memory_order_seq_cst);
    }
};

} // namespace Common
