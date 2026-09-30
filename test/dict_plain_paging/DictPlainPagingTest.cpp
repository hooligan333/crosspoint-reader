// Host tests for the dictionary plain path's height-based pagination: text
// lines cost a full line height, blank lines a half-height paragraph gap.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "DictPlainPaging.h"

namespace {

struct Line {
  uint32_t start;
  uint16_t len;
};

// 'T' = text line, '-' = blank line.
std::vector<Line> makeLines(const char* shape) {
  std::vector<Line> lines;
  for (const char* c = shape; *c; c++) lines.push_back({0, static_cast<uint16_t>(*c == 'T' ? 5 : 0)});
  return lines;
}

std::vector<uint32_t> pages(const char* shape, int lineHeight, int bodyHeight) {
  std::vector<uint32_t> starts;
  dict_plain_paging::paginate(makeLines(shape), lineHeight, bodyHeight, starts);
  return starts;
}

using Starts = std::vector<uint32_t>;

TEST(DictPlainPaging, EmptyBodyIsOneBlankPage) { EXPECT_EQ(pages("", 20, 100), Starts{0}); }

TEST(DictPlainPaging, SingleLineIsOnePage) { EXPECT_EQ(pages("T", 20, 100), Starts{0}); }

TEST(DictPlainPaging, TextOnlyMatchesUniformLinesPerPage) {
  // 100 / 20 = 5 lines per page, as the fixed-count pager had it.
  EXPECT_EQ(pages("TTTTT", 20, 100), Starts{0});
  EXPECT_EQ(pages("TTTTTT", 20, 100), (Starts{0, 5}));
  EXPECT_EQ(pages("TTTTTTTTTTT", 20, 100), (Starts{0, 5, 10}));
  // Non-multiple body height: the remainder is left unused.
  EXPECT_EQ(pages("TTTTTT", 20, 119), (Starts{0, 5}));
}

TEST(DictPlainPaging, BlankLinesCostHalfAHeight) {
  // T - T - T : 20+10+20+10+20 = 80 <= 100, one page.
  EXPECT_EQ(pages("T-T-T", 20, 100), Starts{0});
  // T - T - T - T : 20+10+20+10+20+10+20 = 110 > 100 -> last T overflows.
  // The gap before it still fits (90), so page 2 starts at the text line.
  EXPECT_EQ(pages("T-T-T-T", 20, 100), (Starts{0, 6}));
  // 20+20+10+20+20+10+20 = 120: seven lines fit where the fixed pager (six
  // per page) would have split.
  EXPECT_EQ(pages("TT-TT-T", 20, 120), Starts{0});
}

TEST(DictPlainPaging, GapOpeningAPageIsFree) {
  // T T T T - T : the gap lands at y=80+10=90 (fits), T would reach 110.
  EXPECT_EQ(pages("TTTT-T", 20, 100), (Starts{0, 5}));
  // T T T T T - T : the gap no longer fits (100+10), so it opens page 2 at no
  // cost and the following text sits at the top.
  EXPECT_EQ(pages("TTTTT-T", 20, 100), (Starts{0, 5}));
  const auto lines = makeLines("TTTTT-T");
  EXPECT_EQ(dict_plain_paging::lineAdvance(lines, 5, 5, 20), 0);
  EXPECT_EQ(dict_plain_paging::lineAdvance(lines, 5, 0, 20), 10);
  EXPECT_EQ(dict_plain_paging::lineAdvance(lines, 6, 5, 20), 20);
}

TEST(DictPlainPaging, OddLineHeightRoundsGapDown) {
  // 21 + 10 + 21 + 10 + 21 = 83 <= 83.
  EXPECT_EQ(pages("T-T-T", 21, 83), Starts{0});
  EXPECT_EQ(pages("T-T-T", 21, 82), (Starts{0, 4}));
}

TEST(DictPlainPaging, BodyShorterThanALineStillProgresses) {
  EXPECT_EQ(pages("TTT", 20, 10), (Starts{0, 1, 2}));
  EXPECT_EQ(pages("T-T", 20, 10), (Starts{0, 1}));
}

}  // namespace
