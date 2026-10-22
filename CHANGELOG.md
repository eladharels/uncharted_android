# Changelog

## Unreleased — first Android release

Uncharted 1, 2 and 3 from *The Nathan Drake Collection* run natively on Android (tested on a
Galaxy S24 Ultra, Snapdragon 8 Gen 3 / Adreno 750, with a Turnip driver).

### Added
- Three-way merge of zFitness/gow3_android into zorde23/ucndc_pc: the Uncharted PC port runs on
  Android with FEXCore and the Bionic runtime.
- libSceFiber under the JIT (`guest_cpu_context_save/load`, `src/runtime_fiber_jit.c`) for
  Uncharted 3's job system.
- Game picker on the home screen (Uncharted 1, 2, 3; `uc_game` in `gow3.ini`) with the runtime
  options the PC launcher uses for each game.
- The launcher's backdrop is the game's own `sce_sys/pic1.png` from the player's dump; original
  compass app icon.
- CI: GitHub Actions builds and signs the APK on a fresh runner, reports every compile error per
  run and writes failures as check annotations.

### Changed
- Application id `com.uncharted.android` and Uncharted branding (English, Chinese), so the app
  installs next to the God of War III app.
- The boot linker and the start script use the chosen executable (`eboot.bin`,
  `big2-ps4_Shipping.elf`, `big3-ps4_Shipping.elf`); `libSceFios2.prx` is optional.
- Android build: fmt 11.2.0, NDK 29 in CI, job counts sized to the machine, host header packages
  staged automatically, host compiler flags kept away from the NDK.

- `uncharted-env.txt` in the game folder: runtime option overrides for experiments.

### Fixed
- Black screen once gameplay starts (audio playing, menu remnants frozen on screen): the arm64
  SRT walker now follows table pointers through the unprotected backing view with the uc3
  walker context, records each dword's guest source for the GPU-written fixup, copies the
  ReadConst windows for per-lane offsets, and lets GPU page tracking resolve its faults instead
  of zeroing loads for good. Cached shaders are rebuilt once (`Profile::srt_walker_revision`).
- Gameplay still black under a working HUD: the Uncharted games no longer run in God of War
  III's scaled session, whose UI composition redirected the target the scene is composited
  into. They output 1920x1080 as on the PC, scaled to the screen by the presenter.

- Performance on arm64: the renderer's wait loops back off to sleeping instead of
  `sched_yield` (`common/spin_wait.h`); timeline semaphore queries are rate-limited; ucndc's
  image byte patches (`GOW3_EOP_TICK_WAIT`, `GOW3_JOB_SPIN`/`GOW3_JOB_SLEEP`, `GOW3_UC3_RES`,
  `GOW3_UC3_RTHEAP_MB`) apply on Android, and every game runs with the job workers blocking
  when idle (`GOW3_JOB_SPIN=100 GOW3_JOB_SLEEP=1`).

- Render resolution setting for the collection (native, 720p, 540p, 480p; default 540p)
  through ucndc's `GOW3_UC3_RES` patch: 20-30 FPS at 540p on a Galaxy S24 Ultra.

- Sampling CPU profiler inside the game process (`GOW3_PROFILE`), its output in the log
  export, CI's unstripped-symbols artifact and `tools/android/profile/symbolize.py`.

- From the first on-device profiles (Uncharted 1, 540p): a scalar-copy HLE dispatch the HLE
  leaves to the GPU is handed to the recording thread like any dispatch instead of draining
  the draw pipe first (the drains were a third of the command processor's time); the draw
  recording thread no longer waits for the recording thread's copies at end-of-pipe events
  with nothing to download; stage A's drain and `WaitHostCopies` wake through futexes on
  arm64 instead of polling with sleeps.

### Known gaps
- See [docs/ANDROID_PORT.md](docs/ANDROID_PORT.md#known-gaps).

The changelogs of the projects merged here are in [docs/upstream](docs/upstream).
