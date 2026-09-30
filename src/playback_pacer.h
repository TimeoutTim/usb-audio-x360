// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_PLAYBACK_PACER_H
#define USB_AUDIO360_PLAYBACK_PACER_H
namespace uac {
// Continuous full-speed playback pacing. Explicit feedback is normalized using
// the same bounded shift detection used by snd-usb-audio, then retained until
// a newer valid value arrives. The USB DPC domain is the sole runtime owner.
class PlaybackPacer {
 public:
  PlaybackPacer() : rate_(48u << 16), phase_(0), maximum_(0), async_(false),
                    begun_(false) {}
  bool Begin(bool asynchronous, unsigned maximum) {
    if (maximum < 48 || maximum > 49) return false;
    rate_ = 48u << 16; phase_ = 0; maximum_ = maximum;
    async_ = asynchronous; begun_ = true; return true;
  }
  bool Update(unsigned long raw) {
    if (!begun_ || !async_ || !raw) return false;
    const unsigned long nominal = 48UL << 16;
    while (raw < nominal - nominal / 4) raw <<= 1;
    while (raw > nominal + nominal / 2) raw >>= 1;
    if (raw < (47UL << 16) || raw > ((unsigned long)maximum_ << 16))
      return false;
    rate_ = (unsigned)raw; return true;
  }
  bool Next(unsigned* frames) {
    if (!begun_ || !frames) return false;
    if (!async_) { *frames = 48; return true; }
    unsigned total = phase_ + rate_;
    unsigned count = total >> 16;
    if (count > maximum_) return false;
    phase_ = total & 0xffff;
    *frames = count; return true;
  }
  unsigned rate() const { return rate_; }
 private:
  unsigned rate_, phase_, maximum_;
  bool async_, begun_;
};
}
#endif
