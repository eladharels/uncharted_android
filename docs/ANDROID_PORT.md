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
4. **720p controller-hint panel** drawn black (Uncharted 1): frame-capture the panel's draw
   at 720p and compare with 1080p; likely a size the game derives from the output.
5. Settings that were measured and do not help at 540p: `GOW3_STALE_READBACK`,
   `GOW3_READBACKS=1`, `GOW3_PREP_TRUST_A1`, `GOW3_EAGER_SUBMIT=0` (worse),
   `GOW3_TEXTURE_HELPER=1` (worse). Do not retest without a code change behind them.

## Debugging

- `uncharted-env.txt` in the game folder overrides runtime options without a new APK: one
  `GOW3_NAME=value` per line (an empty value unsets it, `#` starts a comment). Applied after the
  game's own options in `UnchartedGames.kt`; each applied line is echoed at the top of
  `android-game.log`.

- In-app: **About → Export logs** (`android-game.log`, the previous run's log, logcat).
- `GOW3_FEX_LOG=1` prints FEXCore's own log; `TU_DEBUG` options go to Turnip (see the Mesa
  documentation).
- Exit codes of `run-bionic.sh`: 2 means a precondition failed before the game started (missing
  files, or an image prepared for another executable).
