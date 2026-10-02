// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include "mic_test.h"
#include "usb_strings.h"

static short Wave(unsigned index) { return (short)(30000 - (int)(index % 151) * 400); }
static void TestLiveMonitor() {
  usb_mic::LiveMonitor monitor;
  short input[320], output[256];
  assert(!monitor.Playback(output, 256));
  monitor.Start();
  assert(monitor.Playback(output, 256));
  for (unsigned i = 0; i < 256; ++i) assert(output[i] == 0);
  unsigned supplied = 0, played = 0;
  for (unsigned i = 0; i < 320; ++i) input[i] = Wave(supplied++);
  monitor.Record(input, 320, 100);
  for (unsigned batch = 0; batch < 2000; ++batch) {
    assert(monitor.Playback(output, 256));
    for (unsigned i = 0; i < 256; ++i) {
      unsigned frame = played + i;
      int current = Wave(frame / 3), next = Wave(frame / 3 + 1);
      assert(output[i] == current + (next - current) * (int)(frame % 3) / 3);
    }
    unsigned consumed = (played + 256) / 3 - played / 3;
    played += 256;
    for (unsigned i = 0; i < consumed; ++i) input[i] = Wave(supplied++);
    monitor.Record(input, consumed, 100);
    assert(monitor.queued() == 320);
  }
  for (unsigned batch = 0; batch < 20; ++batch) monitor.Playback(output, 256);
  for (unsigned i = 0; i < 256; ++i) assert(output[i] == 0);
  for (unsigned batch = 0; batch < 20; ++batch) monitor.Record(input, 320, 200);
  assert(monitor.queued() <= usb_mic::LiveMonitor::kCapacity);
  monitor.Playback(output, 256);
  assert(monitor.queued() < usb_mic::LiveMonitor::kHighWater);
  monitor.Stop();
  assert(!monitor.Playback(output, 256));
  monitor.Start();
  monitor.Playback(output, 256);
  for (unsigned i = 0; i < 256; ++i) assert(output[i] == 0);
  short levels[] = {10, -20000, 15000};
  usb_mic::PeakReading p = usb_mic::MeasurePeak(levels, 3, 100);
  assert(p.peak == 20000 && !p.clipped);
  p = usb_mic::MeasurePeak(levels, 3, 200);
  assert(p.peak == 32768 && p.clipped);
  p = usb_mic::MeasurePeak(levels, 3, 0);
  assert(p.peak == 0 && !p.clipped);
  levels[0] = -32768;
  assert(usb_mic::MeasurePeak(levels, 3, 50).clipped);
}

static void TestUsbStrings() {
  const unsigned char languages[] = {6, 3, 0x11, 4, 9, 4};
  assert(usb_strings::Language(languages, sizeof(languages)) == 0x409);
  assert(usb_strings::Language(languages, 4) == 0);
  const unsigned char name[] = {8, 3, 'A', 0, '\n', 0, 'B', 0};
  unsigned short output[5];
  assert(usb_strings::Decode(name, sizeof(name), output, 5));
  assert(output[0] == 'A' && output[1] == ' ' && output[2] == 'B' && !output[3]);
  assert(usb_strings::Decode(name, sizeof(name), output, 2));
  assert(output[0] == 'A' && !output[1]);
  assert(!usb_strings::Decode(name, 5, output, 5));
  assert(!output[0]);
  const unsigned char malformed[] = {3, 3, 'A'};
  assert(!usb_strings::Decode(malformed, 3, output, 5));
}

int main() {
  TestLiveMonitor();
  TestUsbStrings();
  puts("Live microphone monitor, peak, and USB string tests passed");
}
