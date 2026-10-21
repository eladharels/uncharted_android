// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <boost/container/flat_map.hpp>
#include <cstdio>
#include <cstdlib>
#include <boost/container/small_vector.hpp>
#include <queue>
#include "common/arch.h"
#ifdef ARCH_X86_64
#include <mutex>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#else
#include <atomic>
#include <vector>
#include "../../../../../src/runtime.h"
#endif
#include "common/decoder.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/value.h"

#ifdef ARCH_X86_64

using namespace Xbyak::util;

static Xbyak::CodeGenerator g_srt_codegen(32_MB);
static const u8* g_srt_codegen_start = nullptr;
// gow3: the shader compiler and the background cache warm-up append walkers from different
// threads.
static std::mutex g_srt_codegen_mutex;

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    std::scoped_lock lock{g_srt_codegen_mutex};
    const auto func_addr = (PFN_SrtWalker)g_srt_codegen.getCurr();
    g_srt_codegen.db(ptr, size);
    g_srt_codegen.ready();
    return func_addr;
}

} // namespace Shader
#endif // ARCH_X86_64

// uc3 walker hooks (Fable R.8.1 and later): shared by the x86-64 machine-code walker and, in the
// android port, the arm64 SRT bytecode (RunSrtProgram), which reads tables through the same
// pointer loader and records the same guest sources for the fixup after the walk.
namespace Shader {

namespace {

SrtPtr PS4_SYSV_ABI DefaultSrtLoadPtr(const u8* host_base, u64 guest_base, u64 off) {
    u64 value;
    std::memcpy(&value, host_base + off, sizeof(value));
    value &= 0xFFFFFFFFFFFFULL;
    return {value, value};
}

std::atomic<PFN_SrtLoadPtr> g_srt_load_ptr{DefaultSrtLoadPtr};
std::atomic<PFN_SrtFixup> g_srt_fixup{nullptr};
std::atomic<PFN_SrtReadDword> g_srt_read_dword{nullptr};
thread_local bool t_srt_sync = true;

} // namespace

void SetSrtWalkerHooks(PFN_SrtLoadPtr load_ptr, PFN_SrtFixup fixup, PFN_SrtReadDword read_dword) {
    g_srt_load_ptr.store(load_ptr ? load_ptr : DefaultSrtLoadPtr);
    g_srt_fixup.store(fixup);
    g_srt_read_dword.store(read_dword);
}

void SrtReadDword(u64 src, u32* out) {
    if (const auto read = g_srt_read_dword.load(std::memory_order_relaxed)) {
        read(src, out);
    } else {
        std::memcpy(out, reinterpret_cast<const void*>(src), sizeof(u32));
    }
}

bool SrtWalkerSyncAllowed() {
    return t_srt_sync;
}

extern "C" int runtime_memory_region_backing(uintptr_t address, uintptr_t* start, uintptr_t* end,
                                             int* mapped, const unsigned char** backing);
namespace {
thread_local PFN_SrtLoadPtr t_srt_inner_load = nullptr;
// uc3 R.22: the unprotected host address that reads guest `address` (the guest address itself
// when it has no backing view).
u64 SrtHostOf(u64 address, u64 need) {
    uintptr_t start = 0, end = 0;
    int mapped = 0;
    const unsigned char* backing = nullptr;
    if (!runtime_memory_region_backing(address, &start, &end, &mapped, &backing) || !mapped ||
        !backing || end - address < need) {
        return address;
    }
    return reinterpret_cast<u64>(backing);
}
void SrtRecordCheck(u64 guest, u32 size) {
    if (t_srt_guest_runs) {
        auto& runs = *t_srt_guest_runs;
        if (!runs.empty() && runs.back().guest + runs.back().size == guest) {
            runs.back().size += size;
        } else {
            runs.push_back({guest, size});
        }
    }
    const u64 host = SrtHostOf(guest, size);
    auto& bytes = *t_srt_check_bytes;
    const u32 off = static_cast<u32>(bytes.size());
    bytes.resize(off + size);
    std::memcpy(bytes.data() + off, reinterpret_cast<const void*>(host), size);
    // R.10 A1 (Fable): reads that continue the previous one extend its run.
    auto& checks = *t_srt_checks;
    if (!checks.empty() && checks.back().host + checks.back().size == host &&
        checks.back().raw_off + checks.back().size == off) {
        checks.back().size += size;
        return;
    }
    checks.push_back({host, off, size});
}
// uc3 R.21: records the page of each table pointer the walker loads (guest_base 0 is user data).
SrtPtr PS4_SYSV_ABI RecordingSrtLoadPtr(const u8* host_base, u64 guest_base, u64 off) {
    if (guest_base != 0 && t_srt_read_pages) {
        t_srt_read_pages->push_back((guest_base + off) >> 12);
        t_srt_read_pages->push_back((guest_base + off + 7) >> 12);
        if (t_srt_ptr_pages) {
            t_srt_ptr_pages->push_back((guest_base + off) >> 12);
            t_srt_ptr_pages->push_back((guest_base + off + 7) >> 12);
        }
        if (t_srt_checks) {
            SrtRecordCheck(guest_base + off, 8);
        }
    }
    return t_srt_inner_load(host_base, guest_base, off);
}
} // namespace

void RunSrtWalker(PFN_SrtWalker walker, const u32* user_data, u32* flat, size_t flat_dw,
                  bool sync, std::vector<SrtGpuLeaf>* leaves) {
    thread_local std::vector<u64> src;
    src.assign(flat_dw, 0);
    SrtWalkCtx ctx{g_srt_load_ptr.load(std::memory_order_relaxed), src.data()};
    if (t_srt_read_pages) {
        t_srt_inner_load = ctx.load_ptr;
        ctx.load_ptr = RecordingSrtLoadPtr;
    }
    const bool was_sync = t_srt_sync;
    t_srt_sync = sync;
#ifdef ARCH_X86_64
    walker(user_data, flat, &ctx);
#else
    RunSrtProgram(walker.program, user_data, flat, &ctx);
#endif
    if (t_srt_read_pages) {
        for (const u64 address : src) {
            if (address) {
                t_srt_read_pages->push_back(address >> 12);
                if (t_srt_checks) {
                    SrtRecordCheck(address, 4);
                }
            }
        }
    }
    if (const auto fixup = g_srt_fixup.load(std::memory_order_relaxed)) {
        fixup(src.data(), flat, flat_dw, leaves);
    }
    t_srt_sync = was_sync;
}

#ifndef ARCH_X86_64
// The bytecode walker reads tables through the loader and never faults in walker code.
bool DecodeSrtWalkerLoad(const void*, SrtWalkerLoad&) {
    return false;
}

} // namespace Shader
#else
bool DecodeSrtWalkerLoad(const void* rip, SrtWalkerLoad& load) {
    // The whole code buffer: walkers loaded from the pipeline cache never set
    // g_srt_codegen_start.
    const u8* code_start = g_srt_codegen.getCode();
    const u8* code_end = code_start + g_srt_codegen.getSize();
    if (rip < code_start || rip >= code_end) {
        return false;
    }
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(
        instruction, operands, const_cast<void*>(rip), 15);
    if (!ZYAN_SUCCESS(status) || instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
        operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
        operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
        return false;
    }
    switch (operands[0].reg.value) {
    case ZYDIS_REGISTER_RDI:
        load = {true, 8, instruction.length};
        return true;
    case ZYDIS_REGISTER_R10D:
        load = {false, 4, instruction.length};
        return true;
    default:
        return false;
    }
}

} // namespace Shader

