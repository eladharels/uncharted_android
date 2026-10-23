# The Android port: how it was made and how it works

This document is for people working on the code. For installing and playing, see the
[README](../README.md).

## Where the code comes from

```
deadinside28/bloodborne_pc ── Supermedo (Windows) ── FelipePrado0/gow3_pc (God of War III)
                                                        │
                                  ┌─────────────────────┴─────────────────────┐
                         zorde23/ucndc_pc                            R-V-L/gow3_pc (Linux)
                     (Uncharted 1-3, Windows)                                  │
                                  │                                 zFitness/gow3_android
                                  │                                (Android, FEXCore, Bionic)
                                  └──────────────┬────────────────────────────┘
                                         this repository
```

Both parents share their history up to commit `998ebc5` of gow3_pc. This port is a real
three-way merge of `zFitness/gow3_android` (branch `android`) into `zorde23/ucndc_pc` from that
common ancestor (merge commit `ceba749`), followed by the fixes listed below. The upstream remotes
make later merges possible:

```bash
git remote add upstream https://github.com/zorde23/ucndc_pc
git remote add android https://github.com/zFitness/gow3_android
git fetch upstream && git fetch android android
git merge upstream/main      # Uncharted fixes from the PC port
git merge android/android    # Android runtime and launcher fixes
```

## Architecture on Android

| Part | Where | On Android |
|---|---|---|
| Game code | the dump's executable | x86-64, run by FEXCore's JIT inside the app process (`cpu/`, `libgow3cpu.so`) |
| Host ↔ guest boundary | `src/guest_cpu.h`, `cpu/guest_cpu_fex.cpp` | Guest calls of host functions go through `mov r10, rcx; syscall; ret` stubs; host calls of guest code through `HandleCallback` |
| PS4 system runtime | `src/runtime_*.c`, `src/probe.c` | native arm64 (Bionic), the `gow3-probe` executable |
| Renderer | `gpu/shadps4/` (shadPS4 video core) | native arm64, Vulkan through a Turnip driver loaded from the app (`gpu/shim/vk_driver_loader.cpp`), frames presented through AHardwareBuffers |
| Launcher | `android-launcher/` (Kotlin, Compose) | prepares the game image natively (`cpp/gow3prepare`, `cpp/gow3link`), runs `tools/android/device/run-bionic.sh` |

The game image (`boot-linked.bin`) is derived on the phone from the chosen executable and the
dump's `libc.prx` (and `libSceFios2.prx` when the game ships it); no game file enters the APK.

## What this port changes

### Fibers under the JIT (Uncharted 3)

Uncharted 3's job system (`NdJobWorkerThread`, "NdJob Fiber") runs every job on a
libSceFiber fiber and moves suspended fibers between worker threads. The PC port
(`src/runtime_fiber.c`) switches fibers with x86-64 assembly that swaps the host's callee-saved
registers and stack, which works because on x86-64 the game and the runtime share one register
file and stack.

Under FEXCore the guest's registers live in the JIT's per-thread `CPUState`, and the runtime
runs on its own host stack. So a switch is a replacement of guest registers:

