// SPDX-License-Identifier: GPL-3.0-or-later
// PowerPC detour support derived from iMoD1998 Detours V3.1, as distributed
// by EinTim23/hiddriver360 and Durg5/riffmaster-rgh360.
#ifndef USB_AUDIO360_DETOUR_H
#define USB_AUDIO360_DETOUR_H

#include <xtl.h>

class PowerPcDetour {
 public:
  PowerPcDetour();
  PowerPcDetour(void* source, const void* target);

  bool Install();
  bool Remove();

  template <typename T>
  T Original() const {
    return T(trampoline_);
  }

 private:
  static SIZE_T WriteFarBranch(void* destination, const void* target,
                               bool linked, bool preserve_register);
  static SIZE_T CopyInstruction(DWORD* destination, const DWORD* source);
  static SIZE_T RelocateBranch(DWORD* destination, const DWORD* source);

  void* source_;
  const void* target_;
  BYTE* trampoline_;
  SIZE_T original_length_;
  BYTE original_[32];

#pragma section(".text", read, execute)
  __declspec(allocate(".text")) static BYTE trampoline_buffer_[4096];
  static SIZE_T trampoline_size_;
};

#endif
