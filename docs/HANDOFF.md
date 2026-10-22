# Session handoff: Uncharted Android port (state as of 2026-10-22)

Everything a new Claude Code session needs to continue the work. Read this first, then
`docs/ANDROID_PORT.md` (the port's own documentation, kept current) and `CHANGELOG.md`.

## 1. The project

- **What:** the unofficial PC port of *Uncharted: The Nathan Drake Collection* (PS4,
  zorde23/ucndc_pc, itself a fork of the God of War III PC port) running natively on Android,
  built on zFitness/gow3_android. The game's x86-64 code runs under FEXCore's JIT; the runtime
  (memory, threads, fibers, libraries), the shadPS4-based Vulkan renderer, audio and input are
  native arm64. A Kotlin launcher prepares the player's own dump and starts the game.
- **Owner:** Elad (eladharels on GitHub). Private repo: https://github.com/eladharels/uncharted_android
- **Test device:** Samsung Galaxy S24 Ultra (SM-S928B, Snapdragon 8 Gen 3 / SM8650, Adreno 750,
  8 cores: 2 efficiency at 2.3 GHz max, 5 performance at 3.0-3.1, 1 prime at 3.4), Android 16 /
  One UI, 10.8 GB RAM. Dump CUSA02344 v01.00. GPU driver: Mesa Turnip
  `v27.0.0-20261021-r4-8G2-OneUI` (installed in the app from a zip).
- **App:** package `com.uncharted.android` (debug builds: `com.uncharted.android.debug`),
  versionName `2.1.7-bionic-v2`.

## 2. Hard rules

- **Two branches.** `android-port` is the working branch (it carries the owner's personal
  launcher icon with Naughty Dog / Sony art in its history: commits f7da596, c2f1fd0).
  `main` is the publishable branch with an original compass icon and **must never contain the
  Sony / Naughty Dog art**. Work on `android-port`, then `git cherry-pick` each code commit onto
  `main`. Never merge `android-port` into `main`, and never cherry-pick a range without checking
  it for the icon commits (this went wrong once and had to be force-reset).
- The repo stays **private** while `android-port` exists.
- **Never commit the keystore** (`uncharted.keystore` or any signing material). Signing secrets
  live only in GitHub Actions secrets.
- Commits: author/committer `Claude <noreply@anthropic.com>`
  (`git -c user.email=noreply@anthropic.com -c user.name=Claude commit ...`), messages ending
  with the attribution lines the session's instructions give.
- The owner prefers Claude sessions run locally on his own computer, without exposing local
  tooling to the internet.
- Keep `docs/ANDROID_PORT.md` ("Next steps" section) and `CHANGELOG.md` updated with every
  finding, including settings that were measured and rejected, so nothing gets retested blind.

## 3. Repository map (what matters for this work)

| Path | What |
|---|---|
| `src/` | the C runtime and the boot program (`src/probe.c`: image patches, fault handler; `runtime_*.c`) |
| `gpu/shadps4/` | the renderer (shadPS4 video core, heavily modified) |
| `gpu/shadps4/video_core/amdgpu/liverpool.cpp` | stage A: the GPU command processor (PM4) |
| `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` | draws, dispatches, bindings, constant ring copies |
| `gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h` | stage B: the draw recording thread, `Drain` |
| `gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.*` | Vulkan recording thread, submissions, host copies |
| `gpu/shadps4/video_core/renderer_vulkan/vk_shader_hle.cpp` | scalar-copy HLE (in-order copies, GPU-only hand-over) |
| `gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.*` | GPU timestamp profiler (`GOW3_GPU_PROFILE=1`) |
| `gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp` | precise readbacks, async readbacks |
| `gpu/shadps4/core/libraries/videoout/driver.cpp` | flips, the 5-second `Frame stats` / `CPU clocks` lines |
| `gpu/shadps4/common/spin_wait.h`, `futex_wait.h` | the arm64 wait primitives |
| `gpu/shim/gow3_threads.h` | core counts, big-core pinning, clock reading |
| `gpu/shim/gow3_profiler.cpp` | on-device sampling CPU profiler (`GOW3_PROFILE`) |
| `gpu/shim/gow3gpu.cpp` | GPU library init, memory manager glue (`ResolveHostCopy`) |
| `tools/android/device/run-bionic.sh` | the start script on the phone (settings → environment) |
| `android-launcher/` | the Kotlin launcher; per-game defaults in `.../data/UnchartedGames.kt` |
| `tools/android/profile/symbolize.py` | turns `profile.txt` into per-thread reports |
| `.github/workflows/android.yml` | CI: builds and signs the APK, uploads APK + symbols |
| `.github/workflows/symbolize.yml` | CI: symbolizes profiles pushed to `profile/**` branches |
| `docs/ANDROID_PORT.md` | the port's documentation: status, next steps, debugging |

## 4. Build, install, test loop

- **CI build:** push to `android-port` (or `main`) → workflow `android`, job
  "Build signed Android release", ~25 min. A newer push to the same branch cancels the running
  build (`concurrency: cancel-in-progress`). A commit message containing `[skip ci]` skips it.
  Artifacts: `uncharted-android-arm64-<sha>` (signed APK zip) and
  `uncharted-android-symbols-<sha>` (unstripped `libgow3gpu.so`, `libgow3cpu.so`, `gow3-probe`;
  14 days). With `gh` on this PC: `gh run download <run-id> -n uncharted-android-arm64-<sha>`
  then `adb install -r <apk>` (the cloud session could not download artifacts; locally it works).
- Watching CI:
  `gh api repos/eladharels/uncharted_android/commits/<sha>/check-runs --jq '.check_runs[] | select(.name=="Build signed Android release") | "\(.status) \(.conclusion)"'`
- **Local build** is not set up yet (CI is the recipe: NDK 29.0.14206865, JDK 17, Python,
  `tools/android/build_bionic_deps.sh`, `build_bionic.sh`, then Gradle). On Windows that means
  WSL2; worth it only if the 25-minute CI loop becomes the bottleneck.
- **On the phone:** game settings are in `/data/user/0/com.uncharted.android/files/gow3data/gow3.ini`,
  logs in `.../files/gow3data/logs/` (`android-game.log`, `android-game.prev.log`, `profile.txt`).
  Those are app-private: a release build cannot be read with `adb shell run-as`. Options: the
  in-app **About → Export logs** (SAF picker, a zip), or a debuggable build (`.debug` package)
  for `run-as`, or making the release `profileable` (simpleperf works, run-as still does not).
  Setting up a direct way to pull logs over adb is a good first task.
- **`uncharted-env.txt`** in the game folder (beside `eboot.bin`, chosen in the launcher):
  `NAME=value` per line, overrides for experiments without a new APK. Accepts `GOW3_*`, `TU_*`,
  `FD_*`, `IR3_*`, `MESA_*`; empty value unsets; echoed at the top of `android-game.log`.
  Normal play needs **no** file (all good settings are defaults).
- The app keeps only the last two game logs; ask for an export right after the run.

## 5. Reading a log

Every 5 seconds (`GOW3_FRAME_STATS` is on):
- `Frame stats: X FPS ... GPU thread N us/draw, M draws/frame, idle ...; blocked: recorder ..., host copies ...`
  — `us/draw` is the command thread's CPU time per draw: the best single per-build metric.
- `Frame pacing: median ...` — frame times are quantized to vblank multiples
  (33.3 / 50 / 66.7 ms), so FPS moves in steps of 30 / 20 / 15.
- `CPU clocks (GHz): cur...; capped to <scaling_max_freq> of <cpuinfo_max_freq>; GPU ...; process CPU X cores (user, kernel)`
- `Draw pipe: ... drains/s ..., stage A waited X%, stage B busy Y%`, `drains by reason/s`,
  `stage A waits over 0.5%: <function>:<line> share ...`
- `uc3 readbacks: N async, X ms waited by faulting threads ...`
- `Stall: ...` blocks for slow frames (draws, dispatches, submissions, fault waits, host copies).
- Header lines to check: `uncharted-env.txt: ...`, `Big cores: renderer threads kept on cores 2,3,4,5,6,7`,
  `GPU: draw preparation workers: 4 (+1 scanner, 8 hardware threads)` (7 there meant the draw
  pipe was off before d8c1836), `GPU driver: ...`, `Resolution: the game renders and outputs 960x540`.

## 6. Current state (build d8c1836, both branches)

- Uncharted 1 is playable at 540p (default render resolution): **30 FPS while the phone is
  cool**, 15-23 FPS once it throttles. Uncharted 2 runs; its train prologue is slow.
  Uncharted 3 starts, not played far.
- Defaults for Uncharted (`UnchartedGames.kt` COMMON): `GOW3_JOB_SPIN=100`, `GOW3_JOB_SLEEP=1`,
  `GOW3_HLE_INORDER_GFX=1`, `GOW3_HLE_PRECISE_WAIT=1`, `GOW3_BIG_CORES=1`, `GOW3_DRAW_PIPE=1`,
  plus the PC port's (`GOW3_EAGER_SUBMIT=128`, `GOW3_ASYNC_SUBMIT=1`, `GOW3_DEFERRED_SUBMIT=1`,
  `GOW3_RING_COPY_THREAD=1`, readbacks mode 2 precise, `GOW3_STALE_READBACK=0`, ...).
