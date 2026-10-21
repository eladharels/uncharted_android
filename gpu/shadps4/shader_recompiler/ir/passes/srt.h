// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include <vector>
#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include "common/arch.h"
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

// uc3 (from SuperMedo's uncharted_pc, GPL): a ReadConst whose offset varies per lane (a constant
// block's pointer plus a per-pixel offset, as addr64 buffer loads compute it) reads a window of the
// block's first dwords that the walker copies to the flat buffer; its flags are the window's start
// with this bit.
constexpr u32 ReadConstWindowFlag = 1u << 16;
// uc3: GOW3_WINDOW_DWORDS=<n> (16-4096, default 256): dwords copied per window.
inline u32 ReadConstWindowDwords() {
    static const u32 dwords = [] {
        const char* env = std::getenv("GOW3_WINDOW_DWORDS");
        const u32 v = env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 256u;
        return v < 16u ? 16u : (v > 4096u ? 4096u : v);
    }();
    return dwords;
}
// uc3: marks the ReadConsts made from an addr64 buffer load (GOW3_WINDOW_ADDR64_ONLY=1 gives only
// those a window). Cleared by the offset assignment; masked off by the SPIR-V emitter.
constexpr u32 ReadConstAddr64Mark = 1u << 17;
inline bool WindowAddr64Only() {
    static const bool only = std::getenv("GOW3_WINDOW_ADDR64_ONLY") != nullptr;
    return only;
}

struct SrtWalkCtx;
#ifdef ARCH_X86_64
// uc3 (Fable R.8.1): the walker takes a context: the pointer loader it calls for each table
// pointer and a table it fills with the guest address of each flattened dword.
using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/,
                                            SrtWalkCtx* /*ctx*/);
inline const void* SrtWalkerCode(PFN_SrtWalker walker) {
    return reinterpret_cast<const void*>(walker);
}
#else
/// On non-x86 hosts the walker is portable SRT bytecode, not executable machine code
/// (gow3_android), extended with the uc3 walker context and ReadConst windows.
/// `ctx`: the uc3 walker context (pointer loader, guest source table), or null for plain reads.
void RunSrtProgram(const u32* program, const u32* user_data, u32* flat_dst,
                   SrtWalkCtx* ctx = nullptr);
struct SrtWalker {
    const u32* program{};
    void operator()(const u32* user_data, u32* flat_dst) const {
        RunSrtProgram(program, user_data, flat_dst);
    }
    explicit operator bool() const { return program != nullptr; }
};
using PFN_SrtWalker = SrtWalker;
inline const void* SrtWalkerCode(PFN_SrtWalker walker) {
    return walker.program;
}
#endif
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

/// uc3: a table pointer as the walker follows it: `host` is where its loads read (the
/// unprotected backing view, or the guest address when there is none), `guest` its guest
/// address.
struct SrtPtr {
    u64 host;
    u64 guest;
};
/// Loads the 8-byte pointer at host_base + off (guest_base + off in the guest; guest_base is 0
/// for user data) and returns where to read the table it points to.
using PFN_SrtLoadPtr = SrtPtr PS4_SYSV_ABI (*)(const u8* host_base, u64 guest_base, u64 off);
/// uc3 (Fable R.8.2): a flattened dword whose guest source the GPU wrote; with GOW3_LEAF_COPY the
/// flat holds 0 there and the draw recording thread copies the 4 bytes on the GPU.
struct SrtGpuLeaf {
    u32 dst_dw;
    u32 pad;
    u64 src;
};
/// Rereads the flattened dwords whose guest source is GPU-written (src[i] is 0 for none), or,
/// given `leaves`, zeroes them and lists them for a GPU copy.
using PFN_SrtFixup = void (*)(const u64* src, u32* flat, size_t count,
                              std::vector<SrtGpuLeaf>* leaves);
/// Reads a GPU-written dword back (synchronous) into `out`.
using PFN_SrtReadDword = void (*)(u64 src, u32* out);

