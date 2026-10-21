#include "gow3_write_log.h"
// gow3: glue between the C loader and the vendored shadPS4 video core.
#include "gow3_overlay.h"
#include "gow3_png.h"
#include "gow3_settings.h"
#include "gow3_copy.h"
#include "gow3_profiler.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "gow3_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../gow3gpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
int runtime_memory_region_backing(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped,
                                  const unsigned char** backing);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const Gow3GpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
// uc3: precise readbacks read-protect GPU-modified pages, and a fault on a copy thread waited
// for the GPU thread, which waited for the copies. ResolveHostCopy brings the GPU's data back
// first (as the fault would have, upstream); the copy itself then reads through the backing
// view, past a protection a page may have gained since (its CPU contents are what the copy
// is meant to carry at this point of the command stream).
static bool PreciseReadbacks() {
    static const bool precise = EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    return precise;
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size, bool force_backing = false) {
    const bool precise = force_backing || PreciseReadbacks();
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        const unsigned char* backing = nullptr;
        if (!runtime_memory_region_backing(source, &start, &end, &mapped, precise ? &backing : nullptr)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (!mapped) std::memset(dest, 0, n);
        else if (backing) std::memcpy(dest, backing, n);
        else std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        source += n; dest += n; size -= n;
    }
}
// Not from under a memory tracker region lock (ForEachUploadRange takes one per region while
// it collects the ranges): the readback takes that lock again, and on the GPU command thread
// it first drains the draw pipe, whose thread may be waiting for the same lock.
void MemoryManager::ResolveHostCopy(VAddr source, u64 size) {
    if (PreciseReadbacks() && rasterizer && rasterizer->IsGpuSideThread() &&
        rasterizer->GetBufferCache().IsRegionGpuModified(source, size)) {
        rasterizer->ReadMemory(source, size, true);
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // gow3: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !Gow3Copy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    Gow3Copy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
void MemoryManager::CopySparseMemoryBacking(VAddr source, u8* dest, u64 size) {
    CopySparseSerial(source, dest, size, true);
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    Gow3WriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, Gow3WriteLog::Backing);
    const bool written = runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
    // 这些写入绕过页跟踪，较大的写入可能覆盖纹理；小写入通常只是 fence/label。
    if (written && rasterizer && size > 8) {
        rasterizer->GetTextureCache().NoteBackingWrite(reinterpret_cast<VAddr>(address), size);
    }
    return written;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    Gow3Stats::Timer timer{Gow3Stats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    Gow3Stats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    Gow3Stats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        Gow3Stats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        Gow3Stats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 Gow3DisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

// gow3: set from Instance::CollectDeviceParameters() and read by the buffer cache (buffer.cpp).
namespace {
std::atomic<u32> g_gpu_vendor_id{0};
} // namespace

void SetGow3GpuVendorId(u32 id) {
    g_gpu_vendor_id.store(id, std::memory_order_relaxed);
}

u32 Gow3GpuVendorId() {
    return g_gpu_vendor_id.load(std::memory_order_relaxed);
}

#ifdef GOW3_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (GOW3_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("gow3:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int gow3gpu_init(const Gow3GpuConfig* config) {
    // native 移植标志：用于确认设备运行的是包含 081df6f 修复的 GPU 库。
    std::printf("GOW3_NATIVE_FIX_081DF6F=active\n");
    // native 移植标志：用于确认设备运行的是包含 195c7ab 修复的 GPU 库。
    std::printf("GOW3_NATIVE_FIX_195C7AB=active\n");
    // native 移植标志：用于确认设备运行的是包含 a101edb 修复的 GPU 库。
    std::printf("GOW3_NATIVE_FIX_A101EDB=active\n");
    // native 移植标志：用于确认设备运行的是 recorder chunk 防护修复的 GPU 库。
    std::printf("GOW3_NATIVE_FIX_SCHEDULER_CHUNK=active\n");
    // native 移植标志：用于确认设备运行的是各向异性过滤设置移植的 GPU 库。
    std::printf("GOW3_NATIVE_FIX_9D0F51C_ANISOTROPY=active\n");
    std::printf("GOW3_NATIVE_FIX_9D0F51C_VIBRATION=active\n");
    Gow3Settings::Load();
    Gow3Profiler::Start(); // uncharted_android: GOW3_PROFILE
#ifdef GOW3_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("GOW3_GPU_USER_DIR")) _putenv_s("GOW3_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("GOW3_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
    const std::string title = config->title ? config->title : "God of War III";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("gow3:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        // uc3: always in the log (the GPU log level hides LOG_INFO): a close request ends the
        // process without any fault report, which otherwise looks like a silent crash.
        std::printf("Window closed (SDL quit event): exiting\n");
        std::fflush(stdout);
        // uc3 (Fable R.8.4): let submitted GPU work finish (bounded) and report what does
        // not; a process exiting with a queue blocked on a never-signalled tick cannot end.
        Vulkan::Semaphore::SettleAll(2000);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void gow3gpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t gow3gpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int gow3gpu_handle_fault(void* ucontext, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned gow3gpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

extern "C" unsigned long gow3gpu_cp_thread_id(void) {
    return Gow3Stats::gpu_thread_id.load();
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("gow3:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int gow3gpu_overlay_captures_input(void) {
    return Gow3Overlay::CapturesInput() ? 1 : 0;
}

// C entry point like the others: libgow3gpu.so only exports C symbols to the runtime bridge, so a
// plain Gow3Overlay::Available() would be an undefined symbol at load time.
extern "C" int gow3gpu_overlay_available(void) {
    return Gow3Overlay::Available() ? 1 : 0;
}

extern "C" int gow3gpu_vibration_percent(void) {
    return Gow3Settings::Get().vibration.load(std::memory_order_relaxed);
}

extern "C" int gow3gpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int gow3gpu_choice_begin(const char* title, const char* const* items, const char* const* images,
                                    int count, int focus) {
    if (!g_window || count <= 0) return 0;
    // The save list is shown as a dialog the player has to read. When the overlay could not come up
    // the dialog would never be drawn while the game kept waiting for an answer and held its input
    // neutral, so refuse here instead of opening a list nobody can see.
    if (!gow3gpu_overlay_available()) {
        LOG_ERROR(Lib_SaveDataDialog,
                  "save list: the overlay is unavailable, refusing the dialog ({} entries)", count);
        return 0;
    }
    // Pictures are decoded here, on the game's thread, before the list is shown.
    std::vector<Gow3Png::Image> pictures(count);
    for (int i = 0; images && i < count; ++i) {
        if (images[i] && !Gow3Png::LoadRgba(images[i], pictures[i])) {
            LOG_WARNING(Lib_SaveDataDialog, "save list: no picture from {}", images[i]);
        }
    }
    Gow3Overlay::BeginChoice(title ? title : "", std::vector<std::string>(items, items + count),
                             std::move(pictures), focus);
    return 1;
}

extern "C" int gow3gpu_choice_poll(void) {
    return Gow3Overlay::PollChoice();
}

extern "C" void gow3gpu_choice_cancel(void) {
    Gow3Overlay::CancelChoice();
}

extern "C" void gow3gpu_pad_publish(uint32_t buttons) {
    Gow3Overlay::PublishPad(buttons);
}

extern "C" int gow3gpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
