// SPDX-License-Identifier: GPL-3.0-or-later
#include "detour.h"

#include <string.h>

#define MASK_N_BITS(n) ((1u << (n)) - 1u)
#define PPC_HI(x) (((DWORD)(x) >> 16) & 0xFFFF)
#define PPC_LO(x) ((DWORD)(x) & 0xFFFF)
#define PPC_BIT32(n) (31 - (n))
#define PPC_OPCODE(op) ((DWORD)(op) << 26)
#define PPC_OP_ADDI PPC_OPCODE(14)
#define PPC_OP_ADDIS PPC_OPCODE(15)
#define PPC_OP_BC PPC_OPCODE(16)
#define PPC_OP_B PPC_OPCODE(18)
#define PPC_OP_BCCTR PPC_OPCODE(19)
#define PPC_OP_ORI PPC_OPCODE(24)
#define PPC_OP_EXT PPC_OPCODE(31)
#define PPC_OP_STD PPC_OPCODE(62)
#define PPC_OP_LD PPC_OPCODE(58)
#define PPC_OPCODE_MASK PPC_OPCODE(63)
#define PPC_EXOP_MTSPR ((DWORD)467 << 1)
#define PPC_EXOP_BCCTR ((DWORD)528 << 1)
#define PPC_SPR(spr) ((((spr) & 0x1F) << 5) | (((spr) >> 5) & 0x1F))

__declspec(allocate(".text")) BYTE PowerPcDetour::trampoline_buffer_[4096] = {};
SIZE_T PowerPcDetour::trampoline_size_ = 0;

PowerPcDetour::PowerPcDetour()
    : source_(0), target_(0), trampoline_(0), original_length_(0) {
  memset(original_, 0, sizeof(original_));
}

PowerPcDetour::PowerPcDetour(void* source, const void* target)
    : source_(source), target_(target), trampoline_(0), original_length_(0) {
  memset(original_, 0, sizeof(original_));
}

SIZE_T PowerPcDetour::WriteFarBranch(void* destination, const void* target,
                                     bool linked, bool preserve_register) {
  const DWORD address = (DWORD)target;
  const DWORD lis = PPC_OP_ADDIS | PPC_HI(address);
  const DWORD ori = PPC_OP_ORI | PPC_LO(address);
  const DWORD mtctr = PPC_OP_EXT | (PPC_SPR(9) << PPC_BIT32(20)) |
                      PPC_EXOP_MTSPR;
  const DWORD bcctr = PPC_OP_BCCTR | (20 << PPC_BIT32(10)) |
                      (linked ? 1 : 0) | PPC_EXOP_BCCTR;

  if (!preserve_register) {
    DWORD code[4] = {lis, ori, mtctr, bcctr};
    if (destination) memcpy(destination, code, sizeof(code));
    return sizeof(code);
  }

  const DWORD std_r0 = PPC_OP_STD | (1 << PPC_BIT32(15)) | ((WORD)-0x30);
  const DWORD ld_r0 = PPC_OP_LD | (1 << PPC_BIT32(15)) | ((WORD)-0x30);
  DWORD code[6] = {std_r0, lis, ori, mtctr, ld_r0, bcctr};
  if (destination) memcpy(destination, code, sizeof(code));
  return sizeof(code);
}

SIZE_T PowerPcDetour::RelocateBranch(DWORD* destination, const DWORD* source) {
  const DWORD instruction = *source;
  if (instruction & 2) {
    *destination = instruction;
    return 4;
  }

  int offset_bits = 0;
  DWORD branch_options = 20;
  BYTE condition_bit = 0;
  switch (instruction & PPC_OPCODE_MASK) {
    case PPC_OP_B:
      offset_bits = 24;
      break;
    case PPC_OP_BC:
      offset_bits = 14;
      branch_options = (instruction >> PPC_BIT32(10)) & MASK_N_BITS(5);
      condition_bit = (instruction >> PPC_BIT32(15)) & MASK_N_BITS(5);
      break;
    default:
      *destination = instruction;
      return 4;
  }

  LONG offset = instruction & (MASK_N_BITS(offset_bits) << 2);
  if (offset >> (offset_bits + 1))
    offset |= ~((LONG)MASK_N_BITS(offset_bits + 2));
  const DWORD address = (DWORD)source + offset;

  DWORD code[6];
  code[0] = PPC_OP_STD | (1 << PPC_BIT32(15)) | ((WORD)-0x30);
  code[1] = PPC_OP_ADDIS | PPC_HI(address);
  code[2] = PPC_OP_ORI | PPC_LO(address);
  code[3] = PPC_OP_EXT | (PPC_SPR(9) << PPC_BIT32(20)) | PPC_EXOP_MTSPR;
  code[4] = PPC_OP_LD | (1 << PPC_BIT32(15)) | ((WORD)-0x30);
  code[5] = PPC_OP_BCCTR | (branch_options << PPC_BIT32(10)) |
            (condition_bit << PPC_BIT32(15)) | (instruction & 1) |
            PPC_EXOP_BCCTR;
  memcpy(destination, code, sizeof(code));
  return sizeof(code);
}

SIZE_T PowerPcDetour::CopyInstruction(DWORD* destination,
                                       const DWORD* source) {
  DWORD opcode = *source & PPC_OPCODE_MASK;
  if (opcode == PPC_OP_B || opcode == PPC_OP_BC)
    return RelocateBranch(destination, source);
  *destination = *source;
  return 4;
}

bool PowerPcDetour::Install() {
  if (!source_ || !target_ || original_length_) return false;
  const SIZE_T hook_size = WriteFarBranch(0, target_, false, false);
  if (hook_size > sizeof(original_)) return false;

  SIZE_T worst_case = (hook_size / 4) * 24 + 24;
  if (trampoline_size_ + worst_case > sizeof(trampoline_buffer_)) return false;

  memcpy(original_, source_, hook_size);
  original_length_ = hook_size;
  trampoline_ = &trampoline_buffer_[trampoline_size_];

  for (SIZE_T i = 0; i < hook_size / 4; ++i) {
    const DWORD* instruction = (const DWORD*)((DWORD)source_ + i * 4);
    trampoline_size_ += CopyInstruction(
        (DWORD*)&trampoline_buffer_[trampoline_size_], instruction);
  }

  const void* resume = (const void*)((DWORD)source_ + hook_size);
  trampoline_size_ += WriteFarBranch(
      &trampoline_buffer_[trampoline_size_], resume, false, true);
  WriteFarBranch(source_, target_, false, false);
  return true;
}

bool PowerPcDetour::Remove() {
  if (!source_ || !original_length_) return false;
  memcpy(source_, original_, original_length_);
  original_length_ = 0;
  source_ = 0;
  return true;
}
