#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
//
// `editable` (dictionary lookups only, not the plugin README viewer) adds a
// pencil button under the page counter, and Confirm, that open the keyboard
// on the headword. A confirmed edit finishes this activity with a
// KeyboardResult carrying the new word; the launcher owns the Dictionary and
// runs the new lookup once this definition has been freed. The secondary
// dictionary's switch button (CROSSPOINT_DICT_SECONDARY) round-trips the same
// way, with a DictionarySwitchResult.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false, bool editable = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition),
        editable(editable) {}

#ifdef CROSSPOINT_DICT_SECONDARY
  // Show the switch button (editable views only, touch boards) labelled with
  // the dictionary a tap flips to; empty hides it. A tap finishes this activity
  // with a DictionarySwitchResult carrying the current page, and the launcher
  // runs the lookup.
  void setSwitchTarget(std::string targetName) { switchTarget = std::move(targetName); }
  // Open on this page instead of the first (clamped to the laid-out pages): a
  // failed switch reopens the previous definition where the user was.
  void setStartPage(const int page) { startPage = page; }
#endif

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  // Header/body column: left edge, top edge and width, outside the
  // orientation's button-hint gutters.
  struct ContentFrame {
    int x;
    int y;
    int width;
  };

  // Pencil tap target (logical coords), larger than the glyph it centers.
  struct EditBox {
    int x;
    int y;
    int size;
  };

#ifdef CROSSPOINT_DICT_SECONDARY
  // Switch button tap target: the swap glyph plus the target's name, left of
  // the pencil on the same row.
  struct SwitchBox {
    int x;
    int y;
    int width;
    int height;
  };
  SwitchBox switchBox() const;
  bool hasSwitchButton() const;
#endif

  ContentFrame contentFrame() const;
  EditBox editBox() const;
  void openEditor();
  BodyArea bodyArea() const;
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;
  void nextPage();
  void previousPage();

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  const bool htmlDefinition;
  const bool editable;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  // Plain path: first `lines` index of each page (height-based, see
  // util/DictPlainPaging.h); size() == totalPages.
  std::vector<uint32_t> pageStarts;
  int currentPage = 0;
  int totalPages = 1;
  ButtonNavigator buttonNavigator;
#ifdef CROSSPOINT_DICT_SECONDARY
  int startPage = 0;
  std::string switchTarget;
  // switchTarget ellipsized to the row in onEnter(), and its drawn width.
  std::string switchLabel;
  int switchLabelWidth = 0;
#endif
};
