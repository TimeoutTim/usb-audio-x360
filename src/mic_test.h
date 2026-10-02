// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_MIC_TEST_H
#define USB_AUDIO360_MIC_TEST_H
#include "mic_gain.h"

namespace usb_mic {
// 16 kHz capture -> 48 kHz monitor. Caller serializes the ring; never blocks.
// A 20 ms prefill absorbs callback jitter; bounded trimming prevents latency
// growth if the capture and render clocks differ. Underruns emit silence.
class LiveMonitor {
 public:
  enum { kCapacity = 1024, kPrime = 320, kHighWater = 768 };
  LiveMonitor() : active_(false), primed_(false), read_(0), write_(0), fraction_(0) {}
  void Start() { Stop(); active_ = true; }
  void Stop() { active_ = primed_ = false; read_ = write_ = fraction_ = 0; }
  int phase() const { return active_ ? 1 : 0; }
  unsigned queued() const { return write_ - read_; }
  void Record(const short* input, unsigned count, long gain) {
    if (!active_) return;
    for (unsigned i = 0; i < count; ++i) {
      if (queued() == kCapacity) { read_ = write_ - kPrime; fraction_ = 0; }
      samples_[write_++ & (kCapacity - 1)] = ApplyGain(input[i], gain);
    }
  }
  bool Playback(short* output, unsigned count) {
    if (!active_) return false;
    for (unsigned i = 0; i < count; ++i) output[i] = 0;
    if (queued() > kHighWater) { read_ = write_ - kPrime; fraction_ = 0; }
    if (!primed_ && queued() < kPrime) return true;
    primed_ = true;
    for (unsigned i = 0; i < count; ++i) {
      if (queued() < 2) { primed_ = false; fraction_ = 0; break; }
      int current = samples_[read_ & (kCapacity - 1)];
      int next = samples_[(read_ + 1) & (kCapacity - 1)];
      // All arithmetic must remain signed on 32-bit PowerPC.
      output[i] = (short)(current + (next - current) * (int)fraction_ / 3);
      if (++fraction_ == 3) { fraction_ = 0; ++read_; }
    }
    return true;
  }
 private:
  bool active_, primed_;
  unsigned read_, write_, fraction_;
  short samples_[kCapacity];
};

struct PeakReading { unsigned peak; bool clipped; };
inline PeakReading MeasurePeak(const short* input, unsigned count, long gain) {
  PeakReading reading = {0, false};
  for (unsigned i = 0; i < count; ++i) {
    long scaled = (long)input[i] * gain / 100;
    if (input[i] == 32767 || input[i] == -32768 ||
        scaled >= 32767 || scaled <= -32768) reading.clipped = true;
    unsigned magnitude = (unsigned)(scaled < 0 ? -scaled : scaled);
    if (magnitude > 32768) magnitude = 32768;
    if (magnitude > reading.peak) reading.peak = magnitude;
  }
  return reading;
}

}  // namespace usb_mic
#endif
