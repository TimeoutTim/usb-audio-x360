// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_TRANSFER_OWNERSHIP_H
#define USB_AUDIO360_TRANSFER_OWNERSHIP_H
namespace usb_transport {
// Serialized by the USB DPC domain, not a replacement for that serialization.
// Stop prevents submissions but never releases submitted storage.
class Ownership {
 public:
  Ownership() : generation_(1), stopped_(false) {
    for (unsigned i = 0; i < 8; ++i) { keys_[i] = 0; pending_[i] = false; }
  }
  unsigned generation() const { return generation_; }
  bool Submit(const void* key, unsigned generation) {
    if (!key || stopped_ || generation != generation_) return false;
    for (unsigned i = 0; i < 8; ++i) {
      if (keys_[i] == key) {
        if (pending_[i]) return false;
        pending_[i] = true; return true;
      }
    }
    for (unsigned i = 0; i < 8; ++i) {
      if (!keys_[i]) { keys_[i] = key; pending_[i] = true; return true; }
    }
    return false;
  }
  bool Complete(const void* key) {
    for (unsigned i = 0; i < 8; ++i) {
      if (keys_[i] == key && pending_[i]) { pending_[i] = false; return true; }
    }
    return false;
  }
  void Stop() { if (!stopped_) { stopped_ = true; ++generation_; } }
  bool Rearm() {
    if (!stopped_ || pending()) return false;
    for (unsigned i = 0; i < 8; ++i) {
      keys_[i] = 0;
      pending_[i] = false;
    }
    stopped_ = false;
    return true;
  }
  bool stopped() const { return stopped_; }
  unsigned pending() const {
    unsigned n = 0;
    for (unsigned i = 0; i < 8; ++i) if (pending_[i]) ++n;
    return n;
  }
 private:
  unsigned generation_;
  bool stopped_;
  const void* keys_[8];
  bool pending_[8];
};
}
#endif
