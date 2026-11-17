# Configuration

The server reads `%APPDATA%\wivrn\config.json`. A file next to the executable
(`<install dir>\share\wivrn\config.json`) is read first and overridden by the per-user file.
Use `-f <file>` to read a specific file instead.

The dashboard that used to edit this file on Linux no longer exists; edit the JSON directly
and restart the server. Settings that belong to the headset (resolution, codec, refresh rate,
hand tracking) are still controlled from the headset app.

## Keys

### `application`

Application(s) to start when a headset connects. Either a string or a list of strings. Each
entry is a command line; arguments are split on spaces, with `"..."` quoting supported.

```json
{
  "application": "\"C:\\Program Files\\Steam\\steam.exe\" steam://rungameid/1234"
}
```

### `encoder`

Encoder selection. A string, a list, or an object per stream:

```json
{
  "encoder": [
    { "encoder": "nvenc", "codec": "h265" },
    { "encoder": "x264", "codec": "h264" }
  ]
}
```

Available encoders on Windows: `nvenc`, `vulkan`, `x264`.

`device` selects the Vulkan device for the Vulkan encoder (an index into the Vulkan device
list, not a `/dev/dri` path).

### `bit-depth`

Bits per colour component, 8 or 10.

### `tcp-only`

Only listen for TCP connections (useful over USB/`adb reverse`).

### `port`

Port for both TCP and UDP. Default 9757.

### `hostname`

Name published through DNS-SD. Defaults to the computer name.

### `publish-service`

`true`/`false`, or `"mdns"`/`"none"`. Disable to stop advertising the server on the network.

### `openvr-compat-path`

Directory of the OpenVR compatibility layer, the one containing `bin\win64\vrclient_x64.dll`
(for example `C:\Program Files\OpenComposite`). Set to `null` to leave
`openvrpaths.vrpath` untouched.

When unset, the server probes `OVR_COMPAT_SEARCH_PATH` (a `;`-separated list baked in at
configure time, default `C:/Program Files/OpenComposite;C:/Program Files/xrizer`).

### `hid-forwarding`

Forward the headset's keyboard, mouse and gamepad to the PC through `SendInput`.
Default `true`.

### `debug-gui`

Show the Monado debug GUI. Only available when built with
`-DWIVRN_FEATURE_DEBUG_GUI=ON`.

### `grip-surface`, `use-steamvr-lh`, `lh-max-extrapolation`, `lh-stick-deadzone`

See the headset settings; the Lighthouse options are Linux-only and ignored on Windows.

## Files

| File | Purpose |
| --- | --- |
| `%APPDATA%\wivrn\config.json` | This configuration |
| `%APPDATA%\wivrn\known_keys.json` | Paired headsets and their names |
| `%APPDATA%\wivrn\cookie` | Secret used for the mDNS TXT record |
| `%TEMP%\wivrn` | IPC socket directory |
