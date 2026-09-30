// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcm_packet.h"
#include "uac_clock.h"
#include "feedback_pacer.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <vector>
typedef std::vector<unsigned char> Bytes;
#define ADD(...) do { const unsigned char p[] = {__VA_ARGS__}; \
  b.insert(b.end(), p, p + sizeof(p)); } while (0)
static Bytes Configuration(bool v2) {
  Bytes b;
  ADD(9,2,0,0,2,1,0,0x80,50);
  if (v2) ADD(8,11,4,2,1,0,0x20,0);
  ADD(9,4,4,0,0,1,1,static_cast<unsigned char>(v2 ? 0x20 : 0),0);
  if (v2) {
    ADD(9,0x24,1,0,2,0,34,0,0);
    ADD(8,0x24,0x0a,9,1,5,0,0);
    ADD(17,0x24,2,7,1,1,0,9,2,3,0,0,0,0,0,0,0);
  } else {
    ADD(9,0x24,1,0,1,21,0,1,5);
    ADD(12,0x24,2,7,1,1,0,2,3,0,0,0);
  }
  ADD(9,4,5,0,0,1,2,static_cast<unsigned char>(v2 ? 0x20 : 0),0);
  ADD(9,4,5,1,static_cast<unsigned char>(v2 ? 2 : 1),1,2,
      static_cast<unsigned char>(v2 ? 0x20 : 0),0);
  if (v2) {
    ADD(16,0x24,1,7,0,1,1,0,0,0,2,3,0,0,0,0);
    ADD(6,0x24,2,1,4,24);
    ADD(7,5,3,5,0x88,1,1);
    ADD(7,5,0x84,0x11,4,0,1);
  } else {
    ADD(7,0x24,1,7,1,1,0);
    ADD(11,0x24,2,1,2,2,16,1,0x80,0xbb,0);
    ADD(9,5,3,9,200,0,1,0,0);
    ADD(7,0x25,1,1,0,0,0);
  }
  b[2] = (unsigned char)b.size(); b[3] = (unsigned char)(b.size() >> 8);
  return b;
}
static void Pipeline(bool v2) {
  Bytes b = Configuration(v2);
  uac::Format f; size_t count;
  assert(uac::Discover(&b[0], b.size(), &f, 1, &count) == uac::kOk && count == 1);
  assert(f.version == (v2 ? 2 : 1));
  if (v2) {
    uac::ClockSetup clock; uac::ClockRequest r;
    assert(clock.Begin(&b[0], b.size(), f.control_interface, f.clock, 0));
    assert(clock.Pending(&r));
    assert(r.request_type == 0xa1 && r.request == 1 && r.value == 0x100 &&
           r.index == 0x904 && r.length == 4);
    const unsigned char rate[] = {0x80,0xbb,0,0};
    clock.Complete(r.token, true, rate, 4, 1);
    assert(clock.Pending(&r) && r.value == 0x200 && r.length == 1);
    const unsigned char valid = 1;
    clock.Complete(r.token, true, &valid, 1, 2);
    assert(clock.state() == uac::ClockSetup::kReady && clock.source() == 9);
  } else assert(f.supports_48000 && f.endpoint_rate_control);

  uac::FeedbackPacer pacer;
  assert(pacer.Begin(0, 49));
  unsigned char storage[1570]; memset(storage, 0xa5, sizeof(storage));
  uac::PcmPacketBuilder builder(storage + 1, 1568, f.sample_bytes,
                                 f.valid_bits, f.data.max_packet_bytes);
  unsigned offset = 0;
  const unsigned expected_frames[] = {47,48,49,48};
  for (unsigned packet = 0; packet < 4; ++packet) {
    unsigned frames = 48;
    if (v2) {
      // Full-speed three-byte feedback; endpoint capacity is four bytes.
      assert(pacer.Update(expected_frames[packet] << 14, 3, packet));
      assert(pacer.Next(packet, &frames) && frames == expected_frames[packet]);
    }
    unsigned short length = 0;
    unsigned char* p = builder.Append(frames, &length);
    assert(p == storage + 1 + offset && length == frames * 2 * f.sample_bytes);
    for (unsigned i = 0; i < frames; ++i)
      builder.Stereo(p + i * 2 * f.sample_bytes, 0x12345678, 0xedcba988);
    const unsigned char narrow[] = {0x34,0x12,0xcb,0xed};
    const unsigned char wide[] = {0,0x56,0x34,0x12,0,0xa9,0xcb,0xed};
    for (unsigned i = 0; i < frames; ++i)
      assert(!memcmp(p + i * 2 * f.sample_bytes, v2 ? wide : narrow,
                     2 * f.sample_bytes));
    offset += length;
    assert(builder.total() == offset);
  }
  assert(offset == (v2 ? 1536u : 768u));
  assert(storage[0] == 0xa5 && storage[1 + offset] == 0xa5);
  unsigned short unchanged = 123;
  assert(!builder.Append(51, &unchanged));
  assert(unchanged == 123 && builder.total() == offset);
}
int main() {
  Pipeline(false); Pipeline(true);
  unsigned char p[8]; unsigned short length = 0;
  uac::PcmPacketBuilder tiny(p, 7, 4, 24, 392);
  assert(!tiny.Append(1, &length) && tiny.total() == 0);
  uac::PcmPacketBuilder invalid(p, 8, 4, 33, 392);
  assert(!invalid.Append(1, &length));
  uac::PcmPacketBuilder packed(p, 8, 3, 24, 392);
  assert(packed.Append(1, &length) == p && length == 6);
  packed.Stereo(p, 0x7fffffff, 0x80000000);
  const unsigned char expected[] = {0xff,0xff,0x7f,0,0,0x80};
  assert(!memcmp(p, expected, 6));
  uac::WritePcm(p, 0xffffffff, 4, 32);
  for (unsigned i = 0; i < 4; ++i) assert(p[i] == 0xff);
  puts("Descriptor-to-clock-to-PCM packet pipeline tests passed");
}
