// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_CAPTURE_PROFILE_H
#define USB_AUDIO360_CAPTURE_PROFILE_H

#include "uac_descriptors.h"

namespace uac {

inline bool SupportedFullSpeedCapture(const Format& f) {
  const unsigned frame_bytes = f.channels * f.sample_bytes;
  return f.direction == kCapture && f.endpoint_layout_supported &&
      f.topology_valid && f.channels >= 1 && f.channels <= 2 &&
      f.sample_bytes == 2 && f.valid_bits == 16 &&
      (f.version != 1 || (f.rate_48000_known && f.supports_48000)) &&
      f.data.interval == 1 && f.data.transactions == 1 &&
      f.data.max_packet_bytes >= 48 * frame_bytes &&
      f.data.max_packet_bytes <= 1023 && !f.feedback.address;
}

inline bool SelectFullSpeedCapture(const Format* formats, size_t count,
                                   size_t* selected) {
  if ((!formats && count) || !selected) return false;
  for (size_t i = 0; i < count; ++i) {
    if (SupportedFullSpeedCapture(formats[i])) {
      *selected = i;
      return true;
    }
  }
  return false;
}

}  // namespace uac
#endif
