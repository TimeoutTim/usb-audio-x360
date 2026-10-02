// SPDX-License-Identifier: GPL-3.0-or-later
#include "guide_layout.h"

#include <assert.h>
#include <limits>
#include <stdio.h>

using guide_layout::Rect;

int main() {
  const Rect media_above = {0, 85, 323, 28};
  const Rect media_below = {0, 144, 323, 65};
  const Rect media_rows[] = {{0, 1, 323, 28}, {0, 29, 323, 28}, {0, 57, 323, 28}};
  float gap_y = -1;
  assert(guide_layout::PlanGap(media_above, media_below, 210, media_rows, 3, 28, &gap_y));
  assert(gap_y == 114.5f);
  Rect below = media_below;
  below.y = 141; // Exact fit is valid; existing rows do not move.
  assert(guide_layout::PlanGap(media_above, below, 210, media_rows, 3, 28, &gap_y));
  assert(gap_y == 113);
  below.y = 140.5f;
  assert(!guide_layout::PlanGap(media_above, below, 210, media_rows, 3, 28, &gap_y));
  below = media_below;
  below.x = 1;
  assert(!guide_layout::PlanGap(media_above, below, 210, media_rows, 3, 28, &gap_y));
  below = media_below;
  below.width = 320;
  assert(!guide_layout::PlanGap(media_above, below, 210, media_rows, 3, 28, &gap_y));
  const Rect gap_obstacle = {10, 120, 5, 5};
  assert(!guide_layout::PlanGap(media_above, media_below, 210, &gap_obstacle, 1, 28, &gap_y));
  assert(!guide_layout::PlanGap(media_above, media_below, 208, 0, 0, 28, &gap_y));
  assert(!guide_layout::PlanGap(media_above, media_below, 210, 0, 1, 28, &gap_y));
  assert(!guide_layout::PlanGap(media_above, media_below, 210, media_rows, 33, 28, &gap_y));
  assert(!guide_layout::PlanGap(media_above, media_below, 210, 0, 0, 0, &gap_y));
  assert(!guide_layout::PlanGap(media_above, media_below, 210, 0, 0, 28, 0));
  below = media_below;
  below.y = std::numeric_limits<float>::quiet_NaN();
  assert(!guide_layout::PlanGap(media_above, below, 210, 0, 0, 28, &gap_y));

  puts("Guide Media gap tests passed");
}
