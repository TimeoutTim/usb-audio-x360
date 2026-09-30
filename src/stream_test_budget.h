// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_STREAM_TEST_BUDGET_H
#define USB_AUDIO360_STREAM_TEST_BUDGET_H
namespace usb_transport {
// Single execution-domain owned budget; unsigned subtraction handles timer wrap.
class StreamTestBudget {
 public:
  StreamTestBudget(unsigned duration = 250, unsigned limit = 128)
      : start_(0), duration_(duration), limit_(limit), active_(false) {
    issued_[0] = issued_[1] = 0;
  }
  void Begin(unsigned now) {
    start_ = now; issued_[0] = issued_[1] = 0; active_ = true;
  }
  void Stop() { active_ = false; }
  bool Take(unsigned direction, unsigned now) {
    if (!active_ || direction > 1 || now - start_ >= duration_ ||
        issued_[direction] >= limit_) return false;
    ++issued_[direction]; return true;
  }
  bool Finished(unsigned now) const {
    return active_ && (now - start_ >= duration_ ||
        (issued_[0] == limit_ && issued_[1] == limit_));
  }
 private:
  unsigned start_, duration_, limit_, issued_[2];
  bool active_;
};
}
#endif
