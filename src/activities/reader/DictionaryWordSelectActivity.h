#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Controller for dictionary lookup: renders the background page, runs the
// lookup against the active StarDict dictionary, and either opens
// DictionaryDefinitionActivity or displays a status popup (Looking up,
// Indexing, Not found, Error).
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string lookupText = {})
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        lookupText(std::move(lookupText)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Popup : uint8_t { None, Busy, Error };

  void queueRelookup();
  void performLookup();
#ifdef CROSSPOINT_DICT_SECONDARY
  void switchDictionary(int fromPage, const std::string& word);
  const char* activeDictionaryName() const;
  bool secondaryAvailable();
#endif

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;

  std::string lookupText;
  bool lookupPending = false;
  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

#ifdef CROSSPOINT_DICT_SECONDARY
  // Secondary dictionary (settings: secondaryDictionaryName). One Dictionary
  // stays open at a time: a switch reopens `dict` on the other folder (open()
  // only resolves paths; each folder keeps its own .qidx/.sidx sidecars, so
  // flipping back is cheap). Every new word selection starts in the primary:
  // a fresh word-select is constructed per selection, so usingSecondary starts
  // false. `lookupText` is the word the current definition was looked up with
  // (the selection, the edited word, or a recursive lookup's word handed back
  // by a switch -- never the matched headword), so a switch applies the other
  // dictionary's own synonym and stem fallbacks to it.
  bool usingSecondary = false;    // which dictionary lookups go to
  bool openIsSecondary = false;   // which one `dict` was last opened on
  bool switchPending = false;     // the next performLookup() is a switch
  bool reopenAfterPopup = false;  // a switch failed: reopen the previous view once the popup clears
  int switchFromPage = 0;         // page the switched-from view was on
  int restorePage = 0;            // page the next view opens on (only a failed switch's reopen sets it)
  // -1 unknown, 0 none/unusable (also set when a switch's target fails to open
  // or index), 1 configured and resolvable
  int8_t secondaryState = -1;
  // Formatted popup text ("Not in <folder>"); empty means popupMsg is shown.
  char popupText[64] = {};
#endif

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
};
