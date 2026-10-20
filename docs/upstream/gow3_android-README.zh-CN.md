# 战神 III 重制版 · Android 移植版

[English](README.md) · **简体中文** · [Português (Brasil)](README.pt-BR.md)

<p align="center">
  <strong>God of War III Remastered · Android / ARM64 port</strong>
</p>

<p align="center">
  将 PS4 版《战神 III 重制版》带到 Android 设备上运行。
  <br />
  基于 FEXCore、Vulkan / Turnip 与专用运行时，项目仍在持续开发中。
</p>

<p align="center">
  <a href="docs/QUICKSTART.md">快速开始</a> ·
  <a href="#系统要求">系统要求</a> ·
  <a href="#工作原理">工作原理</a> ·
  <a href="docs/gow3/ROADMAP.md">路线图</a>
</p>

<p align="center">
  <a href="https://github.com/zFitness/gow3_android/releases/latest"><img src="https://img.shields.io/github/v/release/zFitness/gow3_android?label=release" alt="最新发布" /></a>
  <a href="https://github.com/zFitness/gow3_android/releases"><img src="https://img.shields.io/github/downloads/zFitness/gow3_android/total?label=downloads" alt="下载量" /></a>
  <a href="https://github.com/zFitness/gow3_android/stargazers"><img src="https://img.shields.io/github/stars/zFitness/gow3_android?style=flat" alt="Stars" /></a>
  <a href="#许可证"><img src="https://img.shields.io/badge/license-GPL--2.0--or--later-blue" alt="许可证：GPL 2.0 或更高版本" /></a>
  <a href="https://github.com/zFitness/gow3_android/commits"><img src="https://img.shields.io/github/last-commit/zFitness/gow3_android?label=last%20commit" alt="最近提交" /></a>
  <a href="https://github.com/zFitness/gow3_android/issues"><img src="https://img.shields.io/github/issues/zFitness/gow3_android?label=issues" alt="Issues" /></a>
  <a href="https://ko-fi.com/zfitness"><img src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=kofi&logoColor=white" alt="在 Ko-fi 上支持我" /></a>
</p>

<p align="center">
  <a href="#系统要求"><img src="https://img.shields.io/badge/Android-arm64-3DDC84?logo=android&logoColor=white" alt="Android arm64" /></a>
  <a href="#工作原理"><img src="https://img.shields.io/badge/Vulkan-AC162C?logo=vulkan&logoColor=white" alt="Vulkan" /></a>
</p>

<p align="center">
  <img src="docs/imgs/a.jpg" alt="游戏截图 1" width="45%"/>
  <img src="docs/imgs/b.jpg" alt="游戏截图 2" width="45%"/>
</p>

<p align="center">
  <img src="docs/imgs/home.jpg" alt="Android 启动器首页截图" width="28%" />
  <br />
  <sub>Android 启动器 · 运行时安装与启动</sub>
</p>

《**战神 III 重制版**》（God of War III Remastered，PlayStation 4）的 **Android / ARM64 移植**（开发中）。

