#include <assert.h>
#include <stdio.h>
#include "mic_gain.h"

int main() {
  assert(usb_mic::ApplyGain(1234, 0) == 0);
  assert(usb_mic::ApplyGain(1234, 100) == 1234);
  assert(usb_mic::ApplyGain(10000, 150) == 15000);
  assert(usb_mic::ApplyGain(20000, 200) == 32767);
  assert(usb_mic::ApplyGain(-20000, 200) == -32768);
  for (long sample = -32768; sample <= 32767; ++sample) {
    assert(usb_mic::ApplyGain((short)sample, 100) == sample);
    assert(usb_mic::ApplyGain((short)sample, 0) == 0);
    for (long gain = 10; gain <= 200; gain += 10) {
      long expected = sample * gain / 100;
      if (expected > 32767) expected = 32767;
      if (expected < -32768) expected = -32768;
      assert(usb_mic::ApplyGain((short)sample, gain) == expected);
    }
  }
  puts("microphone gain tests passed");
}
