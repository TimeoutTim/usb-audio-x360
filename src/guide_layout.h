// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef USB_AUDIO360_GUIDE_LAYOUT_H
#define USB_AUDIO360_GUIDE_LAYOUT_H

namespace guide_layout {

struct Rect {
  float x, y, width, height;
};

inline bool Bounded(float value) {
  // Comparisons reject NaN and infinity without CRT-specific float helpers.
  return value >= 0.0f && value <= 16384.0f;
}

inline bool Valid(const Rect& rect) {
  return Bounded(rect.x) && Bounded(rect.y) && Bounded(rect.width) &&
      Bounded(rect.height) && Bounded(rect.x + rect.width) &&
      Bounded(rect.y + rect.height);
}

inline bool Overlaps(const Rect& a, const Rect& b) {
  return a.width > 0.0f && a.height > 0.0f && b.width > 0.0f &&
      b.height > 0.0f && a.x < b.x + b.width && b.x < a.x + a.width &&
      a.y < b.y + b.height && b.y < a.y + a.height;
}

// All coordinates belong to the same parent. On success insert a Home-width
// row at insertion_y and move every action row down by row_height. Validate
// before mutating XUI; unexpected layouts leave the original Guide untouched.
inline bool Plan(const Rect& home, float parent_height, const Rect* rows,
                 unsigned row_count, const Rect* obstacles,
                 unsigned obstacle_count, float row_height,
                 float* insertion_y) {
  if (!insertion_y || row_count > 16 || obstacle_count > 16 ||
      (row_count && !rows) || (obstacle_count && !obstacles) ||
      !Bounded(parent_height) || parent_height <= 0.0f ||
      !Bounded(row_height) || row_height <= 0.0f || !Valid(home) ||
      home.width <= 0.0f || home.height <= 0.0f) return false;

  const Rect inserted = {home.x, home.y + home.height, home.width, row_height};
  if (!Valid(inserted) || inserted.y + inserted.height > parent_height)
    return false;

  for (unsigned i = 0; i < obstacle_count; ++i) {
    if (!Valid(obstacles[i]) || Overlaps(home, obstacles[i]) ||
        Overlaps(inserted, obstacles[i])) return false;
  }

  for (unsigned i = 0; i < row_count; ++i) {
    const Rect& row = rows[i];
    if (!Valid(row) || row.width <= 0.0f || row.height <= 0.0f ||
        row.y < inserted.y) return false;

    // Include the old location in collision checks: even a thin obstacle
    // cannot be crossed by shifting a row completely past it.
    const Rect swept = {row.x, row.y, row.width, row.height + row_height};
    if (!Valid(swept) || swept.y + swept.height > parent_height) return false;
    for (unsigned j = 0; j < obstacle_count; ++j) {
      if (Overlaps(swept, obstacles[j])) return false;
    }
    for (unsigned j = 0; j < i; ++j) {
      if (Overlaps(row, rows[j])) return false;
    }
  }

  *insertion_y = inserted.y;
  return true;
}

}  // namespace guide_layout
#endif
