// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include "common/rdtsc.h"
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "common/assert.h"
#include "gow3_toggles.h"
#include "gow3_graphics.h"
#include "gow3_settings.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {
extern std::atomic<u64> g_gow3_compile_ns;
extern std::atomic<u32> g_gow3_compiles;
extern std::atomic<u64> g_gow3_capture_ns, g_gow3_async_ns, g_gow3_compile_wait_ns;
extern std::atomic<u64> g_gow3_snapshot_ns, g_gow3_guest_wait_ns;
extern std::atomic<u32> g_gow3_pending_draws;
} // namespace Vulkan

namespace Libraries::VideoOut {
// uc3: GOW3_FLASH_TRACE time base: process start (the game window follows within seconds).
static const auto uc3_flash_clock = std::chrono::steady_clock::now();

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    const char* separate = std::getenv("GOW3_PRESENT_THREAD");
    separate_swap = !(separate && separate[0] == '0');
    if (separate_swap) {
        swap_thread = std::jthread([&](std::stop_token token) { SwapThread(token); });
    }
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

void VideoOutDriver::RunPresenter(std::function<void()> work, bool if_idle) {
    if (!separate_swap) {
        work();
        return;
    }
    {
        std::scoped_lock lock{swap_mutex};
        if (if_idle && (swap_busy || !swap_queue.empty())) {
            return; // a redraw of the last frame is pointless while frames are queued
        }
        swap_queue.push_back(std::move(work));
    }
    swap_cv.notify_one();
}

void VideoOutDriver::SwapThread(std::stop_token token) {
    Common::SetCurrentThreadName("gow3:Present");
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock lock{swap_mutex};
            swap_busy = false;
            if (!swap_cv.wait(lock, token, [this] { return !swap_queue.empty(); })) {
                return;
            }
            work = std::move(swap_queue.front());
            swap_queue.pop_front();
            swap_busy = true;
        }
        // GOW3_SKIP_PRESENT=1 drops the flip work instead of running it: the game keeps rendering and
        // submitting draws, but nothing reaches the queue through the present path. Used to tell
        // "the game's own rendering lost the device" apart from "our present submission did".
        static const bool skip_present = [] {
            const char* value = std::getenv("GOW3_SKIP_PRESENT");
            return value && *value && std::strcmp(value, "1") == 0;
        }();
        if (!skip_present) {
            work();
        }
    }
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    liverpool->SetVoPort(&main_port);
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };
        Vulkan::FrameCapture::AddDisplayBuffer(address);

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        presenter->RegisterVideoOutSurface(group, address);
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }
    std::printf("VideoOut: %d display buffers registered from index %d (%s, %ux%u, tiling %u)\n",
                bufferNum, startIndex, GetPixelFormatString(attribute->pixel_format).data(),
                attribute->width, attribute->height, static_cast<u32>(attribute->tiling_mode));

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

