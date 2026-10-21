// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <vector>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();
/// uc3 (Fable R.8.4): waits are cut in slices so a stuck one is reported.
constexpr u64 WAIT_SLICE_NS = 2'000'000'000ULL;

namespace {
std::mutex registry_mutex;
std::vector<Semaphore*>& Registry() {
    static std::vector<Semaphore*> registry;
    return registry;
}
} // namespace

Semaphore::Semaphore(const Instance& instance_, const char* name_)
    : instance{instance_}, name{name_} {
    {
        std::scoped_lock lk{registry_mutex};
        Registry().push_back(this);
    }
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() {
    std::scoped_lock lk{registry_mutex};
    std::erase(Registry(), this);
}

void Semaphore::SettleAll(u32 timeout_ms) {
    std::scoped_lock lk{registry_mutex};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
    for (Semaphore* sem : Registry()) {
        const u64 last = sem->CurrentTick() - 1;
        if (last == 0) {
            continue;
        }
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        const vk::SemaphoreWaitInfo wait_info = {
            .semaphoreCount = 1,
            .pSemaphores = &sem->semaphore.get(),
            .pValues = &last,
        };
        const vk::Result result =
            sem->instance.GetDevice().waitSemaphores(&wait_info, left > 0 ? u64(left) : 0);
        auto [counter_result, counter] =
            sem->instance.GetDevice().getSemaphoreCounterValue(*sem->semaphore);
        if (result != vk::Result::eSuccess) {
            std::printf("gow3: at exit, GPU semaphore %s stuck: counter %llu, last submitted "
                        "tick %llu, known %llu\n",
                        sem->name, static_cast<unsigned long long>(counter),
                        static_cast<unsigned long long>(last),
                        static_cast<unsigned long long>(sem->KnownGpuTick()));
        }
    }
    std::fflush(stdout);
}

namespace {
u64 SteadyNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}
u64 PollIntervalNs() {
    static const u64 ns = [] {
        if (const char* env = std::getenv("GOW3_SEM_POLL_US")) {
            return u64(std::strtoull(env, nullptr, 10)) * 1000;
        }
#ifdef __ANDROID__
        return u64(100'000);
#else
        return u64(0);
#endif
    }();
    return ns;
}
} // namespace

void Semaphore::Refresh(bool force) {
    const u64 start = SteadyNs();
    const u64 interval = PollIntervalNs();
    if (!force && interval != 0 &&
        start - last_query_ns.load(std::memory_order_relaxed) < interval) {
        stat_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    last_query_ns.store(start, std::memory_order_relaxed);
    // uncharted_android (GOW3_FRAME_STATS): what the queries cost, every 2 s per semaphore.
    struct Account {
        Semaphore* sem;
        u64 start;
        ~Account() {
            const u64 end = SteadyNs();
            sem->stat_queries.fetch_add(1, std::memory_order_relaxed);
            sem->stat_query_ns.fetch_add(end - start, std::memory_order_relaxed);
            u64 window = sem->stat_window_ns.load(std::memory_order_relaxed);
            if (window == 0) {
                sem->stat_window_ns.compare_exchange_strong(window, end);
                return;
            }
            if (end - window < 2'000'000'000ull ||
                !sem->stat_window_ns.compare_exchange_strong(window, end)) {
                return;
            }
            static const bool stats = std::getenv("GOW3_FRAME_STATS") != nullptr;
            const u64 queries = sem->stat_queries.exchange(0);
            const u64 ns = sem->stat_query_ns.exchange(0);
            const u64 skipped = sem->stat_skipped.exchange(0);
            if (stats) {
                const double secs = (end - window) / 1e9;
                std::printf("Semaphore %s: %.0f queries/s, %.1f ms/s querying, %.0f reused/s\n",
                            sem->name, queries / secs, ns / 1e6 / secs, skipped / secs);
            }
        }
    } account{this, start};
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

bool Semaphore::WaitFor(u64 tick, u64 timeout_ns) {
    // 不需要等待：GPU 已经越过该 tick
    if (IsFree(tick)) {
        return true;
    }
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };
    const vk::Result result = instance.GetDevice().waitSemaphores(&wait_info, timeout_ns);
    if (result == vk::Result::eErrorDeviceLost) {
        Breadcrumbs::ReportDeviceLost("waiting for a submission");
        ASSERT_MSG(false, "Device lost while waiting for a submission");
    }
    Refresh(true);
    return result == vk::Result::eSuccess;
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    bool reported = false;
    while (instance.GetDevice().waitSemaphores(&wait_info, WAIT_SLICE_NS) != vk::Result::eSuccess) {
        if (!reported) {
            reported = true;
            auto [counter_result, counter] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
            std::printf("gow3: GPU wait on %s stuck over 2 s: waiting for tick %llu, counter %llu, "
                        "known %llu, current %llu\n",
                        name, static_cast<unsigned long long>(tick),
                        static_cast<unsigned long long>(counter),
                        static_cast<unsigned long long>(KnownGpuTick()),
                        static_cast<unsigned long long>(CurrentTick()));
            std::fflush(stdout);
        }
    }
    Refresh(true);
}

} // namespace Vulkan
