// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_DEBUG_COMMAND_H
#define USB_AUDIO360_DEBUG_COMMAND_H
#include <string.h>
namespace usb_debug {
enum Command { kInvalid, kStatus, kResults, kControls, kTone, kSilence, kStop, kTrace };
struct Request { Command command; unsigned duration; };
// Single aligned DWORD publication; no separately mutable duration field.
inline Request Decode(unsigned word) {
  Request r = { kInvalid, 0 };
  if ((word & 0xff000000u) != 0xa5000000u) return r;
  unsigned command = (word >> 16) & 255, duration = word & 65535;
  if ((command == kTone || command == kSilence) && duration >= 250 && duration <= 10000) {
    r.command = static_cast<Command>(command); r.duration = duration;
  } else if ((command == kControls || command == kStop) && !duration)
    r.command = static_cast<Command>(command);
  return r;
}
inline Request Parse(const char* text) {
  Request r = { kInvalid, 0 };
  if (!text || strncmp(text, "uaudio!", 7)) return r;
  text += 7;
  if (!strcmp(text, "status")) r.command = kStatus;
  else if (!strcmp(text, "results")) r.command = kResults;
  else if (!strcmp(text, "controls")) r.command = kControls;
  else if (!strcmp(text, "stop")) r.command = kStop;
  else {
    if (!strncmp(text, "tone duration=", 14)) { r.command = kTone; text += 14; }
    else if (!strncmp(text, "silence duration=", 17)) { r.command = kSilence; text += 17; }
    else if (!strncmp(text, "trace row=", 10)) { r.command = kTrace; text += 10; }
    else return r;
    unsigned n = 0, digits = 0;
    while (*text >= '0' && *text <= '9') {
      if (++digits > 5) { r.command = kInvalid; return r; }
      n = n * 10 + (*text++ - '0');
    }
    if (*text || !digits || (r.command == kTrace ? n > 31 : (n < 250 || n > 10000)))
      r.command = kInvalid;
    else r.duration = n;
  }
  return r;
}
}
#endif
