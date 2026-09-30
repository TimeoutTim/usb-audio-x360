// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_FEEDBACK_PACER_H
#define USB_AUDIO360_FEEDBACK_PACER_H
namespace uac {
// Full-speed, 48 kHz, 1 ms packets. One owner for feedback and OUT phase.
// Supported encodings: three-byte Q10.14 and four-byte Q16.16 samples/frame.
// Deliberately no arbitrary rescaling of malformed values or device quirks.
class FeedbackPacer {
 public:
  FeedbackPacer() : rate_(48u << 16), phase_(0), last_(0), maximum_(0),
                    begun_(false), valid_(false) {}
  bool Begin(unsigned now, unsigned maximum) {
    if (maximum < 48 || maximum > 49) return false;
    rate_ = 48u << 16; phase_ = 0; last_ = now; maximum_ = maximum;
    begun_ = true; valid_ = false; return true;
  }
  bool Update(unsigned long raw, unsigned bytes, unsigned now) {
    if (!begun_) return false;
    unsigned long rate;
    if (bytes == 3 && raw <= 0xffffffUL) rate = raw << 2;
    else if (bytes == 4 && raw <= 0xffffffffUL) rate = raw;
    else return false;
    if (rate < (47UL << 16) || rate > ((unsigned long)maximum_ << 16))
      return false;
    rate_ = (unsigned)rate; last_ = now; valid_ = true; return true;
  }
  bool Next(unsigned now, unsigned* frames) {
    if (!frames || !begun_ || now - last_ >= 1000) return false;
    unsigned total = phase_ + rate_;
    unsigned count = total >> 16;
    if (count > maximum_) return false;
    phase_ = total & 0xffff;
    *frames = count; return true;
  }
  unsigned rate() const { return rate_; }
  unsigned age(unsigned now) const { return now - last_; }
  bool valid() const { return valid_; }
 private:
  unsigned rate_, phase_, last_, maximum_;
  bool begun_, valid_;
};
}
#endif