namespace {

static void DumpSrtProgram(const Shader::Info& info, const u8* code, size_t codesize) {
    using namespace Common::FS;

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}.srtprogram.txt", info.hw_stage, info.pgm_hash);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create, FileType::TextFile};

    u64 address = reinterpret_cast<u64>(code);
    u64 code_end = address + codesize;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = ZYAN_STATUS_SUCCESS;
    while (address < code_end && ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                                     instruction, operands, reinterpret_cast<void*>(address)))) {
        std::string s =
            Common::Decoder::Instance()->disassembleInst(instruction, operands, address);
        s += "\n";
        file.WriteString(s);
        address += instruction.length;
    }
}

static bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    // Only handle if the fault address is within the SRT code range
    const u8* code_start = g_srt_codegen_start;
    const u8* code_end = code_start + g_srt_codegen.getSize();
    const void* code = Common::GetRip(context);
    if (code < code_start || code >= code_end) {
        return false; // Not in SRT code range
    }

    // Patch instruction to zero register
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(instruction, operands,
                                                                       const_cast<void*>(code), 15);

    ASSERT(ZYAN_SUCCESS(status) && instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY);

    size_t len = instruction.length;
    const size_t patch_size = 3;
    u8* code_patch = const_cast<u8*>(reinterpret_cast<const u8*>(code));

    // We can only encounter rdi or r10d as the first operand in a
    // fault memory access for SRT walker.
    switch (operands[0].reg.value) {
    case ZYDIS_REGISTER_RDI:
        // mov rdi, [rdi + (off_dw << 2)] -> xor rdi, rdi
        code_patch[0] = 0x48;
        code_patch[1] = 0x31;
        code_patch[2] = 0xFF;
        break;
    case ZYDIS_REGISTER_R10D:
        // mov r10d, [rdi + (off_dw << 2)] -> xor r10d, r10d
        code_patch[0] = 0x45;
        code_patch[1] = 0x31;
        code_patch[2] = 0xD2;
        break;
    default:
        UNREACHABLE_MSG("Unsupported register for SRT walker patch");
        return false;
    }

    // Fill nops
    memset(code_patch + patch_size, 0x90, len - patch_size);

    LOG_WARNING(Render_Recompiler, "Patched SRT walker at {}, fault address {}", code,
                fault_address);

    return true;
}

#endif // ARCH_X86_64

#ifndef ARCH_X86_64

