// SPDX-License-Identifier: GPL-3.0-or-later
#include "mic_pcm.h"
#include <assert.h>
#include <stdio.h>

int main() {
  usb_mic::Decimator48To16 decimator;
  short input[] = {3, 6, 9, -3, -6, -9, 30};
  short output[4] = {};
  assert(decimator.Convert(input, 7, output, 4) == 2);
  assert(output[0] == 6 && output[1] == -6);
  short tail[] = {60, 90};
  assert(decimator.Convert(tail, 2, output, 4) == 1);
  assert(output[0] == 60);
  decimator.Reset();
  assert(decimator.Convert(input, 6, output, 1) == 1);
  assert(output[0] == 6);
  assert(decimator.Convert(0, 1, output, 1) == 0);
  puts("microphone PCM tests passed");
  return 0;
}
