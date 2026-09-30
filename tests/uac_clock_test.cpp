// SPDX-License-Identifier: GPL-3.0-or-later
#include "uac_clock.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

typedef std::vector<uac::Byte> Bytes;
static Bytes Configuration(unsigned controls = 5) {
  const unsigned char b[] = {9,2,26,0,1,1,0,0x80,50,
                            9,4,4,0,0,1,1,0x20,0,
                            8,0x24,0x0a,9,1,static_cast<unsigned char>(controls),0,0};
  return Bytes(b, b + sizeof(b));
}
static void Add(Bytes& b, const unsigned char* p, size_t n) {
  b.insert(b.end(), p, p + n);
  b[2] = static_cast<unsigned char>(b.size()); b[3] = b.size() >> 8;
}
static uac::ClockRequest Request(uac::ClockSetup& s, unsigned type,
                                 unsigned req, unsigned selector,
                                 unsigned entity, unsigned length) {
  uac::ClockRequest r;
  assert(s.Pending(&r));
  assert(r.request_type == type && r.request == req);
  assert(r.value == selector * 256 && r.index == entity * 256 + 4);
  assert(r.length == length);
  return r;
}
static void Reply(uac::ClockSetup& s, unsigned value, unsigned length,
                  unsigned now = 1) {
  uac::ClockRequest r; assert(s.Pending(&r));
  unsigned char data[4];
  for (unsigned i = 0; i < 4; ++i) data[i] = value >> (8 * i);
  s.Complete(r.token, true, data, length, now);
}
static void Begin(uac::ClockSetup& s, const Bytes& b, unsigned clock = 9,
                  unsigned now = 0) {
  assert(s.Begin(&b[0], b.size(), 4, clock, now));
}
static void ReachSet(uac::ClockSetup& s, const Bytes& b) {
  Begin(s, b);
  Request(s, 0xa1, 1, 1, 9, 4); Reply(s, 44100, 4);
  Request(s, 0xa1, 2, 1, 9, 2); Reply(s, 1, 2);
  uac::ClockRequest r = Request(s, 0xa1, 2, 1, 9, 14);
  const unsigned char range[] = {1,0,0x80,0xbb,0,0,0x80,0xbb,0,0,0,0,0,0};
  s.Complete(r.token, true, range, sizeof(range), 2);
  r = Request(s, 0x21, 1, 1, 9, 4);
  assert(r.output[0] == 0x80 && r.output[1] == 0xbb && !r.output[2] && !r.output[3]);
}