void VideoOutDriver::Flip(const Request& req) {
    // Update HDR status before presenting, then present the frame (gow3: on the swap thread).
    RunPresenter([this, frame = req.frame, hdr = req.port->is_hdr] {
        presenter->SetHDR(hdr);
        presenter->Present(frame);
    });
    Vulkan::FrameCapture::OnFlip(req.index >= 0 ? req.port->buffer_slots[req.index].address_left
                                                : 0);

    // gow3: GOW3_FRAME_STATS=1 prints flip rate and frame time spread every 5 seconds.
    static const bool frame_stats = EmulatorSettingsImpl::Flag("GOW3_FRAME_STATS", false);
    if (frame_stats) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point window_start = Clock::now(), last = window_start;
        static u32 frames;
        static double worst_ms;
        const auto now = Clock::now();
        const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
        worst_ms = std::max(worst_ms, frame_ms);
        static std::vector<double> intervals;
        intervals.push_back(frame_ms);
        last = now;
        // Stall diagnostics: what happened during a long frame.
        static u64 last_gpu_ns, last_images, last_image_bytes, last_buffer_bytes;
        static u64 last_t[6], last_minflt, last_sigf, last_pc, last_pp, last_rc, last_rp;
        const u64 pc = Gow3Stats::protect_calls.load(), pp = Gow3Stats::protect_pages.load(),
                  rc = Gow3Stats::protect_revoke_calls.load(), rp = Gow3Stats::protect_revoke_pages.load();
        const u64 minflt = Gow3Stats::gpu_minor_faults.load(), sigf = Gow3Stats::gpu_signal_faults.load();
        static u64 last_copy_cpu, last_copy_sys, last_copy_flt;
        const u64 copy_cpu = Gow3Stats::t_copy_cpu.load(), copy_sys = Gow3Stats::copy_sys_us.load(),
                  copy_flt = Gow3Stats::copy_minflt.load();
        static u64 last_proc_flt, last_copy_ns, last_copy_bytes, last_rf, last_trf, last_twf;
        const u64 rf = Gow3Stats::read_faults.load(), trf = Gow3Stats::t_read_faults.load(),
                  twf = Gow3Stats::t_write_faults.load();
        // gow3: every frame over 40 ms after the first, at most 10 per 5 s window.
        static bool primed = false;
        static u32 stalls_shown = 0;
        // GOW3_STALL_MS lowers the threshold (R.9.5: 20 catches the frames that miss a vblank).
        static const double stall_ms = [] {
            const char* env = std::getenv("GOW3_STALL_MS");
            return env ? std::max(1.0, std::atof(env)) : 40.0;
        }();
        static const u32 stall_limit = std::getenv("GOW3_STALL_MS") ? 40u : 10u;
        const bool stall = primed && frame_ms > stall_ms && stalls_shown < stall_limit;
        stalls_shown += stall;
        primed = true;
        // Waits and work inside the long frame (window counters are reset every 5 s, so the
        // previous values are reset with them).
        static u64 last_wait[7];
        const u64 wait_now[7] = {Gow3Stats::t_readback_wait.load(), Gow3Stats::tick_wait_ns.load(),
                                 Gow3Stats::sync_recording_ns.load(), Gow3Stats::host_copies_wait_ns.load(),
                                 u64(Vulkan::g_gow3_compile_ns.load()), u64(Vulkan::g_gow3_compiles.load()),
                                 Gow3Stats::t_gc.load()};
        if (stall) {
            std::printf("Stall: %.1f ms frame; waits: readback %.1f ms, GPU ticks %.1f ms, "
                        "recorder %.1f ms, host copies %.1f ms; %llu compiles %.1f ms; GC %.1f ms\n",
                        frame_ms, (wait_now[0] - last_wait[0]) / 1e6, (wait_now[1] - last_wait[1]) / 1e6,
                        (wait_now[2] - last_wait[2]) / 1e6, (wait_now[3] - last_wait[3]) / 1e6,
                        static_cast<unsigned long long>(wait_now[5] - last_wait[5]),
                        (wait_now[4] - last_wait[4]) / 1e6, (wait_now[6] - last_wait[6]) / 1e6);
        }
        std::copy(std::begin(wait_now), std::end(wait_now), std::begin(last_wait));
        {
            // gow3 R.9.5: the draw pipe in this frame (stage A drains, stage B busy), mid-frame
            // submissions and the game threads' waits on async read faults.
            static const double cycles_per_ms = [] {
                const u64 t0 = Common::RawRDTSC();
                const auto c0 = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(2)) {
                }
                return double(Common::RawRDTSC() - t0) / 2.0;
            }();
            static u64 last_drain, last_busy, last_eager, last_async_ns, last_async;
            // R.9.7: this flip's packet, decoded (A) and recorded up to (B), measured from the
            // previous flip (when the game was let go on to this frame).
            static std::int64_t prev_flip_ns = 0;
            const std::int64_t flip_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            // Flip number k (as taken here) matches the k-th game submit / A decode / B arrival.
            static u32 taken = 0;
            const u32 k = (req.eop ? taken++ : taken - 1) % 16;
            const auto at = [&](int stage) {
                return prev_flip_ns
                           ? (Gow3Stats::flip_ring[stage][k].load(std::memory_order_relaxed) -
                              prev_flip_ns) / 1e6
                           : 0.0;
            };
            const double g_ms = at(0), a_ms = at(1), b_ms = at(2);
            static u32 game_hist[8] = {};
            game_hist[std::min<u32>(7, u32(std::max(0.0, g_ms + 8.0) / 4.0))]++;
            prev_flip_ns = flip_ns;
            static u32 flip_hist[8] = {};
            flip_hist[std::min<u32>(7, u32(std::max(0.0, b_ms) / 4.0))]++;
            if (frames == 0 && stall_ms < 40.0) {
                std::printf("       flip packet reached B after the previous flip (4 ms bins):");
                for (u32& n : flip_hist) {
                    std::printf(" %u", n);
                    n = 0;
                }
                std::printf("\n       game submitted the flip after the previous flip (4 ms bins from -8 ms):");
                for (u32& n : game_hist) {
                    std::printf(" %u", n);
                    n = 0;
                }
                std::printf("\n");
            }
            u64 drain = 0, busy = 0, eager = 0;
            presenter->GetRasterizer().PipeCounters(drain, busy, eager);
            const u64 async_ns = Gow3Stats::uc3_rb_async_ns.load();
            const u64 async = Gow3Stats::uc3_rb_async.load();
            if (stall || (stall_ms < 40.0 && frames == 30)) {
                std::printf("       %s pipe: flip submitted by the game %.1f ms, packet at A %.1f ms, at B %.1f ms; stage A drains %.1f ms, stage B busy %.1f ms, "
                            "%llu eager submissions; async fault waits %.1f ms (%llu)\n",
                            stall ? "" : "[median-ish frame]", g_ms, a_ms, b_ms, (drain - last_drain) / cycles_per_ms,
                            (busy - last_busy) / cycles_per_ms,
                            static_cast<unsigned long long>(eager - last_eager),
                            async_ns >= last_async_ns ? (async_ns - last_async_ns) / 1e6 : async_ns / 1e6,
                            static_cast<unsigned long long>(async >= last_async ? async - last_async : async));
            }
            last_drain = drain;
            last_busy = busy;
            last_eager = eager;
            last_async_ns = async_ns;
            last_async = async;
        }
        if (stall) {
            std::printf("       fault handlers: %llu read faults %.1f ms, write faults %.1f ms\n",
                        static_cast<unsigned long long>(rf - last_rf), (trf - last_trf) / 1e6,
                        (twf - last_twf) / 1e6);
        }
        last_rf = rf;
        last_trf = trf;
        last_twf = twf;
        const u64 copy_ns = Gow3Stats::t_copy.load(), copy_bytes = Gow3Stats::copy_bytes.load();
        u64 proc_flt = 0;