namespace {
// Portable SRT bytecode used on ARM64.  The x86-64 path below emits equivalent machine code.
// uncharted_android: OpCopyWindow (a ReadConst window) was added at the end, so programs of the
// earlier set keep their meaning; Profile::srt_walker_revision rebuilds cached ones anyway.
enum SrtOp : u32 {
    OpEnd,
    OpPushImm,
    OpPushDyn,
    OpPop,
    OpCopyImm,
    OpCopyDyn,
    OpUserData,
    OpFlat,
    OpImm,
    OpAdd,
    OpSub,
    OpMul,
    OpShl,
    OpShr,
    OpAnd,
    OpOr,
    OpXor,
    OpNot,
    OpUMin,
    OpUMax,
    OpBitFieldUExtract,
    OpCopyWindow, // dst_start, count: the current table's first `count` dwords
};
constexpr u32 OpZeroed = 0x80000000u;
constexpr u32 SrtMagic = 0x42545253u; // "SRTB"
constexpr u32 MaxDepth = 64;
constexpr u32 MaxValues = 32;
} // namespace

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    u32 magic = 0;
    if (size < 8 || size % 4 != 0 ||
        (std::memcpy(&magic, ptr, sizeof(magic)), magic != SrtMagic)) {
        LOG_ERROR(Render_Recompiler, "SRT walker in shader cache is not an SRT program");
        return {};
    }
    auto* program = new u32[size / sizeof(u32)];
    std::memcpy(program, ptr, size);
    return SrtWalker{program};
}

// uncharted_android: the walk follows the uc3 walker context when one is given (RunSrtWalker):
// table pointers go through ctx->load_ptr, which returns where the table is read (the
// unprotected backing view) and its guest address, and every flattened dword records its guest
// source in ctx->src_table for the fixup after the walk, as the x86-64 walker does.
void RunSrtProgram(const u32* program, const u32* user_data, u32* flat, SrtWalkCtx* ctx) {
    u32* const code = const_cast<u32*>(program);
    // A load that faults reads as zero for this walk (the walk starts again with it zeroed): with
    // the backing view a fault is rare, and the next walk may well read the value. After
    // MaxRunFaults faults in one walk a load is zeroed for good, as upstream does.
    constexpr u32 MaxRunFaults = 16;
    sigjmp_buf* const previous = runtime_fault_recover;
    const int previous_gpu = runtime_fault_recover_gpu;
    // Like the x86-64 walker, whose faults go to GPU page tracking first: on a thread that may
    // read GPU-written bytes back (not inside another speculative read), a load from a watched
    // page is resolved there instead of reading as zero.
    runtime_fault_recover_gpu = previous == nullptr && SrtWalkerSyncAllowed();
    sigjmp_buf recover;
    volatile u32 faulted_pc = 0;
    volatile u32 run_zeroed[MaxRunFaults];
    volatile u32 run_zeroed_count = 0;
    if (sigsetjmp(recover, 0)) {
        if (run_zeroed_count < MaxRunFaults) {
            run_zeroed[run_zeroed_count] = faulted_pc;
            run_zeroed_count = run_zeroed_count + 1;
        } else if (faulted_pc != 0) {
            __atomic_fetch_or(&code[faulted_pc], OpZeroed, __ATOMIC_RELAXED);
            LOG_WARNING(Render_Recompiler, "SRT walker load faulted; using zero");
        }
    }
    runtime_fault_recover = &recover;
    const auto zeroed_in_run = [&](u32 at_pc) {
        for (u32 i = 0; i < run_zeroed_count; ++i) {
            if (run_zeroed[i] == at_pc) {
                return true;
            }
        }
        return false;
    };
    const u8* host_stack[MaxDepth];
    u64 guest_stack[MaxDepth];
    u32 values[MaxValues];
    u32 depth = 0, count = 0;
    // Where the current table is read, and its guest address (0 for the user data).
    const u8* host = reinterpret_cast<const u8*>(user_data);
    u64 guest = 0;
    u64* const src = ctx ? ctx->src_table : nullptr;
    const PFN_SrtLoadPtr load_ptr = ctx ? ctx->load_ptr : nullptr;
    const auto read_dword = [&](u32 at_pc, bool zeroed, u32 offset, u32 dst) {
        u32 value = 0;
        if (!zeroed && host) {
            faulted_pc = at_pc;
            std::memcpy(&value, host + offset, sizeof(value));
        }
        flat[dst] = value;
        if (src) {
            src[dst] = guest && !zeroed ? guest + offset : 0;
        }
    };

    for (u32 pc = 1;;) {
        const u32 op_pc = pc++;
        const u32 word = __atomic_load_n(&code[op_pc], __ATOMIC_RELAXED);
        const u32 op = word & ~OpZeroed;
        const bool zeroed = (word & OpZeroed) != 0 || zeroed_in_run(op_pc);
        switch (op) {
        case OpEnd:
            runtime_fault_recover = previous;
            runtime_fault_recover_gpu = previous_gpu;
            return;
        case OpPushImm:
        case OpPushDyn: {
            const u32 offset = (op == OpPushImm ? code[pc++] : values[--count]) << 2;
            SrtPtr next{0, 0};
            if (!zeroed && host) {
                faulted_pc = op_pc;
                if (load_ptr) {
                    next = load_ptr(host, guest, offset);
                } else {
                    u64 value = 0;
                    std::memcpy(&value, host + offset, sizeof(value));
                    value &= 0xFFFFFFFFFFFFull;
                    next = {value, value};
                }
            }
            host_stack[depth] = host;
            guest_stack[depth] = guest;
            ++depth;
            host = reinterpret_cast<const u8*>(next.host);
            guest = next.guest;
            break;
        }
        case OpPop:
            --depth;
            host = host_stack[depth];
            guest = guest_stack[depth];
            break;
        case OpCopyImm:
        case OpCopyDyn: {
            const u32 offset = (op == OpCopyImm ? code[pc++] : values[--count]) << 2;
            read_dword(op_pc, zeroed, offset, code[pc++]);
            break;
        }
        case OpCopyWindow: {
            const u32 start = code[pc++];
            const u32 dwords = code[pc++];
            for (u32 i = 0; i < dwords; ++i) {
                read_dword(op_pc, zeroed, i << 2, start + i);
            }
            break;
        }
        case OpUserData:
        case OpFlat:
            values[count++] = flat[code[pc++]];
            break;
        case OpImm:
            values[count++] = code[pc++];
            break;
        case OpNot:
            values[count - 1] = ~values[count - 1];
            break;
        case OpBitFieldUExtract: {
            const u32 bits = values[--count];
            const u32 offset = values[--count];
            u32& base = values[count - 1];
            base = (base >> (offset & 31)) & ((1u << (bits & 31)) - 1);
            break;
        }
        default: {
            const u32 b = values[--count];
            u32& a = values[count - 1];
            switch (op) {
            case OpAdd: a += b; break;
            case OpSub: a -= b; break;
            case OpMul: a *= b; break;
            case OpShl: a <<= b & 31; break;
            case OpShr: a >>= b & 31; break;
            case OpAnd: a &= b; break;
            case OpOr: a |= b; break;
            case OpXor: a ^= b; break;
            case OpUMin: a = std::min(a, b); break;
            case OpUMax: a = std::max(a, b); break;
            default: UNREACHABLE_MSG("Bad SRT program op {}", op);
            }
            break;
        }
        }
    }
}

} // namespace Shader

