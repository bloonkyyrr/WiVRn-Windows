<h1 align="center"> WiVRn </h1>

<div align="center">
  
[![License: GPL v3](images/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0) [![CI](https://github.com/bloonkyyrr/WiVRn-Windows/workflows/Build/badge.svg)](https://github.com/bloonkyyrr/WiVRn-Windows/actions/workflows/Build.yml) [![Format](https://github.com/bloonkyyrr/WiVRn-Windows/workflows/Format/badge.svg)](https://github.com/bloonkyyrr/WiVRn-Windows/actions/workflows/Format.yml)
  
</div>
<p align="center"><img src="images/wivrn.svg" width="180"></p>
<h3 align="center">Fully FOSS PCVR streamer</h3>

# About

WiVRn is an application that wirelessly streams a virtual reality game to a standalone VR headset from a **Windows** PC.

The PC side runs as the OpenXR runtime for the applications it launches: the game renders
locally, WiVRn encodes each frame and streams it to the headset, and tracking, audio and
input travel back over the same connection.

WiVRn supports a wide range of VR devices:

| Headset | Supported | Notes |
|:--------:|:--------:|:--------:|
| Quest 1 | ✓ |  |
| Quest 2 | ✓ |  |
| Quest 3 | ✓ |  |
| Quest 3s | ✓ |  |
| Quest Pro | ✓ |  |
| Pico Neo 3 | ✓ |  |
| Pico 4 | ✓ |  |
| HTC Vive Focus 3 | ✓ | Laggy | 
| HTC Vive XR Elite | ✓ | Laggy |
| Samsung Galaxy XR | ✓ |  |
| Other Android VR | ? | Cannot know |
| Play for Dream | ✖ | https://github.com/WiVRn/WiVRn/issues/465 |
| Non-Android VR | ✖ | Not Android |
| Non-VR Android | ✖ | VR required |

# Requirements

- Windows 10 or 11, x64
- A GPU with Vulkan 1.3 support. Hardware encoding uses NVENC on NVIDIA cards or the
  Vulkan video extensions; x264 software encoding is the fallback
- A headset on the same network as the PC (or connected over USB with `adb reverse`)

# Getting started

## PC Server

WiVRn is built for Windows 10/11 x64. Download the installer from the
[release page](https://github.com/bloonkyyrr/WiVRn-Windows/releases), or
[build it yourself](docs/windows.md#building).

The installer places `wivrn-server.exe`, the WiVRn OpenXR runtime and
`openxr_wivrn.json` in the same directory and creates a Start Menu shortcut.

For SteamVR and OpenVR games you also need a compatibility layer such as
[xrizer](https://github.com/Supreeeme/xrizer/) or [OpenComposite](https://gitlab.com/znixian/OpenOVR/),
then point `openvr-compat-path` at its directory in the [configuration](docs/configuration.md).

## Headset Client/App

Install the client on your VR headset:

It should either lead you to the [Meta Store](https://www.meta.com/experiences/7959676140827574/) (for Meta Quest headsets) or to download the correct APK (for other headsets).

> [!WARNING]
> The VR client and PC server need to be on the same version of WiVRn

> [!TIP]
> If the headset fails to connect to the computer, see [troubleshooting](#troubleshooting).

# Usage

## Running

### Prerequisites

- WiVRn publishes its service with DNS-SD, which Windows Firewall restricts by
  default. On first start, allow WiVRn on private networks, or add a rule:
  ```powershell
  New-NetFirewallRule -DisplayName "WiVRn" -Direction Inbound -Protocol UDP -LocalPort 9757 -Action Allow
  New-NetFirewallRule -DisplayName "WiVRn TCP" -Direction Inbound -Protocol TCP -LocalPort 9757 -Action Allow
  ```
- The PC and the headset must be on the same network.

### Start the PC server

Run `wivrn-server.exe` (Start Menu: **WiVRn server**). A window shows the connection state,
and the pairing PIN when a new headset may connect. Its menu disconnects the headset,
starts pairing, opens the configuration folder, or quits the server.

`wivrn-server` runs until you quit it from its window menu. Started from a terminal, the log
output goes to that console; started from the Start Menu, Windows opens a console window for it.

### Start the headset application

On the headset, when installed from the store, simply start `WiVRn`. If you installed the app
via `adb`, it will be in an "unknown sources" section.

On first start, it will ask if you want to enable some features such as microphone, hand tracking,
eye tracking, etc., as they will require permissions to be granted. It is possible to grant those
later from the Settings tab.

It is highly recommended to use default settings and only tweak them if you experience issues.

### Connect to the server

The headset application will start on a server list. Your computer should be visible and have a
connect button. Simply click it to start streaming.

The first time a headset connects it is unknown to the server: the server window shows a
pairing PIN, enter it in the headset app. Paired headsets are remembered in
`%APPDATA%\wivrn\known_keys.json` and reconnect without a PIN.

When the headset is connected, the server registers itself as the active OpenXR runtime for the
applications it launches, so VR applications only work while a session is running.

### Start an application

When the headset is connected and no XR application is running, it will show an application launcher.
Applications in that list are sourced from:

- Steam games that are flagged as VR (the `config/steamapps.vrmanifest` of your Steam installation,
  including library folders declared in `libraryfolders.vdf`).
- Start Menu shortcuts whose target is an executable.

You can set an application to be started automatically when your headset is connected in
[the configuration](docs/configuration.md#application).

# Building

A source build needs Visual Studio 2022 (C++23 toolset), the Vulkan SDK 1.4.304 or newer, and a
[vcpkg](https://vcpkg.io/) checkout with `VCPKG_ROOT` set. vcpkg installs the dependencies
declared in `vcpkg.json` on the first configure.

```powershell
cmake --preset windows
cmake --build --preset windows
```

From an "x64 Native Tools" prompt, Ninja works too: use the `windows-ninja` preset for both
steps, which writes to `build-windows-ninja` instead of `build-windows`. With the Visual Studio
generator the outputs sit in `build-windows\server\RelWithDebInfo`; `cpack --preset windows`
turns them into the NSIS installer.

The `windows` preset uses the Visual Studio 2022 generator. With a newer Visual Studio, pass
its generator on the command line, for example
`cmake --preset windows -G "Visual Studio 18 2026"`; the CI job picks the generator of the
installed Visual Studio automatically.

See [building on Windows](docs/windows.md#building) for the full requirements, the installed
layout, and the OpenVR/OpenXR runtime registration.

# Configuration

Most settings are controlled through the headset app. Server-specific settings live in
`%APPDATA%\wivrn\config.json`; see [configuration](docs/configuration.md).

# Troubleshooting

<details><summary>My computer is not seen by the headset</summary>

If the server list is empty in the headset app:
- Make sure your computer is connected on the same network as your headset
- Check that Windows Firewall allows `wivrn-server.exe` on private networks
- Check that `publish-service` in [the configuration](docs/configuration.md#publish-service)
  is not set to `none`
- Make sure no other process is bound to port 9757</details>

<details><summary>My headset does not connect to my computer</summary>
  
- Make sure inbound UDP and TCP 9757 are allowed in Windows Firewall
- The server and client must be the same version.</details>

<details><summary>How do I use a wired connection manually?</summary>

- Make sure WiVRn is installed and running on your computer
- Make sure you have the WiVRn app installed on your headset
- With the device connected by cable, run in a terminal (using `adb` may require developer mode):
   - ```powershell
      adb reverse tcp:9757 tcp:9757
      adb shell am start -a android.intent.action.VIEW -d "wivrn+tcp://localhost" org.meumeu.wivrn
      ```
   - Depending on your install type, you may need to replace `org.meumeu.wivrn` (Meta Store install) with:
      - `org.meumeu.wivrn.github` for [releases](https://github.com/WiVRn/WiVRn/releases) on Github
      - `org.meumeu.wivrn.github.nighly` for Github nightlies (wirvn-apk [repository](https://github.com/WiVRn/WiVRn-APK/releases))
      - `org.meumeu.wivrn.github.testing` for Github CI builds
      - `org.meumeu.wivrn.local` for developer builds
- Pair the headset as described in [connect to the server](#connect-to-the-server).</details>

<details><summary>Where are the logs?</summary>

- The server logs to its console window
- From the install directory in PowerShell, keep a copy with `.\wivrn-server.exe > wivrn.log 2>&1`</details>

<details><summary>I have high motion latency, black borders following my view, hear corrupted audio or see a corrupted, pixelated image</summary>

- When connecting through USB, make sure the headset isn't connected through WiFi (switch off WiFi)
- Reset the settings using the button at the bottom of the settings tab
- Try switching to software encoding
- Decrease the bitrate
- Decrease the resolution in the WiVRn app
- Connect through USB or use a better WiFi router.

Note: WiVRn isn't properly optimized for NVIDIA GPUs due to the lack of developers with NVIDIA hardware. Motion latency may be significantly worse at rendering resolutions higher than default.</details>

# Community Support

We are available on either **Discord** or **Matrix space**:

[![LVRA Discord](https://img.shields.io/discord/1065291958328758352?style=for-the-badge&logo=discord)](https://discord.gg/EHAYe3tTYa) [![LVRA Matrix](https://img.shields.io/matrix/linux-vr-adventures:matrix.org?logo=matrix&style=for-the-badge)](https://matrix.to/#/#linux-vr-adventures:matrix.org)

Please use the `wivrn` chat room for questions or issues specific to WiVRn.

# Contributing

## Translations

See [translating](docs/translating.md) for procedure.

# Credits
WiVRn uses the following software:
- [ambientCG](https://ambientcg.com/)
- [Boost.Locale](https://github.com/boostorg/locale)
- [Boost.PFR](https://github.com/boostorg/pfr)
- [CLI11](https://github.com/CLIUtils/CLI11)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [fastgltf](https://github.com/spnda/fastgltf)
- [FreeType](https://freetype.org/)
- [glm](http://glm.g-truc.net/)
- [HarfBuzz](https://harfbuzz.github.io/)
- [libarchive](https://libarchive.org/)
- [libpng](http://www.libpng.org/pub/png/)
- [Monado](https://monado.freedesktop.org/)
- [nlohmann/json](https://github.com/nlohmann/json)
- [nvenc](https://developer.nvidia.com/nvidia-video-codec-sdk) optional, for hardware encoding on NVIDIA
- [OpenSSL](https://www.openssl.org/)
- [spdlog](https://github.com/gabime/spdlog)
- [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)
- [Vulkan](https://www.vulkan.org/)
- [WebXR input profiles](https://www.npmjs.com/package/@webxr-input-profiles/motion-controllers)
- [x264](https://www.videolan.org/developers/x264.html) optional, for software encoding
