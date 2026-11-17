# Building on Windows

Windows support is being ported incrementally. Phase 0 adds the build-system
scaffolding: Linux-only subsystems are disabled by default on Windows and can be
re-enabled as their native implementations land.

## Requirements

- CMake 3.28 or newer
- Visual Studio 2022 (MSVC) with the C++23 toolset, or a recent MinGW-w64
- Vulkan SDK 1.4.304 or newer
- [vcpkg](https://vcpkg.io/) (recommended) for the third-party dependencies

Dependencies currently expected from vcpkg (or an equivalent installation):

```
vcpkg install eigen3 nlohmann-json cli11 libarchive libpng openssl boost x264
```

Monado is still fetched by CMake; a Windows-capable Monado build is required for
the server library.

## Configure and build

```powershell
cmake --preset windows `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build-windows
```

The `windows` preset intentionally disables the following until their Windows
backends exist:

| Option | Status |
| --- | --- |
| `WIVRN_USE_VAAPI` | Linux-only, off |
| `WIVRN_USE_PIPEWIRE` | Linux-only, off |
| `WIVRN_USE_SYSTEMD` | Linux-only, off |
| `WIVRN_USE_AVAHI` | Avahi-specific; Windows uses DNS-SD directly, off |
| `WIVRN_USE_GLIB` | DBus/GLib integration, off |
| `WIVRN_USE_LIBNOTIFY` | needs a Windows notification backend, off |
| `WIVRN_USE_LIBRSVG` | off |
| `WIVRN_USE_V4L2` | Linux-only, off |
| `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE` | Linux-only, off |
| `WIVRN_BUILD_WIVRNCTL` | systemd-only, off |
| `WIVRN_BUILD_DASHBOARD` | KDE/Qt Linux integration, off |
| `WIVRN_BUILD_CLIENT` | Android-only, off |

All phases in the porting plan are implemented. The Windows server still needs
a Windows-capable Monado build; the remaining limitations are listed below.

## Input

Keyboard and mouse input from the headset is injected with `SendInput`. Linux
evdev key codes are translated to Windows virtual-key codes; keys without a
mapping are ignored instead of being injected as a different key. Relative
mouse motion and both scroll axes are forwarded as-is.

Gamepad input is exposed to OpenXR applications through `/user/gamepad`.
Mirroring it to non-OpenXR applications needs a virtual gamepad driver such as
ViGEmBus, which this port does not install; the mirroring option is a no-op on
Windows and logs a one-time notice.

## Audio

The Windows server uses WASAPI loopback capture of the default render endpoint
and sends that stream to the headset speaker. Because Windows has no in-process
way to publish a virtual audio device, microphone forwarding is not advertised:
apps keep using the default capture device. Routing game audio exclusively to
the headset requires a virtual audio driver, which this port does not install.