#endif

namespace {
using namespace Shader;

struct PassInfo {
    u32 windowed_reads = 0; // uc3: GOW3_WINDOW_TRACE
    u32 addr64_windowed = 0;
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            // Keep order with the following criteria:
            // 1. If immediate, sort by immediate value
            // 2. If not immediate, sort by instruction order
            // 3. Immediates come first, then instructions
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block; // Sort by block address
                }

                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate(); // Immediates come first
            }
        }
    };

    // map offset to inst
    using PtrUserList =
        boost::container::flat_map<IR::Value, Shader::IR::Inst*, PointerListCompare>;

    Optimization::SrtGvnTable gvn_table;
    // keys are GetUserData, ReadConst or ReadConstBuffer instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u16 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
} // namespace

namespace Shader::Optimization {

namespace {

static inline u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        return inst_info.flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static inline void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        inst_info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(inst_info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}

#ifdef ARCH_X86_64
static bool ComputeOffset(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                          const IR::Value& off_dw);

#define ABORT_ON_FAILURE(expr)                                                                     \
    if (!(expr)) {                                                                                 \
        return false;                                                                              \
    }

#define POP_ABORT_ON_FAILURE(expr)                                                                 \
    if (!(expr)) {                                                                                 \
        c.add(rsp, 8);                                                                             \
        return false;                                                                              \
    }

static bool EmitComputeOffsetIAdd32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.add(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.add(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.add(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetISub32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.neg(reg);
        c.add(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.sub(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.neg(reg);
        c.add(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetIMul32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.imul(reg, reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.imul(reg, reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.imul(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetShiftLeftLogical32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                                PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.shl(reg, inst->Arg(0).U32());
        c.mov(ecx, reg);
        c.mov(reg, inst->Arg(0).U32());
        c.shl(reg, cl);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.shl(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.pop(rcx);
        c.shl(reg, cl);
    }
    return true;
}

static bool EmitComputeOffsetShiftRightLogical32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                                 PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, reg);
        c.mov(reg, inst->Arg(0).U32());
        c.shr(reg, cl);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.shr(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.pop(rcx);
        c.shr(reg, cl);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseAnd32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.and_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.and_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.and_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseOr32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                         PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.or_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.or_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.or_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseXor32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.xor_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.xor_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.xor_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseNot32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
    c.not_(reg);
    return true;
}

static bool EmitComputeOffsetUMin32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, inst->Arg(0).U32());
        c.cmp(reg, ecx);
        c.cmova(reg, ecx);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.mov(ecx, inst->Arg(1).U32());
        c.cmp(reg, ecx);
        c.cmova(reg, ecx);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.cmp(reg, dword[rsp]);
        c.cmova(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetUMax32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, inst->Arg(0).U32());
        c.cmp(reg, ecx);
        c.cmovb(reg, ecx);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.mov(ecx, inst->Arg(1).U32());
        c.cmp(reg, ecx);
        c.cmovb(reg, ecx);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.cmp(reg, dword[rsp]);
        c.cmovb(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitFieldUExtract(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                              PassInfo& pass_info, IR::Inst* inst) {
    // We asume that the count is always less than 32, Is this correct?
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        c.mov(reg, inst->Arg(0).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
    }
    bool in_stack = false;
    if (inst->Arg(1).IsImmediate()) {
        c.shr(reg, inst->Arg(1).U32());
    } else {
        c.push(reg.cvt64());
        in_stack = true;
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, reg);
        c.shr(dword[rsp], cl);
    }
    if (inst->Arg(2).IsImmediate()) {
        if (in_stack) {
            c.pop(reg.cvt64());
        }
        c.and_(reg, (1U << inst->Arg(2).U32()) - 1);
    } else {
        if (!in_stack) {
            c.push(reg.cvt64());
        }
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(2)));
        c.mov(ecx, reg);
        c.mov(edx, 1);
        c.shl(edx, cl);
        c.dec(edx);
        c.and_(dword[rsp], edx);
        c.pop(reg.cvt64());
    }
    return true;
}

#endif

static bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
    case IR::Opcode::ReadFirstLane: // uc3 (SuperMedo): see ComputeOffset
        return true;
    default:
        return false;
    }
}

// uc3 (from SuperMedo's uncharted_pc): whether an offset is known when the walker runs (user data,
// constants and arithmetic on them), checked before ComputeOffset (which logs an error for values
// that vary per lane).
static bool IsBindTimeOffset(const IR::Value& value) {
    if (value.IsImmediate()) {
        return true;
    }
    const IR::Inst* inst = value.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConstBuffer:
        return true;
    case IR::Opcode::ReadConst:
        return !(inst->Flags<u32>() & ReadConstWindowFlag);
    default:
        break;
    }
    if (!IsAllowedOffsetInstruction(inst)) {
        return false;
    }
    for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
        if (!IsBindTimeOffset(inst->Arg(arg))) {
            return false;
        }
    }
    return true;
}

#ifdef ARCH_X86_64
static bool ComputeOffset(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                          const IR::Value& off_dw) {
    ASSERT(reg != ecx && reg != edx);
    auto inst = off_dw.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        c.mov(reg, ptr[rsi + (static_cast<u32>(inst->Arg(0).ScalarReg()) << 2)]);
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        if (inst->GetOpcode() == IR::Opcode::ReadConst &&
            (pass_info.DeduplicateInstruction(inst)->Flags<u32>() & ReadConstWindowFlag)) {
            return false; // uc3 (SuperMedo): varies per lane
        }
        if (u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst)); offset != 0) {
            c.mov(reg, ptr[rsi + (offset << 2)]);
            return true;
        }
        return false;
    case IR::Opcode::IAdd32:
        ABORT_ON_FAILURE(EmitComputeOffsetIAdd32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ISub32:
        ABORT_ON_FAILURE(EmitComputeOffsetISub32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::IMul32:
        ABORT_ON_FAILURE(EmitComputeOffsetIMul32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ShiftLeftLogical32:
        ABORT_ON_FAILURE(EmitComputeOffsetShiftLeftLogical32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ShiftRightLogical32:
        ABORT_ON_FAILURE(EmitComputeOffsetShiftRightLogical32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseAnd32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseAnd32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseOr32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseOr32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseXor32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseXor32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseNot32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseNot32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::UMin32:
        ABORT_ON_FAILURE(EmitComputeOffsetUMin32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::UMax32:
        ABORT_ON_FAILURE(EmitComputeOffsetUMax32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitFieldUExtract:
        ABORT_ON_FAILURE(EmitComputeOffsetBitFieldUExtract(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ReadFirstLane:
        // uc3 (SuperMedo): a uniform offset moved through a vector register (v_mov +
        // v_readfirstlane) has the value of its operand; per-lane operands fail below as before.
        if (WindowAddr64Only()) {
            LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                      magic_enum::enum_name(inst->GetOpcode()));
            return false;
        }
        return ComputeOffset(c, reg, pass_info, inst->Arg(0));
    default:
        LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                  magic_enum::enum_name(inst->GetOpcode()));
        return false;
    }
}

#endif

#ifdef ARCH_X86_64
// uc3 (Fable R.8.1): registers of the walker: rdi = where the current table is read (host),
// r9 = its guest address (0 for user data), rsi = flat buffer, r8 = SrtWalkCtx. Pointers are
// loaded by ctx->load_ptr, which reads them (back) and returns the unprotected backing view of
// the table, so the walker does not fault on pages shared with GPU-written data. Each leaf
// stores its guest source address in ctx->src_table for the fixup after the walk.
static inline bool PushPtr(Xbyak::CodeGenerator& c, PassInfo& pass_info, const IR::Value& off_dw) {
    if (!off_dw.IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, r10d, pass_info, off_dw));
        c.shl(r10d, 2);
    }
    c.push(rdi);
    c.push(r9);
    c.push(rsi);
    c.push(r8);
    c.mov(rsi, r9);
    if (off_dw.IsImmediate()) {
        c.mov(edx, off_dw.U32() << 2);
    } else {
        c.mov(edx, r10d);
    }
    c.mov(r11, rsp);
    c.and_(rsp, -16);
    c.push(r11);
    c.push(r11);
    c.call(ptr[r8]);
    c.pop(r11);
    c.pop(rsp);
    c.pop(r8);
    c.pop(rsi);
    c.mov(rdi, rax);
    c.mov(r9, rdx);
    return true;
}

static inline void PopPtr(Xbyak::CodeGenerator& c) {
    c.pop(r9);
    c.pop(rdi);
}

static void VisitPointer(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         Xbyak::CodeGenerator& c) {
    if (subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0 ||
        subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0) {
        return;
    }

    if (!PushPtr(c, pass_info, off_dw)) {
        LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
        return;
    }
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    // First copy all the src data from this tree level
    // That way, all data that was contiguous in the guest SRT is also contiguous in the
    // flattened buffer.
    // TODO src and dst are contiguous. Optimize with wider loads/stores
    // TODO if this subtree is dynamically indexed, don't compact it (keep it sparse)
    boost::container::small_vector<IR::Inst*, 8> windowed;
    for (auto [src_off_dw, use] : *use_list) {
        if (src_off_dw.IsImmediate()) {
            c.lea(r11, ptr[r9 + (src_off_dw.U32() << 2)]);
            c.mov(r10d, ptr[rdi + (src_off_dw.U32() << 2)]);
        } else {
            // uc3: GOW3_WINDOW_ADDR64_ONLY=1: only the addr64 reads get a window; the others are
            // left as before the SuperMedo port (no offset).
            const bool is_addr64 = use->GetOpcode() == IR::Opcode::ReadConst &&
                                   (use->Flags<u32>() & ReadConstAddr64Mark);
            const bool may_window = !WindowAddr64Only() || is_addr64;
            if (use->GetOpcode() == IR::Opcode::ReadConst && !IsBindTimeOffset(src_off_dw)) {
                if (may_window) {
                    windowed.push_back(use); // uc3 (SuperMedo): an offset that varies per lane
                    pass_info.addr64_windowed += is_addr64;
                } else {
                    LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                }
                continue;
            }
            if (!ComputeOffset(c, r10d, pass_info, src_off_dw)) {
                if (use->GetOpcode() == IR::Opcode::ReadConst && may_window) {
                    windowed.push_back(use);
                    pass_info.addr64_windowed += is_addr64;
                    continue;
                }
                LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                continue;
            }
            c.shl(r10d, 2);
            c.mov(r10d, r10d);
            c.lea(r11, ptr[r9 + r10]);
            c.mov(r10d, dword[rdi + r10]);
        }
        c.mov(ptr[rsi + (pass_info.dst_off_dw << 2)], r10d);
        c.mov(rax, qword[r8 + 8]);
        c.mov(qword[rax + (pass_info.dst_off_dw << 3)], r11);

        SetFlatbufOffset(use, pass_info.dst_off_dw);
        pass_info.dst_off_dw++;
    }
    // uc3 (SuperMedo): reads at offsets that vary per lane share a copy of the block's first
    // ReadConstWindowDwords dwords (ReadConstWindowFlag). Each copied dword also records its guest
    // source in the R.8 table, so GPU-written dwords are re-read like any other.
    if (!windowed.empty()) {
        const u32 start = pass_info.dst_off_dw;
        pass_info.windowed_reads += static_cast<u32>(windowed.size());
        for (u32 i = 0; i < ReadConstWindowDwords(); ++i) {
            c.lea(r11, ptr[r9 + (i << 2)]);
            c.mov(r10d, ptr[rdi + (i << 2)]);
            c.mov(ptr[rsi + ((start + i) << 2)], r10d);
            c.mov(rax, qword[r8 + 8]);
            c.mov(qword[rax + ((start + i) << 3)], r11);
        }
        pass_info.dst_off_dw += ReadConstWindowDwords();
        for (IR::Inst* use : windowed) {
            use->SetFlags<u32>(start | ReadConstWindowFlag);
        }
    }

    // Then visit any children used as pointers
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, c);
        }
    }

    PopPtr(c);
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    Xbyak::CodeGenerator& c = g_srt_codegen;

    if (pass_info.srt_roots.empty()) {
        return;
    }
    std::scoped_lock lock{g_srt_codegen_mutex};

    // Register the signal handler for SRT walker, if not already registered
    if (g_srt_codegen_start == nullptr) {
        g_srt_codegen_start = c.getCurr();
        auto* signals = Core::Signals::Instance();
        // Call after the memory invalidation handler
        constexpr u32 priority = 1;
        signals->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    }

    info.srt_info.walker_func = c.getCurr<PFN_SrtWalker>();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);

