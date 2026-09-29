# USB Audio 360

USB Audio 360 provides audio through inexpensive USB audio adapters when an
Xbox 360 is connected to a screen without speakers or an audio output, such
as a computer LCD. It lets headphones or powered speakers receive the
console's stereo mix without requiring an HDMI audio extractor and a separate
headphone amplifier.

It is implemented as an experimental DashLaunch plugin and supports a narrow
USB Audio Class 1 (UAC1) playback profile.

## Tested hardware

Development and hardware validation used a SABRENT AU-MMSA USB External
Stereo Sound Adapter with USB VID:PID `0d8c:0014`. Compatibility is determined
by the UAC1 interface profile described below rather than by this identifier,
so other adapters with the same profile may also work.

The MVP is hardware-tested on retail kernel `2.0.17559.0`. It supports a
descriptor-compatible UAC1 output with:

- 48 kHz, stereo, signed 16-bit PCM;
- configuration 1, AudioStreaming interface 1, alternate setting 1;
- adaptive full-speed isochronous OUT endpoint 1;
- a 200-byte maximum packet size and 1 ms interval;
- endpoint sample-frequency control; and
- no explicit feedback endpoint.

This is homebrew software for a modified (for example, RGH, JTAG, or
BadAvatar) Xbox 360 console. It is not compatible with an unmodified retail
console, and it is not an official Microsoft driver.

## How it works

The plugin hooks the kernel USB add/remove completion path and claims the
otherwise unsupported UAC1 playback interface. It captures the console's
final stereo render mix, converts it from planar floating-point samples to
interleaved signed 16-bit PCM, and keeps two four-frame isochronous transfers
queued to the device.

For a device connected during boot, the plugin waits for startup to settle,
validates the cached descriptors and USB topology, and re-enumerates only the
matching root port. It refuses this operation unless exactly one compatible
device is found and that device is the only physical device in its controller
pool. It never resets an entire USB controller. A notification is displayed
after the first successful audio transfer and when the device disconnects.

## Install

1. Build `bin/usb_audio360.xex` or obtain a release binary.
2. Copy it to the console's storage, for example:
   `Usb:\Plugins\usb_audio360.xex`.
3. Add it to a free DashLaunch plugin slot in `launch.ini`:

   ```ini
   plugin2 = Usb:\Plugins\usb_audio360.xex
   ```

4. Reboot with a compatible USB audio device attached.

## Warning

Best effort has been made to ensure this plugin fails gracefully; however,
during development there were occasions where the console failed to boot due
to a bug in the plugin. Testing was performed on a console modified with
BadAvatar.

If the plugin prevents the console from booting, recover by either:

1. Booting the console without the DAC connected.
2. Connecting the softmod storage to a computer and removing the plugin entry
   from `launch.ini`.

This plugin is heavily experimental and is not recommended for an installation
without a tested recovery path.

## Build

The repository does not contain Microsoft's proprietary Xbox 360 XDK or
XexTool. Building currently requires:

- Linux with Wine (no reason this could not be built on Windows);
- Xbox 360 XDK 21256.3; and
- XexTool v6.

Set paths explicitly when they are not beside the repository:

```bash
XDK_INSTALLER=/path/to/XBOX360-SDK-21256.3.exe \
XEXTOOL=/path/to/xextool.exe \
./tools/build_plugin.sh
```

The script extracts the required XDK files into a temporary build directory,
compiles the PowerPC DLL, packages it with `imagexex`, applies XexTool, and
writes `bin/usb_audio360.xex`.

## AI code-generation disclosure

This project was developed with substantial assistance from generative AI.
The AI assisted with reverse-engineering hypotheses, implementation,
refactoring, documentation, and build automation under human direction.
Behavior was iteratively reviewed and tested on real Xbox 360 hardware by the
project maintainer. AI-generated code can contain subtle errors; contributors
and users should audit the source and treat native kernel plugins as
high-risk software.

## What doesn't work

- Audio produced before the plugin loads, such as the startup animation
- Microphone input

## License

USB Audio 360 is licensed under GPL-3.0-or-later. The PowerPC detour code is
derived from GPL-licensed work; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
