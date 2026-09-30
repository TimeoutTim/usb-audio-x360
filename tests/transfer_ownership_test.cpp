// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_ownership.h"
#include "stream_test_budget.h"
#include <assert.h>
#include <stdio.h>

int main() {
  int trbs[9] = {0};
  usb_transport::Ownership slots;
  const unsigned generation = slots.generation();
  assert(!slots.Submit(0, generation));
  assert(!slots.Submit(&trbs[0], generation + 1));
  assert(!slots.Complete(&trbs[0]));
  assert(slots.Submit(&trbs[0], generation));
  assert(!slots.Submit(&trbs[0], generation));
  assert(slots.pending() == 1);
  // A synchronous callback sees the transfer as owned before host submission
  // returns; the caller must not set pending again on return.
  assert(slots.Complete(&trbs[0]));
  assert(slots.pending() == 0);
  assert(!slots.Complete(&trbs[0]));
  assert(slots.Submit(&trbs[0], generation));
  for (unsigned i = 1; i < 8; ++i) assert(slots.Submit(&trbs[i], generation));
  assert(slots.pending() == 8);
  assert(!slots.Submit(&trbs[8], generation));
  // Stop (including timeout policy) does not manufacture completions or allow
  // storage reuse. Pending completions/cancellations can still be accounted.
  slots.Stop();
  assert(slots.pending() == 8);
  assert(slots.generation() != generation);
  slots.Stop();
  assert(!slots.Submit(&trbs[8], generation));
  for (unsigned i = 0; i < 8; ++i) assert(slots.Complete(&trbs[i]));
  assert(slots.pending() == 0);
  assert(!slots.Submit(&trbs[0], slots.generation()));
  const unsigned stopped_generation = slots.generation();
  assert(slots.stopped());
  assert(slots.Rearm());
  assert(!slots.stopped());
  assert(slots.generation() == stopped_generation);
  assert(slots.Submit(&trbs[0], slots.generation()));
  assert(slots.Complete(&trbs[0]));
  assert(!slots.Rearm());
  // Model a request prepared before removal but dispatched afterwards.
  usb_transport::Ownership disconnected;
  unsigned queued_generation = disconnected.generation();
  disconnected.Stop();
  assert(!disconnected.Submit(&trbs[0], queued_generation));
  assert(!disconnected.Complete(&trbs[0]));
  // Model four primed slots, callback-side reuse, and budget expiry. This
  // validates accounting only; it does not execute Xbox callback code.
  usb_transport::Ownership stream;
  usb_transport::StreamTestBudget budget;
  budget.Begin(0);
  unsigned submitted = 0, completed = 0;
  for (unsigned i = 0; i < 4; ++i) {
    assert(budget.Take(i / 2, 0));
    assert(stream.Submit(&trbs[i], 1)); ++submitted;
  }
  for (unsigned now = 4; now < 250; now += 4) {
    for (unsigned i = 0; i < 4; ++i) {
      assert(stream.Complete(&trbs[i])); ++completed;
      assert(budget.Take(i / 2, now));
      assert(stream.Submit(&trbs[i], 1)); ++submitted;
      assert(stream.pending() == 4);
    }
  }
  for (unsigned i = 0; i < 4; ++i) {
    assert(stream.Complete(&trbs[i])); ++completed;
    assert(!budget.Take(i / 2, 252));
  }
  assert(stream.pending() == 0);
  assert(submitted == completed);
  // A second test reuses only fully drained slots, without resetting ownership.
  budget.Begin(1000);
  for (unsigned i = 0; i < 4; ++i) {
    assert(budget.Take(i / 2, 1000));
    assert(stream.Submit(&trbs[i], 1));
  }
  budget.Stop();
  assert(stream.pending() == 4); // Stop is not a fabricated completion.
  for (unsigned i = 0; i < 4; ++i) {
    assert(!stream.Submit(&trbs[i], 1));
    assert(stream.Complete(&trbs[i]));
    assert(!budget.Take(i / 2, 1001));
  }
  assert(stream.pending() == 0);
  stream.Stop(); // Removal is permanent even after all slots drain.
  budget.Begin(2000);
  assert(!stream.Submit(&trbs[0], 1));
  assert(stream.Rearm());
  assert(stream.Submit(&trbs[0], stream.generation()));
  assert(stream.Complete(&trbs[0]));
  puts("Transfer ownership tests passed");
}