    c.mov(r8, rdx);
    c.xor_(r9d, r9d);
    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, c);
    }

    c.ret();
    c.ready();

    info.srt_info.walker_func_size =
        c.getCurr() - reinterpret_cast<const u8*>(info.srt_info.walker_func);

    if (EmulatorSettings.IsDumpShaders()) {
        DumpSrtProgram(info, reinterpret_cast<const u8*>(info.srt_info.walker_func),
                       info.srt_info.walker_func_size);
    }

    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

#else

using SrtProgram = std::vector<u32>;

#define ABORT_ON_FAILURE(expr)                                                                     \
    if (!(expr)) {                                                                                 \
        return false;                                                                              \
    }

static bool ComputeOffset(SrtProgram& p, PassInfo& pass_info, const IR::Value& off_dw);

static bool EmitOperand(SrtProgram& p, PassInfo& pass_info, const IR::Value& value) {
    if (value.IsImmediate()) {
        p.insert(p.end(), {OpImm, value.U32()});
        return true;
    }
    return ComputeOffset(p, pass_info, value);
}

static bool EmitComputeOffsetOp(SrtProgram& p, PassInfo& pass_info, IR::Inst* inst,
                                SrtOp op, size_t args) {
    ASSERT(!inst->AreAllArgsImmediates());
    for (size_t i = 0; i < args; ++i) {
        ABORT_ON_FAILURE(EmitOperand(p, pass_info, inst->Arg(i)));
    }
    p.push_back(op);
    return true;
}

