# Building USB Audio 360

The repository does not contain Microsoft's proprietary Xbox 360 XDK or
XexTool.

## Requirements

- Linux with Wine (the build can also be adapted to run on Windows)
- Xbox 360 XDK 21256.3
- XexTool v6
- GCC and Clang C++ compilers for the portable tests
- Python 3, 7-Zip, Xvfb, and Xauth

On Ubuntu, install the open-source dependencies with:

```bash
sudo apt update
sudo apt install -y build-essential clang python3 p7zip-full xvfb xauth
```

Wine and the proprietary tools must be installed separately.

## Build the plugin

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