- Presentation for Uncharted: native 1920x1080 output scaled by the presenter, no God of War III
  UI composition (`run-bionic.sh`); the render size comes from `GOW3_UC3_RES` patches
  (540p default; 720p draws the controller-hint panel black).

## 7. What was learned (chronological, condensed)

1. **Black gameplay** had two causes: the arm64 SRT walker (fixed with a uc3 context:
   `RunSrtProgram`, `OpCopyWindow`) and God of War III's scaled session (fixed by native
   1080p output for Uncharted).
2. **Resolution:** 1080p render 12-15 FPS, 540p 20-30. GPU timer at 540p: busy 9-21 ms per
   frame (27-48%); the GPU is not the limit at 540p.
3. **Wait loops on arm64:** the PC port's spin/yield backoff burned 25-50 ms/frame of kernel
   time; replaced with spin→yield→sleep (`SpinWait`) and futex waits.
4. **Drains:** stage A draining stage B at every scalar-copy HLE dispatch was a third of its
   time; GPU-only copies now hand over without a drain, small copies go in order on stage B
   with a precise host-read wait. Typical frame 50 → 33 ms.
5. **Profiler:** `GOW3_PROFILE=1` + CI symbolization; return addresses carry pointer
   authentication bits (stripped with `xpaclri`); `libc.so+0x9be20/24` is the syscall stub (the
   caller tells futex / sched_yield / nanosleep apart).
