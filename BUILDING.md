# Building USB Audio 360

The repository does not contain Microsoft's proprietary Xbox 360 XDK or
XexTool.

## Requirements

- Xbox 360 XDK 21256.3 (not redistributed with this repository)

The plugin builds either on Linux under Wine or natively on Windows.

### Linux

- Linux with Wine
- XexTool v6
- GCC and Clang C++ compilers for the portable tests
- Python 3, 7-Zip, Xvfb, and Xauth

On Ubuntu, install the open-source dependencies with:

```bash
sudo apt update
sudo apt install -y build-essential clang python3 p7zip-full xvfb xauth
```

Wine and the proprietary tools must be installed separately.

### Windows

- Windows with PowerShell 5.1 or later
- An installed XDK (default path:
  `C:\Program Files (x86)\Microsoft Xbox 360 SDK`)

No Wine, Xvfb, 7-Zip extraction, or XexTool is needed — the installed XDK
tools run natively.

## Build the plugin

### Linux

Place the XDK installer and XexTool beside the repository, or provide their
paths explicitly:

```bash
XDK_INSTALLER=/path/to/XBOX360-SDK-21256.3.exe \
XEXTOOL=/path/to/xextool.exe \
./tools/build_plugin.sh
```

The script extracts the required XDK files into the configured runtime cache,
compiles the PowerPC DLL, packages it with `imagexex`, applies XexTool, and writes
`bin/usb_audio360.xex`. The Guide controls use the system's native XUI runtime;
no separate UI resource needs to be installed.

### Windows

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_plugin.ps1
```

The script copies the sources and `src/xex.xml` into a build directory
(`%TEMP%\usb-audio360-build\build` by default), compiles all translation
units with the XDK PowerPC `cl.exe`, links the PowerPC DLL against
`xapilib.lib`, `xboxkrnl.lib`, and `libcMT.lib`, packages the result with
`imagexex` (compressed, flash, sysdll — see `src/xex.xml`), and writes
`bin\usb_audio360.xex` plus its SHA-256.

Expected result: a warning-free build apart from C4325 in `detour.h`
(section attributes on `.text` are ignored by the PowerPC compiler —
harmless and identical to the Linux build).

| Parameter   | Default                              | Purpose                                      |
|-------------|--------------------------------------|----------------------------------------------|
| `-XdkPath`  | `C:\Program Files (x86)\Microsoft Xbox 360 SDK` | XDK installation directory          |
| `-BuildDir` | `%TEMP%\usb-audio360-build\build`    | Intermediate object/PE/XEX directory         |
| `-DebugApi` | `0`                                  | `1` enables the debug API (`bin\debug\`)     |

Example with a custom XDK location and debug API:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_plugin.ps1 `
  -XdkPath 'D:\XDK' -DebugApi 1
```

Differences from the Linux build: no XexTool post-check (the Linux script
verifies the XEX flags with `xextool -l`; on Windows the flags are set via
`src/xex.xml` during `imagexex` — install XexTool separately to verify them
manually), and no XDK extraction (the installer's CAB payload is only
unpacked on Linux when the XDK is not installed).

## Run portable tests

```bash
bash tools/test_uac.sh
python3 -m unittest tests/debug_client_test.py
```

## Continuous integration

GitHub Actions builds every pushed commit using a dedicated self-hosted Linux
runner. Version tags matching `v*` also publish the validated XEX and its
SHA-256 file as a GitHub Release.

See the [runner deployment guide](deploy/github-runner/README.md) for the
Ubuntu LXC setup and private toolchain installation.

## Technical documentation

- [USB Audio core and test scope](docs/uac-core.md)
- [Debug API](docs/debug-api.md)
- [Xbox USB transport contract](docs/xbox-usb-transport-contract.md)
- [UAC2 baseline audit](docs/uac2-baseline-audit.md)
