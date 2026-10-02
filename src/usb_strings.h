// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_USB_STRINGS_H
#define USB_AUDIO360_USB_STRINGS_H
#include <stddef.h>
namespace usb_strings {
inline unsigned Read16(const unsigned char* p) { return p[0] | (p[1] << 8); }
inline bool Valid(const unsigned char* p, size_t size) {
  return p && size >= 2 && p[1] == 3 && p[0] >= 2 &&
      p[0] <= size && (p[0] & 1) == 0;
}
inline unsigned Language(const unsigned char* p, size_t size) {
  if (!Valid(p, size) || p[0] < 4) return 0;
  for (unsigned i = 2; i < p[0]; i += 2)
    if (Read16(p + i) == 0x409) return 0x409;
  return Read16(p + 2);
}
template <class Character>
bool Decode(const unsigned char* p, size_t size, Character* out, size_t capacity) {
  if (!out || !capacity) return false;
  out[0] = 0;
  if (!Valid(p, size)) return false;
  size_t written = 0;
  for (unsigned i = 2; i < p[0] && written + 1 < capacity; i += 2) {
    unsigned code = Read16(p + i);
    // Keep names safe for an information panel; never permit line/control or
    // bidirectional formatting injection. Replace non-BMP pairs consistently.
    if (code < 32 || code == 127 || (code >= 0x200b && code <= 0x202e) ||
        (code >= 0x2066 && code <= 0x2069)) code = ' ';
    if (code >= 0xd800 && code <= 0xdfff) code = '?';
    out[written++] = (Character)code;
  }
  out[written] = 0;
  return written != 0;
}
}
#endif
