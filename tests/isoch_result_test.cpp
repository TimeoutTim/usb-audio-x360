// SPDX-License-Identifier: GPL-3.0-or-later
#include "isoch_result.h"
#include <assert.h>
#include <stdio.h>

int main() {
  using namespace usb_transport;
  const unsigned char three[] = {0, 0, 12};
  const unsigned char four[] = {0, 0, 48, 0};
  unsigned long raw = 0;
  assert(ReadFeedbackPacket(0xc0050003UL, 3, 4, three, &raw));
  assert(raw == 0xc0000UL);
  assert(ReadFeedbackPacket(0, 3, 3, three, &raw));
  assert(raw == 0xc0000UL);
  assert(ReadFeedbackPacket(0, 4, 4, four, &raw));
  assert(raw == 0x300000UL);
  const unsigned long errors[] = {0xc005100eUL, 0xc0051003UL,
      0xc000003fUL, 0xc000003cUL, 0xc0051005UL, 0xffffffffUL};
  for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    raw = 123;
    assert(!ReadFeedbackPacket(errors[i], 3, 4, three, &raw));
    assert(raw == 123);
  }
  for (unsigned actual = 0; actual < 8; ++actual) {
    for (unsigned requested = 0; requested < 8; ++requested) {
      for (unsigned short_read = 0; short_read < 2; ++short_read) {
        const unsigned long status = short_read ? 0xc0050003UL : 0;
        const bool expected = (requested == 3 || requested == 4) &&
            (actual == 3 || actual == 4) && actual <= requested;
        raw = 123;
        assert(ReadFeedbackPacket(status, actual, requested, four, &raw) == expected);
        if (!expected) assert(raw == 123);
      }
    }
  }
  assert(IsochInSucceeded(0xc0050003UL, 0, 4));
  assert(!IsochInSucceeded(0, 5, 4));
  assert(!ReadFeedbackPacket(0, 3, 4, 0, &raw));
  assert(!ReadFeedbackPacket(0, 3, 4, three, 0));
  puts("Isochronous IN status and feedback payload tests passed");
}
