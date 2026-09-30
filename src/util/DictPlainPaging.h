#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Height-based pagination for the dictionary's plain word-wrap path. A text
// line costs `lineHeight`; a blank line (len == 0) is a paragraph gap costing
// half of that. A gap that would open a page costs nothing and draws nothing.
// Runs of blank lines are expected to be collapsed by the wrapper already.
namespace dict_plain_paging {

inline int gapHeight(const int lineHeight) { return lineHeight / 2; }

// Vertical advance of line `i` on a page that starts at `pageStart`.
template <typename LineT>
int lineAdvance(const std::vector<LineT>& lines, const size_t i, const size_t pageStart, const int lineHeight) {
  if (lines[i].len != 0) return lineHeight;
  return i == pageStart ? 0 : gapHeight(lineHeight);
}

// Fills `pageStarts` with the first line index of every page (always at least
// one entry, 0, so an empty body is a single blank page). A text line always
// fits on an otherwise empty page, whatever `bodyHeight` is.
template <typename LineT>
void paginate(const std::vector<LineT>& lines, const int lineHeight, const int bodyHeight,
              std::vector<uint32_t>& pageStarts) {
  pageStarts.clear();
  const int perPage = lineHeight > 0 && bodyHeight > lineHeight ? bodyHeight / lineHeight : 1;
  pageStarts.reserve(lines.size() / static_cast<size_t>(perPage) + 2);
  pageStarts.push_back(0);
  size_t pageStart = 0;
  int y = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    int advance = lineAdvance(lines, i, pageStart, lineHeight);
    if (y > 0 && y + advance > bodyHeight) {
      pageStart = i;
      pageStarts.push_back(static_cast<uint32_t>(i));
      y = 0;
      advance = lineAdvance(lines, i, pageStart, lineHeight);
    }
    y += advance;
  }
}

}  // namespace dict_plain_paging