#ifndef _WIN32
        if (rusage usage{}; getrusage(RUSAGE_SELF, &usage) == 0) {
            proc_flt = usage.ru_minflt;
        }
#endif
        const u64 t_now[6] = {Gow3Stats::t_resident.load(), Gow3Stats::t_protect.load(),
                              Gow3Stats::t_image_create.load(), Gow3Stats::t_refresh.load(),
                              Gow3Stats::t_staging.load(), Gow3Stats::t_host_wait.load()};
        static u64 last_draws, last_dispatches, last_subs, last_sys, last_user, last_invol, last_vol;
        const u64 draws = Gow3Stats::draws.load(), dispatches = Gow3Stats::dispatches.load(),
                  subs = Gow3Stats::submissions.load(), sys_us = Gow3Stats::gpu_sys_us.load(),
                  user_us = Gow3Stats::gpu_user_us.load(), invol = Gow3Stats::gpu_invol_switches.load(),
                  vol = Gow3Stats::gpu_vol_switches.load();
        u64 gpu_ns = 0;
#ifdef _WIN32
        // gow3: Windows has no per-thread clock id; the thread's user + kernel time instead.
        static HANDLE gpu_thread = nullptr;
        if (!gpu_thread) {
            if (const unsigned long id = Gow3Stats::gpu_thread_id.load()) {
                gpu_thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
            }
        }
        if (FILETIME created, exited, kernel, user;
            gpu_thread && GetThreadTimes(gpu_thread, &created, &exited, &kernel, &user)) {
            const auto ns = [](const FILETIME& t) {
                return ((u64(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;
            };
            gpu_ns = ns(kernel) + ns(user);
        }
#else
        if (const int clock = Gow3Stats::gpu_thread_clock.load(); clock != -1) {
            timespec ts{};
            clock_gettime(static_cast<clockid_t>(clock), &ts);
            gpu_ns = u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
        }
#endif
        const u64 images = Gow3Stats::images_registered.load();
        const u64 image_bytes = Gow3Stats::image_upload_bytes.load();
        const u64 buffer_bytes = Gow3Stats::buffer_upload_bytes.load();
        if (stall) {
            std::printf("       GPU thread on CPU %.1f ms; %llu images registered, "
                        "%.1f MB image uploads, %.1f MB buffer uploads\n",
                        (gpu_ns - last_gpu_ns) / 1e6,
                        static_cast<unsigned long long>(images - last_images),
                        (image_bytes - last_image_bytes) / 1e6,
                        (buffer_bytes - last_buffer_bytes) / 1e6);
            std::printf("       %llu draws, %llu dispatches, %llu submissions; GPU thread user %.1f ms, "
                        "kernel %.1f ms, %llu preempted, %llu waits\n",
                        static_cast<unsigned long long>(draws - last_draws),
                        static_cast<unsigned long long>(dispatches - last_dispatches),
                        static_cast<unsigned long long>(subs - last_subs), (user_us - last_user) / 1e3,
                        (sys_us - last_sys) / 1e3, static_cast<unsigned long long>(invol - last_invol),
                        static_cast<unsigned long long>(vol - last_vol));
        }
        if (stall) {
            std::printf("       ms in: resident %.1f, protect %.1f, image create %.1f, "
                        "image refresh %.1f, staging %.1f, waiting for host copies %.1f\n",
                        (t_now[0] - last_t[0]) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                        (t_now[2] - last_t[2]) / 1e6, (t_now[3] - last_t[3]) / 1e6,
                        (t_now[4] - last_t[4]) / 1e6, (t_now[5] - last_t[5]) / 1e6);
            std::printf("       GPU thread page faults %llu (process %llu), protection faults %llu; "
                        "protect calls %llu (%llu pages), of which write-revoking %llu (%llu pages)\n",
                        static_cast<unsigned long long>(minflt - last_minflt),
                        static_cast<unsigned long long>(proc_flt - last_proc_flt),
                        static_cast<unsigned long long>(sigf - last_sigf),
                        static_cast<unsigned long long>(pc - last_pc),
                        static_cast<unsigned long long>(pp - last_pp),
                        static_cast<unsigned long long>(rc - last_rc),
                        static_cast<unsigned long long>(rp - last_rp));
        }
        last_pc = pc;
        last_pp = pp;
        last_rc = rc;
        last_rp = rp;
        last_minflt = minflt;
        if (stall && copy_ns > last_copy_ns) {
            std::printf("       guest copies %.1f MB in %.1f thread-ms, %.1f ms on CPU (kernel %.1f ms, "
                        "%llu page faults in large copies) (%.2f GB/s per thread)\n",
                        (copy_bytes - last_copy_bytes) / 1e6, (copy_ns - last_copy_ns) / 1e6,
                        (copy_cpu - last_copy_cpu) / 1e6, (copy_sys - last_copy_sys) / 1e3,
                        static_cast<unsigned long long>(copy_flt - last_copy_flt),
                        double(copy_bytes - last_copy_bytes) / double(copy_ns - last_copy_ns));
        }
        last_copy_ns = copy_ns;
        last_copy_cpu = copy_cpu;
        last_copy_sys = copy_sys;
        last_copy_flt = copy_flt;
        last_copy_bytes = copy_bytes;
        last_proc_flt = proc_flt;
        last_sigf = sigf;
        std::copy(std::begin(t_now), std::end(t_now), std::begin(last_t));
        last_draws = draws;
        last_dispatches = dispatches;
        last_subs = subs;
        last_sys = sys_us;
        last_user = user_us;
        last_invol = invol;
        last_vol = vol;
        last_gpu_ns = gpu_ns;
        last_images = images;
        last_image_bytes = image_bytes;
        last_buffer_bytes = buffer_bytes;
        ++frames;
        Gow3Stats::flips.fetch_add(1, std::memory_order_relaxed);
        const double window = std::chrono::duration<double>(now - window_start).count();
        if (window >= 5.0) {
            const u32 compiles = Vulkan::g_gow3_compiles.exchange(0);
            const u64 compile_ns = Vulkan::g_gow3_compile_ns.exchange(0);
            const double capture_ms = Vulkan::g_gow3_capture_ns.exchange(0) / 1e6;
            const double async_ms = Vulkan::g_gow3_async_ns.exchange(0) / 1e6;
            const double wait_ms = Vulkan::g_gow3_compile_wait_ns.exchange(0) / 1e6;
            const double snapshot_ms = Vulkan::g_gow3_snapshot_ns.exchange(0) / 1e6;
            const double guest_wait_ms = Vulkan::g_gow3_guest_wait_ns.exchange(0) / 1e6;
            const u32 pending_draws = Vulkan::g_gow3_pending_draws.exchange(0);
            static u64 uc3_window_draws = 0;
            const u64 uc3_rb_inflight = Gow3Stats::uc3_rb_inflight.exchange(0);
            // uc3: draws left out for want of a pipeline (pending compiles included).
            std::printf("uc3 draw stats: %llu draws, %llu without pipeline (%u of them pending "
                        "compiles), %llu indirect, %llu indirect without pipeline; readbacks: "
                        "%llu all-at-once, %.1f MB; %llu with hot windows (+%llu windows); "
                        "fault write ticks: %llu open, %llu in flight (%.1f ticks behind), "
                        "%llu done, %llu unknown; fast path: %llu side copies (%.2f ms, %llu after waiting for the write), "
                        "%llu full waits; GPU: %llu pass ends, %llu barriers (%llu on graphics stages), "
                        "%llu layout transitions (%llu on 1080p+ images)\n",
                        (unsigned long long)(Gow3Stats::draws.load() - uc3_window_draws),
                        (unsigned long long)Gow3Stats::uc3_no_pipeline_draws.exchange(0), pending_draws,
                        (unsigned long long)Gow3Stats::uc3_indirect_draws.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_no_pipeline_indirect.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_readback_all.exchange(0),
                        Gow3Stats::uc3_readback_all_bytes.exchange(0) / 1e6,
                        (unsigned long long)Gow3Stats::uc3_readback_hot.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_readback_hot_windows.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_rb_open.exchange(0),
                        (unsigned long long)uc3_rb_inflight,
                        uc3_rb_inflight ? double(Gow3Stats::uc3_rb_inflight_ticks.exchange(0)) /
                                              double(uc3_rb_inflight)
                                        : 0.0,
                        (unsigned long long)Gow3Stats::uc3_rb_done.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_rb_unknown.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_rb_side.exchange(0),
                        Gow3Stats::uc3_rb_side_ns.exchange(0) / 1e6,
                        (unsigned long long)Gow3Stats::uc3_rb_side_waits.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_rb_full.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_pass_ends.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_barriers.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_barriers_gfx.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_transits.exchange(0),
                        (unsigned long long)Gow3Stats::uc3_transits_big.exchange(0));
            {
                const u64 async = Gow3Stats::uc3_rb_async.exchange(0);
                std::printf("uc3 readbacks: %llu async, %.1f ms waited by faulting threads, %llu "
                            "left protected (rewritten), %llu GPU-side overlap waits; sync: %llu "
                            "GPU-side, %llu write faults, %llu other; SRT walker loads: %llu "
                            "from backing, %llu read back, %llu stale (GOW3_SRT_STALE); after the walk: %llu "
                            "pointers, %llu dwords read back in %llu walks; leaf copy: %llu "
                            "zeroed (%llu only in async windows, %llu new addresses), %llu read back (sharps, no ring), of them %llu no ring, %llu GPU "
                            "copies, %llu dead (GOW3_LEAF_DEAD); constant ring: %llu copies, %llu full; leaf "
                            "downloads: %llu queued, %llu landed, %llu rewritten, %llu no slot\n",
                            (unsigned long long)async, Gow3Stats::uc3_rb_async_ns.exchange(0) / 1e6,
                            (unsigned long long)Gow3Stats::uc3_rb_async_dirty.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_rb_async_overlap.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_rb_sync_gpu.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_rb_sync_write.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_rb_sync_other.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_emulated.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_sync.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_stale.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_ptr_sync.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_leaf_sync.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_sync_walks.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_leaf_zeroed.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_by_window.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_new_addr.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_srt_leaf_sharp.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_noring.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_copies.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_dead.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_ring_copies.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_ring_full.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_dl_queued.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_dl_landed.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_dl_dirty.exchange(0),
                            (unsigned long long)Gow3Stats::uc3_leaf_dl_full.exchange(0));
            }
            uc3_window_draws = Gow3Stats::draws.load();
            const u64 direct = Vulkan::Scheduler::direct_recordings.exchange(0);
            const u64 faults = Gow3Stats::tracker_faults.exchange(0);
            // GPU command thread CPU time per draw: comparable between builds even when the scene
            // (and so the frame rate) differs a little.
            static u64 window_draws = 0, window_gpu_us = 0;
            const u64 all_draws = Gow3Stats::draws.load();
            const u64 gpu_us = Gow3Stats::gpu_user_us.load() + Gow3Stats::gpu_sys_us.load();
            const double us_per_draw = all_draws > window_draws
                                           ? double(gpu_us - window_gpu_us) / (all_draws - window_draws)
                                           : 0.0;
            const double draws_per_frame = frames ? double(all_draws - window_draws) / frames : 0.0;
            window_draws = all_draws;
            window_gpu_us = gpu_us;
            std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); "
                        "%u shader/pipeline compiles, %.1f ms; async: capture %.1f ms, workers "
                        "%.1f ms, sync wait %.1f ms, snapshots %.1f ms, GPU capture wait %.1f ms, "
                        "%u pending draws; %llu recorder syncs; "
                        "%.0f write faults/s, %lld hot pages; GPU thread %.2f us/draw, "
                        "%.0f draws/frame, idle %.1f%%; blocked: recorder %.1f%%, host copies "
                        "%.1f%% (%.0f/frame), copy threads %.1f%%, GPU ticks %.1f%%; "
                        "reduced-size draws %.0f/frame of %.0f in the scene\n",
                        frames / window, worst_ms, EmulatorSettings.GetVblankFrequency(), compiles,
                        compile_ns / 1e6, capture_ms, async_ms, wait_ms, snapshot_ms, guest_wait_ms,
                        pending_draws,
                        static_cast<unsigned long long>(direct),
                        faults / window, static_cast<long long>(Gow3Stats::hot_pages.load()),
                        us_per_draw, draws_per_frame,
                        Gow3Stats::gpu_idle_ns.exchange(0) / (window * 1e7),
                        Gow3Stats::sync_recording_ns.exchange(0) / (window * 1e7),
                        Gow3Stats::host_copies_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(Gow3Stats::host_copy_waits.exchange(0)) / frames : 0.0,
                        Gow3Stats::copy_threads_wait_ns.exchange(0) / (window * 1e7),
                        Gow3Stats::tick_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(Gow3Stats::reduced_draws.exchange(0)) / frames : 0.0,
                        frames ? double(Gow3Stats::scene_draws.exchange(0)) / frames : 0.0);
            presenter->GetRasterizer().GetPipelineCache().ReportCompilerStats();
            presenter->GetRasterizer().GetPipelineCache().ReportPrepTrust();
            Vulkan::WaitDiagnostics::Report();
            // Frame pacing: spread of the guest flip intervals (judder that the mean hides).
            if (intervals.size() > 2) {
                std::vector<double> sorted = intervals;
                std::sort(sorted.begin(), sorted.end());
                const double median = sorted[sorted.size() / 2];
                double sum = 0, sq = 0;
                u32 spikes = 0;
                for (const double ms : intervals) {
                    sum += ms;
                    sq += ms * ms;
                    spikes += ms > 1.5 * median;
                }
                const double mean = sum / intervals.size();
                std::printf("Frame pacing: median %.2f ms, stddev %.2f ms, p99 %.2f ms, "
                            "%u frames over 1.5x median\n",
                            median, std::sqrt(std::max(0.0, sq / intervals.size() - mean * mean)),
                            sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], spikes);
            }
            intervals.clear();
            window_start = now;
            frames = 0;
            worst_ms = 0;
            stalls_shown = 0;
            std::fill(std::begin(last_wait) + 1, std::begin(last_wait) + 6, 0);
        }
    }

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        std::unique_lock lock{port->port_mutex};
        if (port->early_released[port->prev_index]) {
            // uc3 T3: released at submission; the game may own it again already.
            port->early_released[port->prev_index] = false;
        } else {
            port->buffer_labels[port->prev_index] = 0;
            port->SignalVoLabel();
        }
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    RunPresenter([this] {
        const auto empty_frame = presenter->PrepareBlankFrame(true);
        presenter->Present(empty_frame, false, false);
    }, true);
}

