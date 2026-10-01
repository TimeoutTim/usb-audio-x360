// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_MIC_PACKET_POLICY_H_
#define USB_AUDIO360_MIC_PACKET_POLICY_H_

namespace usb_audio360 {

// XHV2 may retry a capture request immediately. Completing an undersized
// request with padded silence therefore creates an unbounded polling loop.
// Only complete when one whole requested PCM frame is available; USB capture
// then provides the natural real-time pacing.
inline bool MicrophoneFrameReady(long read, long write,
                                 unsigned long requested_samples) {
  return requested_samples != 0 &&
      static_cast<unsigned long>(write - read) >= requested_samples;
}

// A USB input runs continuously even when no title is requesting voice data.
// Treat the next request after an idle interval as a new capture session so
// the caller can discard audio accumulated while no consumer was listening.
// Unsigned subtraction deliberately remains correct across tick wraparound.
inline bool MicrophoneSessionRestarted(unsigned int previous_tick,
                                       unsigned int current_tick,
                                       unsigned int idle_threshold_ms) {
  return previous_tick == 0 ||
      current_tick - previous_tick >= idle_threshold_ms;
}

}  // namespace usb_audio360

#endif  // USB_AUDIO360_MIC_PACKET_POLICY_H_
