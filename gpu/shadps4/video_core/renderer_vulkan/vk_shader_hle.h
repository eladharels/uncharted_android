// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// uc3: shaders whose HLE writes guest memory: they run on the command processor thread, with
/// the draw pipe drained (GOW3_SCALAR_COPY_HLE).
bool IsGuestMemoryShaderHLE(unsigned long long pgm_hash);

/// uc3 (GOW3_HLE_INORDER=1): a scalar copy from a compute queue whose drain was skipped:
/// the destination write runs in order on the recording thread. False: drain and dispatch.
bool HleInOrderEnabled();
void NoteInOrderReject(unsigned reason);
/// uncharted_android: whether ExecuteShaderHLE would write guest memory for this scalar copy
/// (a guest copy, alone or beside the GPU copy); false when it would only let the dispatch run
/// on the GPU, which needs no drain of the draw pipe. Stage A, before the dispatch is handed over.
bool ScalarCopyHleWritesGuest(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                              Rasterizer& rasterizer);
/// uncharted_android: the part of the HLE a GPU-only scalar copy still needs on the recording
/// thread (the scene depth image into its source buffer before the copy).
void ScalarCopyHleGpuOnlyPrepare(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer);
bool ExecuteScalarCopyInOrder(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                              Rasterizer& rasterizer);

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
