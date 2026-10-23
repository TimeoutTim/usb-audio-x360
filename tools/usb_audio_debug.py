#!/usr/bin/env python3
"""Single-client debug mailbox over standard XBDM getmem/setmem. No retries."""
import argparse
import json
import re
import socket
import struct
import time
from pathlib import Path


class Monitor:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(5)
        self.stream = self.sock.makefile("rb")
        try:
            if not self.line().startswith("201"):
                raise RuntimeError("Unexpected XBDM greeting")
        except Exception:
            self.close()
            raise

    def close(self):
        self.stream.close()
        self.sock.close()

    def line(self):
        value = self.stream.readline(8193)
        if not value.endswith(b"\n") or len(value) > 8192:
            raise RuntimeError("Invalid or oversized XBDM response")
        return value.decode("ascii", errors="strict").strip()

    def command(self, text):
        self.sock.sendall((text + "\r\n").encode("ascii"))
        response = self.line()
        if response.startswith("200"):
            return [response]
        if not response.startswith("202"):
            raise RuntimeError(response)
        lines = []
        for _ in range(1024):
            line = self.line()
            if line == ".":
                return lines
            lines.append(line)
        raise RuntimeError("Oversized multiline response")

    def words(self, address, count):
        raw = bytes.fromhex("".join(self.command(f"getmem addr=0x{address:x} length=0x{count*4:x}")))
        if len(raw) != count * 4:
            raise RuntimeError("Short memory read")
        return struct.unpack(f">{count}I", raw)


def locate(monitor, manifest, module):
    matches = []
    loaded = []
    for line in monitor.command("modules"):
        fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
        name = fields.get("name", "").strip('"')
        if name:
            loaded.append(name)
        if name.lower() == module.lower():
            matches.append(fields)
    if len(matches) != 1:
        raise RuntimeError("Expected exactly one loaded target module %r; loaded: %s"
                           % (module, ", ".join(loaded) or "<none>"))
    fields = matches[0]
    for key, field in (("checksum", "check"), ("timestamp", "timestamp")):
        if int(fields[field], 16) != manifest[key]:
            raise RuntimeError("Loaded binary does not match the build manifest")
    if manifest["version"] != 1 or manifest["marker"] != 0x55414342:
        raise RuntimeError("Unsupported mailbox manifest")
    base, size = int(fields["base"], 16), int(fields["size"], 16)
    counts = {"UsbAudioDebugMailbox": 16, "UsbAudioDiagnostic": 64,
              "UsbAudioControlDiagnostic": 16, "UsbAudioToneDiagnostic": 16,
              "UsbAudioActivationDiagnostic": 16, "UsbAudioSetupTrace": 256}
    addresses = {}
    for name, count in counts.items():
        offset = manifest["offsets"][name]
        if not isinstance(offset, int) or offset < 0 or offset % 4 or offset + count * 4 > size:
            raise RuntimeError("Manifest address outside loaded module")
        addresses[name] = base + offset
    box = monitor.words(addresses["UsbAudioDebugMailbox"], 16)
    if box[:3] != (0x55414d42, 1, manifest["marker"]):
        raise RuntimeError("Mailbox unavailable or incompatible")
    if monitor.words(addresses["UsbAudioDiagnostic"] + 63 * 4, 1)[0] != manifest["marker"]:
        raise RuntimeError("Wrong initialized build marker")
    expected = tuple(addresses[n] for n in counts if n != "UsbAudioDebugMailbox")
    if box[8:13] != expected:
        raise RuntimeError("Mailbox diagnostic addresses do not match manifest")
    return addresses, box