void VideoOutDriver::DrawLastFrame() {
    RunPresenter([this] {
        const auto frame = presenter->PrepareLastFrame();
        if (frame != nullptr) {
            presenter->Present(frame, true);
        }
    }, true);
}

namespace {
// uc3 (from SuperMedo's uncharted_pc, GPL): end-of-pipe flips the GPU side has reached, for the
// frames-ahead limit in sceGnmSubmitAndFlipCommandBuffersForWorkload.
std::atomic<u64> eop_flips_reached{0};
std::mutex eop_flip_mutex;
std::condition_variable eop_flip_cv;
} // namespace

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
        {
            std::scoped_lock lock{eop_flip_mutex};
            eop_flips_reached.fetch_add(1, std::memory_order_relaxed);
        }
        eop_flip_cv.notify_all();
    }

    return true;
}

u64 EopFlipsReached() {
    return eop_flips_reached.load(std::memory_order_relaxed);
}

void WaitEopFlipsReached(u64 count, u32 timeout_ms) {
    std::unique_lock lock{eop_flip_mutex};
    eop_flip_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [count] {
        return eop_flips_reached.load(std::memory_order_relaxed) >= count;
    });
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    Vulkan::Frame* frame;
    if (index == -1) {
        // uc3: GOW3_FLASH_TRACE=1 — a blank flip shows one black frame.
        static const bool flash_trace = std::getenv("GOW3_FLASH_TRACE") != nullptr;
        if (flash_trace) {
            std::printf("Flash trace: %.2f s: blank flip (buffer index -1)\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - uc3_flash_clock).count());
        }
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    size_t queued = 0;
    {
        std::scoped_lock lock{mutex};
        requests.push({
            .frame = frame,
            .port = port,
            .flip_arg = flip_arg,
            .index = index,
            .eop = is_eop,
        });
        queued = requests.size();
    }
    request_cv.notify_one();

    // uc3 T3 (Opus R.9, GOW3_EARLY_VO_LABEL=1): the buffer submitted before this one was copied
    // by its own PrepareFrame already, and later GPU writes to it are ordered after that copy:
    // the game may render into it now instead of at the next vblank tick (like triple
    // buffering). Only with at most one flip waiting, so the game stays at most a frame ahead.
    static const bool early_label = [] {
        const char* env = std::getenv("GOW3_EARLY_VO_LABEL");
        return env && env[0] == '1';
    }();
    if (early_label && index >= 0) {
        std::unique_lock lock{port->port_mutex};
        const int previous = port->last_submitted;
        port->last_submitted = index;
        if (previous >= 0 && previous != index && queued <= 1 &&
            port->buffer_labels[previous] != 0) {
            port->buffer_labels[previous] = 0;
            port->early_released[previous] = true;
            port->SignalVoLabel();
        }
    }
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    // gow3: frame limit (see EmulatorSettings::GetFrameLimit). A request waits in the queue
    // until its slot; slots advance by one period (no drift) but never lag behind by more.
    // gow3: the in-game menu changes the limit live (Gow3Settings::fps_limit); GOW3_FPS_LIMIT
    // and the uncapped presets keep their fixed limit.
    const bool fixed_limit = std::getenv("GOW3_FPS_LIMIT") || EmulatorSettings.GetVblankFrequency() == 480;
    const auto current_limit = [&] {
        return fixed_limit ? EmulatorSettings.GetFrameLimit()
                           : u32(Gow3Graphics::PresentLimit(Gow3Settings::Get().fps_limit.load(),
                                                            Gow3Settings::Get().applied_engine_fps.load()));
    };
    u32 frame_limit = current_limit();
    auto frame_period = frame_limit ? std::chrono::nanoseconds(1000000000 / frame_limit)
                                    : std::chrono::nanoseconds(0);
    auto next_flip = std::chrono::steady_clock::now();
    std::printf("VideoOut: vblank %u Hz, frame limit %u FPS\n",
                EmulatorSettings.GetVblankFrequency(), frame_limit);

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    // gow3: with a frame limit (uncapped presets) a queued flip is presented as soon as it
    // arrives and its slot allows, between vblanks, instead of on the next vblank tick. Only
    // when the limit exceeds the vblank rate does that matter; at or below it the outer
    // vblank-paced flip below already satisfies the limit (and the sub-vblank path stalls the
    // guest on this port, so keep it off there).
    bool immediate_flips = frame_limit > EmulatorSettings.GetVblankFrequency();

    while (!token.stop_requested()) {
        if (const u32 limit = current_limit(); limit != frame_limit) {
            frame_limit = limit;
            frame_period = limit ? std::chrono::nanoseconds(1000000000 / limit) : std::chrono::nanoseconds(0);
            immediate_flips = limit > EmulatorSettings.GetVblankFrequency();
            next_flip = std::chrono::steady_clock::now();
            std::printf("VideoOut: frame limit %u FPS\n", limit);
        }
        timer.Start();
        const auto tick_deadline = std::chrono::steady_clock::now() + vblank_period;

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        const auto now = std::chrono::steady_clock::now();
        const bool flip_slot = !frame_limit || now >= next_flip;
        if (flip_slot && vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (request && frame_limit) {
                next_flip = std::max(next_flip + frame_period, now - frame_period);
            }
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                FRAME_END;
            }
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : main_port.vblank_events) {
                auto equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }

        if (!immediate_flips || main_port.flip_rate != 0) {
            timer.End();
            continue;
        }
        while (!token.stop_requested()) {
            {
                std::unique_lock lk{mutex};
                if (!request_cv.wait_until(lk, tick_deadline,
                                           [&] { return !requests.empty(); })) {
                    break; // next vblank
                }
            }
            if (std::chrono::steady_clock::now() < next_flip) {
                if (next_flip >= tick_deadline) {
                    break;
                }
                std::this_thread::sleep_until(next_flip);
            }
            const auto now = std::chrono::steady_clock::now();
            const auto request = receive_request();
            if (request) {
                next_flip = std::max(next_flip + frame_period, now - frame_period);
                Flip(request);
                FRAME_END;
            }
        }
        std::this_thread::sleep_until(tick_deadline);
    }
}

} // namespace Libraries::VideoOut
