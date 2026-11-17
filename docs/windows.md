# WiVRn on Windows

The PC server targets Windows 10/11 x64 with MSVC. The headset client targets Android.

## Requirements

- Windows 10 or 11, x64
- CMake 3.28 or newer
- Visual Studio 2022 or newer (2026 tested) with the C++23 toolset
- Vulkan SDK 1.4.304 or newer (`VULKAN_SDK` set)
- glslang 16.x or newer, with `glslangValidator.exe` on `PATH`: the server and Monado
  compile GLSL shaders at build time (the vcpkg `glslang` port ships libraries only).
  `spirv-opt` is used when found and skipped otherwise.
- [vcpkg](https://vcpkg.io/), with `VCPKG_ROOT` pointing at the checkout
- Git (the Monado patches and the FetchContent sources are fetched with it)

Third-party dependencies come from the manifest in `vcpkg.json`; vcpkg installs them
in manifest mode on the first configure.

## Building

```powershell
cmake --preset windows
cmake --build --preset windows
```

The `windows` preset uses the Visual Studio 2022 generator. If the installed Visual Studio is
newer, override the generator on the command line — CMake accepts `-G` on top of a preset:

```powershell
cmake --preset windows -G "Visual Studio 18 2026"
cmake --build --preset windows
```

Or, from a "x64 Native Tools" prompt, with Ninja (independent of the Visual Studio version):

```powershell
cmake --preset windows-ninja
cmake --build --preset windows-ninja
```

Outputs land in `build-windows\server\RelWithDebInfo` (the Ninja preset writes to
`build-windows-ninja`): `wivrn-server.exe`, the OpenXR runtime `openxr_wivrn.dll` and
`openxr_wivrn.json` (which points the OpenXR loader at that DLL; Monado's
`libmonado` management library is Linux-only, so no `monado_wivrn.dll` is built here).

### Installer

```powershell
cpack --config build-windows/CPackConfig.cmake -C RelWithDebInfo
```

This produces an NSIS installer that installs everything into `bin/`, keeps the OpenXR
manifest next to the libraries it references, and adds a Start Menu entry.

Monado is still fetched by CMake; a Windows-capable Monado build is required for the
server library. Monado's IPC layer is the remaining porting risk (see below).

## How the runtime is selected

- **OpenXR.** The OpenXR loader resolves the runtime from
  `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`. Writing that value needs administrator
  rights, so the server always sets `XR_RUNTIME_JSON` for itself (the variable is inherited
  by the applications it launches) and updates the registry when it can. The value is
  restored when the session ends.
- **OpenVR.** `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` is backed up to
  `openvrpaths.vrpath.wivrn-backup` and replaced with a single `runtime` entry pointing at
  the configured compatibility layer, which must contain
  `bin\win64\vrclient_x64.dll`. It is restored on exit, and at the next start if the
  previous run did not shut down cleanly.

## Paths

| What | Where |
| --- | --- |
| Configuration | `%APPDATA%\wivrn\config.json` |
| Known headset keys | `%APPDATA%\wivrn\known_keys.json` |
| Service cookie | `%APPDATA%\wivrn\cookie` |
| Runtime files | `%TEMP%\wivrn` |
| Program data | `<install dir>\share\wivrn` |

## Input

Keyboard and mouse input from the headset is injected with `SendInput`. Linux evdev key
codes are translated to Windows virtual-key codes; keys without a mapping are ignored
instead of being injected as a different key. Relative mouse motion and both scroll axes
are forwarded as-is.

Windows blocks synthetic input for elevated windows (UIPI): if the foreground application
runs as administrator, forwarding has no effect.

Gamepad input is exposed to OpenXR applications through `/user/gamepad`. Mirroring it to
non-OpenXR applications needs a virtual gamepad driver such as ViGEmBus, which this port
does not install; the mirroring option is a no-op on Windows and logs a one-time notice.

## Audio

The Windows server uses WASAPI loopback capture of the default render endpoint and sends
that stream to the headset speaker. Because Windows has no in-process way to publish a
virtual audio device, microphone forwarding is not advertised: apps keep using the default
capture device. Routing game audio exclusively to the headset requires a virtual audio
driver, which this port does not install.

## Logs

The server logs to the console it is started from. To keep a copy:

```powershell
.\wivrn-server.exe > wivrn.log 2>&1
```

Set `XRT_LOG=debug` for verbose Monado logging, and `WIVRN_DUMP_VIDEO=<path>` to dump the
encoded stream.

## Known limitations

- Monado's Windows build and its IPC channel (`XRT_IPC_MSG_SOCK_FILENAME`, which the
  patched server exposes as a listening socket) are unverified. The server creates a
  loopback TCP listener and passes its descriptor to Monado; a Monado build that expects a
  Unix socket will not work on Windows.
- `patches/monado/0002-Use-extern-socket-fd.patch` and
  `patches/monado/0007-don-t-verify-GL-stuff.patch` were written against the Linux paths of
  Monado and may need a Windows variant.
- The SteamVR Lighthouse driver is Linux-only and disabled on Windows.
- Perfetto tracing needs `-DWIVRN_USE_PERFETTO=ON -DPERFETTO_SDK_DIR=<dir>` and is off by
  default.
