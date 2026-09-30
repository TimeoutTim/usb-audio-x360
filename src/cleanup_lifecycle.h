// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_CLEANUP_LIFECYCLE_H_
#define USB_AUDIO360_CLEANUP_LIFECYCLE_H_

namespace usb_transport {

class CleanupLifecycle {
 public:
  CleanupLifecycle() : expected_(0), completed_(0), begun_(false),
                       finalized_(false) {}

  bool Begin(unsigned request_count) {
    if (begun_ || request_count > 3) return false;
    expected_ = request_count ? ((1u << request_count) - 1) : 0;
    completed_ = 0;
    begun_ = true;
    finalized_ = false;
    return true;
  }

  bool Complete(unsigned index) {
    if (index >= 3) return false;
    const unsigned bit = 1u << index;
    if (!begun_ || !(expected_ & bit) || (completed_ & bit))
      return false;
    completed_ |= bit;
    return true;
  }

  bool Ready(unsigned transfer_count) const {
    return begun_ && !finalized_ && !transfer_count &&
           completed_ == expected_;
  }

  bool Finalize(unsigned transfer_count) {
    if (!Ready(transfer_count)) return false;
    finalized_ = true;
    return true;
  }

  bool Rearm() {
    if (!begun_ || !finalized_) return false;
    expected_ = completed_ = 0;
    begun_ = finalized_ = false;
    return true;
  }

  unsigned pending() const {
    unsigned mask = expected_ & ~completed_;
    unsigned result = 0;
    for (; mask; mask >>= 1) result += mask & 1;
    return result;
  }
  bool begun() const { return begun_; }
  bool finalized() const { return finalized_; }

 private:
  unsigned expected_;
  unsigned completed_;
  bool begun_;
  bool finalized_;
};

}  // namespace usb_transport

#endif  // USB_AUDIO360_CLEANUP_LIFECYCLE_H_
