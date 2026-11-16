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
| `WIVRN_USE_AVAHI` | needs a Windows mDNS backend, off |
| `WIVRN_USE_GLIB` | DBus/GLib integration, off |
| `WIVRN_USE_LIBNOTIFY` | needs a Windows notification backend, off |
| `WIVRN_USE_LIBRSVG` | off |
| `WIVRN_USE_V4L2` | Linux-only, off |
| `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE` | Linux-only, off |
| `WIVRN_BUILD_WIVRNCTL` | systemd-only, off |
| `WIVRN_BUILD_DASHBOARD` | KDE/Qt Linux integration, off |
| `WIVRN_BUILD_CLIENT` | Android-only, off |

Remaining porting work is tracked in the phase plan: networking (`wivrn_sockets`),
process/service management, IPC/service discovery, audio, and input forwarding.
