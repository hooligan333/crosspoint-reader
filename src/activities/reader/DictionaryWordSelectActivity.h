#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
  };

  enum class Popup : uint8_t { None, Busy, Error };

  void extractWords();
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void performLookup(const char* word);
#ifdef CROSSPOINT_DICT_SECONDARY
  void switchDictionary(int fromPage);
  const char* activeDictionaryName() const;
  bool secondaryAvailable();
#endif
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;
  unsigned long lastHorizontalMoveTime = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

#ifdef CROSSPOINT_DICT_SECONDARY
  // Secondary dictionary (settings: secondaryDictionaryName). One Dictionary
  // stays open at a time: a switch reopens `dict` on the other folder (open()
  // only resolves paths; each folder keeps its own .qidx/.sidx sidecars, so
  // flipping back is cheap). Per lookup session: every new word selection
  // starts in the primary.
  bool usingSecondary = false;    // which dictionary lookups go to
  bool openIsSecondary = false;   // which one `dict` was last opened on
  bool switchPending = false;     // the next performLookup() is a switch
  bool reopenAfterPopup = false;  // a switch failed: reopen the previous view once the popup clears
  int switchFromPage = 0;         // page the switched-from view was on
  int restorePage = 0;            // page the next view opens on (only a failed switch's reopen sets it)
  // -1 unknown, 0 none/unusable (also set when a switch's target fails to open
  // or index), 1 configured and resolvable
  int8_t secondaryState = -1;
  // The word the current definition was looked up with (the page token or the
  // edited word, never the matched headword), so a switch applies the other
  // dictionary's own synonym and stem fallbacks to it.
  std::string sessionWord;
  // Formatted popup text ("Not in <folder>"); empty means popupMsg is shown.
  char popupText[64] = {};
#endif

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;
};