6. **Big cores** (`GOW3_BIG_CORES=1`): the scheduler ran the bursty renderer threads on the
   efficiency cores; pinning took the command thread from 7-12 to 5-7 µs per draw on a cool
   phone. Now default.
7. **Thermal cap — the main limit now.** The phone sets `scaling_max_freq` to 2.4-2.8 GHz from
   the start and to **1.5-1.6 GHz** on the big cores after 40 s to a few minutes of gameplay,
   and keeps it there. Per draw: 5.7-7 µs at 2.4 GHz, 8-12 µs at 1.5. The process uses
   **4.2-5.1 cores** continuously, 1.2-2.1 of them kernel time. Replacing libc++'s
   `atomic::wait` (yields for 60 µs before sleeping) with direct futex waits did **not** lower
   it, so the kernel time is elsewhere.
8. **Draw pipe bug (fixed in d8c1836):** a core taken offline by thermal management at start
   made `Available()` read 7, and the draw pipe (needs 8) stayed off for the session: 10.4 vs
   5.8 µs per draw. Now `Available()` uses the configured cores and `GOW3_DRAW_PIPE=1` is a
   default.
9. **Stage A's biggest wait (profile boat-5):** precise readbacks of GPU-written buffers bound
   as constants (`vk_rasterizer.cpp` ~line 689 → `MemoryManager::ResolveHostCopy` →
   `Rasterizer::ReadMemory`, draining the pipe and waiting for the GPU), at compute dispatches
   and draws: a GPU→CPU→GPU round trip for data that need not leave the GPU.

**Measured and rejected at 540p** (do not retest without a code change): `GOW3_STALE_READBACK=1`,
`GOW3_READBACKS=1` (worse), `GOW3_PREP_TRUST_A1=1`, `GOW3_EAGER_SUBMIT=0` (worse),
`GOW3_TEXTURE_HELPER=1` (worse), `GOW3_DRAW_PIPE=0` (worse), `GOW3_HLE_INORDER_GFX=1` without
the precise wait, timeline semaphore throttling, the Turnip driver variant (the `8G2-OneUI`
build is Standard + one Adreno 740 flag; the stock Qualcomm driver is unsupported).
Uncharted 2: `GOW3_HLE_INORDER` and `GOW3_ACB_DISPATCH_PIPE` must stay on (off: 5 FPS).

