// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_ini.h"
#include <stdio.h>
#include <string.h>

namespace audio_settings {
unsigned Mask(Field field) { return (field < 16 ? 255u : 1u) << field; }
unsigned Get(unsigned levels, Field field) { return (levels & Mask(field)) >> field; }
unsigned Set(unsigned levels, Field field, unsigned value) {
  unsigned limit = field == Volume ? 100 : field == MicGain ? 200 : 1;
  if (value > limit) value = limit;
  return (levels & ~Mask(field)) | (value << field);
}
static char* Trim(char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
  size_t n = strlen(p);
  while (n && (p[n-1] == ' ' || p[n-1] == '\t' || p[n-1] == '\r')) p[--n] = 0;
  return p;
}
static bool Key(const char* text, unsigned* key) {
  if (strlen(text) != 11 || text[0] != '[' || text[5] != ':' || text[10] != ']') return false;
  unsigned result = 0;
  for (unsigned i = 1; i < 10; ++i) {
    if (i == 5) continue;
    char c = text[i];
    unsigned d = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
    if (d == 16) return false;
    result = (result << 4) | d;
  }
  *key = result;
  return true;
}
bool Parse(const char* data, size_t size, Table* table) {
  if (!data || !table || size > 32768) return false;
  memset(table, 0, sizeof(*table));
  char name[128] = {};
  Entry* current = 0;
  size_t offset = size >= 3 && (unsigned char)data[0] == 0xef &&
      (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf ? 3 : 0;
  while (offset < size) {
    char line[256]; size_t n = 0;
    while (offset < size && data[offset] != '\n') {
      if (!data[offset] || n == sizeof(line)-1) return false;
      line[n++] = data[offset++];
    }
    if (offset < size) ++offset;
    line[n] = 0;
    char* p = Trim(line);
    if (!*p) continue;
    if (*p == ';' || *p == '#') {
      p = Trim(p + 1);
      if (strlen(p) < sizeof(name)) strcpy(name, p);
      continue;
    }
    if (*p == '[') {
      unsigned key;
      if (!Key(p, &key)) return false;
      unsigned i = 0;
      while (i < table->count && table->entries[i].key != key) ++i;
      if (i == table->count) {
        if (i == kCapacity) return false;
        ++table->count;
        table->entries[i].key = key;
        table->entries[i].levels = kDefault;
      }
      current = &table->entries[i];
      if (*name) strcpy(current->name, name);
      name[0] = 0;
      continue;
    }
    char* eq = strchr(p, '=');
    if (!eq || !current) return false;
    *eq = 0;
    char* value = Trim(eq + 1);
    p = Trim(p);
    Field field;
    if (!strcmp(p, "volume")) field = Volume;
    else if (!strcmp(p, "microphone_gain")) field = MicGain;
    else if (!strcmp(p, "output_muted")) field = OutputMute;
    else if (!strcmp(p, "microphone_muted")) field = MicMute;
    else continue;
    unsigned v = 0;
    if (!*value) return false;
    for (; *value; ++value) {
      if (*value < '0' || *value > '9' || v > 200) return false;
      v = v * 10 + (*value - '0');
    }
    unsigned limit = field == Volume ? 100 : field == MicGain ? 200 : 1;
    if (v > limit) return false;
    current->levels = Set(current->levels, field, v);
  }
  return true;
}
bool Format(const Table& table, char* data, size_t capacity, size_t* size) {
  if (!data || !size || table.count > kCapacity) return false;
  const char header[] = "; USB Audio 360 settings. Edit while the console is off.\r\n";
  size_t used = sizeof(header)-1;
  if (used >= capacity) return false;
  memcpy(data, header, used);
  for (unsigned i = 0; i < table.count; ++i) {
    const Entry& e = table.entries[i];
    char name[128]; unsigned j = 0;
    for (; j < 127 && e.name[j]; ++j)
      name[j] = (unsigned char)e.name[j] < 32 || e.name[j] == 127 ? ' ' : e.name[j];
    name[j] = 0;
    char line[384];
    int n = sprintf(line, "\r\n; %s\r\n[%04x:%04x]\r\nvolume=%u\r\nmicrophone_gain=%u\r\noutput_muted=%u\r\nmicrophone_muted=%u\r\n",
        *name ? name : "USB Audio Device", e.key >> 16, e.key & 65535,
        Get(e.levels, Volume), Get(e.levels, MicGain), Get(e.levels, OutputMute), Get(e.levels, MicMute));
    if (n < 0 || used + (size_t)n >= capacity) return false;
    memcpy(data + used, line, n); used += n;
  }
  data[used] = 0; *size = used; return true;
}
}
