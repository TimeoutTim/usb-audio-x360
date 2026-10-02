// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote_volume.h"
#include "remote_volume_policy.h"
#include "audio.h"

extern "C" LONG XexGetProcedureAddress(HANDLE, DWORD, PVOID);
namespace {
struct Registration;
typedef VOID (NTAPI* Callback)(Registration*, const BYTE*);
struct Registration { Callback callback; LONG priority; LIST_ENTRY link; };
C_ASSERT(sizeof(Registration) == 16);
C_ASSERT(sizeof(unsigned int) == 4);
typedef VOID (NTAPI* RegisterFn)(Registration*, BOOL);
static RegisterFn g_register = 0;
static bool g_registered = false;
static volatile LONG g_busy = 0;
static remote_volume::Policy g_policy;

VOID NTAPI OnMessage(Registration*, const BYTE* message) {
  if (!message || message[0] != 0x83 || message[1] != 0x23) return;
  // Never block the system dispatcher on another callback. No allocation,
  // logging, device access, or audio processing belongs on this path.
  if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
  remote_volume::Action action = g_policy.Input(message, 16, GetTickCount());
  // These setters only update atomic user state/feedback flags. The existing
  // audio worker applies the gain; the Guide reads the very same state.
  if (action == remote_volume::Up) AudioSetVolume(AudioGetVolume() + 5);
  else if (action == remote_volume::Down) AudioSetVolume(AudioGetVolume() - 5);
  else if (action == remote_volume::ToggleMute)
    AudioSetOutputMuted(!AudioIsOutputMuted());
  InterlockedExchange(&g_busy, 0);
}
static Registration g_registration = {OnMessage, 0};
}  // namespace

VOID RemoteVolumeInitialize(HANDLE kernel) {
  // Caller has already checked kernel 17559, whose registration ABI and
  // shared-lock dispatcher were inspected and tested by the passive probe.
  if (g_registered || !kernel) return;
  if (XexGetProcedureAddress(kernel, 39, &g_register) < 0 || !g_register) return;
  g_policy = remote_volume::Policy();
  g_register(&g_registration, TRUE);
  g_registered = true;
}

VOID RemoteVolumeShutdown() {
  if (!g_registered) return;
  // Removal takes the exclusive dispatcher lock and drains active callbacks
  // before the resident registration node or callback code can disappear.
  g_register(&g_registration, FALSE);
  g_registered = false;
}