static bool ComputeOffset(SrtProgram& p, PassInfo& pass_info, const IR::Value& off_dw) {
    auto inst = off_dw.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        p.insert(p.end(), {OpUserData, static_cast<u32>(inst->Arg(0).ScalarReg())});
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        if (inst->GetOpcode() == IR::Opcode::ReadConst &&
            (pass_info.DeduplicateInstruction(inst)->Flags<u32>() & ReadConstWindowFlag)) {
            return false; // uc3 (SuperMedo): varies per lane
        }
        if (u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst)); offset != 0) {
            p.insert(p.end(), {OpFlat, offset});
            return true;
        }
        return false;
    case IR::Opcode::ReadFirstLane:
        // uc3 (SuperMedo): a uniform offset moved through a vector register has the value of its
        // operand, as in the x86-64 walker.
        if (WindowAddr64Only()) {
            LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                      magic_enum::enum_name(inst->GetOpcode()));
            return false;
        }
        return ComputeOffset(p, pass_info, inst->Arg(0));
    case IR::Opcode::IAdd32: return EmitComputeOffsetOp(p, pass_info, inst, OpAdd, 2);
    case IR::Opcode::ISub32: return EmitComputeOffsetOp(p, pass_info, inst, OpSub, 2);
    case IR::Opcode::IMul32: return EmitComputeOffsetOp(p, pass_info, inst, OpMul, 2);
    case IR::Opcode::ShiftLeftLogical32: return EmitComputeOffsetOp(p, pass_info, inst, OpShl, 2);
    case IR::Opcode::ShiftRightLogical32: return EmitComputeOffsetOp(p, pass_info, inst, OpShr, 2);
    case IR::Opcode::BitwiseAnd32: return EmitComputeOffsetOp(p, pass_info, inst, OpAnd, 2);
    case IR::Opcode::BitwiseOr32: return EmitComputeOffsetOp(p, pass_info, inst, OpOr, 2);
    case IR::Opcode::BitwiseXor32: return EmitComputeOffsetOp(p, pass_info, inst, OpXor, 2);
    case IR::Opcode::BitwiseNot32: return EmitComputeOffsetOp(p, pass_info, inst, OpNot, 1);
    case IR::Opcode::UMin32: return EmitComputeOffsetOp(p, pass_info, inst, OpUMin, 2);
    case IR::Opcode::UMax32: return EmitComputeOffsetOp(p, pass_info, inst, OpUMax, 2);
    case IR::Opcode::BitFieldUExtract:
        return EmitComputeOffsetOp(p, pass_info, inst, OpBitFieldUExtract, 3);
    default:
        LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                  magic_enum::enum_name(inst->GetOpcode()));
        return false;
    }
}

