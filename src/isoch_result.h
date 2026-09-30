// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_ISOCH_RESULT_H
#define USB_AUDIO360_ISOCH_RESULT_H
namespace usb_transport {
// Retail 17559 OHCI table entry 9. Only isochronous IN permits short reads.
// Keep the raw status separately; this does not apply to control or OUT.
inline bool IsochInSucceeded(unsigned long status, unsigned actual,
                             unsigned requested) {
  return actual <= requested &&
         (status == 0 || status == 0xc0050003UL);
}

// Decode only complete feedback payloads. Empty/partial packets are not rates.
// No format normalization or playback timing changes occur here.
inline bool ReadFeedbackPacket(unsigned long status, unsigned actual,
                               unsigned requested, const unsigned char* data,
                               unsigned long* raw) {
  if (!data || !raw || (requested != 3 && requested != 4) ||
      (actual != 3 && actual != 4) ||
      !IsochInSucceeded(status, actual, requested)) return false;
  unsigned long value = (unsigned long)data[0] |
      ((unsigned long)data[1] << 8) | ((unsigned long)data[2] << 16);
  if (actual == 4) value |= (unsigned long)data[3] << 24;
  *raw = value;
  return true;
}
}
#endif
