# Debug-only XBDM smoke tests

Build with `USB_AUDIO360_DEBUG_API=1 bash tools/build_plugin.sh`.
Outputs: `bin/debug/usb_audio360.xex` and its matching `usb_audio360.json`
manifest. Marker: `0x55414342`. The default build excludes this mailbox.

## Transport

This reuses the legacy `PluginCommand` mechanism from development commit
`813e55c`: standard XBDM `setmem` writes an aligned, big-endian DWORD,
and the audio worker consumes it using `InterlockedExchange`. No custom XBDM
command registration, monitor replacement or kernel patch is needed.
The installed monitor resolved its module handle but returned `0xc0000263`
for the command-registration export, so the earlier command processor and
lookup retries have been removed.

Use one client at a time on a trusted development network. The mailbox is not
an authentication boundary and does not make XBDM memory access safe for
untrusted clients. The client verifies module checksum/timestamp, bounds,
initialized build marker, ABI and diagnostic pointers against the local
build manifest before any command write. Keep that manifest with its binary.
There are no embedded console addresses or host IPs. Address discovery is
relative to the module base reported by XBDM, not a fixed load address.

## Client

```sh
python3 tools/usb_audio_debug.py CONSOLE_HOST status
python3 tools/usb_audio_debug.py CONSOLE_HOST silence --duration 1000
python3 tools/usb_audio_debug.py CONSOLE_HOST results
python3 tools/usb_audio_debug.py CONSOLE_HOST controls
python3 tools/usb_audio_debug.py CONSOLE_HOST tone --duration 5000
python3 tools/usb_audio_debug.py CONSOLE_HOST stop
python3 tools/usb_audio_debug.py CONSOLE_HOST trace --row 0
```

Use `--module NAME.xex` if the installed plugin was renamed, and `--manifest`
for a manifest outside the default debug output directory. Duration is
250..10000 milliseconds; rows are 0..31. Read-only status/results/trace use
`getmem`. Mutating commands wait up to three seconds for acknowledgement.
A timeout leaves the outcome unknown: the client never retries a command.
Do not launch concurrent clients; a sequence check detects some interference,
but remote read-then-write is not a multi-client lock.

The debug build completes setup and opens endpoints once, then waits without
playing audio. Tone preserves the quiet peak of 900 on a signed-16-bit scale,
ramps, and repeats the prior one-second tone envelope every two seconds.
No volume increase, arbitrary control request, user PCM, USB reset, code
execution or module unload is exposed. This still uses stage 5's full-speed
explicit-feedback profile, not a complete generic UAC1/UAC2 test harness.

## Mailbox ABI v1

`UsbAudioDebugMailbox[16]`:

| Word | Meaning |
| --- | --- |
| 0..2 | Ready magic 0x55414d42, ABI 1, build marker |
| 3 | Command word, cleared atomically by worker |
| 4 | Acknowledgement counter, advanced after dispatch/rejection |
| 5 | Result: 0 none, 1 accepted, 2 dispatched, 0xffffffff rejected |
| 6..7 | Test run counter, permanent stopped flag |
| 8..12 | Pointers to main/control/tone/activation/setup-trace diagnostics |
| 13..15 | Reserved |

Only word 3 is client-writable. Encode `0xa5000000 | (command << 16) | duration`.
Commands: 3 controls, 4 tone, 5 silence, 6 stop. Controls/stop require duration
zero. Unknown commands, reserved bits and invalid durations are rejected.
This is a single-word publication with no separately mutable parameter buffer.
Readback acknowledgement means dispatch, not completed USB transfers or sound.

Status snapshots may advance while a test runs. `out` and `feedback` are
completed/submitted pairs. `stage=8` means finished/idle; `complete=1` means
matching completion accounting, not audible sound. `verified=1` means
activation readbacks finished. Volume is raw hexadecimal signed 8.8 dB.
`result` describes the latest mutating command, not streaming success.
The setup trace retains the first 32 requests per attachment, without wrapping.

## Safety and validation

Worker dispatches USB through the existing USB DPC domain. New tests require
idle slots, matching completion counts, no pending control, no stop/error latch,
and zero adapter ownership, rechecked in the USB domain. Only test counters,
tone phase and pacing budget reset. Attachment generation and stop/error
latches are never cleared. Endpoint objects stay open and owned.

Stop halts replenishment, not ownership. Pending transfers retain storage until
callbacks complete; a one-second drain deadline applies. Timeout, removal or
accounting failure prevents restart. Other packet errors also block another
test; recorded initial not-accessed misses remain tolerated. The per-direction
submission cap is 4096 batches in addition to the time limit.

Validation order: mailbox status/rejected start with no device; connect and
verify idle setup; two short silent runs without reconnecting; stop/drain a
longer silent run; announced quiet tone. Host tests do not prove hardware
ordering. One installation/reboot is required; binary changes and detached or
faulted sessions still require reboot.

Hardware validation completed for marker `0x55414342` with the AirPods Max
UAC2 test device. The manifest matched the loaded binary; a 250 ms silence
command with no device attached was consumed and rejected without transfers.
After attachment, post-activation readback reported alternate 1, 48 kHz,
unmuted output and a valid master-volume value.

Two one-second silent tests ran on the same open endpoints and drained with
matching submission/completion counts (251 batches per direction each time),
no non-startup packet errors and no reconnect or reboot. A longer silent test
was remotely stopped and drained with 2347 matching batches per direction.
Fresh control readback then completed on the same attachment.

Finally, a remotely requested ten-second quiet-tone run completed 2501 matching
OUT/feedback batches, generated 229985 nonzero stereo frames at peak 900, and
reported only one current-frame startup miss per direction. The user confirmed
the tone was audible. This establishes the tested UAC2 control, format,
feedback pacing, packet construction, repeated-run and remote mailbox paths.
The subsequent production build, marker `0x55414343`, was then hardware-tested
with the same device and successfully played continuous captured Xbox system
audio. The production result validates the end-to-end path; it does not imply
compatibility with every UAC2 topology or format.
