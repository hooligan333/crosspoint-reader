// Host tests for the secondary dictionary switch button (DictSwitchLayout.h):
// the label/hit-box caps that keep page-turn taps on the top strip, the page
// carried through a failed switch's reopen, and the STR_DICT_NOT_IN format
// contract (translation catalogs are a printf surface -Wformat never sees).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "DictSwitchLayout.h"

namespace dsl = dict_switch_layout;

namespace {

// Portrait 480 px row, as DictionaryDefinitionActivity lays it out: 24 px
// glyph in a 44 px square (10 px inset) at x = 10, label 6 px after the glyph,
// pencil tap target at 480 - 20 - 24 - 10.
constexpr int ROW_X = 0;
constexpr int ROW_W = 480;
constexpr int PAD = 10;
constexpr int BOX_X = 10;
constexpr int LABEL_X = BOX_X + PAD + 24 + 6;
constexpr int PENCIL_X = ROW_W - 20 - 24 - PAD;
constexpr int MIN_SIZE = 44;

}  // namespace

TEST(DictSwitchLayout, LabelStopsAtRowMidpoint) {
  // The midpoint (240) is left of the pencil (426): it is the cap.
  EXPECT_EQ(dsl::labelMaxWidth(LABEL_X, PAD, PENCIL_X, ROW_X, ROW_W), 240 - PAD - LABEL_X);
}

TEST(DictSwitchLayout, LabelStopsAtPencilWhenItIsCloser) {
  // A narrow row whose pencil sits left of the midpoint.
  EXPECT_EQ(dsl::labelMaxWidth(LABEL_X, PAD, 150, ROW_X, 400), 150 - PAD - LABEL_X);
}

TEST(DictSwitchLayout, LabelHonoursLandscapeGutter) {
  // A left button-hint gutter shifts the row and its midpoint.
  const int rowX = 40;
  const int rowW = 760;
  const int labelX = rowX + LABEL_X;
  EXPECT_EQ(dsl::labelMaxWidth(labelX, PAD, rowX + rowW - 54, rowX, rowW), rowX + rowW / 2 - PAD - labelX);
}

TEST(DictSwitchLayout, HitBoxEndsAtDrawnLabelPlusPadding) {
  const int widest = dsl::labelMaxWidth(LABEL_X, PAD, PENCIL_X, ROW_X, ROW_W);
  // Widest label: the hit box ends exactly at the midpoint, not at the pencil.
  EXPECT_EQ(BOX_X + dsl::hitWidth(BOX_X, LABEL_X, widest, PAD, MIN_SIZE, PENCIL_X), ROW_W / 2);
  // A short label: glyph, gap, label and padding only.
  EXPECT_EQ(dsl::hitWidth(BOX_X, LABEL_X, 30, PAD, MIN_SIZE, PENCIL_X), LABEL_X + 30 + PAD - BOX_X);
}

TEST(DictSwitchLayout, HitBoxKeepsMinimumTarget) {
  EXPECT_EQ(dsl::hitWidth(BOX_X, LABEL_X, 0, PAD, MIN_SIZE, PENCIL_X), MIN_SIZE);
  EXPECT_EQ(dsl::hitWidth(BOX_X, BOX_X + 2, 1, 1, MIN_SIZE, PENCIL_X), MIN_SIZE);
}

TEST(DictSwitchLayout, HitBoxNeverReachesPencil) {
  EXPECT_EQ(dsl::hitWidth(BOX_X, LABEL_X, 1000, PAD, MIN_SIZE, PENCIL_X), PENCIL_X - BOX_X);
}

TEST(DictSwitchLayout, ClampPage) {
  EXPECT_EQ(dsl::clampPage(0, 5), 0);
  EXPECT_EQ(dsl::clampPage(3, 5), 3);
  EXPECT_EQ(dsl::clampPage(4, 5), 4);
  EXPECT_EQ(dsl::clampPage(5, 5), 4);  // re-layout came out shorter
  EXPECT_EQ(dsl::clampPage(9, 1), 0);  // single-page definition
  EXPECT_EQ(dsl::clampPage(2, 0), 0);  // degenerate page count
  EXPECT_EQ(dsl::clampPage(-1, 5), 0);
}

// Every catalog that defines STR_DICT_NOT_IN must keep it a one-%s format:
// the word-select popup snprintf()s the target folder name into it.
TEST(DictSwitchLayout, NotInStringHasExactlyOnePercentS) {
  namespace fs = std::filesystem;
  const std::string key = "STR_DICT_NOT_IN:";
  int catalogs = 0;
  for (const auto& entry : fs::directory_iterator(TRANSLATIONS_DIR)) {
    if (entry.path().extension() != ".yaml") continue;
    std::ifstream in(entry.path());
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind(key, 0) != 0) continue;
      catalogs++;
      int conversions = 0;
      bool onlyPercentS = true;
      for (size_t i = 0; i < line.size(); i++) {
        if (line[i] != '%') continue;
        if (i + 1 < line.size() && line[i + 1] == '%') {
          i++;  // literal percent
          continue;
        }
        conversions++;
        if (i + 1 >= line.size() || line[i + 1] != 's') onlyPercentS = false;
      }
      EXPECT_EQ(conversions, 1) << entry.path().filename() << ": " << line;
      EXPECT_TRUE(onlyPercentS) << entry.path().filename() << ": " << line;
    }
  }
  EXPECT_GE(catalogs, 1);  // English defines it
}
