// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_PLAYBACK_PROFILE_H
#define USB_AUDIO360_PLAYBACK_PROFILE_H

#include "uac_descriptors.h"

namespace uac {

// The endpoint's wMaxPacketSize is capacity, not the size of every packet the
// selected sample rate will send. Full-speed isochronous endpoints may advertise
// up to 1023 bytes, while this driver generates at most 49 stereo frames.
inline bool SupportedFullSpeedPlayback(const Format& f) {
  const unsigned frames = f.sync == kAsynchronous ? 49 : 48;
  const unsigned generated = frames * f.channels * f.sample_bytes;
  if (f.data.interval != 1 || f.data.transactions != 1 ||
      f.data.max_packet_bytes < generated || f.data.max_packet_bytes > 1023)
    return false;
  return !f.feedback.address ||
      (f.feedback.interval == 1 && f.feedback.transactions == 1);
}

// Prefer playback on the interface that caused the claim. Some composite UAC
// devices enumerate a capture-only AudioStreaming interface first; in that
// case, fall back to a descriptor-associated playback interface on the same
// configured device instead of treating the capture interface as output.
inline bool SelectFullSpeedPlayback(const Format* formats, size_t count,
                                    Byte preferred_interface, size_t* selected) {
  if ((!formats && count) || !selected) return false;
  for (unsigned pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < count; ++i) {
      const bool preferred = formats[i].interface_number == preferred_interface;
      if (preferred != (pass == 0) || !SupportedFullSpeedPlayback(formats[i]))
        continue;
      *selected = i;
      return true;
    }
  }
  return false;
}

}  // namespace uac
#endif