static void VisitPointer(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         SrtProgram& p) {
    if (subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0 ||
        subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0) {
        return;
    }
    if (off_dw.IsImmediate()) {
        p.insert(p.end(), {OpPushImm, off_dw.U32()});
    } else {
        SrtProgram offset;
        if (!ComputeOffset(offset, pass_info, off_dw)) {
            LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
            return;
        }
        p.insert(p.end(), offset.begin(), offset.end());
        p.push_back(OpPushDyn);
    }
    auto* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);
    // uncharted_android: the uc3 ReadConst windows, as the x86-64 VisitPointer emits them.
    boost::container::small_vector<IR::Inst*, 8> windowed;
    for (auto [src_off_dw, use] : *use_list) {
        if (src_off_dw.IsImmediate()) {
            p.insert(p.end(), {OpCopyImm, src_off_dw.U32(), pass_info.dst_off_dw});
        } else {
            const bool is_addr64 = use->GetOpcode() == IR::Opcode::ReadConst &&
                                   (use->Flags<u32>() & ReadConstAddr64Mark);
            const bool may_window = !WindowAddr64Only() || is_addr64;
            if (use->GetOpcode() == IR::Opcode::ReadConst && !IsBindTimeOffset(src_off_dw)) {
                if (may_window) {
                    windowed.push_back(use); // an offset that varies per lane
                    pass_info.addr64_windowed += is_addr64;
                } else {
                    LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                }
                continue;
            }
            SrtProgram offset;
            if (!ComputeOffset(offset, pass_info, src_off_dw)) {
                if (use->GetOpcode() == IR::Opcode::ReadConst && may_window) {
                    windowed.push_back(use);
                    pass_info.addr64_windowed += is_addr64;
                    continue;
                }
                LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                continue;
            }
            p.insert(p.end(), offset.begin(), offset.end());
            p.insert(p.end(), {OpCopyDyn, pass_info.dst_off_dw});
        }
        SetFlatbufOffset(use, pass_info.dst_off_dw++);
    }
    if (!windowed.empty()) {
        const u32 start = pass_info.dst_off_dw;
        pass_info.windowed_reads += static_cast<u32>(windowed.size());
        p.insert(p.end(), {OpCopyWindow, start, ReadConstWindowDwords()});
        pass_info.dst_off_dw += ReadConstWindowDwords();
        for (IR::Inst* use : windowed) {
            use->SetFlags<u32>(start | ReadConstWindowFlag);
        }
    }
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, p);
        }
    }
    p.push_back(OpPop);
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    if (pass_info.srt_roots.empty()) {
        return;
    }
    SrtProgram program{SrtMagic};
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);
    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, program);
    }
    program.push_back(OpEnd);
    const size_t size = program.size() * sizeof(u32);
    info.srt_info.walker_func = RegisterWalkerCode(reinterpret_cast<const u8*>(program.data()), size);
    info.srt_info.walker_func_size = size;
    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