## 8. Next steps, in order

1. **Local tooling first** (this is why the session moved to the PC):
   - A way to pull logs and `profile.txt` over adb (debuggable or profileable variant, or a
     launcher option that also writes the export to shared storage).
   - Live checks while the owner plays: `adb logcat`, `adb shell dumpsys thermalservice`,
     `adb shell cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq`,
     `/sys/class/thermal/thermal_zone*/temp`, screenshots with `adb exec-out screencap -p`.
   - `simpleperf` (NDK) with the app `profileable` for kernel-side stacks: attribute the
     1.2-2.1 cores of kernel time (suspects: page faults and `mprotect` from precise readbacks,
     ~2,000 protect calls/s; Turnip ioctls; futex traffic).
   - `perfetto` for a timeline of threads, scheduler and CPU/GPU frequencies together.
2. **Lower the CPU load (heat)** so the cap comes later or not at all: whatever simpleperf shows;
   candidate knobs already exposed: `GOW3_PREP_WORKERS` (4 SCHED_IDLE workers),
   `GOW3_PIPE_SPIN_US` (50), `GOW3_YIELD_US` (20), `GOW3_SPIN_US` (20), `GOW3_WAIT_SLEEP_US` (50).
   Also check Samsung's Game Booster / game performance profile (a 2.5 GHz cap before warm-up
   looks like a profile, not heat). An Android performance-hint session (ADPF) for the
   renderer threads is worth trying.
3. **GPU-side constant copies:** for a GPU-written window bound as a constant, copy it on the GPU
   into the constant ring instead of reading it back (finding 9). Also what Uncharted 2's train
   prologue (800-1,000 compute dispatches per frame) waits on.
4. Per-draw cost on stages A/B/VkRecorder (busy samples of the profile).
5. Known bugs: Uncharted 2 GPU hang once in the train prologue (device lost, ps `3371ae8d`
   after cs `1c3672c5` 480x1x1); 720p controller-hint panel drawn black (RenderDoc on a
   debuggable build); a character texture glitch reported once (not yet described).
6. Frame generation (Lossless Scaling's LSFG through the bundled engine) exists in the launcher:
   needs the player's `Lossless.dll` and VSync on; untested with Uncharted.

## 9. Profiling recipes

- **CPU profile:** `GOW3_PROFILE=1` in `uncharted-env.txt` (or `start,interval,duration,hz`,
  default `30,30,6,1000`). Writes `logs/profile.txt`: `W` windows, `M` maps, `T` threads,
  `C tid user_ms sys_ms` per-thread CPU time (new in d8c1836), `S` stacks.
  Symbolize with the build's symbols artifact:
  `python tools/android/profile/symbolize.py profile.txt --symbols <dir> --top 60 --stacks 12`
  (needs `llvm-symbolizer`; the NDK has one). Or push `profiles/<name>/profile.txt` +
  `profiles/<name>/run.txt` (the build's workflow run id) to the `profile/boat-1` branch and
  CI commits `report.txt` (profiles boat-1..5 are there).
- **GPU profile:** `GOW3_GPU_PROFILE=1`: every 5 s, GPU busy / idle ms per frame and the time by
  render pass and dispatch.
- **Driver options:** `TU_DEBUG=sysmem` / `gmem` etc. through the env file.

## 10. Recent commits (android-port; main has cherry-picks of each)

```
d8c1836 Draw pipe always on for Uncharted; CPU time per thread in the profile
3637b9d Stage-to-stage waits in the futex directly, not through libc++'s polling
f73f79d Clock line: the caps, the GPU clock and the process's CPU use
91bc436 CPU clocks in the 5-second statistics
0bcb25e Big cores by default for Uncharted; GOW3_BIG_CORES=2 for the whole process
73a49a4 GOW3_BIG_CORES=1: renderer threads kept on the big cores
c9514c1 Driver knobs: TU_DEBUG and MESA options through uncharted-env.txt, GPU profile busy/idle summary
d6e7276 In-order scalar copies up to 64 KiB
ba585d7 In-order scalar copies on the graphics queue with the precise host-read wait
```
Latest APK (d8c1836): https://github.com/eladharels/uncharted_android/actions/runs/39788209079
