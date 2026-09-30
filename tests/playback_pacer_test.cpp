// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback_pacer.h"
#include <assert.h>
#include <stdio.h>
int main() {
  uac::PlaybackPacer fixed;
  unsigned frames = 0, total = 0;
  assert(!fixed.Next(&frames));
  assert(fixed.Begin(false, 48));
  assert(!fixed.Update(48u << 16));
  for (unsigned i = 0; i < 1000; ++i) {
    assert(fixed.Next(&frames) && frames == 48); total += frames;
  }
  assert(total == 48000);

  uac::PlaybackPacer async;
  assert(async.Begin(true, 49));
  // Full-speed Q10.14, 48.5 frames/ms, normalized to internal Q16.16.
  assert(async.Update((48u << 14) + (1u << 13)));
  total = 0;
  for (unsigned i = 0; i < 1000; ++i) {
    assert(async.Next(&frames));
    assert(frames == 48 || frames == 49); total += frames;
  }
  assert(total == 48500 && async.rate() == (48u << 16) + (1u << 15));
  // Native Q16.16 is accepted; malformed values do not replace last good rate.
  assert(async.Update(48u << 16));
  assert(!async.Update(0));
  assert(!async.Update(60u << 16));
  assert(async.rate() == 48u << 16);
  assert(!async.Begin(true, 47));
  assert(!async.Begin(true, 50));
  puts("Continuous playback pacing tests passed");
}
