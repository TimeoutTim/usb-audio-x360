// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_test_budget.h"
#include <assert.h>
#include <stdio.h>
int main() {
  usb_transport::StreamTestBudget b;
  assert(!b.Take(0, 0));
  b.Begin(100);
  assert(!b.Take(2, 100));
  assert(b.Take(0, 349));
  assert(!b.Take(0, 350));
  assert(b.Finished(350));
  usb_transport::StreamTestBudget cap;
  cap.Begin(0);
  for (unsigned i = 0; i < 128; ++i) {
    assert(cap.Take(0, 0)); assert(cap.Take(1, 0));
  }
  assert(!cap.Take(0, 0)); assert(!cap.Take(1, 0));
  assert(cap.Finished(0));
  cap.Begin(1000); // A drained session can start a fresh independent budget.
  assert(cap.Take(0, 1000) && cap.Take(1, 1000));
  cap.Stop();
  assert(!cap.Take(0, 1001) && !cap.Take(1, 1001));
  cap.Begin(2000);
  assert(cap.Take(0, 2000));
  usb_transport::StreamTestBudget wrap;
  wrap.Begin(0xffffff80u);
  assert(wrap.Take(0, 0x79));
  assert(!wrap.Take(0, 0x7a));
  assert(wrap.Finished(0x7a));
  usb_transport::StreamTestBudget longer(30000, 8192);
  longer.Begin(0xfffff000u);
  assert(longer.Take(0, 0xfffff000u + 29999u));
  assert(!longer.Take(0, 0xfffff000u + 30000u));
  assert(longer.Finished(0xfffff000u + 30000u));
  usb_transport::StreamTestBudget longer_cap(30000, 8192);
  longer_cap.Begin(0);
  for (unsigned i = 0; i < 8192; ++i) {
    assert(longer_cap.Take(0, 0)); assert(longer_cap.Take(1, 0));
  }
  assert(!longer_cap.Take(0, 1)); assert(!longer_cap.Take(1, 1));
  assert(longer_cap.Finished(1));
  usb_transport::StreamTestBudget empty(0, 0);
  empty.Begin(0);
  assert(!empty.Take(0, 0)); assert(empty.Finished(0));
  puts("Bounded stream test budget tests passed");
}
