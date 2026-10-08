#pragma once

#include <algorithm>

// Pure geometry and paging helpers for the definition view's secondary
// dictionary switch button (CROSSPOINT_DICT_SECONDARY). Host-tested in
// test/dict_switch_layout.
namespace dict_switch_layout {

// Room for the button's label, which starts at `labelX`. The button (label
// plus `pad` of touch padding) stops short of the pencil's tap target and of
// the row's horizontal midpoint, so a long dictionary name cannot turn the
// whole top strip into a switch target. <= 0 means no room for a label.
inline int labelMaxWidth(const int labelX, const int pad, const int pencilX, const int rowX, const int rowWidth) {
  return std::min(pencilX, rowX + rowWidth / 2) - pad - labelX;
}

// Width of the tap target that starts at `x`: up to the end of the drawn label
// (at `labelX`, `labelWidth` wide) plus `pad` of touch padding. Never narrower
// than `minSize` (the glyph's square), never past `limitX` (the pencil's tap
// target).
inline int hitWidth(const int x, const int labelX, const int labelWidth, const int pad, const int minSize,
                    const int limitX) {
  const int drawnRight = labelWidth > 0 ? labelX + labelWidth + pad : x;
  return std::min(std::max(drawnRight - x, minSize), limitX - x);
}

// A page carried over from an earlier view of the same definition, clamped to
// this layout's page count (a reopen lays out identically, but never index
// past the end if it does not).
inline int clampPage(const int page, const int totalPages) {
  if (totalPages <= 1 || page <= 0) return 0;
  return std::min(page, totalPages - 1);
}

}  // namespace dict_switch_layout
