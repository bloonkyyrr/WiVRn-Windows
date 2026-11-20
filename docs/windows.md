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

## How headsets find the computer

The server advertises `_wivrn._tcp` over mDNS from its own UDP socket: it sends the
PTR/SRV/TXT/A records for `_wivrn._tcp.local`, including every local IPv4 address, and
refreshes them every 5 seconds. It also sends a goodbye packet when it stops, so a
closed server disappears from the headset's list instead of lingering there.

The SRV target is `<computer>-wivrn.local` rather than `<computer>.local`: Bonjour and
the Windows mDNS responder also answer for the plain computer name and hand out
addresses the headset cannot reach (the link-local `169.254.x.x` address of an idle
adapter, for example). Link-local addresses are only advertised when the computer has
no other IPv4 address, because a headset that tries one first waits for the connection
to time out.

Windows' DNS-SD registration API (`DnsServiceRegister`) is not used: it fails with
`ERROR_INVALID_PARAMETER` on some systems, which left the computer missing from the
headset's server list. Allow `wivrn-server.exe` through Windows Firewall on private
networks so the headset can reach the advertised port, as described in the README.

## USB connection over ADB

WiVRn streams over the network by default. When `adb` (Android platform-tools) is on
`PATH`, the server also looks for headsets connected by cable, forwards its TCP port into
the headset (`adb reverse tcp:<port> tcp:<port>`) and lists the devices in the status
window. The **USB → Connect the headset over USB** menu action then starts the WiVRn app
with a `wivrn+tcp://localhost` intent, which makes it stream over the cable instead of
the network.

- The headset needs developer mode and USB debugging, and the debugging prompt shown
  inside the headset has to be accepted once per computer.
- The port is forwarded again by itself when the cable is unplugged and replugged.
- `--no-adb` turns the feature off; no `adb` process is started then.
- Without `adb` on `PATH` nothing is started and the status window says so.

## How the runtime is selected

- **OpenXR.** The OpenXR loader resolves the runtime from
  `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`. Writing that value needs administrator
  rights, so the server always sets `XR_RUNTIME_JSON` for itself (the variable is inherited
  by the applications it launches) and updates the registry when it can. The value is
  restored when the session ends.
- **Restoring.** The value that was active before WiVRn is stored in
  `%APPDATA%\wivrn\active_runtime_backup.json` and put back when the session ends, and at
  the next start if the previous run did not shut down cleanly. A runtime that was
  registered before WiVRn therefore keeps working afterwards.
- **OpenVR.** `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` is backed up to
  `openvrpaths.vrpath.wivrn-backup` and replaced with a single `runtime` entry pointing at
  the configured compatibility layer, which must contain
  `bin\win64\vrclient_x64.dll`. It is restored on exit, and at the next start if the
  previous run did not shut down cleanly.

## Paths

| What | Where |
| --- | --- |
| Configuration | `%APPDATA%\wivrn\config.json` |
| Previous runtime | `%APPDATA%\wivrn\active_runtime_backup.json` |
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

## Runtime menu

The server window lists the current OpenXR runtime and the OpenVR compatibility layer
in its status text, and its **Runtime** menu can:

- **Make WiVRn the active OpenXR runtime**: writes
  `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`, keeping the previous value for
  restoring. This needs administrator rights; without them the menu action reports the
  error and only `XR_RUNTIME_JSON` is used, which is enough for applications the server
  launches itself.
- **Restore the previous OpenXR runtime**: puts back the value saved before WiVRn took
  over.
- **Open the runtime folder**: shows the directory holding `openxr_wivrn.json`.

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
