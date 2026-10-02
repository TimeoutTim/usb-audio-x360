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

// Fill an existing gap without relocating native controls. Authored Guide tab
// widths animate, so validate against the adjacent rows and parent height.
inline bool PlanGap(const Rect& above, const Rect& below, float parent_height,
                    const Rect* obstacles, unsigned count, float row_height,
                    float* insertion_y) {
  if (!insertion_y || count > 32 || (count && !obstacles) ||
      !Valid(above) || !Valid(below) || above.width <= 0 || above.height <= 0 ||
      below.width != above.width || below.x != above.x || below.height <= 0 ||
      !Bounded(parent_height) || !Bounded(row_height) || row_height <= 0 ||
      below.y + below.height > parent_height) return false;
  const float gap = below.y - (above.y + above.height);
  if (gap < row_height) return false;
  const Rect row = {above.x, above.y + above.height + (gap - row_height) / 2,
                    above.width, row_height};
  if (!Valid(row)) return false;
  for (unsigned i = 0; i < count; ++i)
    if (!Valid(obstacles[i]) || Overlaps(row, obstacles[i])) return false;
  *insertion_y = row.y;
  return true;
}

}  // namespace guide_layout
#endif
