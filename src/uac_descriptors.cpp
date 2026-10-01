// SPDX-License-Identifier: GPL-3.0-or-later
#include "uac_descriptors.h"
#include <string.h>

namespace uac {
namespace {
unsigned Read16(const Byte* p) { return p[0] | (unsigned(p[1]) << 8); }
unsigned Read24(const Byte* p) { return Read16(p) | (unsigned(p[2]) << 16); }
unsigned long Read32(const Byte* p) {
  return Read24(p) | (static_cast<unsigned long>(p[3]) << 24);
}

bool AudioInterface(const Byte* p, Byte subclass, Byte protocol) {
  return p[1] == 4 && p[0] >= 9 && p[5] == 1 &&
         p[6] == subclass && p[7] == protocol;
}

bool Validate(const Byte* b, size_t available, size_t* total) {
  if (!b || available < 9 || b[0] < 9 || b[1] != 2 || !b[5]) return false;
  *total = Read16(b + 2);
  if (*total < b[0] || *total > available) return false;
  for (size_t o = b[0]; o < *total; o += b[o]) {
    if (*total - o < 2 || b[o] < 2 || b[o] > *total - o) return false;
    if ((b[o + 1] == 4 && b[o] < 9) ||
        (b[o + 1] == 5 && b[o] < 7) ||
        (b[o + 1] == 11 && b[o] < 8)) return false;
  }
  return true;
}

// Associate each AS interface with its own AC function. Never borrow a clock
// or terminal from a different audio function in a composite device.
bool ControlFor(const Byte* b, size_t total, Byte stream, Byte version,
                Byte* control) {
  unsigned matches = 0;
  Byte ac = 0;
  bool in_ac = false;
  for (size_t o = b[0]; o < total; o += b[o]) {
    const Byte* p = b + o;
    if (p[1] == 11) in_ac = false;
    if (p[1] == 4) {
      in_ac = AudioInterface(p, 1, 0) && p[3] == 0;
      ac = p[2];
    }
    if (version == 1 && in_ac && p[1] == 0x24 && p[0] >= 8 &&
        p[2] == 1 && p[0] >= 8u + p[7]) {
      for (unsigned i = 0; i < p[7]; ++i) {
        if (p[8 + i] == stream) { *control = ac; ++matches; }
      }
    }
    if (version == 2 && p[1] == 11 && p[0] >= 8 && p[4] == 1 &&
        p[5] == 0 && p[6] == 0x20 && stream >= p[2] &&
        unsigned(stream) < unsigned(p[2]) + p[3]) {
      // UAC2's AudioControl interface is first in its IAD group.
      for (size_t q = b[0]; q < total; q += b[q]) {
        if (AudioInterface(b + q, 1, 0x20) && b[q + 2] == p[2] &&
            b[q + 3] == 0) {
          *control = p[2]; ++matches;
        }
      }
    }
  }
  return matches == 1;
}

bool TerminalClock(const Byte* b, size_t total, Byte ac, Byte terminal,
                   Byte version, Direction direction, Byte* clock) {
  bool in_ac = false;
  unsigned matches = 0;
  for (size_t o = b[0]; o < total; o += b[o]) {
    const Byte* p = b + o;
    if (p[1] == 11) in_ac = false;
    if (p[1] == 4)
      in_ac = AudioInterface(p, 1, version == 2 ? 0x20 : 0) &&
              p[2] == ac && p[3] == 0;
    const Byte subtype = direction == kPlayback ? 2 : 3;
    const unsigned minimum = version == 2
        ? (direction == kPlayback ? 17 : 12)
        : (direction == kPlayback ? 12 : 9);
    // Playback links to a USB-streaming input terminal; capture links to a
    // USB-streaming output terminal. Do not borrow an unrelated terminal.
    if (in_ac && p[1] == 0x24 && p[0] >= minimum && p[2] == subtype &&
        p[3] == terminal && Read16(p + 4) == 0x0101) {
      *clock = version == 2 ? (direction == kPlayback ? p[7] : p[8]) : 0;
      ++matches;
    }
  }
  return matches == 1 && (version == 1 || *clock != 0);
}

bool ReadEndpoint(const Byte* p, Endpoint* ep) {
  unsigned raw = Read16(p + 4);
  unsigned transactions = (raw >> 11) & 3;
  if (!(p[2] & 15) || (p[2] & 0x70) || (raw & 0xe000) ||
      transactions == 3 || !(raw & 0x7ff) || !p[6] || p[6] > 16)
    return false;
  ep->address = p[2];
  ep->interval = p[6];
  ep->max_packet_bytes = static_cast<unsigned short>(raw & 0x7ff);
  ep->transactions = static_cast<Byte>(transactions + 1);
  return true;
}

bool Candidate(const Byte* b, size_t total, size_t begin, size_t end,
               Format* f) {
  const Byte* iface = b + begin;
  memset(f, 0, sizeof(*f));
  f->configuration = b[5];
  f->interface_number = iface[2];
  f->alternate = iface[3];
  f->version = iface[7] == 0x20 ? 2 : 1;
  bool general = false, format = false, pcm = false;
  unsigned output_count = 0, input_count = 0, feedback_count = 0;
  unsigned endpoint_count = 0;
  Endpoint output_endpoint = {}, input_endpoint = {};
  Sync output_sync = kNoSync, input_sync = kNoSync;
  Byte output_sync_address = 0;
  unsigned class_endpoint_count = 0;
  f->endpoint_layout_supported = true;
  for (size_t o = begin + iface[0]; o < end; o += b[o]) {
    const Byte* p = b + o;
    if (p[1] == 0x24 && p[0] >= 3 && p[2] == 1) {
      if (general) return false;
      general = true;
      if (f->version == 1) {
        if (p[0] < 7) return false;
        f->terminal = p[3]; pcm = Read16(p + 5) == 1;
      } else {
        if (p[0] < 16) return false;
        f->terminal = p[3]; f->channels = p[10];
        pcm = p[5] == 1 && (Read32(p + 6) & 1) != 0;
      }
    } else if (p[1] == 0x24 && p[0] >= 3 && p[2] == 2) {
      if (format || p[0] < 4 || p[3] != 1) return false;
      format = true;
      if (f->version == 1) {
        if (p[0] < 8) return false;
        f->channels = p[4]; f->sample_bytes = p[5]; f->valid_bits = p[6];
        f->rate_48000_known = true;
        if (!p[7]) {
          if (p[0] < 14 || Read24(p + 8) > Read24(p + 11)) return false;
          f->supports_48000 = Read24(p + 8) <= 48000 && Read24(p + 11) >= 48000;
        } else {
          if (p[0] < 8u + 3u * p[7]) return false;
          for (unsigned i = 0; i < p[7]; ++i) {
            if (Read24(p + 8 + i * 3) == 48000) {
              f->supports_48000 = true;
              if (p[7] == 1) f->fixed_48000 = true;
            }
          }
        }
      } else {
        if (p[0] < 6) return false;
        f->sample_bytes = p[4]; f->valid_bits = p[5];
      }
    } else if (p[1] == 5) {
      ++endpoint_count;
      if ((p[3] & 3) != 1) f->endpoint_layout_supported = false;
      unsigned usage = (p[3] >> 4) & 3;
      if (!(p[2] & 0x80) && usage == 0) {
        ++output_count;
        if (output_count == 1) {
          if (!ReadEndpoint(p, &output_endpoint)) return false;
          output_sync = static_cast<Sync>((p[3] >> 2) & 3);
          if (f->version == 1 && p[0] >= 9) output_sync_address = p[8];
        } else {
          f->endpoint_layout_supported = false;
        }
      } else if ((p[2] & 0x80) && (usage == 0 || usage == 2)) {
        ++input_count;
        if (input_count == 1) {
          if (!ReadEndpoint(p, &input_endpoint)) return false;
          input_sync = static_cast<Sync>((p[3] >> 2) & 3);
        } else {
          f->endpoint_layout_supported = false;
        }
      } else if ((p[2] & 0x80) &&
                 usage == 1) {
        ++feedback_count;
        if (feedback_count == 1) {
          if (!ReadEndpoint(p, &f->feedback)) return false;
        } else {
          f->endpoint_layout_supported = false;
        }
      } else {
        // Retain the candidate for diagnostics/selection.
        f->endpoint_layout_supported = false;
      }
    } else if (p[1] == 0x25 && f->version == 1) {
      // A few UAC1 firmwares place the class-specific endpoint descriptor
      // before the standard endpoint descriptor. There is only one data
      // endpoint descriptor per supported alternate, so retain its controls
      // independent of that noncanonical ordering.
      if (p[0] < 7 || p[2] != 1) return false;
      if (++class_endpoint_count == 1)
        f->endpoint_rate_control = (p[3] & 1) != 0;
      else
        f->endpoint_layout_supported = false;
    }
  }
  if (!general || !format || !pcm || !f->terminal ||
      (output_count == input_count))
    return false;
  f->direction = output_count ? kPlayback : kCapture;
  f->data = output_count ? output_endpoint : input_endpoint;
  f->sync = output_count ? output_sync : input_sync;
  if (endpoint_count != iface[4]) f->endpoint_layout_supported = false;
  if (f->direction == kPlayback && f->sync == kAsynchronous) {
    if (!feedback_count ||
        (output_sync_address && output_sync_address != f->feedback.address) ||
        (f->version == 1 && output_sync_address != f->feedback.address) ||
        f->feedback.max_packet_bytes < 3 || f->feedback.max_packet_bytes > 4)
      f->endpoint_layout_supported = false;
  } else if (feedback_count || output_sync_address) {
    f->endpoint_layout_supported = false;
  }
  const bool associated = ControlFor(b, total, f->interface_number, f->version,
                                     &f->control_interface);
  const bool terminal = associated && TerminalClock(
      b, total, f->control_interface, f->terminal, f->version,
      f->direction, &f->clock);
  // UAC1 endpoint direction is sufficient to identify playback; its AC
  // topology is not needed for this driver's endpoint-based setup. UAC2 must
  // retain an unambiguous clock association.
  f->topology_valid = f->version == 1 || terminal;
  return true;
}
}  // namespace

Result Discover(const Byte* b, size_t available, Format* formats,
                size_t capacity, size_t* count) {
  if (!count) return kMalformed;
  *count = 0;
  size_t total = 0;
  if ((!formats && capacity) || !Validate(b, available, &total)) return kMalformed;
  size_t found = 0;
  for (size_t o = b[0]; o < total;) {
    size_t end = o + b[o];
    if (b[o + 1] == 4) {
      while (end < total && b[end + 1] != 4 && b[end + 1] != 11)
        end += b[end];
      if (b[o + 3] && (AudioInterface(b + o, 2, 0) ||
                         AudioInterface(b + o, 2, 0x20))) {
        Format f;
        if (Candidate(b, total, o, end, &f)) {
          if (found == capacity) return kCapacityExceeded;
          formats[found++] = f;
        }
      }
    }
    o = end;
  }
  *count = found;
  return kOk;
}

bool ControlReadable(Byte controls, unsigned selector) {
  if (!selector || selector > 4) return false;
  unsigned access = (controls >> (2 * (selector - 1))) & 3;
  return access == 1 || access == 3;
}

bool ControlWritable(Byte controls, unsigned selector) {
  if (!selector || selector > 4) return false;
  return ((controls >> (2 * (selector - 1))) & 3) == 3;
}

bool FindClock(const Byte* b, size_t available, Byte control, Byte id,
               ClockEntity* entity) {
  if (!entity) return false;
  memset(entity, 0, sizeof(*entity));
  size_t total = 0;
  if (!id || !Validate(b, available, &total)) return false;
  bool in_ac = false;
  unsigned matches = 0;
  ClockEntity result;
  memset(&result, 0, sizeof(result));
  for (size_t o = b[0]; o < total; o += b[o]) {
    const Byte* p = b + o;
    if (p[1] == 11) in_ac = false;
    if (p[1] == 4)
      in_ac = AudioInterface(p, 1, 0x20) && p[2] == control && !p[3];
    if (!in_ac || p[1] != 0x24 || p[0] < 4 || p[3] != id) continue;
    if (p[2] == 0x0a) {
      if (p[0] < 8) return false;
      result.kind = kClockSource;
      result.attributes = p[4]; result.controls = p[5];
    } else if (p[2] == 0x0b) {
      if (p[0] < 7 || !p[4] || p[0] < 7u + p[4]) return false;
      result.kind = kClockSelector;
      result.input_count = p[4];
      memcpy(result.inputs, p + 5, p[4]);
      result.controls = p[5 + p[4]];
    } else if (p[2] == 0x0c) {
      if (p[0] < 7) return false;
      result.kind = kClockMultiplier;
      result.input_count = 1; result.inputs[0] = p[4];
      result.controls = p[5];
    } else continue;
    result.id = id;
    for (unsigned i = 0; i < result.input_count; ++i)
      if (!result.inputs[i]) return false;
    if (++matches > 1) return false;
  }
  if (matches != 1) return false;
  *entity = result;
  return true;
}
}  // namespace uac
