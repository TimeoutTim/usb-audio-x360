// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_UAC_DESCRIPTORS_H
#define USB_AUDIO360_UAC_DESCRIPTORS_H

#include <stddef.h>

// Portable discovery only: no USB requests, kernel types, allocation or I/O.
// A discovered format is NOT permission to start streaming. UAC2 rates and
// clock selection require control requests; transport limits are separate.
namespace uac {
typedef unsigned char Byte;
enum Result { kOk, kMalformed, kCapacityExceeded };
enum Sync { kNoSync, kAsynchronous, kAdaptive, kSynchronous };

struct Endpoint {
  Byte address;
  Byte interval;
  unsigned short max_packet_bytes;
  Byte transactions;  // 1..3 for high-speed; host must validate bus speed.
};

struct Format {
  Byte configuration;
  Byte control_interface;
  Byte interface_number;
  Byte alternate;
  Byte version;
  Byte terminal;
  Byte clock;  // Entity ID, not necessarily a clock SOURCE.
  Byte channels;
  Byte sample_bytes;
  Byte valid_bits;
  Sync sync;
  Endpoint data;
  Endpoint feedback;
  bool endpoint_rate_control;
  bool rate_48000_known;  // UAC2 deliberately leaves rate discovery to controls.
  bool supports_48000;
};

enum ClockKind { kClockSource, kClockSelector, kClockMultiplier };
struct ClockEntity {
  ClockKind kind;
  Byte id;
  Byte attributes;
  Byte controls;
  Byte input_count;
  Byte inputs[255];
};

// Look up UAC2 clock entities in a specific AudioControl interface. Selectors
// retain ALL input IDs: the active pin must be read using GET_CUR, never guessed.
bool FindClock(const Byte* configuration, size_t available,
               Byte control_interface, Byte entity_id, ClockEntity* entity);
bool ControlReadable(Byte controls, unsigned selector);
bool ControlWritable(Byte controls, unsigned selector);

// available is the actual received byte count, NOT the allocation size.
// On any error count is zero and callers must discard all output entries.
Result Discover(const Byte* configuration, size_t available,
                Format* formats, size_t capacity, size_t* count);
}  // namespace uac
#endif