#endif

static bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    // This handles the following pattern by combining the addition with the offset
    // s_add_u32    s12, s2, <offset>
    // s_addc_u32   s13, s3, 0
    // s_load_dword s16, s[12:13], 0x0

    if (inst.GetOpcode() != IR::Opcode::ReadConst) {
        return;
    }
    IR::Inst* addr = inst.Arg(0).Inst();
    IR::Inst* hi = addr->Arg(1).TryInst();
    IR::Inst* lo = addr->Arg(0).TryInst();
    if (!hi || !lo) {
        return;
    }

    if (lo->GetOpcode() != IR::Opcode::IAdd32 || hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }

    IR::Value add_offset;
    IR::Value base;
    if (base = lo->Arg(0); IsReadConstSource(base)) {
        add_offset = lo->Arg(1);
    } else if (base = lo->Arg(1); IsReadConstSource(base)) {
        add_offset = lo->Arg(0);
    } else {
        return;
    }

    IR::Inst* sel = hi->Arg(1).TryInst();
    if (!sel || sel->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }

    IR::Value sel_true = sel->Arg(1);
    IR::Value sel_false = sel->Arg(2);
    if (!sel_true.IsImmediate() || !sel_false.IsImmediate() || sel_true.U32() != 1 ||
        sel_false.U32() != 0) {
        return;
    }

    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (auto [user, operand] : addr->Uses()) {
        ASSERT(user->GetOpcode() == IR::Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        IR::U32 offset = IR::U32{user->Arg(1)};
        IR::U32 dw_add_offset =
            add_offset.IsImmediate()
                ? ir.Imm32(add_offset.U32() >> 2u)
                : IR::U32{ir.ShiftRightLogical(IR::U32{add_offset}, ir.Imm32(2u))};
        if (offset.IsImmediate() && dw_add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + dw_add_offset.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, dw_add_offset));
        }
    }
}

} // Anonymous namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }

            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                // Only flatten ReadConstBuffer if it was marked as a sharp source in the
                // resource discovery pass
                auto inst_info = inst.Flags<IR::BufferInstInfo>();
                if (!inst_info.sharp_source) {
                    continue;
                }
            }

            SimplifyReadConstAddressAdd(inst);

            all_readconsts.push_back(&inst);

            auto offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }

            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* inst{queue.front()};
                queue.pop();

                if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = inst->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(inst);
                        auto offset = inst->Arg(1);
                        if (!offset.IsImmediate()) {
                            auto arg_inst = offset.Inst();
                            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                                visited.push_back(arg_inst);
                                queue.push(arg_inst);
                            }
                        }
                    }
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                    inst->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(inst)) {
                    continue;
                }
                for (size_t arg = inst->NumArgs(); arg--;) {
                    const IR::Value arg_value = inst->Arg(arg);
                    if (arg_value.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* arg_inst = arg_value.Inst();
                    if (std::ranges::find(visited, arg_inst) == visited.end()) {
                        visited.push_back(arg_inst);
                        queue.push(arg_inst);
                    }
                }
            }
        }
    }

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            // This is a duplicate of a readconst we've already visited
            continue;
        }

        IR::Inst* base = inst->Arg(0).Inst();
        if (auto* inst = base->Arg(0).TryInst();
            inst && inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        // uc3 (SuperMedo): addr64 buffer loads (ReadConst) may compute their pointer in other
        // ways; those stay unresolved (as with a ReadFirstLane base) instead of stopping the game.
        if (!IsReadConstSource(base->Arg(0)) || !IsReadConstSource(base->Arg(1))) {
            LOG_ERROR(Render_Recompiler, "ReadConst base not from constant memory");
            continue;
        }

        IR::Inst* ptr_lo = base->Arg(0).Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

        user_list[inst->Arg(1)] = inst;

        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        if (readconst->GetOpcode() == IR::Opcode::ReadConst) {
            readconst->SetFlags(original->Flags<u32>()); // uc3: with ReadConstWindowFlag
            continue;
        }
        SetFlatbufOffset(readconst, GetFlatbufOffset(original));
    }

    // uc3: GOW3_WINDOW_TRACE=1: which shaders read a ReadConst window, and how many of those
    // reads come from an addr64 buffer load (the hair) rather than another per-lane offset.
    static const bool window_trace = std::getenv("GOW3_WINDOW_TRACE") != nullptr;
    if (window_trace && pass_info.windowed_reads != 0) {
        std::printf("uc3 window: shader %016llx: %u per-lane ReadConst (%u from addr64)\n",
                    static_cast<unsigned long long>(program.info.pgm_hash),
                    pass_info.windowed_reads, pass_info.addr64_windowed);
    }

    program.info.RefreshFlatBuf();
}

} // namespace Shader::Optimization
