"""Offline tests: mismatched identity/ABI cannot cause mailbox writes."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("client", Path(__file__).resolve().parents[1] / "tools/usb_audio_debug.py")
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
NAMES = ("UsbAudioDebugMailbox", "UsbAudioDiagnostic", "UsbAudioControlDiagnostic",
         "UsbAudioToneDiagnostic", "UsbAudioActivationDiagnostic", "UsbAudioSetupTrace")
MANIFEST = dict(version=1, marker=0x55414342, checksum=1, timestamp=2,
                offsets=dict(zip(NAMES, (0x100, 0x200, 0x300, 0x340, 0x380, 0x400))))


class Fake:
    def __init__(self):
        self.writes = []
        self.memory = {}
        for name, count in zip(NAMES, (16, 64, 16, 16, 16, 256)):
            self.memory[0x80000000 + MANIFEST["offsets"][name]] = [0] * count
        self.box = self.memory[0x80000100]
        self.box[:3] = [0x55414d42, 1, 0x55414342]
        self.box[8:13] = [0x80000000 + MANIFEST["offsets"][n] for n in NAMES[1:]]
        self.memory[0x80000200][63] = MANIFEST["marker"]

    def words(self, address, count):
        for start, words in self.memory.items():
            index = (address - start) // 4
            if 0 <= index and index + count <= len(words):
                return tuple(words[index:index + count])
        raise AssertionError("unexpected read")

    def command(self, text):
        if text == "modules":
            return ['name="usb_audio360.xex" base=0x80000000 size=0x1000 check=0x1 timestamp=0x2']
        assert text.startswith("setmem addr=0x8000010c data=")
        self.writes.append(text)
        self.box[4] += 1
        self.box[5] = 2
        return ["200- set 4 bytes"]


class Tests(unittest.TestCase):
    def test_read_only(self):
        fake = Fake()
        client.execute(fake, MANIFEST, "usb_audio360.xex", "status")
        self.assertFalse(fake.writes)

    def test_command(self):
        fake = Fake()
        self.assertEqual(client.execute(fake, MANIFEST, "usb_audio360.xex", "tone", 5000)["ack"], 1)
        self.assertEqual(fake.writes, ["setmem addr=0x8000010c data=A5041388"])

    def test_reject_mismatch(self):
        for key, value in (("checksum", 3), ("timestamp", 3), ("marker", 0), ("version", 2)):
            fake = Fake()
            manifest = copy.deepcopy(MANIFEST)
            manifest[key] = value
            with self.assertRaises(RuntimeError):
                client.execute(fake, manifest, "usb_audio360.xex", "tone")
            self.assertFalse(fake.writes)

    def test_reject_mailbox(self):
        for index in (0, 1, 2, 3, 8):
            fake = Fake()
            fake.box[index] += 1
            with self.assertRaises(RuntimeError):
                client.execute(fake, MANIFEST, "usb_audio360.xex", "silence")
            self.assertFalse(fake.writes)

    def test_bounds(self):
        fake = Fake()
        for duration in (0, 249, 10001):
            with self.assertRaises(ValueError):
                client.execute(fake, MANIFEST, "usb_audio360.xex", "tone", duration)
        self.assertFalse(fake.writes)


if __name__ == "__main__":
    unittest.main()
