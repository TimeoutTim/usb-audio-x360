# USB Audio 360

USB Audio 360 provides audio through inexpensive USB audio adapters when an
Xbox 360 is connected to a screen without speakers or an audio output, such
as a computer LCD. It lets headphones or powered speakers receive the
console's stereo mix without requiring an HDMI audio extractor and a separate
headphone amplifier.

It is implemented as an experimental DashLaunch plugin. The current
development source supports descriptor-selected full-speed USB Audio Class 1
(UAC1) and USB Audio Class 2 (UAC2) stereo playback profiles at 48 kHz.

**Development branch:** continuous captured Xbox system audio has passed
hardware validation with both UAC1 and UAC2 output devices. Physical removal
stops new transfers, drains cancellation completions, and re-arms the driver
after a one-second settling period so another compatible device can be used
without rebooting. See [test scope and procedure](docs/uac-core.md).

## Tested hardware

| Device | USB audio class | USB VID:PID | Connection | Validation |
| --- | --- | --- | --- | --- |
| SABRENT AU-MMSA USB External Stereo Sound Adapter | UAC1 | `0d8c:0014` | USB-A | Continuous Xbox system audio |
| Apple AirPods Max USB Audio | UAC2 | `05ac:110c` | USB-A-to-USB-C cable | Continuous Xbox system audio |
| [SPACETOUCH USB Audio](https://www.amazon.com.au/dp/B0GT9K55KX?ref=ppx_yo2ov_dt_b_fed_asin_title) | UAC1 | `0666:0880` | USB-A | Continuous Xbox system audio |
| Sennheiser MOMENTUM 3 Wireless | UAC1 | `1377:6004` | USB-A-to-USB-C cable | Continuous Xbox system audio |

Compatibility is selected from USB Audio descriptors rather than these device
identifiers. The list records hardware that has been tested successfully; it
is not an allowlist or a guarantee that every UAC1 or UAC2 topology and format
will work. Descriptor discovery retains bounded PCM playback candidates even
when this MVP cannot stream them; a separate compatibility selector chooses a
safe native 48 kHz stereo profile and prefers 16-bit PCM.

The driver is hardware-tested on retail kernel `2.0.17559.0`. Its current
descriptor and transport policy supports:

- 48 kHz stereo signed PCM in two-, three-, or four-byte sample containers;
- descriptor-selected configurations, interfaces, alternates, and endpoints;
- full-speed isochronous playback with a 1 ms interval and one transaction;
- endpoint capacities up to the USB full-speed limit of 1023 bytes;
- UAC1 fixed-rate `SYNC_NONE`, adaptive/synchronous playback, or asynchronous
  playback with explicit feedback; and
- UAC1 endpoint sample-frequency control and UAC2 clock-entity control.

The SPACETOUCH validates a capture-first composite layout whose playback
interface advertises a shared 600-byte capacity for its 48/96 kHz and
16/24-bit alternates. The driver selects its 48 kHz/16-bit alternate and sends
192-byte packets; it does not infer packet length from advertised capacity.
The MOMENTUM 3 validates UAC1 interface-first endpoint-rate setup and tolerant
handling of a class-specific endpoint descriptor that appears before its
standard endpoint descriptor.

This is homebrew software for a modified (for example, RGH, JTAG, or
BadAvatar) Xbox 360 console. It is not compatible with an unmodified retail
console, and it is not an official Microsoft driver.

## How it works

The plugin hooks the kernel USB add/remove completion path and claims an
otherwise unsupported UAC1 or UAC2 playback interface. It captures the
console's final stereo render mix and converts it from planar floating-point
samples into the descriptor-selected signed PCM container. UAC1 sample-rate
control is endpoint-based. Fixed 48 kHz UAC1 profiles do not receive an
unnecessary rate write; variable-rate profiles use interface-first setup with
one bounded deactivate/rate/reactivate fallback. UAC2 follows the terminal's
clock graph and uses AudioControl clock requests. Asynchronous devices use
their explicit feedback endpoint to choose each packet's frame count.

The render callback only publishes PCM into a bounded ring. Two four-packet
OUT slots—and two feedback-IN slots when needed—are replenished from USB
completion in the controller's serialized execution domain. If the DAC was
already attached before DashLaunch loaded the plugin, the driver waits for
startup to settle and re-enumerates that device's root port. This boot-only
recovery fails closed unless the cached descriptors, kernel signature,
controller state, node pool, and root-port mapping are valid and the DAC is
the only physical device in that controller pool. It never resets an entire
USB controller. A notification is displayed after the first successful audio
transfer and when the device disconnects. On physical removal, static transfer
storage is reused only after all submitted transfers have completed or been
cancelled. The driver asynchronously closes its audio, feedback, and
default-control endpoints using dedicated close requests, waits for every
close callback, and only then completes kernel removal and re-arms.
Re-arm checks are rate-limited and bounded; failure to drain within five
seconds leaves the driver stopped until reboot. Repeated UAC1 reconnects and a
UAC1-to-UAC2 switch have passed this cleanup path without rebooting. Hotplug
and removal never reset a USB controller or port.

Only one USB audio playback interface is claimed at a time. If multiple DACs
are connected, the first compatible interface enumerated remains the active
device and every additional interface stays on the kernel's normal unsupported
device path; the plugin does not open its endpoints or modify its driver state.
Connecting or removing an ignored DAC therefore cannot disturb active playback.
If the active DAC is removed, an already-connected secondary is not promoted
automatically—unplug and reconnect the desired DAC after cleanup completes.

## Install

1. Build `bin/usb_audio360.xex` or obtain a release binary.
2. Copy it to the console's storage, for example:
   `Usb:\Plugins\usb_audio360.xex`.
3. Add it to a free DashLaunch plugin slot in `launch.ini`:

   ```ini
   plugin2 = Usb:\Plugins\usb_audio360.xex
   ```

4. Reboot with a compatible USB audio device attached.

## Volume control

Hold **Back** on any connected controller and tap **D-pad Up** or
**D-pad Down** to change USB audio volume in 5% steps. The software volume
ranges from 0% to 100% and resets to 100% when the console restarts. A short
audio cue plays at the new level after each change, providing audible feedback
without leaving a long-lived notification on screen.

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

GitHub Actions builds every pushed commit using a dedicated native Linux runner.
Version tags matching `v*` also publish the validated XEX and its SHA-256 file
as a GitHub Release. See the [runner deployment guide](deploy/github-runner/README.md)
for the Ubuntu LXC setup and private toolchain installation.

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
