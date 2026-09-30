// SPDX-License-Identifier: GPL-3.0-or-later
#include "uac_descriptors.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

typedef std::vector<uac::Byte> Bytes;
static void Add(Bytes& b, const unsigned char* p, size_t n) {
  b.insert(b.end(), p, p + n);
}
#define APPEND(b, ...) do { const unsigned char p[] = {__VA_ARGS__}; \
  Add(b, p, sizeof(p)); } while (0)

static void Finish(Bytes& b) {
  b[2] = static_cast<unsigned char>(b.size());
  b[3] = static_cast<unsigned char>(b.size() >> 8);
}
static Bytes Header(bool v2) {
  Bytes b;
  APPEND(b, 9,2,0,0,2,3,0,0x80,50);
  if (v2) APPEND(b, 8,11,4,2,1,0,0x20,0);
  APPEND(b, 9,4,4,0,0,1,1,static_cast<unsigned char>(v2 ? 0x20 : 0),0);
  if (v2) {
    APPEND(b, 9,0x24,1,0,2,0,34,0,0);
    // Read-only frequency control: discovery must not require a writable clock.
    APPEND(b, 8,0x24,0x0a,9,1,5,0,0);
    APPEND(b, 17,0x24,2,7,1,1,0,9,2,3,0,0,0,0,0,0,0);
  } else {
    APPEND(b, 9,0x24,1,0,1,21,0,1,5);
    APPEND(b, 12,0x24,2,7,1,1,0,2,3,0,0,0);
  }
  APPEND(b, 9,4,5,0,0,1,2,static_cast<unsigned char>(v2 ? 0x20 : 0),0);
  return b;
}
static void Stream(Bytes& b, bool v2, unsigned char alt,
                   bool general, bool format, bool endpoints) {
  APPEND(b, 9,4,5,alt,static_cast<unsigned char>(endpoints ? (v2 ? 2 : 1) : 0),
         1,2,static_cast<unsigned char>(v2 ? 0x20 : 0),0);
  if (general) {
    if (v2) APPEND(b, 16,0x24,1,7,0,1,1,0,0,0,2,3,0,0,0,0);
    else APPEND(b, 7,0x24,1,7,1,1,0);
  }
  if (format) {
    if (v2) APPEND(b, 6,0x24,2,1,4,24);
    else APPEND(b, 11,0x24,2,1,2,2,16,1,0x80,0xbb,0);
  }
  if (endpoints) {
    if (v2) {
      APPEND(b, 7,5,3,5,0x88,1,1);
      APPEND(b, 7,5,0x84,0x11,4,0,1);
    } else {
      APPEND(b, 9,5,3,9,200,0,1,0,0);
      APPEND(b, 7,0x25,1,1,0,0,0);
    }
  }
  Finish(b);
}
static size_t Parse(const Bytes& b, uac::Format* f, size_t cap = 8) {
  size_t count = 999;
  assert(uac::Discover(&b[0], b.size(), f, cap, &count) == uac::kOk);
  return count;
}

