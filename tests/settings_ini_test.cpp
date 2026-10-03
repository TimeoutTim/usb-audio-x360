// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_ini.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
using namespace audio_settings;
static Table table, roundtrip;
static char output[32769];
static bool ParseText(const char* p) { return Parse(p, strlen(p), &table); }
int main() {
  assert(Get(kDefault, Volume) == 100 && Get(kDefault, MicGain) == 100);
  unsigned v = Set(kDefault, Volume, 23);
  v = Set(v, MicGain, 150); v = Set(v, OutputMute, 1);
  assert(Get(v, Volume) == 23 && Get(v, MicGain) == 150 && Get(v, OutputMute) == 1);
  assert(Get(v, MicMute) == 0);
  assert(Get(Set(v, Volume, 999), Volume) == 100);
  assert(ParseText("; SABRENT\r\n[0d8c:0014]\r\nvolume=35\r\nmicrophone_gain=125\r\noutput_muted=1\r\n"
                   "; AirPods Max\n[05AC:110C]\nvolume=60\nmicrophone_muted=1\n"));
  assert(table.count == 2 && table.entries[0].key == 0x0d8c0014);
  assert(!strcmp(table.entries[0].name, "SABRENT"));
  assert(!strcmp(table.entries[1].name, "AirPods Max"));
  assert(Get(table.entries[0].levels, Volume) == 35);
  assert(Get(table.entries[1].levels, Volume) == 60);
  assert(Get(table.entries[1].levels, MicGain) == 100);
  size_t size = 0;
  assert(Format(table, output, sizeof(output), &size));
  assert(Parse(output, size, &roundtrip));
  assert(!memcmp(&table, &roundtrip, sizeof(table)));
  assert(!Format(table, output, 20, &size));
  assert(!ParseText("[0d8c:0014]\nvolume=101\n"));
  assert(!ParseText("[0d8c:0014]\nmicrophone_gain=-1\n"));
  assert(!ParseText("[0d8c:0014]\noutput_muted=2\n"));
  assert(!ParseText("[0d8c:0014]\nvolume=42949672960\n"));
  assert(!ParseText("[0d8c:0014]\nvolume=12x\n"));
  assert(!ParseText("[not a device]\nvolume=30\n"));
  assert(!Parse("abc\0xyz", 7, &table));
  assert(ParseText("\xef\xbb\xbf; Device\n[0000:0000]\nvolume=0\n"));
  assert(ParseText("[1234:5678]\nvolume=5\n[1234:5678]\nmicrophone_gain=80\n"));
  assert(table.count == 1 && Get(table.entries[0].levels, Volume) == 5);
  strcpy(table.entries[0].name, "Headset\n[bad]\rName");
  assert(Format(table, output, sizeof(output), &size));
  assert(Parse(output, size, &roundtrip) && roundtrip.count == 1);
  memset(&table, 0, sizeof(table)); table.count = kCapacity;
  for (unsigned i = 0; i < kCapacity; ++i) {
    table.entries[i].key = i; table.entries[i].levels = kDefault;
  }
  assert(Format(table, output, sizeof(output), &size));
  assert(Parse(output, size, &roundtrip) && roundtrip.count == kCapacity);
  const char extra[] = "\n[ffff:ffff]\nvolume=25\n";
  memcpy(output + size, extra, sizeof(extra));
  assert(!Parse(output, size + sizeof(extra)-1, &roundtrip));
  assert(!SaveDue(1999, 0, 0u-10000, 10000));
  assert(SaveDue(2000, 0, 0u-10000, 10000));
  assert(!SaveDue(9999, 0, 0, 10000));
  assert(SaveDue(10000, 0, 0, 10000));
  assert(!SaveDue(29999, 0, 0, 30000));
  assert(SaveDue(30000, 0, 0, 30000));
  assert(SaveDue(0x1000, 0xfffff000, 0xffff0000, 10000));
  puts("Settings INI and save scheduling tests passed");
}
