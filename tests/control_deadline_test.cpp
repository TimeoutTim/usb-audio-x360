// SPDX-License-Identifier: GPL-3.0-or-later
#include "control_deadline.h"
#include <assert.h>
#include <stdio.h>
int main() {
  using namespace usb_transport;
  assert(ControlDeadline(0x00) == 5000);
  assert(ControlDeadline(0x01) == 5000);
  assert(ControlDeadline(0x80) == 5000);
  assert(ControlDeadline(0x21) == 1000);
  assert(ControlDeadline(0xa1) == 1000);
  assert(!ControlExpired(100, 5099, 5000, false));
  assert(ControlExpired(100, 5100, 5000, false));
  assert(!ControlExpired(100, 5100, 5000, true));
  assert(!ControlExpired(0xfffff000u, 0x387, 5000, false));
  assert(ControlExpired(0xfffff000u, 0x388, 5000, false));
  assert(ControlExpired(0, 1000, ControlDeadline(0x21), false));
  puts("Control deadline tests passed");
}
