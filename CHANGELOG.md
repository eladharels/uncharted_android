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

- `uncharted-env.txt` in the game folder: runtime option overrides for experiments, the
  Turnip driver's own (`TU_DEBUG`, `FD_DEV_FEATURES`, `MESA_*`) included.
- `GOW3_GPU_PROFILE=1` reports the GPU's busy and idle time per frame apart.
- Logs over adb: the game log and `profile.txt` are also copied to
  `/sdcard/Android/data/<package>/files/logs` and the game log goes to logcat
  (`UnchartedGame`); `tools/android/pull-logs.ps1` pulls them. The app is `profileable` from the
  shell for simpleperf and perfetto.
- Guest mutexes spin before they block (`GOW3_MUTEX_SPIN`, default 200 tries; 0 = off), as the
  PS4's libthr does, instead of a futex wait and wake at every collision. Measured worse (kernel
  2.2 → 0.3 cores, user 2.0 → 4.9, total +0.5-1 core in the Uncharted 2 menus): the default is
  now 0.
- `Guest sync/s` line in the 5-second statistics: guest mutex locks (and contended ones),
  condition-variable waits, timeouts, signals and broadcasts per second. First reading (Uncharted
  2 menus): 1.6-2.0 million locks/s, 18-25% contended, ~60 cond waits/s, ~2,400 broadcasts/s.
- `Guest lock callers` line: the guest code taking the locks (1 in 64 sampled, image offsets).
- `GOW3_JOB_SLEEP=1` also removes the job loop's second "may sleep" check (after the lock), so
  the workers wait on their condition variable instead of locking and unlocking without pause
  (2 million locks a second on the phone); `GOW3_JOB_SLEEP_WAIT=0` keeps the old patch. Measured
  (Uncharted 2 gameplay): 4.2-5.0 cores → 1.4, but 25 → 16 FPS (the 10 ms waits time out).
- `GOW3_JOB_WAIT_US` (default 1000): the job workers' condition wait is shortened from 10 ms.
  Measured (Uncharted 2 gameplay): 30 FPS at 2.2 cores, against 24.8 FPS at 5.0 cores with the
  old patch (the old-patch run on a hotter phone); 250 us 27 FPS, 3000 us 26 FPS.
- A buffer descriptor with a format the shader translator cannot handle (a reserved one, read
  from memory still being written) reads as zero instead of stopping the game.
- `GOW3_SHADER_DUMP_DIR`: where `GOW3_DUMP_SHADERS=1` writes (the app's external folder on
  Android, so adb can pull the dumps from a release build).
- Finding: 5 of 7 GPU hangs are one compute-then-full-screen-draw pair (`cs 1e8e11d0`, `ps
  b1032a3f`), present since the first build; not caused by the job-loop change (crash-rate run:
  old patch 2 hangs in 4 boots, new patch 0 in 5).
- Uncharted runs with `TU_DEBUG=nocb,sysmem` (no concurrent binning, direct rendering): Turnip's
  command processor faulted on the Adreno 750 (opcode errors, the cause of the device-lost
  hangs); gameplay test: 10 faults in 2 runs by default, 0 with both, same FPS.
  `uncharted-env.txt` can change or clear it.
- `GOW3_IMAGE_FREE_DELAY=N` (default 0): deleted images are freed N submissions later, to test
  whether the device-lost hangs are the GPU reading images freed while draws still queued in
  the draw pipe use them (the two shaders of the common hang are loop-free).
- `tools/android/autorun.sh`: an unattended measured session on the phone (adb over wireless
  debugging, launcher by UI dump, Cross presses into gameplay, summary, back to the Claude app).
- Finding (Uncharted 2 menus, 4.3 cores in use): the six game job workers take 3.5 cores, 86% of
  their samples in futex calls of one contended guest mutex (`runtime_mutex.c`); the renderer ~0.5.
  `GOW3_JOB_SPIN=0` does not change it (menus and gameplay). Two new GPU crashes seen with that
  option: Uncharted 2 `MapNumberConversion` data_fmt 15, Uncharted 1 device lost (ps `b2415af8`).
- The renderer threads are kept on the big cores (`GOW3_BIG_CORES=1`, the default; `2` for the
  whole process). The 5-second statistics print the cores' clocks and caps, the GPU clock and
  the process's CPU use: the phone caps the clocks at 1.5 GHz under the game's 4-5 cores of
  load. The stage-to-stage waits sleep in the futex directly instead of libc++'s polling
  (`sched_yield` for 60 µs per wait). The profile records each thread's CPU time per window.
- The draw pipe is always on for Uncharted: a core the phone had taken offline at start made
  the renderer count 7 cores and leave the pipe off (stage A then did everything, 20 FPS).

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

- Scalar copies on the graphics queue are written in order by the draw recording thread
  (`GOW3_HLE_INORDER_GFX`), waiting for the Vulkan recording thread only while a deferred read
  of the destination is pending (`GOW3_HLE_PRECISE_WAIT`), instead of draining the draw pipe.

### Known gaps
- See [docs/ANDROID_PORT.md](docs/ANDROID_PORT.md#known-gaps).

The changelogs of the projects merged here are in [docs/upstream](docs/upstream).
