// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_UAC_CLOCK_H
#define USB_AUDIO360_UAC_CLOCK_H
#include "uac_descriptors.h"

namespace uac {
// All calls must be serialized by the owner. Requests are VALUE snapshots:
// the transport must own its own buffers until USB completion/draining.
struct ClockRequest {
  unsigned token;
  Byte request_type;
  Byte request;
  unsigned short value, index, length;  // Host endian; transport serializes.
  Byte output[4];
};

class ClockSetup {
 public:
  enum State { kIdle, kPending, kReady, kFailed, kCancelled };
  enum Error { kNone, kDescriptor, kCycle, kUnsupported, kTransfer,
               kResponseLength, kResponseValue, kRateUnavailable,
               kRateReadback, kInvalidClock, kTimeout, kTokenExhausted };
  ClockSetup();
  // UAC2 only. Configuration is borrowed and must remain immutable/alive until
  // Ready, Failed or Cancelled. The AS interface must already be inactive.
  // Begin refuses an active setup. Completion does NOT enable an alternate.
  bool Begin(const Byte* configuration, size_t length, Byte control_interface,
             Byte clock_entity, unsigned now_ms);
  bool Pending(ClockRequest* request) const;
  // actual_length includes OUT data bytes transferred (4 for SET_CUR).
  // Stale/duplicate tokens are ignored, including across Cancel/Begin.
  void Complete(unsigned token, bool success, const Byte* data,
                size_t actual_length, unsigned now_ms);
  void Tick(unsigned now_ms);
  void Cancel();
  State state() const { return state_; }
  Error error() const { return error_; }
  Byte source() const { return state_ == kReady ? entity_.id : 0; }

 private:
  enum Step { kSelector, kNumerator, kDenominator, kCurrentRate,
              kRangeCount, kRanges, kSetRate, kVerifyRate, kValidity };
  void Visit(Byte id, unsigned now);
  void Issue(Step step, Byte selector, unsigned length, unsigned now,
             bool write = false, bool range = false);
  void ValidateClock(unsigned now);
  void Fail(Error error);
  State state_;
  Error error_;
  Step step_;
  ClockRequest request_;
  unsigned serial_, started_, issued_;
  const Byte* configuration_;
  size_t length_;
  Byte control_;
  Byte visited_[256];
  ClockEntity entity_;
  unsigned numerator_, range_count_;
  ClockSetup(const ClockSetup&);
  ClockSetup& operator=(const ClockSetup&);
};
}  // namespace uac
#endif
