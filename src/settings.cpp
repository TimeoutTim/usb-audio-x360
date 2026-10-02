// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include "audio.h"
#include "diagnostics.h"
#include <stdio.h>
#include <string.h>

namespace {
using namespace audio_settings;
static CRITICAL_SECTION g_lock;
static Table g_table;
static volatile LONG g_ready = 0, g_active = -1, g_generation = 0;
static volatile LONG g_fallback = kDefault;
static char g_path[MAX_PATH], g_backup[MAX_PATH], g_temp[MAX_PATH];
// Large buffers belong to this worker, not its stack or the audio thread.
static Table g_snapshot;
static char g_bytes[32769];

static volatile LONG* Levels(LONG index) {
  return index >= 0 && index < (LONG)kCapacity
      ? (volatile LONG*)&g_table.entries[index].levels : &g_fallback;
}
// 1=valid, 0=missing, -1=unreadable/invalid. Never overwrite an unread file.
static int Read(const char* path, Table* table) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
  DWORD high = 0, size = GetFileSize(f, &high), got = 0;
  bool ok = !high && size && size <= 32768 && ReadFile(f, g_bytes, size, &got, 0) && got == size;
  CloseHandle(f);
  return ok && Parse(g_bytes, size, table) ? 1 : -1;
}
static bool Load(bool* writable) {
  *writable = false;
  int primary = Read(g_path, &g_snapshot);
  if (primary == 1) { *writable = true; return true; }
  int backup = Read(g_backup, &g_snapshot);
  if (backup == 1) {
    // Use a valid backup, but don't rotate an unreadable/corrupt primary over
    // it. Only a missing primary can be safely replaced without inspection.
    *writable = primary == 0;
    return true;
  }
  if (primary == 0 && backup == 0) {
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    *writable = true;
    return true;
  }
  return false;
}
static bool MergeLoaded() {
  EnterCriticalSection(&g_lock);
  unsigned missing = 0;
  for (unsigned i = 0; i < g_snapshot.count; ++i) {
    unsigned j = 0;
    while (j < g_table.count && g_table.entries[j].key != g_snapshot.entries[i].key) ++j;
    if (j == g_table.count) ++missing;
  }
  if (g_table.count + missing > kCapacity) { LeaveCriticalSection(&g_lock); return false; }
  for (unsigned i = 0; i < g_snapshot.count; ++i) {
    unsigned j = 0;
    while (j < g_table.count && g_table.entries[j].key != g_snapshot.entries[i].key) ++j;
    // Never change a live record on delayed storage recovery. It may already
    // contain user adjustments; its slot must remain stable for atomic setters.
    if (j == g_table.count) g_table.entries[g_table.count++] = g_snapshot.entries[i];
  }
  LeaveCriticalSection(&g_lock);
  return true;
}
static bool Save() {
  size_t size = 0;
  if (!Format(g_snapshot, g_bytes, sizeof(g_bytes), &size)) return false;
  HANDLE f = CreateFileA(g_temp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(f, g_bytes, (DWORD)size, &written, 0) && written == size;
  if (ok) ok = FlushFileBuffers(f) != FALSE;
  CloseHandle(f);
  if (!ok) return false;
  DWORD attrs = GetFileAttributesA(g_path);
  if (attrs != (DWORD)-1) {
    if (!DeleteFileA(g_backup) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    if (!MoveFileA(g_path, g_backup)) return false;
  } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
  if (MoveFileA(g_temp, g_path)) return true;
  // If replacement fails, retain or restore the previous complete file.
  MoveFileA(g_backup, g_path);
  return false;
}
static void UpdateName() {
  AudioDeviceInfo info;
  if (!AudioGetDeviceInfo(&info) || !info.connected || !info.product_name[0]) return;
  char encoded[256] = {}, name[128] = {};
  if (!WideCharToMultiByte(CP_UTF8, 0, info.product_name, -1, encoded, sizeof(encoded), 0, 0)) return;
  size_t length = strlen(encoded);
  if (length >= sizeof(name)) {
    length = sizeof(name)-1;
    while (length && ((unsigned char)encoded[length] & 0xc0) == 0x80) --length;
  }
  memcpy(name, encoded, length);
  for (size_t c = 0; c < length; ++c)
    if ((unsigned char)name[c] < 32 || name[c] == 127) name[c] = ' ';
  unsigned key = ((unsigned)info.vendor_id << 16) | info.product_id;
  EnterCriticalSection(&g_lock);
  for (unsigned i = 0; i < g_table.count; ++i) {
    Entry& e = g_table.entries[i];
    if (e.key == key && strcmp(e.name, name)) {
      strcpy(e.name, name);
      InterlockedIncrement(&g_generation);
      break;
    }
  }
  LeaveCriticalSection(&g_lock);
}
}

VOID SettingsInitialize() {
  InitializeCriticalSection(&g_lock);
  // Reuse the resolved module-directory alias, never a title-relative path.
  if (DiagnosticsSettingsPath(g_path, sizeof(g_path))) {
    sprintf(g_backup, "%s.bak", g_path);
    sprintf(g_temp, "%s.tmp", g_path);
  }
}
LONG SettingsGet(audio_settings::Field field) {
  LONG index = InterlockedCompareExchange(&g_active, 0, 0);
  return audio_settings::Get(InterlockedCompareExchange(Levels(index), 0, 0), field);
}
BOOL SettingsSet(audio_settings::Field field, LONG value) {
  LONG index = InterlockedCompareExchange(&g_active, 0, 0);
  volatile LONG* target = Levels(index);
  LONG old = InterlockedCompareExchange(target, 0, 0);
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    LONG next = audio_settings::Set(old, field, value < 0 ? 0 : (unsigned)value);
    if (next == old) return FALSE;
    LONG actual = InterlockedCompareExchange(target, next, old);
    if (actual == old) {
      if (index >= 0) InterlockedIncrement(&g_generation);
      return TRUE;
    }
    old = actual;
  }
  return FALSE; // Never wait indefinitely in the IR callback.
}
VOID SettingsSelectDevice(WORD vendor, WORD product) {
  // Only the audio worker selects devices, before opening their endpoints.
  // Disk I/O never holds this lock; setters/getters never acquire it.
  if (!InterlockedCompareExchange(&g_ready, 0, 0)) {
    InterlockedExchange(&g_active, -1);
    return; // Storage is optional: do not wait on a blocked initial read.
  }
  unsigned key = ((unsigned)vendor << 16) | product;
  EnterCriticalSection(&g_lock);
  unsigned i = 0;
  while (i < g_table.count && g_table.entries[i].key != key) ++i;
  if (i == g_table.count && i < kCapacity) {
    Entry& e = g_table.entries[i];
    e.key = key; e.levels = kDefault; e.name[0] = 0;
    ++g_table.count;
    InterlockedIncrement(&g_generation);
  }
  InterlockedExchange(&g_active, i < kCapacity ? (LONG)i : -1);
  LeaveCriticalSection(&g_lock);
}
DWORD WINAPI SettingsWorker(void*) {
  bool writable = false;
  if (g_path[0] && Load(&writable)) g_table = g_snapshot;
  InterlockedExchange(&g_ready, 1);
  LONG saved = 0, observed = 0;
  DWORD changed = GetTickCount(), attempted = changed - 10000u;
  DWORD retry = 10000;
  DWORD last_load = changed;
  for (;;) {
    Sleep(500);
    if (!writable && g_path[0] && GetTickCount() - last_load >= 30000u) {
      bool can_write = false;
      if (Load(&can_write) && can_write) writable = MergeLoaded();
      last_load = GetTickCount();
    }
    UpdateName();
    LONG generation = InterlockedCompareExchange(&g_generation, 0, 0);
    DWORD now = GetTickCount();
    if (generation != observed) { observed = generation; changed = now; }
    if (!writable || generation == saved || !SaveDue(now, changed, attempted, retry)) continue;
    EnterCriticalSection(&g_lock);
    LONG snapshot_generation = InterlockedCompareExchange(&g_generation, 0, 0);
    g_snapshot.count = g_table.count;
    for (unsigned i = 0; i < g_table.count; ++i) {
      g_snapshot.entries[i].key = g_table.entries[i].key;
      strcpy(g_snapshot.entries[i].name, g_table.entries[i].name);
      g_snapshot.entries[i].levels = InterlockedCompareExchange(Levels(i), 0, 0);
    }
    LeaveCriticalSection(&g_lock);
    bool ok = Save();
    attempted = GetTickCount();
    retry = ok ? 10000 : 30000;
    if (ok) saved = snapshot_generation;
  }
}
