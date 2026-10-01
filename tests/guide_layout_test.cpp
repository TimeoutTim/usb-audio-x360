// SPDX-License-Identifier: GPL-3.0-or-later
#include "guide_layout.h"

#include <assert.h>
#include <limits>
#include <stdio.h>

using guide_layout::Plan;
using guide_layout::Rect;

int main() {
  const Rect home = {0.0f, 0.0f, 323.0f, 28.0f};
  Rect rows[] = {
    {0.0f, 28.0f, 323.0f, 28.0f},
    {0.0f, 56.0f, 323.0f, 28.0f},
    {0.0f, 84.0f, 323.0f, 28.0f},
    {0.0f, 112.0f, 323.0f, 28.0f}
  };
  Rect obstacle = {0.0f, 173.0f, 323.0f, 40.0f};
  float insertion = -1.0f;
  assert(Plan(home, 200.0f, rows, 4, &obstacle, 1, 28.0f, &insertion));
  assert(insertion == 28.0f);  // Last shifted row ends at 168; TempScene is 173.
  // Existing decorations can extend beyond the authored parent viewport.

  // Exact edge contact fits. Fractional overlap or viewport overflow does not.
  obstacle.y = 168.0f;
  assert(Plan(home, 220.0f, rows, 4, &obstacle, 1, 28.0f, &insertion));
  obstacle.y = 167.5f;
  insertion = -1.0f;
  assert(!Plan(home, 220.0f, rows, 4, &obstacle, 1, 28.0f, &insertion));
  assert(insertion == -1.0f);
  assert(Plan(home, 168.0f, rows, 4, 0, 0, 28.0f, &insertion));
  assert(!Plan(home, 167.5f, rows, 4, 0, 0, 28.0f, &insertion));

  // Other columns and zero-area decorations impose no vertical restriction.
  obstacle.x = 323.0f;
  obstacle.y = 56.0f;
  assert(Plan(home, 220.0f, rows, 4, &obstacle, 1, 28.0f, &insertion));
  obstacle.x = 322.5f;
  assert(!Plan(home, 220.0f, rows, 4, &obstacle, 1, 28.0f, &insertion));
  obstacle.x = 0.0f;
  obstacle.height = 0.0f;
  assert(Plan(home, 220.0f, rows, 4, &obstacle, 1, 28.0f, &insertion));
  obstacle.width = 0.0f;
  obstacle.height = 100.0f;
  assert(Plan(home, 220.0f, rows, 4, &obstacle, 1, 28.0f, &insertion));

  // Detect crossing an obstacle even if the row's final position clears it.
  const Rect short_row = {0.0f, 56.0f, 323.0f, 4.0f};
  obstacle.x = 0.0f;
  obstacle.y = 65.0f;
  obstacle.width = 323.0f;
  obstacle.height = 4.0f;
  assert(!Plan(home, 220.0f, &short_row, 1, &obstacle, 1, 28.0f, &insertion));

  // Home is expected to precede every native action row; overlapping layouts
  // are rejected even if their translated positions would fit the viewport.
  Rect changed = rows[0];
  changed.y = 27.0f;
  assert(!Plan(home, 220.0f, &changed, 1, 0, 0, 28.0f, &insertion));
  rows[1].y = 55.0f;
  assert(!Plan(home, 220.0f, rows, 4, 0, 0, 28.0f, &insertion));
  rows[1].y = 56.0f;
  const Rect reverse_rows[] = {rows[3], rows[1], rows[2], rows[0]};
  assert(Plan(home, 220.0f, reverse_rows, 4, 0, 0, 28.0f, &insertion));
  assert(Plan(home, 56.0f, 0, 0, 0, 0, 28.0f, &insertion));
  assert(!Plan(home, 55.0f, 0, 0, 0, 0, 28.0f, &insertion));
  const Rect offset_home = {20.0f, 5.0f, 323.0f, 28.0f};
  assert(Plan(offset_home, 61.0f, 0, 0, 0, 0, 28.0f, &insertion));
  assert(insertion == 33.0f);

  // Both the injected row and unchanged Home must clear existing obstacles.
  obstacle.y = 30.0f;
  assert(!Plan(home, 220.0f, 0, 0, &obstacle, 1, 28.0f, &insertion));
  obstacle.y = 10.0f;
  assert(!Plan(home, 220.0f, 0, 0, &obstacle, 1, 28.0f, &insertion));

  // Bounded arrays fail before dereferencing; unsupported geometry must never
  // produce an apparently valid plan (particularly NaN comparisons).
  assert(!Plan(home, 220.0f, rows, 17, 0, 0, 28.0f, &insertion));
  assert(!Plan(home, 220.0f, 0, 0, &obstacle, 17, 28.0f, &insertion));
  assert(!Plan(home, 220.0f, 0, 1, 0, 0, 28.0f, &insertion));
  assert(!Plan(home, 220.0f, 0, 0, 0, 1, 28.0f, &insertion));
  assert(!Plan(home, 220.0f, 0, 0, 0, 0, 28.0f, 0));
  assert(!Plan(home, 220.0f, 0, 0, 0, 0, 0.0f, &insertion));
  assert(!Plan(home, 220.0f, 0, 0, 0, 0, -28.0f, &insertion));
  assert(!Plan(home, 20000.0f, 0, 0, 0, 0, 28.0f, &insertion));

  const float bad_values[] = {
    -1.0f, 20000.0f, std::numeric_limits<float>::infinity(),
    -std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::quiet_NaN()
  };
  for (unsigned i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); ++i) {
    const float bad = bad_values[i];
    assert(!Plan(home, bad, 0, 0, 0, 0, 28.0f, &insertion));
    assert(!Plan(home, 220.0f, 0, 0, 0, 0, bad, &insertion));
    for (unsigned field = 0; field < 4; ++field) {
      changed = home;
      if (field == 0) changed.x = bad;
      if (field == 1) changed.y = bad;
      if (field == 2) changed.width = bad;
      if (field == 3) changed.height = bad;
      assert(!Plan(changed, 220.0f, 0, 0, 0, 0, 28.0f, &insertion));
      assert(!Plan(home, 220.0f, &changed, 1, 0, 0, 28.0f, &insertion));
      assert(!Plan(home, 220.0f, 0, 0, &changed, 1, 28.0f, &insertion));
    }
  }
  changed = home;
  changed.width = 0.0f;
  assert(!Plan(changed, 220.0f, 0, 0, 0, 0, 28.0f, &insertion));
  changed = rows[0];
  changed.height = 0.0f;
  assert(!Plan(home, 220.0f, &changed, 1, 0, 0, 28.0f, &insertion));
  puts("Guide layout tests passed");
}
