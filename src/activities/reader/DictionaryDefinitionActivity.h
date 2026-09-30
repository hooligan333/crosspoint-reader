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
// Confirm stays upstream's in-definition lookup, #4063.)
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false, bool editable = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        content{.headword = std::move(headword), .text = std::move(definition)},
        htmlDefinition(htmlDefinition),
        editable(editable) {}

  bool enableLookup(Dictionary& dictionary, DictLocation location);

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
};
