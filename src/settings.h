// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <xtl.h>
#include "settings_ini.h"
VOID SettingsInitialize();
DWORD WINAPI SettingsWorker(void*);
VOID SettingsSelectDevice(WORD vendor, WORD product);
LONG SettingsGet(audio_settings::Field field);
BOOL SettingsSet(audio_settings::Field field, LONG value);
