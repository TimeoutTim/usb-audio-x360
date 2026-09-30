// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_TEST_TONE_H
#define USB_AUDIO360_TEST_TONE_H
namespace uac {
// Signed 16-bit scale: peak 900 (~-31 dBFS). 100 ms silence, 1 s tone,
// then silence. Five-ms amplitude ramps avoid abrupt edges. 48 kHz only.
inline int TestToneSample(unsigned frame) {
  static const short wave[48] = {
    0,117,233,344,450,548,636,714,779,831,869,892,
    900,892,869,831,779,714,636,548,450,344,233,117,
    0,-117,-233,-344,-450,-548,-636,-714,-779,-831,-869,-892,
    -900,-892,-869,-831,-779,-714,-636,-548,-450,-344,-233,-117
  };
  if (frame < 4800 || frame >= 52800) return 0;
  unsigned position = frame - 4800;
  unsigned gain = 240;
  if (position < gain) gain = position;
  unsigned remaining = 47999 - position;
  if (remaining < gain) gain = remaining;
  return wave[position % 48] * (int)gain / 240;
}
}
#endif
