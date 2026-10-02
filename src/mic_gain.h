// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_MIC_GAIN_H
#define USB_AUDIO360_MIC_GAIN_H

namespace usb_mic {

inline short ApplyGain(short sample, long percent) {
  long scaled = (long)sample * percent / 100;
  if (scaled > 32767) return 32767;
  if (scaled < -32768) return -32768;
  return (short)scaled;
}

}  // namespace usb_mic
#endif