int main() {
  typedef uac::ClockSetup S;
  Bytes ro = Configuration(), rw = Configuration(7);
  S s;
  Begin(s, ro); Request(s, 0xa1, 1, 1, 9, 4); Reply(s, 48000, 4);
  Request(s, 0xa1, 1, 2, 9, 1); Reply(s, 1, 1);
  assert(s.state() == S::kReady && s.source() == 9);
  Begin(s, ro); Reply(s, 44100, 4);
  assert(s.error() == S::kRateUnavailable);
  ReachSet(s, rw); Reply(s, 0, 4, 3);
  Request(s, 0xa1, 1, 1, 9, 4); Reply(s, 48000, 4, 4);
  Request(s, 0xa1, 1, 2, 9, 1); Reply(s, 1, 1, 5);
  assert(s.state() == S::kReady);
  ReachSet(s, rw); Reply(s, 0, 4, 3); Reply(s, 44100, 4, 4);
  assert(s.error() == S::kRateReadback);
  Begin(s, ro); Reply(s, 48000, 4); Reply(s, 0, 1);
  assert(s.error() == S::kInvalidClock);
  Bytes no_validity = Configuration(1);
  Begin(s, no_validity); Reply(s, 48000, 4);
  assert(s.state() == S::kReady);  // Do not query unsupported validity control.

  // Follow the active selector pin; never SET a selector speculatively.
  Bytes graph = ro;
  const unsigned char selector[] = {9,0x24,0x0b,10,2,11,9,1,0};
  const unsigned char multiplier[] = {7,0x24,0x0c,11,9,5,0};
  Add(graph, selector, sizeof(selector)); Add(graph, multiplier, sizeof(multiplier));
  Begin(s, graph, 10); Request(s, 0xa1, 1, 1, 10, 1); Reply(s, 2, 1);
  Request(s, 0xa1, 1, 1, 9, 4); Reply(s, 48000, 4); Reply(s, 1, 1);
  assert(s.state() == S::kReady);
  Begin(s, graph, 10); Reply(s, 1, 1);
  Request(s, 0xa1, 1, 1, 11, 2); Reply(s, 2, 2);
  Request(s, 0xa1, 1, 2, 11, 2); Reply(s, 2, 2);
  Request(s, 0xa1, 1, 1, 9, 4); s.Cancel();
  Begin(s, graph, 11); Reply(s, 2, 2); Reply(s, 1, 2);
  assert(s.error() == S::kUnsupported);
  Begin(s, graph, 10); Reply(s, 0, 1);
  assert(s.error() == S::kResponseValue);
  Begin(s, graph, 10); Reply(s, 3, 1);
  assert(s.error() == S::kResponseValue);
  // Self-referencing selected input fails without recursion or another request.
  graph[31] = 10;
  Begin(s, graph, 10); Reply(s, 1, 1);
  assert(s.error() == S::kCycle);

  // Old callbacks cannot advance this connection, even after reconnecting.
  Begin(s, ro);
  uac::ClockRequest old = Request(s, 0xa1, 1, 1, 9, 4);
  assert(!s.Begin(&ro[0], ro.size(), 4, 9, 0));
  s.Cancel(); Begin(s, ro);
  uac::ClockRequest current = Request(s, 0xa1, 1, 1, 9, 4);
  assert(current.token != old.token);
  s.Complete(old.token, true, 0, 4000, 1);
  assert(s.state() == S::kPending);
  Reply(s, 48000, 4);
  s.Complete(current.token, false, 0, 0, 1); // Duplicate completion.
  assert(s.state() == S::kPending); Reply(s, 1, 1);
  assert(s.state() == S::kReady);

  Begin(s, ro, 9, 0xfffffff0u); s.Tick(0x20);
  assert(s.state() == S::kPending); s.Tick(0x400);
  assert(s.error() == S::kTimeout);
  Begin(s, ro); old = Request(s, 0xa1, 1, 1, 9, 4);
  s.Complete(old.token, true, 0, 4, 1000);
  assert(s.error() == S::kTimeout);

  for (unsigned n = 0; n <= 8; ++n) {
    if (n == 4) continue;
    Begin(s, ro); Reply(s, 48000, n);
    assert(s.error() == S::kResponseLength);
  }
  Begin(s, ro); old = Request(s, 0xa1, 1, 1, 9, 4);
  s.Complete(old.token, true, 0, 4, 1);
  assert(s.error() == S::kResponseLength);
  Begin(s, ro); old = Request(s, 0xa1, 1, 1, 9, 4);
  s.Complete(old.token, false, 0, 0, 1);
  assert(s.error() == S::kTransfer);
  Begin(s, rw); Reply(s, 44100, 4); Reply(s, 17, 2);
  assert(s.error() == S::kUnsupported);
  // Range length/header mismatches must not cause reads outside the response.
  for (unsigned n = 0; n <= 15; ++n) {
    Begin(s, rw); Reply(s, 44100, 4); Reply(s, 1, 2);
    old = Request(s, 0xa1, 2, 1, 9, 14);
    unsigned char data[15] = {2,0};
    s.Complete(old.token, true, data, n, 2);
    assert(s.error() == (n == 14 ? S::kResponseValue : S::kResponseLength));
  }
  puts("UAC2 clock state-machine tests passed");
}
