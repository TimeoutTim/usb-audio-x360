// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
namespace audio_settings {
const unsigned kCapacity = 64;
const unsigned kDefault = 100u | (100u << 8);
enum Field { Volume = 0, MicGain = 8, OutputMute = 16, MicMute = 17 };
struct Entry { unsigned key, levels; char name[128]; };
struct Table { unsigned count; Entry entries[kCapacity]; };
unsigned Mask(Field field);
unsigned Get(unsigned levels, Field field);
unsigned Set(unsigned levels, Field field, unsigned value);
bool Parse(const char* data, size_t size, Table* table);
bool Format(const Table& table, char* data, size_t capacity, size_t* size);
inline bool SaveDue(unsigned now, unsigned changed, unsigned attempted,
                    unsigned interval) {
  return now - changed >= 2000u && now - attempted >= interval;
}
}
