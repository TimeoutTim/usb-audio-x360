// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_SURROUND_DOWNMIX_H
#define USB_AUDIO360_SURROUND_DOWNMIX_H

namespace usb_mix {

// MEC render capture gives us a planar float buffer laid out as
// samples[channel * kRenderStride + frame] for six channels in WAVE/SMPTE
// order: L, R, C, LFE, Ls, Rs. Fold 5.1 content into the stereo pair the
// USB headset actually plays using the standard ITU-R BS.775 coefficients.
// Center and surrounds go into both ears at -3 dB (1/sqrt(2)) so perceived
// loudness is preserved; the LFE channel is discarded (kLfeGain = 0) as
// consumer headsets reproduce it poorly.
const int kRenderChannels = 6;
const int kRenderStride = 256;
const float kCenterGain = 0.70710678f;   // 1/sqrt(2), -3 dB
const float kSurroundGain = 0.70710678f;  // 1/sqrt(2), -3 dB
const float kLfeGain = 0.0f;

// Downmix one frame of the planar 6-channel buffer into stereo. Identity
// for pure stereo content (only channels 0 and 1 contribute). The caller
// still applies master volume gain and the existing hard clamp afterwards;
// this helper does not clip.
inline void FoldFrame(const float* samples, unsigned frame, float* left,
                      float* right) {
  float l = samples[0 * kRenderStride + frame];
  float r = samples[1 * kRenderStride + frame];
  float c = samples[2 * kRenderStride + frame] * kCenterGain;
  // float lfe = samples[3 * kRenderStride + frame] * kLfeGain;  // discarded
  float ls = samples[4 * kRenderStride + frame] * kSurroundGain;
  float rs = samples[5 * kRenderStride + frame] * kSurroundGain;
  *left = l + c + ls;
  *right = r + c + rs;
}

// Measure the peak absolute magnitude of one planar channel, expressed as
// an unsigned 16-bit scale factor where 65535 corresponds to full scale
// (|sample| >= 1.0). Used by the debug API's render-peak diagnostics so a
// tester can verify channel routing on hardware before trusting the fold.
inline unsigned ChannelPeak16(const float* plane, unsigned count) {
  float peak = 0.0f;
  for (unsigned i = 0; i < count; ++i) {
    float value = plane[i];
    if (value < 0.0f) value = -value;
    if (value > peak) peak = value;
  }
  if (peak >= 1.0f) return 65535;
  return (unsigned)(peak * 65535.0f + 0.5f);
}

}  // namespace usb_mix
#endif
