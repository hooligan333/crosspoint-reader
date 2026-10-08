#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ClipSelectionActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
//
// `editable` (dictionary lookups only, not the plugin README viewer) adds a
// pencil button (touch boards: an action row between the header and the body)
// and a long-held Confirm (any Confirm when there is nothing to look up) that
// open the keyboard on the headword. A confirmed edit finishes this activity
// with a KeyboardResult carrying the new word; the launcher owns the Dictionary
// and runs the new lookup once this definition has been freed. (Fork r8: short
// Confirm stays upstream's in-definition lookup, #4063.) The secondary
// dictionary's switch button (CROSSPOINT_DICT_SECONDARY) round-trips the same
// way, with a DictionarySwitchResult.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false, bool editable = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        content{.headword = std::move(headword), .text = std::move(definition)},
        htmlDefinition(htmlDefinition),
        editable(editable) {}

  bool enableLookup(Dictionary& dictionary, DictLocation location);

#ifdef CROSSPOINT_DICT_SECONDARY
  // Show the switch button (editable views only, touch boards) labelled with
  // the dictionary a tap flips to; empty hides it. A tap finishes this activity
  // with a DictionarySwitchResult carrying the current page (and, for a view
  // reached by an in-definition lookup, that lookup's word), and the launcher
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
  struct PlainSelectionLine;

  struct Line {
    uint32_t start;
    uint16_t len;
    bool bold;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };

  // Pencil tap target (logical coords), larger than the glyph it centers.
  struct EditBox {
    int x;
    int y;
    int size;
  };

  BodyArea bodyArea() const;
  // Height of the touch action row (pencil) above the body; 0 when not shown.
  int actionRowHeight() const;
  EditBox editBox() const;
  void openEditor();
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

  struct Definition {
    std::string headword;
    std::string text;
    std::vector<std::unique_ptr<Page>> pages;
    std::vector<Line> lines;
    // Plain path: first `lines` index of each page (height-based, see
    // util/DictPlainPaging.h); size() == totalPages.
    std::vector<uint32_t> pageStarts;
    int currentPage = 0;
    int totalPages = 1;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;
#ifdef CROSSPOINT_DICT_SECONDARY
    // The word this view was looked up with by an in-definition (recursive)
    // lookup; empty for the root view (word-select holds that word).
    std::string queryWord;
#endif

    void releasePayload() {
      std::string{}.swap(text);
      std::vector<std::unique_ptr<Page>>{}.swap(pages);
      std::vector<Line>{}.swap(lines);
      std::vector<uint32_t>{}.swap(pageStarts);
    }

    // [first, last) `lines` indices of plain page `page`.
    void pageLines(const int page, size_t& first, size_t& last) const {
      first = pageStarts[page];
      last = static_cast<size_t>(page) + 1 < pageStarts.size() ? pageStarts[page + 1] : lines.size();
    }
  };

  struct HistoryEntry {
    std::string headword;
    DictLocation location;
    int page = 0;
    int selectionPage = 0;
    ClipSelectionActivity::Selection selection;
    bool styled = false;
#ifdef CROSSPOINT_DICT_SECONDARY
    std::string queryWord;  // Definition::queryWord of the stacked view
#endif
  };

  struct LookupState {
    explicit LookupState(Dictionary& dictionary, DictLocation location) : dictionary(dictionary), location(location) {}
    Dictionary& dictionary;  // Owned by the retained DictionaryWordSelectActivity parent.
    DictLocation location;
    std::array<HistoryEntry, 16> history;
    size_t depth = 0;
    std::string pendingWord;
    std::vector<std::unique_ptr<Page>> selectionPages;
  };

  bool prepareDefinition(Definition& next, bool forcePlain = false);
  bool layoutHtmlPages(Definition& next);
  bool wrapText(Definition& next);
  bool reserveLines(Definition& next, size_t count);
  int measureSpan(int fontId, const char* text, size_t len, EpdFontFamily::Style family = EpdFontFamily::REGULAR) const;
  void drawBody(int fontId, int x, int startY, int currentPage) const;
  void startSelection(int x = -1, int y = -1);
  bool buildPlainSelectionPages();
  void navigateDefinition(bool back);
  bool restoreCurrentDefinition(bool styled);
  void showMessage(StrId message);
  void nextPage();
  void previousPage();

  Definition content;
  const bool htmlDefinition;
  const bool editable;
  std::unique_ptr<LookupState> lookup;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  unsigned long messageTime = 0;
  bool messageVisible = false;
  bool initialLayoutFailed = false;
  ButtonNavigator buttonNavigator;
#ifdef CROSSPOINT_DICT_SECONDARY
  int startPage = 0;
  std::string switchTarget;
  // switchTarget ellipsized to the row in onEnter(), and its drawn width.
  std::string switchLabel;
  int switchLabelWidth = 0;
#endif
};
