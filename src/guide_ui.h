// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <xtl.h>

// Called from the low-priority notification worker. Installation is delayed
// until the dashboard and any Guide customizer have finished loading.
VOID GuideUiTick();
