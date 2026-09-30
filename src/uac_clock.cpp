// SPDX-License-Identifier: GPL-3.0-or-later
#include "uac_clock.h"
#include <limits.h>
#include <string.h>

namespace uac {
namespace {
typedef char Require32BitUnsigned[UINT_MAX == 0xffffffffu ? 1 : -1];
const unsigned kRate = 48000;
const unsigned kRequestTimeout = 1000;
const unsigned kSetupTimeout = 5000;
const unsigned kMaxRanges = 16;
unsigned Read16(const Byte* p) { return unsigned(p[0]) | (unsigned(p[1]) << 8); }
unsigned Read32(const Byte* p) {
  return Read16(p) | (Read16(p + 2) << 16);
}
}

ClockSetup::ClockSetup() : state_(kIdle), error_(kNone), step_(kCurrentRate),
    serial_(0), started_(0), issued_(0), configuration_(0), length_(0),
    control_(0), numerator_(0), range_count_(0) {
  memset(&request_, 0, sizeof(request_));
  memset(&entity_, 0, sizeof(entity_));
  memset(visited_, 0, sizeof(visited_));
}

bool ClockSetup::Begin(const Byte* configuration, size_t length, Byte control,
                       Byte clock, unsigned now) {
  if (state_ == kPending) return false;
  configuration_ = configuration; length_ = length; control_ = control;
  memset(visited_, 0, sizeof(visited_));
  error_ = kNone; state_ = kPending; started_ = now;
  Visit(clock, now);
  return state_ == kPending;
}

void ClockSetup::Fail(Error error) {
  error_ = error; state_ = kFailed; configuration_ = 0;
}

void ClockSetup::Cancel() {
  state_ = kCancelled; error_ = kNone; configuration_ = 0;
}

bool ClockSetup::Pending(ClockRequest* request) const {
  if (state_ != kPending || !request) return false;
  *request = request_;
  return true;
}

void ClockSetup::Tick(unsigned now) {
  // Unsigned subtraction deliberately handles the millisecond timer wrapping.
  if (state_ == kPending &&
      (now - issued_ >= kRequestTimeout || now - started_ >= kSetupTimeout))
    Fail(kTimeout);
}

void ClockSetup::Issue(Step step, Byte selector, unsigned length, unsigned now,
                       bool write, bool range) {
  if (serial_ == UINT_MAX) { Fail(kTokenExhausted); return; }
  memset(&request_, 0, sizeof(request_));
  request_.token = ++serial_;
  request_.request_type = write ? 0x21 : 0xa1;
  request_.request = range ? 2 : 1;
  request_.value = static_cast<unsigned short>(unsigned(selector) << 8);
  request_.index = static_cast<unsigned short>((unsigned(entity_.id) << 8) | control_);
  request_.length = static_cast<unsigned short>(length);
  if (write) {
    for (unsigned i = 0; i < 4; ++i)
      request_.output[i] = static_cast<Byte>(kRate >> (8 * i));
  }
  step_ = step; issued_ = now;
}

void ClockSetup::Visit(Byte id, unsigned now) {
  if (visited_[id]) { Fail(kCycle); return; }
  visited_[id] = 1;
  if (!FindClock(configuration_, length_, control_, id, &entity_)) {
    Fail(kDescriptor); return;
  }
  if (!ControlReadable(entity_.controls, 1)) { Fail(kUnsupported); return; }
  if (entity_.kind == kClockSelector) {
    Issue(kSelector, 1, 1, now);
  } else if (entity_.kind == kClockMultiplier) {
    if (!ControlReadable(entity_.controls, 2)) { Fail(kUnsupported); return; }
    Issue(kNumerator, 1, 2, now);
  } else {
    Issue(kCurrentRate, 1, 4, now);
  }
}

void ClockSetup::ValidateClock(unsigned now) {
  if (ControlReadable(entity_.controls, 2)) Issue(kValidity, 2, 1, now);
  else if (((entity_.controls >> 2) & 3) == 2) Fail(kUnsupported);
  else { state_ = kReady; configuration_ = 0; }
}

void ClockSetup::Complete(unsigned token, bool success, const Byte* data,
                          size_t length, unsigned now) {
  if (state_ != kPending || token != request_.token) return;
  Tick(now);
  if (state_ != kPending) return;
  if (!success) { Fail(kTransfer); return; }
  if (length != request_.length || ((request_.request_type & 0x80) && !data)) {
    Fail(kResponseLength); return;
  }
  switch (step_) {
    case kSelector:
      if (!data[0] || data[0] > entity_.input_count) Fail(kResponseValue);
      else Visit(entity_.inputs[data[0] - 1], now);
      break;
    case kNumerator:
      numerator_ = Read16(data);
      if (!numerator_) Fail(kResponseValue);
      else Issue(kDenominator, 2, 2, now);
      break;
    case kDenominator:
      if (!Read16(data)) Fail(kResponseValue);
      // Non-unity ratios need source-rate translation, not a guessed 48k SET.
      else if (Read16(data) != numerator_) Fail(kUnsupported);
      else Visit(entity_.inputs[0], now);
      break;
    case kCurrentRate:
      if (Read32(data) == kRate) ValidateClock(now);
      else if (!ControlWritable(entity_.controls, 1)) Fail(kRateUnavailable);
      else Issue(kRangeCount, 1, 2, now, false, true);
      break;
    case kRangeCount:
      range_count_ = Read16(data);
      if (!range_count_ || range_count_ > kMaxRanges) Fail(kUnsupported);
      else Issue(kRanges, 1, 2 + range_count_ * 12, now, false, true);
      break;
    case kRanges: {
      if (Read16(data) != range_count_) { Fail(kResponseValue); break; }
      bool supported = false;
      for (unsigned i = 0; i < range_count_; ++i) {
        const Byte* r = data + 2 + 12 * i;
        unsigned minimum = Read32(r), maximum = Read32(r + 4);
        unsigned resolution = Read32(r + 8);
        if (!minimum || minimum > maximum) { Fail(kResponseValue); return; }
        if (minimum <= kRate && maximum >= kRate &&
            (!resolution || (kRate - minimum) % resolution == 0)) supported = true;
      }
      if (!supported) Fail(kRateUnavailable);
      else Issue(kSetRate, 1, 4, now, true);
      break;
    }
    case kSetRate:
      Issue(kVerifyRate, 1, 4, now);
      break;
    case kVerifyRate:
      if (Read32(data) != kRate) Fail(kRateReadback);
      else ValidateClock(now);
      break;
    case kValidity:
      if (data[0] == 0) Fail(kInvalidClock);
      else if (data[0] != 1) Fail(kResponseValue);
      else { state_ = kReady; configuration_ = 0; }
      break;
  }
}
}  // namespace uac
