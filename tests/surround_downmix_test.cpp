// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "surround_downmix.h"

static bool Near(float a, float b) { return fabs((double)a - (double)b) < 1e-6; }

int main() {
  using namespace usb_mix;
  float samples[kRenderChannels * kRenderStride];
  float left, right;

  // Stereo passthrough is an exact identity: only channels 0 and 1 contribute.
  for (int i = 0; i < kRenderChannels * kRenderStride; ++i) samples[i] = 0.0f;
  samples[0 * kRenderStride + 0] = 0.25f;  // L
  samples[1 * kRenderStride + 0] = -0.5f;  // R
  FoldFrame(samples, 0, &left, &right);
  assert(Near(left, 0.25f));
  assert(Near(right, -0.5f));

  // Center folds into both ears at -3 dB.
  for (int i = 0; i < kRenderChannels * kRenderStride; ++i) samples[i] = 0.0f;
  samples[2 * kRenderStride + 0] = 1.0f;  // C
  FoldFrame(samples, 0, &left, &right);
  assert(Near(left, kCenterGain));
  assert(Near(right, kCenterGain));

  // Surrounds route to their matching ear only: Ls -> L, Rs -> R.
  for (int i = 0; i < kRenderChannels * kRenderStride; ++i) samples[i] = 0.0f;
  samples[4 * kRenderStride + 0] = 0.8f;  // Ls
  FoldFrame(samples, 0, &left, &right);
  assert(Near(left, 0.8f * kSurroundGain));
  assert(Near(right, 0.0f));

  for (int i = 0; i < kRenderChannels * kRenderStride; ++i) samples[i] = 0.0f;
  samples[5 * kRenderStride + 0] = 0.6f;  // Rs
  FoldFrame(samples, 0, &left, &right);
  assert(Near(left, 0.0f));
  assert(Near(right, 0.6f * kSurroundGain));

  // LFE is discarded entirely.
  for (int i = 0; i < kRenderChannels * kRenderStride; ++i) samples[i] = 0.0f;
  samples[3 * kRenderStride + 0] = 1.0f;  // LFE
  FoldFrame(samples, 0, &left, &right);
  assert(Near(left, 0.0f));
  assert(Near(right, 0.0f));

  // Frame addressing respects the planar stride: last frame of a channel.
  for (int i = 0; i < kRenderChannels * kRenderStride; ++i) samples[i] = 0.0f;
  unsigned last = kRenderStride - 1;
  samples[0 * kRenderStride + last] = 0.3f;  // L, frame 255
  samples[2 * kRenderStride + last] = 0.4f;  // C, frame 255
  FoldFrame(samples, last, &left, &right);
  assert(Near(left, 0.3f + 0.4f * kCenterGain));
  assert(Near(right, 0.4f * kCenterGain));

  // ChannelPeak16: zero, half scale, negative half scale, full scale.
  float plane[kRenderStride];
  for (int i = 0; i < kRenderStride; ++i) plane[i] = 0.0f;
  assert(ChannelPeak16(plane, kRenderStride) == 0);
  plane[7] = 0.5f;
  assert(ChannelPeak16(plane, kRenderStride) == 32768);
  plane[7] = -0.5f;
  assert(ChannelPeak16(plane, kRenderStride) == 32768);
  plane[7] = 1.0f;
  assert(ChannelPeak16(plane, kRenderStride) == 65535);
  plane[7] = -1.0f;
  assert(ChannelPeak16(plane, kRenderStride) == 65535);
  plane[7] = 1.5f;  // over full scale clamps
  assert(ChannelPeak16(plane, kRenderStride) == 65535);
  plane[7] = 0.0f;
  plane[kRenderStride - 1] = -0.25f;  // last frame still scanned
  assert(ChannelPeak16(plane, kRenderStride) == 16384);

  puts("surround downmix tests passed");
}
