// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_tone.h"
#include <assert.h>
#include <stdio.h>
int main() {
  bool positive = false, negative = false;
  for (unsigned frame = 0; frame < 96000; ++frame) {
    int sample = uac::TestToneSample(frame);
    assert(sample >= -900 && sample <= 900);
    if (frame < 4800 || frame >= 52800) assert(sample == 0);
    if (sample > 0) positive = true;
    if (sample < 0) negative = true;
  }
  assert(positive && negative);
  assert(uac::TestToneSample(4800) == 0);
  assert(uac::TestToneSample(52799) == 0);
  assert(uac::TestToneSample(0xffffffffu) == 0);
  for (unsigned frame = 5040; frame < 52000; ++frame)
    assert(uac::TestToneSample(frame) == uac::TestToneSample(frame + 48));
  puts("Bounded low-level test tone tests passed");
}