int main() {
  uac::Format formats[8];
  Bytes v1 = Header(false);
  Stream(v1, false, 6, true, true, true);
  assert(Parse(v1, formats) == 1);
  assert(formats[0].configuration == 3 && formats[0].control_interface == 4);
  assert(formats[0].interface_number == 5 && formats[0].alternate == 6);
  assert(formats[0].data.address == 3 && formats[0].data.max_packet_bytes == 200);
  assert(formats[0].supports_48000 && formats[0].endpoint_rate_control);
  assert(formats[0].sample_bytes == 2 && formats[0].valid_bits == 16);

  // Continuous UAC1 rate range, followed by a range excluding 48 kHz.
  Bytes continuous = v1;
  for (size_t o = 9; o < continuous.size(); o += continuous[o]) {
    if (continuous[o + 1] != 0x24 || continuous[o] != 11) continue;
    const unsigned char range[] = {14,0x24,2,1,2,2,16,0,
                                  0x44,0xac,0,0x00,0x77,1};
    continuous.erase(continuous.begin() + o, continuous.begin() + o + 11);
    continuous.insert(continuous.begin() + o, range, range + sizeof(range));
    break;
  }
  Finish(continuous);
  assert(Parse(continuous, formats) == 1);
  for (size_t o = 9; o < continuous.size(); o += continuous[o]) {
    if (continuous[o + 1] == 0x24 && continuous[o] == 14) {
      continuous[o + 11] = 0x44; continuous[o + 12] = 0xac;
      continuous[o + 13] = 0;
    }
  }
  assert(Parse(continuous, formats) == 0);

  Bytes v2 = Header(true);
  Stream(v2, true, 2, true, true, true);
  assert(Parse(v2, formats) == 1);
  assert(formats[0].clock == 9 && formats[0].terminal == 7);
  assert(formats[0].feedback.address == 0x84 && formats[0].sample_bytes == 4);
  assert(!formats[0].rate_48000_known && !formats[0].supports_48000);

  uac::ClockEntity clock;
  assert(uac::FindClock(&v2[0], v2.size(), 4, 9, &clock));
  assert(clock.kind == uac::kClockSource && clock.id == 9);
  assert(uac::ControlReadable(clock.controls, 1));
  assert(!uac::ControlWritable(clock.controls, 1));
  assert(uac::ControlReadable(clock.controls, 2));
  assert(!uac::FindClock(&v2[0], v2.size(), 3, 9, &clock));
  assert(!uac::FindClock(&v2[0], v2.size(), 4, 8, &clock));
  assert(!uac::ControlReadable(2, 1));  // Reserved access encoding.
  assert(!uac::ControlReadable(255, 0));
  assert(!uac::ControlWritable(255, 5));
  assert(uac::ControlWritable(3, 1));

  // Add a selector and multiplier to the AC interface, before AS begins.
  Bytes graph = Header(true);
  graph.resize(graph.size() - 9);
  APPEND(graph, 9,0x24,0x0b,10,2,9,11,3,0);
  APPEND(graph, 7,0x24,0x0c,11,9,5,0);
  Stream(graph, true, 1, true, true, true);
  assert(uac::FindClock(&graph[0], graph.size(), 4, 10, &clock));
  assert(clock.kind == uac::kClockSelector && clock.input_count == 2);
  assert(clock.inputs[0] == 9 && clock.inputs[1] == 11);
  assert(uac::FindClock(&graph[0], graph.size(), 4, 11, &clock));
  assert(clock.kind == uac::kClockMultiplier && clock.inputs[0] == 9);
  // Same ID in a different AC function must not collide with this one.
  APPEND(graph, 9,4,12,0,0,1,1,0x20,0);
  APPEND(graph, 8,0x24,0x0a,9,1,3,0,0);
  Finish(graph);
  assert(uac::FindClock(&graph[0], graph.size(), 4, 9, &clock));
  assert(!uac::ControlWritable(clock.controls, 1));
  assert(uac::FindClock(&graph[0], graph.size(), 12, 9, &clock));
  assert(uac::ControlWritable(clock.controls, 1));
  APPEND(graph, 8,0x24,0x0a,9,1,3,0,0);
  Finish(graph);
  assert(!uac::FindClock(&graph[0], graph.size(), 12, 9, &clock));

  // Mutate selector/multiplier lengths and pin counts as well as source data.
  for (size_t i = 0; i < graph.size(); ++i) {
    for (unsigned value = 0; value < 256; ++value) {
      Bytes mutated = graph; mutated[i] = static_cast<unsigned char>(value);
      uac::FindClock(&mutated[0], mutated.size(), 4, 10, &clock);
      uac::FindClock(&mutated[0], mutated.size(), 4, 11, &clock);
    }
  }

  // The old implementation combined these incomplete alternates into a match.
  Bytes split = Header(true);
  Stream(split, true, 1, true, false, false);
  Stream(split, true, 2, false, true, true);
  assert(Parse(split, formats) == 0);
  Stream(split, true, 3, true, true, true);
  assert(Parse(split, formats) == 1 && formats[0].alternate == 3);
  Stream(split, true, 4, true, true, true);
  assert(Parse(split, formats) == 2 && formats[1].alternate == 4);
  size_t count = 123;
  assert(uac::Discover(&split[0], split.size(), formats, 1, &count) ==
         uac::kCapacityExceeded && count == 0);

  // Actual transfer length bounds every read, including configuration headers.
  for (size_t n = 0; n < v2.size(); ++n) {
    count = 123;
    assert(uac::Discover(&v2[0], n, formats, 8, &count) == uac::kMalformed);
    assert(count == 0);
  }
  Bytes bad = v2;
  bad[9] = 0;
  assert(uac::Discover(&bad[0], bad.size(), formats, 8, &count) == uac::kMalformed);
  bad = v2; bad.push_back(1); Finish(bad);
  assert(uac::Discover(&bad[0], bad.size(), formats, 8, &count) == uac::kMalformed);
  assert(uac::Discover(0, 0, formats, 8, &count) == uac::kMalformed);
  assert(uac::Discover(&v2[0], v2.size(), 0, 8, &count) == uac::kMalformed);

  // No IAD association: do not guess which audio function owns the stream.
  bad = v2; bad[10] = 0xff;
  assert(Parse(bad, formats) == 0);
  // Wrong terminal link must not borrow an unrelated clock.
  bad = v2;
  for (size_t o = 9; o < bad.size(); o += bad[o])
    if (bad[o + 1] == 0x24 && bad[o] == 16) bad[o + 3] = 99;
  assert(Parse(bad, formats) == 0);
  // No feedback endpoint, invalid sample width, invalid endpoint interval.
  bad = v2; bad[bad.size() - 5] = 4;
  assert(Parse(bad, formats) == 0);
  bad = v2; bad[bad.size() - 1] = 0;
  assert(Parse(bad, formats) == 0);
  bad = v2;
  for (size_t o = 9; o < bad.size(); o += bad[o])
    if (bad[o + 1] == 0x24 && bad[o] == 6) bad[o + 5] = 33;
  assert(Parse(bad, formats) == 0);

  // Exercise every single-byte mutation under ASan/UBSan. These are safety
  // checks, not assertions that a mutated descriptor describes a valid device.
  for (size_t i = 0; i < v2.size(); ++i) {
    for (unsigned value = 0; value < 256; ++value) {
      bad = v2; bad[i] = static_cast<unsigned char>(value);
      uac::Discover(&bad[0], bad.size(), formats, 8, &count);
      uac::FindClock(&bad[0], bad.size(), 4, 9, &clock);
    }
  }
  puts("UAC descriptor tests passed (including truncation and byte-mutation sweeps)");
}
