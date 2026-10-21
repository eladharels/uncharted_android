// SPDX-License-Identifier: GPL-2.0-or-later
// uncharted_android: a sampling CPU profiler of this process, for phones, where no system
// profiler can be attached. GOW3_PROFILE=1 (or "start,interval,duration,hz", seconds) samples
// the renderer's threads at `hz` and every other thread at hz/10 for `duration` seconds, from
// `start` seconds after boot and every `interval` seconds after that, and writes each window to
// $GOW3_LOG_DIR/profile.txt: the thread list, the deduplicated stacks (frame-pointer chains)
// with their counts, the memory map, and the frame rate of the window. The log export includes
// the file; tools/android/profile/symbolize.py turns it into per-thread reports with the
// unstripped libraries CI keeps as the "symbols" artifact.
#pragma once

namespace Gow3Profiler {
/// Starts the sampler thread when GOW3_PROFILE is set (no-op otherwise, and on Windows).
void Start();
} // namespace Gow3Profiler