游戏的原始可执行文件是 x86-64 架构（PS4 使用的同样是 x86-64）。在 Android 设备上，游戏代码由
**FEXCore** 动态翻译后在 ARM64 上执行；一套为本作专门编写的运行时替代了 PS4 的系统库，图形部分则被
翻译为 Vulkan（经 Turnip 驱动连接 Adreno GPU）。它不是通用模拟器：整个项目只服务这一款游戏。本项目
fork 自 [R-V-L 移植版](https://github.com/R-V-L/gow3_pc)，后者基于
[deadinside28 的 bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)，并合入了
[cuesta4 的 shadPS4 分支](https://github.com/cuesta4/shadPS4) 中针对战神 III 的修复。
本项目的直接基础是 `gow3_pc`；在 Android 适配层参考并移植了部分
[ducvd89 的 Bloodborne-Android](https://github.com/ducvd89/Bloodborne-Android) 实现，包括运行时、系统服务、
输入、日志和启动器等部分。因此，`Bloodborne-Android` 是本项目 Android 侧的重要参考来源，但并不是本项目的直接父项目。
同时，本项目也使用了 [fexdroid](https://github.com/cobrabm12/fexdroid) 的部分 Android / FEX 相关实现。

> [!IMPORTANT]
> **本项目不包含任何游戏文件。** 你需要使用从自己的 PS4 导出的游戏文件（当前测试目标为 CUSA01740，
> 版本 01.02）。本项目与 Sony Interactive Entertainment、Santa Monica Studio 无关；God of War 是
> Sony Interactive Entertainment 的商标。

## 项目状态

| 项目 | 当前状态 |
| --- | --- |
| Android / ARM64 运行链路 | 🚧 开发中，持续进行真机验证 |
| Vulkan / Turnip 图形渲染 | ✅ 已接入 |
| Kotlin / Compose 启动器 | ✅ 已接入 |
| 稳定性能数据 | ⏳ 暂无 |

> 这是一个面向单款游戏的专用移植项目，并非通用 PS4 模拟器。

## 快速开始

从拿到项目到跑起 debug 的完整教程（Android 移植，macOS Apple Silicon 主机）见
**[docs/QUICKSTART.md](docs/QUICKSTART.md)**。

## 性能表现

Android 端仍在真机验证中，暂无稳定的性能数据。

## 已实现功能

- **运行链路**：x86-64 游戏代码由 FEXCore 在 ARM64 上执行（`cpu/`，`libgow3cpu.so`），图形经 shadPS4
  图形核心的 ARM64 构建（`libgow3gpu.so`）翻译为 Vulkan，经 Turnip 连接 Adreno GPU。
- **Kotlin / Compose launcher**（`android-launcher/`）：安装 runtime（SHA-256 校验）、管理 Surface、
  输入、音频、设置与进程生命周期。
- 复用了桌面版同一套游戏运行时、PS4 系统库重实现、图形核心与补丁逻辑；Android 端当前在真机上验证中，
  分辨率、FPS 与修改器补丁尚未做字节级校验。

## 系统要求

- **ARM64（arm64-v8a）的 Android 设备**，无需 root。
- 支持 Vulkan 的 GPU，且驱动（Turnip）较新（Adreno 设备）。
- 你自己的游戏副本：**CUSA01740 并已应用 01.02 更新**（包含 `eboot.bin`、`sce_module`、`sce_sys` 的目录）。
  下载包中不包含任何游戏文件。

构建 Android 端需要一台宿主机（当前流程基于 macOS Apple Silicon），依赖与步骤见
[docs/QUICKSTART.md](docs/QUICKSTART.md)。

## 工作原理

| 模块 | 位置 | 作用 |
| --- | --- | --- |
| 准备 | `scripts/` | 读取 `eboot.bin`，应用重定位，并把游戏自带的 `libc` 与 `libSceFios2` 链接为单一镜像 |
| 加载器 | `src/probe.c` | 加载镜像、安装经过校验的 hook（修改器、倍率、血条），并启动游戏原始代码 |
| 运行时 | `src/runtime_*.c` | 重新实现 PS4 系统函数：内存、线程、文件、音频、视频、手柄、存档与服务 |
| GPU | `gpu/shadps4/` | shadPS4 图形核心，将 PS4 GPU 指令翻译为 Vulkan，并包含后台着色器缓存、延迟回读等优化 |
| 游戏内菜单 | `gpu/shim/gow3_overlay.cpp` | Dear ImGui 菜单与性能叠加层；设置位于 `gpu/shim/gow3_settings.cpp` |
| 补丁 | `patches/God_of_War_III_Remastered.xml`、`scripts/patches.py` | 启动时在内存中应用，不修改游戏文件 |
| 启动 | `android-launcher/` | Android 端由 launcher 准备镜像、启动 `gow3-probe` |

更多文档：[模组](docs/MODS.md)、[帧图与放大器](docs/upscaler.md)、[GPU 并行工作](docs/parallel_gpu.md)
与[路线图](docs/gow3/ROADMAP.md)。

## 赞助

如果你喜欢这个项目，欢迎通过 [Ko-fi](https://ko-fi.com/zfitness) 赞助支持我，或用支付宝扫码收款：

<p align="center">
  <a href="https://ko-fi.com/zfitness"><img src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=kofi&logoColor=white" alt="在 Ko-fi 上支持我" /></a>
  <img src="Downloads/收款.jpg" alt="支付宝收款码" width="200" />
</p>

感谢每一位赞助者的支持！

## 致谢

本项目 fork 自 R-V-L 的 Linux 移植版，后者又源自 FelipePrado0 的原创项目：

- **deadinside28**：[bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)，最初的原生移植。
- **gow3_pc**（R-V-L）：[gow3_pc](https://github.com/R-V-L/gow3_pc)，本项目所基于的分支。
- **cuesta4**：[专注于 God of War III Remastered 的 shadPS4 分支](https://github.com/cuesta4/shadPS4)（分支 `eltutz`）。
- **shadPS4 团队**：[shadPS4](https://github.com/shadps4-emu/shadPS4) 渲染器。
- **fexdroid**：[fexdroid](https://github.com/cobrabm12/fexdroid)。
- **ducvd89**：[Bloodborne-Android](https://github.com/ducvd89/Bloodborne-Android)。本项目在 Android 适配层
  参考并移植了其中运行时、系统服务、输入、日志和启动器等部分实现。

此外还使用了：[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)、[FFmpeg](https://ffmpeg.org)、
[SDL3](https://github.com/libsdl-org/SDL)、[Dear ImGui](https://github.com/ocornut/imgui)、
[sirit](https://github.com/shadps4-emu/sirit)、[magic_enum](https://github.com/Neargye/magic_enum)、
[miniz](https://github.com/richgel999/miniz)、[xbyak](https://github.com/herumi/xbyak)、
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)、
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan)、AMD FidelityFX SDK、[LLVM](https://llvm.org)
以及 [Pillow](https://python-pillow.org)。

## 许可证

GNU GPL v2 或更高版本（见 [LICENSE](LICENSE)）。第三方组件保留各自的许可证。