struct SrtWalkCtx {
    PFN_SrtLoadPtr load_ptr; // [r8 + 0]
    u64* src_table;          // [r8 + 8]
};

void SetSrtWalkerHooks(PFN_SrtLoadPtr load_ptr, PFN_SrtFixup fixup,
                       PFN_SrtReadDword read_dword = nullptr);
/// Runs a walker. `sync` false: GPU-written bytes are not read back. `leaves`: see PFN_SrtFixup.
void RunSrtWalker(PFN_SrtWalker walker, const u32* user_data, u32* flat, size_t flat_dw,
                  bool sync = true, std::vector<SrtGpuLeaf>* leaves = nullptr);
/// uc3 R.21: while set on a thread, RunSrtWalker adds the guest pages it reads (table pointers
/// and flattened leaves, 4 KiB pages) to this list (unsorted, may repeat).
inline thread_local std::vector<u64>* t_srt_read_pages = nullptr;
/// R.10 A1 v8 (diagnostics): with t_srt_read_pages, the pages of the table pointers alone.
inline thread_local std::vector<u64>* t_srt_ptr_pages = nullptr;
/// uc3 R.22: with t_srt_read_pages, also the value of every guest location the walk read (table
/// pointers: 8 bytes, flattened leaves: 4), at the host address that reads it unprotected. Equal
/// values later mean the walk would produce the same flat.
/// R.10 A1 (Fable): contiguous reads are one run, compared with a copy of the bytes read.
struct SrtCheck {
    u64 host;
    u32 raw_off; ///< offset of the bytes read in the draw's raw copy
    u32 size;    ///< bytes; SrtCheckSameAsPrev: same range and bytes in the previous draw
};
inline constexpr u32 SrtCheckSameAsPrev = 1u << 31;
/// R.10 A1 v9 (Fable): the guest ranges the walk read (contiguous reads merged), for telling a
/// page write that misses them from one that hits them.
struct SrtGuestRun {
    u64 guest;
    u32 size;
};
inline thread_local std::vector<SrtGuestRun>* t_srt_guest_runs = nullptr; ///< set with t_srt_checks
inline thread_local std::vector<SrtCheck>* t_srt_checks = nullptr;
inline thread_local std::vector<u8>* t_srt_check_bytes = nullptr; ///< set with t_srt_checks
/// True when every check still holds. `skip_same`: the previous draw's checks held in the
/// same wait epoch, so the runs marked SrtCheckSameAsPrev are not compared again.
inline bool SrtChecksHold(const SrtCheck* checks, u32 count, const u8* raw, bool skip_same,
                          u32* compared_bytes = nullptr) noexcept {
    u32 bytes = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto& check = checks[i];
        if (skip_same && (check.size & SrtCheckSameAsPrev)) {
            continue;
        }
        const u32 size = check.size & ~SrtCheckSameAsPrev;
        bytes += size;
        if (__builtin_memcmp(reinterpret_cast<const void*>(check.host), raw + check.raw_off,
                             size) != 0) {
            if (compared_bytes) {
                *compared_bytes = bytes;
            }
            return false;
        }
    }
    if (compared_bytes) {
        *compared_bytes = bytes;
    }
    return true;
}
/// Reads one GPU-written dword back through the hook (or plainly when there is none).
void SrtReadDword(u64 src, u32* out);
/// True while the walker of this thread may read GPU-written bytes back.
bool SrtWalkerSyncAllowed();

/// uc3: a load the SRT walker faulted on: `mov rdi, [mem]` (8 bytes) or `mov r10d, [mem]`
/// (4 bytes). False when `rip` is not walker code or the instruction is not one of those.
struct SrtWalkerLoad {
    bool to_rdi;
    u32 size;
    u32 length;
};
bool DecodeSrtWalkerLoad(const void* rip, SrtWalkerLoad& load);

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