def status(monitor, addresses):
    box = monitor.words(addresses["UsbAudioDebugMailbox"], 16)
    main = monitor.words(addresses["UsbAudioDiagnostic"], 64)
    tone = monitor.words(addresses["UsbAudioToneDiagnostic"], 16)
    activation = monitor.words(addresses["UsbAudioActivationDiagnostic"], 16)

    def halves(word):
        return [word & 0xFFFF, word >> 16]

    # Per-channel render peaks (0..65535 each, 65535 = full scale) packed
    # into mailbox words 13..15 as L|R, C|LFE, Ls|Rs. Zero before the first
    # 1 s publish window simply reads as no activity yet.
    return dict(run=box[6], ack=box[4], result=box[5], pending=box[3], stopped=box[7],
                stage=main[32], error=hex(main[38]), complete=main[59],
                out=[main[15], main[4]], feedback=[main[13], main[5]],
                missed=[main[8], main[9]], errors=[main[10], main[14]],
                verified=activation[0], alternate=activation[2], hz=activation[5],
                mute=activation[7], volume_raw=hex(activation[9]),
                nonzero_frames=tone[7], peak=tone[8],
                render_peaks=dict(zip(("L", "R", "C", "LFE", "Ls", "Rs"),
                                      halves(box[13]) + halves(box[14]) + halves(box[15]))))


def watch_line(now, peaks):
    return "%s %s" % (now, " ".join("%s=%d" % (key, peaks[key])
                                    for key in ("L", "R", "C", "LFE", "Ls", "Rs")))


def watch(monitor, manifest, module, interval=1.0):
    if interval <= 0:
        raise ValueError("Interval must be positive")
    addresses, _ = locate(monitor, manifest, module)
    while True:
        print(watch_line(time.strftime("%H:%M:%S"),
                         status(monitor, addresses)["render_peaks"]), flush=True)
        time.sleep(interval)


def execute(monitor, manifest, module, command, duration=2000, row=0):
    addresses, box = locate(monitor, manifest, module)
    if command in ("status", "results"):
        return status(monitor, addresses)
    if command == "trace":
        if not 0 <= row <= 31:
            raise ValueError("Trace row must be 0..31")
        return {"row": row, "words": monitor.words(addresses["UsbAudioSetupTrace"] + row * 32, 8)}
    codes = {"controls": 3, "tone": 4, "silence": 5, "stop": 6}
    if command not in codes:
        raise ValueError("Unsupported command")
    if command in ("tone", "silence"):
        if not 250 <= duration <= 10000:
            raise ValueError("Duration must be 250..10000 ms")
    else:
        duration = 0
    if box[3]:
        raise RuntimeError("Mailbox occupied; do not overwrite or retry")
    encoded = 0xa5000000 | (codes[command] << 16) | duration
    address = addresses["UsbAudioDebugMailbox"] + 12
    monitor.command(f"setmem addr=0x{address:x} data={encoded:08X}")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        current = monitor.words(addresses["UsbAudioDebugMailbox"], 16)
        if current[4] != box[4]:
            if current[4] != ((box[4] + 1) & 0xffffffff):
                raise RuntimeError("Another client used the mailbox; result ambiguous")
            result = status(monitor, addresses)
            if current[5] != 2:
                raise RuntimeError("Command rejected: " + json.dumps(result))
            return result
        time.sleep(0.05)
    raise RuntimeError("Acknowledgement timeout; outcome unknown. Do not retry automatically")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("command", choices=("status", "results", "controls", "tone", "silence", "stop", "trace", "watch"))
    parser.add_argument("--module", default="usb_audio360.xex")
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parents[1] / "bin/debug/usb_audio360.json")
    parser.add_argument("--port", type=int, default=730)
    parser.add_argument("--duration", type=int, default=2000)
    parser.add_argument("--row", type=int, default=0)
    parser.add_argument("--interval", type=float, default=1.0)
    args = parser.parse_args()
    monitor = None
    try:
        manifest = json.loads(args.manifest.read_text())
        monitor = Monitor(args.host, args.port)
        if args.command == "watch":
            watch(monitor, manifest, args.module, args.interval)
        else:
            print(json.dumps(execute(monitor, manifest, args.module, args.command, args.duration, args.row), indent=2))
    except KeyboardInterrupt:
        print()
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        parser.exit(1, str(error) + "\n")
    finally:
        if monitor:
            monitor.close()


if __name__ == "__main__":
    main()
