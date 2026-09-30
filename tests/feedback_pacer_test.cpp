// SPDX-License-Identifier: GPL-3.0-or-later
#include "feedback_pacer.h"
#include <assert.h>
#include <stdio.h>
int main() {
  uac::FeedbackPacer p;
  unsigned frames = 123;
  assert(!p.Next(0, &frames));
  assert(!p.Begin(0, 47)); assert(!p.Begin(0, 50));
  assert(p.Begin(0, 49));
  assert(p.Next(0, &frames) && frames == 48);
  assert(p.Update(0xc0000, 3, 0) && p.rate() == (48u << 16));
  assert(p.Update(0x300000, 4, 0) && p.valid());
  assert(!p.Update(0, 3, 1));
  assert(!p.Update(0xffffffffUL, 4, 1));
  assert(!p.Update(0x1000000UL, 3, 1));
  assert(!p.Update(0x300000, 2, 1));
  assert(!p.Update(0xc0000, 4, 1)); // No guessing a different scale.
  assert(p.rate() == (48u << 16));
  assert(p.Next(999, &frames)); assert(!p.Next(1000, &frames));
  // Phase is preserved across updates and across four-packet batch boundaries.
  assert(p.Begin(0, 49));
  unsigned total = 0;
  for (unsigned i = 0; i < 10000; ++i) {
    assert(p.Update((48u << 16) + 32768, 4, i));
    assert(p.Next(i, &frames)); assert(frames == 48 || frames == 49);
    total += frames;
  }
  assert(total == 485000);
  assert(p.Begin(0, 49)); total = 0;
  for (unsigned i = 0; i < 10000; ++i) {
    assert(p.Update((47u << 16) + 32768, 4, i));
    assert(p.Next(i, &frames)); assert(frames == 47 || frames == 48);
    total += frames;
  }
  assert(total == 475000);
  assert(p.Begin(0xfffffff0u, 48));
  assert(!p.Update((48u << 16) + 1, 4, 0));
  assert(p.Next(0x3d7, &frames)); // 999 ms across wrap
  assert(!p.Next(0x3d8, &frames));
  assert(p.Update(48u << 16, 4, 0x3d8));
  assert(p.Next(0x3d8, &frames) && frames == 48);
  assert(!p.Next(0x3d8, 0));
  puts("Feedback pacing tests passed");
}
