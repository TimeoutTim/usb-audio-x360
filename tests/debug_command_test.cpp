// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug_command.h"
#include <assert.h>
#include <stdio.h>
int main() {
  using namespace usb_debug;
  assert(Decode(0xa5041388).command == kTone && Decode(0xa5041388).duration == 5000);
  assert(Decode(0xa50503e8).command == kSilence);
  assert(Decode(0xa5030000).command == kControls);
  assert(Decode(0xa5060000).command == kStop);
  assert(Decode(0xa5040000).command == kInvalid);
  assert(Decode(0xa5060001).command == kInvalid);
  assert(Decode(0xa5070000).command == kInvalid);
  assert(Decode(0x00041388).command == kInvalid);
  assert(Parse("uaudio!status").command == kStatus);
  assert(Parse("uaudio!results").command == kResults);
  assert(Parse("uaudio!controls").command == kControls);
  assert(Parse("uaudio!stop").command == kStop);
  assert(Parse("uaudio!tone duration=250").duration == 250);
  assert(Parse("uaudio!silence duration=10000").command == kSilence);
  assert(Parse("uaudio!trace row=31").duration == 31);
  assert(Parse("uaudio!trace row=0").command == kTrace);
  const char* bad[] = {0,"", "other!status", "uaudio!", "uaudio!reset",
    "uaudio!tone", "uaudio!tone duration=", "uaudio!tone duration=-1",
    "uaudio!tone duration=249", "uaudio!tone duration=10001",
    "uaudio!tone duration=4294967296", "uaudio!tone duration=5000 junk",
    "uaudio!stop\r\nreset", "uaudio!trace row=32", "uaudio!trace row=-1"};
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    assert(Parse(bad[i]).command == kInvalid);
  puts("Debug command parser tests passed");
}
