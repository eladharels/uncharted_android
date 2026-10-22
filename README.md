# Uncharted: The Nathan Drake Collection — Android

An unofficial, native Android port of **Uncharted 1, 2 and 3** from *Uncharted: The Nathan Drake
Collection* (PS4). It runs your own PS4 dump on an Android phone with an Adreno GPU, with no PS4
emulator: the game's original x86-64 code runs in a JIT, and everything around it (the PS4 system
runtime, the shadPS4-based renderer, audio, input) is native ARM64 code.

> **No game files are included.** You need a dump of the collection from a PS4 you own.
> This project is not affiliated with, endorsed by or connected to Sony Interactive
> Entertainment or Naughty Dog. *Uncharted* and *PlayStation* are their trademarks.

## Status

| Game | Executable | Android |
|---|---|---|
| Uncharted: Drake's Fortune | `eboot.bin` | Playable, 20-30 FPS at 540p (30 while the phone is cool) |
| Uncharted 2: Among Thieves | `big2-ps4_Shipping.elf` | Runs; the train prologue is slow (see Tips) |
| Uncharted 3: Drake's Deception | `big3-ps4_Shipping.elf` | Starts; not played far on Android yet |

The default render resolution is 540p (Settings → Render resolution; 720p and native 1080p are
slower, and 720p draws the controller-hint panel black). Tested on a Samsung Galaxy S24 Ultra (Snapdragon 8 Gen 3, Adreno 750, Android 16 / One UI) with
dump CUSA02344 v01.00, the Mesa Turnip driver `v27.0.0-20261021-r4-8G2-OneUI`.
Other Adreno phones should work but have not been tried; Mali and other non-Adreno GPUs are not
supported (the port relies on Turnip). Reports from other devices are welcome (see
[Reporting a problem](#reporting-a-problem)).

## What you need

- An **arm64 Android phone with an Adreno GPU** (Snapdragon). Android 9 or newer; a recent
  flagship (Snapdragon 8 Gen 2 or newer) is recommended. About 8 GB of RAM or more.
- **Your own dump** of *Uncharted: The Nathan Drake Collection*, extracted to a folder that holds
  `eboot.bin`, `big2-ps4_Shipping.elf`, `big3-ps4_Shipping.elf`, `sce_sys/` and `sce_module/`
  (and the `u1data`, `u2data`, `u3data` folders). The PC port was developed with CUSA02344
  v01.02; v01.00 also runs. If you have the 01.02 update, merge it into the base folder.
- About 50 GB of free storage for the dump.

## Install and play

1. Download the APK from the [Releases](../../releases) page (or a CI build, see
   [Building](#building)) and install it. Android may ask you to allow installs from this source.
2. Copy your dump to the phone, for example to
   `/storage/emulated/0/games/Uncharted The Nathan Drake Collection/Game`.
3. Open **Uncharted Collection**, grant storage access and pick that folder in
   **Settings → General**. The home screen then shows the collection and your game's own
   background art.
4. **GPU driver:** in **Settings → Driver → Download drivers**, get a **Turnip** build for your
   GPU and select it on the home screen. On the Galaxy S24 Ultra (Adreno 750) use
   **Mesa Turnip `…-8G2-OneUI`** (not the `-Linux`, `-Wayland`, `A8xx` or `710-720` builds).
   Samsung phones generally need a *OneUI* build. The phone's own driver is not supported.
5. **Game:** pick **Uncharted 1, 2 or 3** on the home screen.
6. Set the **FPS limit** to 30 to start with, then tap **LAUNCH**. The first start of each game
   compiles shaders and takes longer.

Controls: on-screen buttons, or a Bluetooth/USB controller (map its buttons in
**Settings → Controls**). The in-game menu (drawer) has save management, restart and quit.

### Tips

- **30 FPS** is the sustainable setting on a phone; the games run at most at 60. A clip-on cooler
  helps keep clocks up in long sessions.
- If a game **freezes with sound still playing**, lower the FPS limit or resolution: the GPU is
  overloaded.
- **Black screen or corrupted colours:** make sure a Turnip driver is selected, then try another
  Turnip build (Purple Turnip, K11MCH1) from the driver list.
- The launcher keeps your settings in `gow3.ini` in the app's private folder; saves are exported
  and imported from **About → Save data**.
- **Known issues:** Uncharted 2's train prologue runs at 5-15 FPS (800-1,000 compute dispatches
  per frame; under investigation) and once lost the GPU there; at 720p Uncharted 1 draws the
  controller-hint panel as a black box (540p and 1080p are fine).

### Reporting a problem

Use **About → Export logs** in the app: it writes `Uncharted-Logs-<date>.zip` with the game
log, the previous run's log, logcat and your settings. Open an issue with that file, your phone
model, the driver you used and what you did.

## How it works

This repository merges two projects that share one history, both descended from deadinside28's
*bloodborne_pc* via Felipe Prado's God of War III port:

- **zorde23/ucndc_pc** — the unofficial native PC port of the Nathan Drake Collection
  (Windows): the game-specific runtime, the Uncharted fixes to the renderer and libSceFiber.
- **zFitness/gow3_android** — the Android runtime of the God of War III port: FEXCore runs the
  game's x86-64 code inside the app, the runtime and the shadPS4 video core are built for
  Android (Bionic), Vulkan goes to a Turnip driver, and a Kotlin launcher prepares the game.

The work specific to this port is in [docs/ANDROID_PORT.md](docs/ANDROID_PORT.md). Most notably:
Uncharted 3's job system runs on fibers, which the PC port switches with x86-64 assembly; on
Android a fiber switch is done inside the JIT by replacing the guest registers when the host call
returns (`guest_cpu_context_save/load`, `src/runtime_fiber_jit.c`).

## Building

The APK is built by GitHub Actions (`.github/workflows/android.yml`) on every push to `main`,
`android-port` or a `v*` tag, and on demand (**Actions → android → Run workflow**). The signed
APK is uploaded as the run's artifact; a `v*` tag also publishes a release.

The workflow signs with your own key, given as four repository secrets
(**Settings → Secrets and variables → Actions**):

| Secret | Value |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | the keystore file, base64 (`base64 -w0 my.keystore`) |
| `ANDROID_KEYSTORE_PASSWORD` | the keystore password |
| `ANDROID_KEY_ALIAS` | the key alias |
| `ANDROID_KEY_PASSWORD` | the key password |

Create a key once with
`keytool -genkeypair -keystore my.keystore -alias uncharted -keyalg RSA -keysize 2048 -validity 10000`,
keep the file safe and never commit it: an APK only installs over a previous one signed with the
same key.

To build locally (Linux x86-64 host) see [docs/ANDROID_PORT.md#building-locally](docs/ANDROID_PORT.md#building-locally).
A first build from scratch takes about 25 minutes on a 4-core machine.

## Credits

- **deadinside28** — [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), the native
  PS4-on-PC runtime this all builds on.
- **Supermedo** (Mohammed Albarghouthi) — the Windows port and
  [uncharted_pc](https://github.com/Supermedo/uncharted_pc).
- **Felipe Prado** — [gow3_pc / SpartanPort](https://github.com/FelipePrado0/gow3_pc).
- **zorde23 (mathe)** — [ucndc_pc](https://github.com/zorde23/ucndc_pc), the Nathan Drake
  Collection on PC.
- **zFitness** — [gow3_android](https://github.com/zFitness/gow3_android), the Android port of
  God of War III this port's Android side comes from.
- **ducvd89** — [Bloodborne-Android](https://github.com/ducvd89/Bloodborne-Android) and
  [bloodframe](https://github.com/ducvd89/bloodframe).
- **cuesta4**, the **shadPS4** team, the **FEX-Emu** team, the Mesa **Turnip** developers and the
  driver packagers (The412Banner, MrPurple666, K11MCH1, StevenMXZ, whitebelyash).
- Third-party libraries: see [docs/upstream/ucndc_pc-README.md](docs/upstream/ucndc_pc-README.md#credits)
  and the licences under `third_party/` and `gpu/third_party/`.

The original READMEs of the projects merged here are kept in [docs/upstream](docs/upstream).

## License

GNU General Public License v2 or later, as the projects it is built from ([LICENSE](LICENSE)).
Third-party components keep their own licences. Game data, game art and trademarks belong to
their owners and are not part of this project.
