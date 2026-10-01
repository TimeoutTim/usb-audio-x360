// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_MIC_PCM_H
#define USB_AUDIO360_MIC_PCM_H

#include <stddef.h>

namespace usb_mic {

// Stateful 48 kHz -> 16 kHz decimator. A three-sample box filter prevents the
// worst aliasing of a sample-dropper while keeping the capture path bounded.
class Decimator48To16 {
 public:
  Decimator48To16() : count_(0), sum_(0) {}

  void Reset() { count_ = 0; sum_ = 0; }

  size_t Convert(const short* input, size_t count, short* output,
                 size_t capacity) {
    if ((!input && count) || (!output && capacity)) return 0;
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
      sum_ += input[i];
      if (++count_ != 3) continue;
      if (written == capacity) {
        // Drop a complete output sample, never retain an unbounded backlog.
        count_ = 0; sum_ = 0;
        continue;
      }
      output[written++] = static_cast<short>(sum_ / 3);
      count_ = 0; sum_ = 0;
    }
    return written;
  }

 private:
  unsigned count_;
  long sum_;
};

}  // namespace usb_mic
#endif
