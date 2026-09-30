#!/usr/bin/env python3
"""Generate build-specific mailbox offsets and module identity (build artifact)."""
import json
import re
import struct
import sys
from pathlib import Path


def generate(map_path, pe_path):
    data = Path(pe_path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    text = Path(map_path).read_text()
    names = ("UsbAudioDebugMailbox", "UsbAudioDiagnostic", "UsbAudioControlDiagnostic",
             "UsbAudioToneDiagnostic", "UsbAudioActivationDiagnostic", "UsbAudioSetupTrace")
    offsets = {}
    for name in names:
        match = re.search(r"\s" + name + r"\s+([0-9a-fA-F]{8})\s", text)
        if not match:
            raise ValueError("Missing map symbol: " + name)
        offsets[name] = int(match[1], 16) - base
    return {"version": 1, "marker": 0x55414342, "offsets": offsets,
            "checksum": struct.unpack_from("<I", data, pe + 24 + 64)[0],
            "timestamp": struct.unpack_from("<I", data, pe + 8)[0]}


if __name__ == "__main__":
    Path(sys.argv[3]).write_text(json.dumps(generate(sys.argv[1], sys.argv[2]), indent=2) + "\n")