- `guest_cpu_context_save(GuestContext*)`, called from a host function the guest called, copies
  the guest context it will resume with when that host function returns: `rbx, rbp, r12-r15`,
  `rsp` (at the return address of the guest's call), `rdi, rsi`, MXCSR and the x87 control word.
- `guest_cpu_context_load(const GuestContext*)` makes the guest resume with another context
  instead; `CallHost` applies it after the host function returns and resets the call-return
  prediction stack. The stub's `ret` then pops the saved return address of that context, so a
  suspended fiber returns from its own `sceFiberSwitch`; a new fiber's stack holds its entry point
  (popped by the `ret`) and, above it, a host trap as the entry function's return address.

`src/runtime_fiber_jit.c` implements libSceFiber on top of this. No host frame is left on a
fiber's stack, so a context is complete as soon as it is saved: it is marked suspended at once and
another thread may resume it. Switches are allowed only at one host-to-guest call depth (the
thread's entry), which is where Uncharted 3 switches.

### Renderer

- The x86-only timing and spin intrinsics the Uncharted renderer added (`__rdtsc`,
  `__builtin_ia32_pause`) go through `Common::RawRDTSC()` (`CNTVCT_EL0` on arm64) and
  `COMMON_CPU_RELAX()`.
- Queue setup keeps gow3_android's sparse-binding queue and the Uncharted readback side queue
  (`GOW3_READBACK_SIDEQ`).
- The SRT walker (`flatten_extended_userdata_pass.cpp`, `srt.h`): x86-64 keeps the PC port's
  machine-code walker; arm64 uses gow3_android's portable bytecode, extended with the Uncharted
  walker context (pointer loads through the backing view, guest sources for the GPU-written
  fixup) and ReadConst windows (`OpCopyWindow`). A faulting load reads as zero for one walk
  only, and GPU page tracking resolves the walker's faults first (`runtime_fault_recover_gpu`).
  Without this, gameplay rendered black.
- Presentation: the Uncharted games skip God of War III's scaled session (`GOW3_RENDER_RES`,
  UI composition at the output size), which hid the scene behind the HUD. They output
  1920x1080 as on the PC and the presenter scales it to the screen (`run-bionic.sh`;
  `GOW3_UC_SCALED=1` brings the scaled session back).
- `IsExecuteError` for arm64 (instruction aborts), duplicate declarations from the merge removed.

### Runtime

- The PC port's x86 code hooks (God of War III cheats, mouse camera callbacks) are off under the
  JIT, as in gow3_android. ucndc's game patches that only change bytes of the image
  (`GOW3_EOP_TICK_WAIT`, `GOW3_JOB_SPIN`/`GOW3_JOB_SLEEP`, `GOW3_UC3_RES`,
  `GOW3_UC3_RTHEAP_MB`) are applied on Android too, before the JIT translates the code.
- The job workers: Naughty Dog's six `NdJobWorkerThread`s spin for work without blocking during
  gameplay. On a phone that takes most of the cores from the renderer's threads, so every game
  runs with `GOW3_JOB_SPIN=100 GOW3_JOB_SLEEP=1` (the PC port's Uncharted 3 setting).
- The renderer's wait loops (`common/spin_wait.h`): on arm64 they spin `GOW3_SPIN_US` (20 µs),
  yield as long, then sleep `GOW3_WAIT_SLEEP_US` (50 µs) per poll, instead of `sched_yield`
  for the whole wait (the x86-64 loops reached it after ~150 µs of PAUSE; arm64's YIELD is a
  cycle, so they reached it at once: 25-50 ms of kernel time per frame in the GPU thread).
- Timeline semaphore queries are rate-limited (`GOW3_SEM_POLL_US`, 100 µs) and their cost is
  logged with `GOW3_FRAME_STATS`.
- Render resolution: the launcher's *Render resolution* setting (native, 720p, 540p, 480p)
  chooses the game's own render and output size through ucndc's `GOW3_UC3_RES` patch
  (`run-bionic.sh` sets `GOW3_UC3_RES`, `GOW3_UC3_RES_ALL=1` and `GOW3_UC3_MOVIE_FIX=1`, the
  PC launcher's combination). The default is 540p: the phone's GPU is the limit at 1080p, and
  the game threads wait less for GPU readbacks when the GPU finishes sooner (Galaxy S24 Ultra:
  12-15 FPS at 1080p, 20-30 at 540p).
- Mouse input is stubbed on Android (`gpu/shim/window_android.cpp`).

### Launcher

- `UnchartedGames.kt`: the collection is recognised by its executables (any region); `uc_game` in
  `gow3.ini` picks the game, and each game gets the runtime options the PC launcher
  (`launcher/uc_launcher.py`) uses for it. The home screen shows a picker for it.
- `Gow3Prepare.kt` and `gow3_prepare_jni.cpp`: the chosen executable is linked instead of
  `eboot.bin`; `libSceFios2.prx` is linked only when the dump has it. `launch.env` carries the
  executable and the game's options; `run-bionic.sh` imports them and checks the image's digest
  against the chosen executable.
- Application id `com.uncharted.android`, so it installs next to the God of War III app.
- The launcher backdrop is the player's own `sce_sys/pic1.png` (or `pic0.png`), read at run
  time (`GameArt.kt`). The app icon is an original compass; no game artwork is packaged.

### Build and CI

The Android build scripts assumed the original developer's workstation (cached dependencies, a
24-core machine, a local NDK). For a fresh machine they now:

- size job counts and core pinning to the machine;
- install NDK `29.0.14206865` (CI) and find it under `ANDROID_HOME`;
- keep the conda host environment's x86 compiler flags (`CFLAGS`, `LDFLAGS`, ...) away from the
  NDK builds;
- stage the header-only host packages (`tools/android/inputs/stage_header_deps.sh`);
- build fmt 11.2.0 (pinned by commit), which the shadPS4 v0.19 core needs;
- always ship `libSDL3.so` in the runtime payload.

The workflow keeps going past compile errors and, when a build fails, writes each step's outcome
and the failing log's error lines as check annotations, so a failure can be read through the
checks API as well as the Actions page.

## Building locally

Host: Linux x86-64 with `clang`, `cmake`, `ninja`, `meson`, `pkg-config`, `python3`, JDK 17, the
Android SDK (platform 35, build-tools 35.0.0) and NDK 29.0.14206865. The CI workflow is the
reference; the steps are:

```bash
git clone --recursive https://github.com/<you>/uncharted_android && cd uncharted_android
micromamba create -p .deps/env -c conda-forge -f ci/environment.yml   # host packages (Boost, ...)
micromamba activate .deps/env
bash ci/setup-deps.sh "$CONDA_PREFIX"           # robin-map, Zydis, Vulkan-Headers
bash tools/android/inputs/fetch_steam_libs.sh   # libraries of the launcher's Steam sign-in
bash tools/android/build_bionic_deps.sh         # FFmpeg, SDL3, fmt, ... for Android
bash tools/android/build_bionic.sh              # FEX, libgow3gpu, gow3-probe
bash tools/android/apk/build_release_apk.sh     # out/release/*.apk
```

Without signing variables the APK is signed with a generated local development key; set
`GOW3_KEYSTORE`, `GOW3_KEYSTORE_PASSWORD`, `GOW3_KEY_ALIAS` and `GOW3_KEY_PASSWORD` to use your
own. `docs/QUICKSTART.md` (from gow3_android) has more detail on the host dependencies.

## Known gaps

- Performance: 20-30 FPS at 540p on a Galaxy S24 Ultra (30 FPS limit), 12-15 at 1080p. At
  540p the limit is the two renderer threads with Uncharted 1's 2,000-3,800 draws per frame
  (`GOW3_STAGEB_PROFILE=1`, boat section): the recording thread spends 8.5-9 µs per draw
  (binding 3.3 µs, of which textures 1.1 and buffers 0.6; begin 0.6; inputs 0.7; the rest in
  the Vulkan calls) plus ~84 µs per recorded task (mostly the ~760 submissions/s, 630 of them
  eager), and the command processor 9.5 µs per draw (pipeline check 4.4, of which the SRT walk
  3.65; post 3.9) while waiting for the recording thread 40% of the time. Readback settings
  (`GOW3_STALE_READBACK`, `GOW3_READBACKS=1`, `GOW3_PREP_TRUST_A1`) no longer change the
  frame rate there, and `GOW3_TEXTURE_HELPER=1` makes it worse. Next: profile the recording
  thread on the device (simpleperf) and cut the per-draw cost.
- At 720p (1280x720) the controller-hint panel at the bottom right is drawn as a solid black box
  with a blocky corner (its blurred backdrop); 1080p and 540p are fine.
- A separate update folder is not overlaid for the Uncharted executables: merge the update into
  the base dump.
- Some internal names (packages, file names, settings keys) still say `gow3`, inherited from
  the God of War III port; renaming them would make upstream merges harder.

## Next steps (as of 2026-10-22)

Measured on a Galaxy S24 Ultra at 540p, in this order of expected gain:

0. **First on-device profile (2026-10-22, Uncharted 1 boat, 540p, 20 FPS window):** no
   renderer thread was more than a third busy (command processor 28%, draw recording 34%,
   Vulkan recording 31%; copy threads, submit and shader threads idle), the game's main
   thread slept 98% and its six job workers were blocked in futexes most of the time (as
   `GOW3_JOB_SPIN`/`GOW3_JOB_SLEEP` intend; `libc.so+0x9be24` is the syscall stub, the caller
   tells futex from `sched_yield` and `nanosleep`). The frame was latency: stage A draining at
   every scalar-copy HLE dispatch (36% of its time; ~20 per frame, two thirds small guest
   copies, a third 2.6 MB GPU copies), stage B waiting for the Vulkan recording thread at every
   end-of-pipe event (`ProcessDownloadImages` → `WaitHostCopies`, 22%), and the 50 µs sleep
   quanta of the wait loops between them. Changed: GPU-only HLE copies are handed over without
   a drain (`ScalarCopyHleWritesGuest`), the end-of-pipe wait is skipped when nothing is
   downloading, the drain and `WaitHostCopies` wake through futexes. Measured without
   gain: `GOW3_HLE_INORDER_GFX=1` alone (its in-order copy waited for the Vulkan recording
   thread at every copy). With `GOW3_HLE_PRECISE_WAIT=1` that wait is conditional, and both
   are on: the typical frame went from 50 ms to 33 ms (the 30 FPS cap); the heavy frames
   (2,000+ draws) still take 50-60 ms. Stage A's remaining waits (35% of its time): the VO
   label wait (12.5%, the frame cap and the flip), `OnSubmit`'s per-frame tick (6%), readback
   commands from faulting game threads (`ProcessCommands`, 7%), in-order copy fallbacks
   (`pending`, 7%). The profiler's stacks are reliable from build 3fc77f7 on (PAC bits).
   The frame time still scales with the draw count at ~15 µs per draw (command thread
   ~10 µs, draw recording ~8.5 µs, Vulkan recording unmeasured); cutting that is the next
   lever, with a profile of the busy samples of those three threads.
   **The driver and the GPU (2026-10-22):** at 540p neither limits the frame. The game never
   waits for the GPU (VO label waits all "already free", stage A's GPU-tick waits 0.1-0.7%),
   the Vulkan recording thread is idle 60-70% of the time even in the 14 FPS windows, and the
   driver's own CPU work inside it is 22-24% of wall time (~10 µs per draw, on a thread with
   slack). At 1080p the same scenes ran 12-15 FPS: there the GPU side is the limit. The frame
   has ~220 render pass ends, ~210 barriers and ~285 layout transitions whatever the draw
   count (`uc3 draw stats` line); on a tiling GPU each pass end is a tile resolve and each
   barrier between draws a drain, which costs nothing at 540p and is the first suspect at
   1080p. Tools for this round: `GOW3_GPU_PROFILE=1` (GPU ms per frame, busy and idle apart,
   by render pass and dispatch), `TU_DEBUG=sysmem` / `TU_DEBUG=gmem` through
   `uncharted-env.txt` (forcing direct or tiled rendering tells whether the pass structure
   matters), and `GOW3_BIG_CORES=1` (the renderer threads kept on the big cores: the
   scheduler reads their burst-and-wait pattern as light load). The driver build itself is not
   a lever: the `8G2-OneUI` variant is the packager's Standard build plus a flag for the
   Adreno 740, and the phone's own driver is not supported by the renderer.
   **Measured (same day):** the GPU timer at 540p: busy 9-21 ms per frame (27-48% of the
   frame), idle the rest; `TU_DEBUG` tests therefore skipped at 540p. `GOW3_BIG_CORES=1`
   with the GPU timer, first run after 40 minutes of rest: the command thread at 5-7 µs per
   draw, frames of 1,850-2,250 draws at the 30 FPS cap (33.3 ms median), 24-31 FPS through
   the boat section. The run right after it, big cores alone: 7.7 µs per draw and the same
   waits for 15 s, then a step to 10-12 µs per draw with stage B 45-50% busy at the same
   draws per second, 19-23 FPS for the rest of the run. Every stage slower at the same work,
   starting a fixed time into the session, reads as thermal throttling of the CPU clocks
   rather than as an effect of either option; the `CPU clocks (GHz)` line now printed with
   each 5-second window is there to settle it. First clock readings (Uncharted 2, train
   prologue, two short sessions): the big cores at 1.5-1.6 GHz and the prime core at 1.6
   during gameplay (2.4 in the menus, 2.7 the highest seen; hardware maxima ~3.1 and 3.3),
   falling from 1.9 to 1.5 over a session. Whether that is a thermal cap (scaling_max_freq
   lowered) or the governor's choice for a bursty load (cur below an untouched max) decides
   the remedy, so the line now also prints the caps when they sit below the hardware
   maximum, the GPU clock (kgsl) and the process's CPU use in cores. **Settled (Uncharted 1,
   5-minute session on a warm phone):** it is a cap. `scaling_max_freq` sat at 2.4-2.6 GHz
   from the start (the phone's game/thermal policy; the hardware allows 3.1-3.4) and dropped
   to 1.5 GHz on the performance cores and 1.6 on the prime core 40 s into gameplay, where
   it stayed; the per-draw cost on the command thread went from 5.7-7.3 µs at 2.4 GHz to
   7.3-12 µs at 1.5. The process used 4.2-5.1 cores throughout, 1.2-2.1 of them kernel
   time: the stages' wait loops. libc++'s `std::atomic::wait` polls with `sched_yield` for
   60 µs before it sleeps, and the drain, stage B's idle wait, the host-copy wait and the
   ring copier's waits all went through it thousands of times per second; they now wait in
   the futex directly after a short spin (`common/futex_wait.h`). The remaining levers are
   the same kind: the backoff's yield phase (`GOW3_YIELD_US`, default 20), stage B's 50 µs
   spin (`GOW3_PIPE_SPIN_US`), the four SCHED_IDLE draw-preparation workers that soak up
   every idle core (`GOW3_PREP_WORKERS=0`), and the ~2,000 `mprotect` calls per second of
   the precise readbacks (each a TLB shootdown across the cores). A CPU profile
   (`GOW3_PROFILE=1`) with the clock line says where the 4-5 cores go. The earlier long
   sessions (9-12 µs per draw throughout) were all capped ones.
   **Two more findings (same day, builds 3637b9d):** the futex waits did not lower the CPU
   use (4.2-4.9 cores before and after), so the kernel time is elsewhere (page faults and
   `mprotect` of the readbacks, the driver's ioctls); the profile now records every thread's
   CPU time per window (`C` lines, the table at the top of the report) to attribute it
   exactly. And the run without the env file ran with the draw pipe off: the phone had a
   core offline at start (thermal), `Available()` read 7, and the pipe needs 8. On Android
   `Available()` is now the configured core count and `GOW3_DRAW_PIPE=1` is in the Uncharted
   defaults. In the profile, stage A's largest wait (10-25% of its time at 15 FPS) is the
   precise readback of a GPU-written buffer bound as a constant (`ResolveHostCopy` →
   `ReadMemory`, draining the pipe and waiting for the GPU) at compute dispatches and
   draws: a GPU-to-CPU-to-GPU round trip for data that never needed to leave the GPU. A
   GPU-side copy into the constant ring for GPU-written windows is the next lever; it is
   also what Uncharted 2's train prologue (1,000 dispatches per frame) is waiting on.
   **Where the cores go (2026-10-23, Uncharted 2, build 7c53eca, first profile symbolized on the
   PC):** eight windows of a light scene (~190 draws per frame, the menus; 30 FPS throughout)
   with the process at 4.2-4.3 cores, 2.2-2.3 of them kernel time, and the caps at 2.4-2.7 GHz
   falling to 1.7. The six game job workers (`NdJobWorkerThread`) take 3.5 of those cores
   (0.55-0.72 each, half kernel); the whole renderer ~0.5 (command thread, draw recording and
   Vulkan recording ~0.1 each). Each worker's samples: 49% blocked in the futex of a guest
   `mutex_lock` (`runtime_mutex.c:83`), 37% in the futex wake of `mutex_unlock` (`:97`), only 4-5%
   in `cond_timedwait`, the idle wait the game meant them to use, the rest JIT code. The six
   workers fight over one guest mutex continuously, and Bionic's `pthread_mutex` takes every
   collision to the kernel (no spin before the futex wait, a wake syscall on every contended
   unlock), where FreeBSD's, which the PS4's libthr is built on, spins first. That, not the
   renderer, is the heat: the load is the same in a menu as in gameplay (4.2-5.1 cores
   measured on 2026-10-22). Next: a short adaptive spin (trylock with a CPU `yield`) in the guest
   `mutex_lock` before the futex wait; a quick test first without a new APK: `GOW3_JOB_SPIN=0`
   in `uncharted-env.txt` (workers block on the condition variable at once). The same profile in
   gameplay will tell whether the workers' share holds under load.
   **Measured, same day: `GOW3_JOB_SPIN=0` does not help.** Uncharted 2 menus 4.3 cores (user
   2.1, kernel 2.2), as without it; Uncharted 1 gameplay (2,650 draws per frame, 17.5 FPS, caps
   at 1.5 GHz) 3.7-5.0 cores, the six workers 2.3 of them, each still 53% in `mutex_lock` and
   33% in `mutex_unlock`. The contention is not in the spin loop the patch shortens: the
   workers collide on the lock before they get there, in the menus and in gameplay alike. The
   lever is the guest mutex itself.
   **Measured, same day: spinning in the guest mutex makes it worse** (`GOW3_MUTEX_SPIN=200`,
   build 48d346b, same Uncharted 2 menus at 30 FPS): kernel time fell from 2.2 to 0.2-0.3 cores,
   but user time rose from 2.0 to 4.5-5.0, the process from 4.2-4.3 to 4.8-5.8 cores; each
   worker 0.63-0.83 cores, nearly all of it in the trylock loop. The lock is contended without
   pause, so a spin only turns the futex sleeps (half of the workers' samples before) into busy
   waiting: the futex path was throttling them. The cost is the workers' polling itself, not the
   price of a collision. Next: find what keeps them on that lock (the broadcasts the game sends
   tens of thousands of times a second waking all six, or a poll under the lock): count guest
   lock, broadcast and wait calls per second in the 5-second statistics.
   **Counted (build bdc1e5b, Uncharted 2 menus and intro, 30 FPS, caps at 1.5-1.7 GHz, 4.3-5.7
   cores):** 1.6-2.0 million guest mutex locks per second, 290,000-460,000 of them (18-25%)
   finding the lock taken; only 40-120 condition-variable waits per second (none timing out),
   ~42 signals and 2,000-2,750 broadcasts. So it is not the broadcasts (a few thousand a
   second cannot cause 400,000 collisions) and the workers almost never reach their
   condition-variable sleep: they take a mutex some 300,000 times a second each, a poll under
   a lock. Each collision is a futex wait and a futex wake in Bionic, which is the 1.5-2.5
   cores of kernel time. Next: which lock and which guest code (the caller's guest address,
   sampled in `mutex_lock`, against the executable in the dump), then a patch of that loop
   like `GOW3_JOB_SPIN`'s, or a cheaper host path for that one mutex.
   **Found (build e48ee3f):** every sampled lock is the job workers' own, one mutex, taken at
   Uncharted 2 `0xb3c4e7` (the call right after `GOW3_JOB_SPIN`'s compare). The loop tests the
   "may sleep" byte (`[manager+0x59]`) twice: before the spin count (the je `GOW3_JOB_SLEEP`
   removes) and again after locking (`found + 0xa3` in all three games), where a clear byte skips
   the 10 ms condition wait and goes straight to the unlock; and the spin count is not reset on
   that path. In the states where the game clears the byte (the menus and gameplay), the
   patched workers therefore lock and unlock on every pass with no pause and no wait: our own
   patch turned the game's PAUSE spin into a lock spin. The second je is now removed with the
   first (`GOW3_JOB_SLEEP_WAIT=0` keeps it), so they sleep on the condition variable, which the
   game broadcasts ~2,000 times a second in that mode. Disassembly: rebuild an ELF from the dump
   with `scripts/prepare.py`'s `parse_self` and `llvm-objdump` (image offsets are the ELF's
   virtual addresses).
   **A/B (build c87ec43, Uncharted 2 gameplay at ~933 draws per frame, automated runs with
   `tools/android/autorun.sh`):** old patch (`GOW3_JOB_SLEEP_WAIT=0`, two runs) 24-28 FPS,
   4.2-5.0 cores (kernel 1.4-1.7), 1.7-2.7 million guest locks/s; new patch (two runs) 16-17 FPS,
   **1.4 cores** (kernel 0.2), 65,000 locks/s, but ~540 condition waits/s of which ~520 timed out:
   the work is not announced on that condition variable, so idle workers slept the full 10 ms
   before they looked again. Hence `GOW3_JOB_WAIT_US` (default 1000; the 10 ms at `found + 0xb4`,
   same in all three games): the workers wake every N us. One of three new-patch runs ended in a
   GPU hang 26 s in, at boot (stuck after dispatch cs `1e8e11d0`, before draw ps `b1032a3f`); the
   other two and both old-patch runs did not: not yet known whether it is related.
   Big cores stay the default (no run showed harm). The frame times are quantized to vblank
   multiples (33.3, 50, 66.7 ms) by the game's own pacing, so the FPS figures move in steps
   of 30, 20, 15.
1. **Uncharted 1, recording thread (stage B), 8.5-9 µs per draw.** Profile it on the device
   (a self-profiling mode: bundle simpleperf from the NDK, record a few seconds from the
   overlay, export with the logs) and cut the per-draw cost: resource binding (3.3 µs),
   the Vulkan calls (~4 µs), and the ~84 µs per recorded task (submissions). Target: the
   20-24 FPS stretches at a steady 30.
2. **Uncharted 2, train prologue, 800-1,000 compute dispatches per frame.** Each is a sync
   point (`A:dispatch` 108-145 µs on stage A, game threads waiting up to 115 ms for results).
   `GOW3_HLE_INORDER=1 GOW3_ACB_DISPATCH_PIPE=1` must stay on (off: 2,500 drains/s, 5 FPS).
   Look at what the dispatches are (scalar copies the HLE could take, or real compute) and
   whether the readback waits can be shortened.
3. **Uncharted 2, GPU hang in the train prologue** (device lost on a full-screen draw,
   ps `3371ae8d` after cs `1c3672c5` 480x1x1; happened once, not reproduced). Watch for it; if it
   repeats, dump that pixel shader and check it against native resolution.
   **Two new GPU crashes (2026-10-23, build 7c53eca, both runs with `GOW3_JOB_SPIN=0`, so a
   timing effect of it cannot be excluded yet):** Uncharted 2, ~2.5 minutes in, the first heavier
   scene (503 draws per frame) after `RunSrtProgram: SRT walker load faulted; using zero`
   warnings: `pixel_format.h:361 MapNumberConversion: Unreachable code! data_fmt = 15` (a buffer
   format the recompiler does not map). Uncharted 1, ~4 minutes of gameplay: device lost at
   submit, the GPU stuck on an indexed draw, vs `c4e9ae30` ps `b2415af8`, 4,779 indices (the
   breadcrumbs in `android-game.log`). Retest both without the option before chasing them.
4. **720p controller-hint panel** drawn black (Uncharted 1): frame-capture the panel's draw
   at 720p and compare with 1080p; likely a size the game derives from the output.
5. Settings that were measured and do not help at 540p: `GOW3_STALE_READBACK`,
   `GOW3_READBACKS=1`, `GOW3_PREP_TRUST_A1`, `GOW3_EAGER_SUBMIT=0` (worse),
   `GOW3_TEXTURE_HELPER=1` (worse), `GOW3_JOB_SPIN=0` (no change in CPU use, 2026-10-23). Do
   not retest without a code change behind them.

## Debugging

- **CPU profile on the phone**: `GOW3_PROFILE=1` in `uncharted-env.txt` (or
  `start,interval,duration,hz` in seconds, default `30,30,6,1000`) runs a sampling profiler
  inside the game process (`gpu/shim/gow3_profiler.cpp`): from 30 s after boot, every 30 s, it
  samples the renderer's threads at 1 kHz and the others at 100 Hz for 6 s, following the
  frame-pointer chains, and appends the window to `logs/profile.txt` (thread list, stacks with
  counts, memory map, FPS of the window). The log export includes the file. On a PC,
  `tools/android/profile/symbolize.py profile.txt --symbols DIR` prints per-thread self and
  inclusive reports; DIR is CI's `uncharted-android-symbols-<sha>` artifact of the same build
  (unstripped `libgow3gpu.so`, `libgow3cpu.so`, `gow3-probe`). Addresses in the Vulkan driver
  or libc come out as library+offset.
- **GPU profile on the phone**: `GOW3_GPU_PROFILE=1` in `uncharted-env.txt` (the renderer's
  timestamp profiler, `vk_gpu_profiler.h`): every 5 s, `GPU profile: X ms/frame busy, Y
  ms/frame idle between N submissions, ...` then the GPU time per frame by render pass
  (size, formats, shader hashes), compute dispatch and HLE copy. Busy against the frame time
  says whether the GPU limits; the idle time is the GPU waiting for the CPU (the frame cap
  included).
- `uncharted-env.txt` in the game folder overrides runtime options without a new APK: one
  `NAME=value` per line (an empty value unsets it, `#` starts a comment). Applied after the
  game's own options in `UnchartedGames.kt`; each applied line is echoed at the top of
  `android-game.log`. Besides the port's `GOW3_*` options it passes the Turnip driver's:
  `TU_DEBUG` (`sysmem` forces direct rendering, `gmem` tiled, `nolrz`, `noubwc`, several
  comma-separated), `FD_DEV_FEATURES`, `IR3_SHADER_DEBUG` and `MESA_*`.
- `CPU clocks (GHz): ...` with each 5-second `Frame stats` window: the cores' current clocks
  (`scaling_cur_freq`); `capped to ... of ...` when the allowed maximum sits below the
  hardware's (thermal management); the GPU clock where kgsl exposes it; and the process's
  CPU use over the window in cores (user and kernel).
- Wait loops on the phone: `GOW3_SPIN_US` (20), `GOW3_YIELD_US` (20) and `GOW3_WAIT_SLEEP_US`
  (50) are the phases of the generic backoff (`common/spin_wait.h`); `GOW3_PIPE_SPIN_US` (50)
  stage B's spin before it sleeps; `GOW3_PREP_WORKERS` (4) the SCHED_IDLE draw-preparation
  workers. The stage-to-stage waits (drain, host copies, ring copier) sleep in the futex
  directly (`common/futex_wait.h`), with no polling to tune.
- `GOW3_MUTEX_SPIN` (0; 200 only in build 48d346b, measured worse): a contended guest mutex
  (`scePthreadMutexLock`, `pthread_mutex_lock`) is tried that many times with a CPU pause before
  the thread blocks in the futex (`src/runtime_mutex.c`); 0 blocks at once. The runtime's exit
  report counts the contended locks taken while spinning and those that slept.
- `Guest sync/s: ...` with each 5-second window: the guest's mutex locks per second and how many
  found the lock taken, condition-variable waits (and how many timed out), signals and
  broadcasts (`runtime_mutex_stats`). For the job workers' lock traffic. Then `Guest lock
  callers`: one lock in 64 sampled, the top five by share, each as the image offset the lock
  returns to, `<-` the next two image addresses on the guest stack (its callers, heuristically),
  and the mutex; offsets read against the executable at the logged `Image base`.
- `GOW3_BIG_CORES` (default 1 for Uncharted): the command processor, the draw recording,
  Vulkan recording and ring copy threads are kept on the cores whose maximum clock is at
  least 80% of the fastest core's (`Big cores:` line in the log). `2` extends it to the whole
  process (the main thread at GPU initialization, inherited by the game's threads); an empty
  value in `uncharted-env.txt` turns it off.

- In-app: **About → Export logs** (`android-game.log`, the previous run's log, logcat).
- Over adb, from a release build: the launcher copies the game log, the previous run's log
  and `profile.txt` (after each profiler window) to the app's external folder, and the game
  log line by line to logcat with the tag `UnchartedGame`.
  `tools/android/pull-logs.ps1` pulls the folder into `logs-<time>/` (or by hand:
  `adb pull /sdcard/Android/data/com.uncharted.android/files/logs`); live:
  `adb logcat -s UnchartedGame`. The app is `profileable` from the shell, so `simpleperf` and
  `perfetto` can attach to the game process; `run-as` still needs a debug build.
- `GOW3_FEX_LOG=1` prints FEXCore's own log.
- Exit codes of `run-bionic.sh`: 2 means a precondition failed before the game started (missing
  files, or an image prepared for another executable).
