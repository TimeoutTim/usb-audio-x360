// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdio.h>

#include "mic_packet_policy.h"

int main() {
  using usb_audio360::MicrophoneFrameReady;
  using usb_audio360::MicrophoneSessionRestarted;
  assert(!MicrophoneFrameReady(0, 0, 320));
  assert(!MicrophoneFrameReady(0, 319, 320));
  assert(MicrophoneFrameReady(0, 320, 320));
  assert(MicrophoneFrameReady(1000, 1320, 320));
  assert(MicrophoneFrameReady(1000, 1640, 320));
  assert(!MicrophoneFrameReady(1000, 1320, 0));
  assert(MicrophoneSessionRestarted(0, 50, 1000));
  assert(!MicrophoneSessionRestarted(100, 1099, 1000));
  assert(MicrophoneSessionRestarted(100, 1100, 1000));
  assert(MicrophoneSessionRestarted(0xfffffff0U, 0x000003e0U, 1000));
  puts("microphone packet pacing tests passed");
  return 0;
}
