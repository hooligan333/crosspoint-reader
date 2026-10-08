#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"
#ifdef CROSSPOINT_DICT_SECONDARY
#include <strings.h>  // strncasecmp

#include "util/DictionaryRegistry.h"
#endif

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

void indexBuildYield(void*) { vTaskDelay(1); }

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  fontId = SETTINGS.getReaderFontId();
  lookupPending = true;
  popupMsg = StrId::STR_DICT_LOOKING_UP;
  popup = Popup::Busy;
  requestUpdate();
}

#ifdef CROSSPOINT_DICT_SECONDARY
const char* DictionaryWordSelectActivity::activeDictionaryName() const {
  return usingSecondary ? SETTINGS.secondaryDictionaryName : SETTINGS.dictionaryName;
}

// Checked once per word-select session (one directory listing): a secondary
// that is unset, the same folder as the primary (compared case-insensitively,
// as FAT resolves it), or missing from the card gets no switch button. A switch
// whose target then fails to open or index also clears it (performLookup).
bool DictionaryWordSelectActivity::secondaryAvailable() {
  if (secondaryState < 0) {
    std::string basePath;
    const char* secondary = SETTINGS.secondaryDictionaryName;
    secondaryState = secondary[0] != '\0' &&
                             strncasecmp(secondary, SETTINGS.dictionaryName, sizeof(SETTINGS.dictionaryName)) != 0 &&
                             DictionaryRegistry::resolveBasePath(secondary, basePath)
                         ? 1
                         : 0;
  }
  return secondaryState == 1;
}

// The definition's switch button: the word in the other dictionary. The
// definition is already freed (result handlers run after the pop), and `dict`
// is reopened on the target, so memory stays flat. `fromPage` is where the user
// was, for the reopen if the target cannot show the word. Run from the next
// loop pass (lookupPending), like every other lookup here.
void DictionaryWordSelectActivity::switchDictionary(const int fromPage, const std::string& word) {
  if (!word.empty()) lookupText = word;
  switchFromPage = fromPage;
  usingSecondary = !usingSecondary;
  switchPending = true;
  queueRelookup();  // busy popup first: `page` is gone, see queueRelookup()
}
#endif

void DictionaryWordSelectActivity::performLookup() {
  {
    RenderLock lock;
    popupMsg = StrId::STR_DICT_LOOKING_UP;
    popup = Popup::Busy;
#ifdef CROSSPOINT_DICT_SECONDARY
    popupText[0] = '\0';
#endif
  }
#ifdef CROSSPOINT_DICT_SECONDARY
  const bool switching = switchPending;
  switchPending = false;
  const int startPage = restorePage;  // non-zero only for a failed switch's reopen
  restorePage = 0;
  // `dict` is open on the other dictionary: reopen it (and re-ask needsIndex(),
  // so a never-indexed target goes through the Indexing popup below).
  if (dictOpenAttempted && openIsSecondary != usingSecondary) dictOpenAttempted = false;
#endif
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
#ifdef CROSSPOINT_DICT_SECONDARY
    openIsSecondary = usingSecondary;
    dictOpenOk = dict.open(activeDictionaryName());
#else
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
#endif
    // needsIndex() opens and validates the .qidx sidecar, so ask it once per
    // open rather than once per word: the answer only changes when we build
    // the sidecar ourselves, which is handled below.
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  {
    RenderLock lock;
    popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  }
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    dictNeedsIndex = !ok;  // a successful build leaves the sidecar fresh; a failed one retries
  }

  std::string definition;
  std::string headword;
  Dictionary::LookupResult result = Dictionary::LookupResult::NotFound;
  const bool found = ok && dict.lookup(lookupText.c_str(), definition, headword, &result);

#ifdef CROSSPOINT_DICT_SECONDARY
  if (switching && !found) {
    // Every way the target can fail to show the word (a miss, an open or index
    // failure, a lookup error) ends the same: its popup, then -- once the popup
    // clears, from loop() -- a best-effort reopen of the previous definition on
    // the page the user was on. The reopen is an ordinary lookup (switching is
    // false), so if it fails too its own popup shows and word-select closes as
    // upstream's does: no loop, no second retry.
    const char* const target = activeDictionaryName();
    usingSecondary = !usingSecondary;  // stay with the dictionary the view came from
    restorePage = switchFromPage;
    reopenAfterPopup = true;
    if (!ok) {
      // The target would not open or index (folder or card gone, index build
      // failed): drop the switch button for the rest of the session instead of
      // failing -- or re-running a slow failing index build -- on every tap.
      // The reopen goes back to the other folder, so it never rebuilds this one.
      secondaryState = 0;
    }
    if (ok && result == Dictionary::LookupResult::NotFound) {
      // Translation strings bypass -Wformat: STR_DICT_NOT_IN must keep exactly
      // one %s and no other conversion in every catalog (host-tested in
      // test/dict_switch_layout).
      snprintf(popupText, sizeof(popupText), tr(STR_DICT_NOT_IN), target);
      popup = Popup::Error;
      popupTime = millis();
      requestUpdate();
      return;
    }
    // Open, index and lookup errors fall through to their usual popups.
  }
#endif

  // Fork: a genuine miss still opens the (blank) definition view under the
  // cleaned word, so the pencil can correct it; only real failures get a popup.
  const bool notFound = ok && !found && result == Dictionary::LookupResult::NotFound;
  if (found || notFound) {
    {
      RenderLock lock;
      popup = Popup::None;
      page.reset();
    }
    if (notFound) {
      headword = Dictionary::cleanWord(lookupText.c_str());
      if (headword.empty()) headword = lookupText;
    }
    auto activity = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                    std::move(definition),
                                                                    found && dict.definitionsAreHtml(),
                                                                    /*editable=*/true);
    // A blank miss has no entry to look words up in, so it gets no lookup state.
    // A copy of the query, not upstream's move: the fork still needs lookupText
    // here (edited-word re-lookups and the secondary-dictionary switch).
    if (!activity || (found && !activity->enableLookup(dict, std::string(lookupText)))) {
      LOG_ERR("DICT", "OOM: definition activity");
      popup = Popup::Error;
      popupMsg = StrId::STR_DICT_LOW_MEMORY;
      popupTime = millis();
      requestUpdate();
      return;
    }
#ifdef CROSSPOINT_DICT_SECONDARY
    if (secondaryAvailable()) {
      activity->setSwitchTarget(usingSecondary ? SETTINGS.dictionaryName : SETTINGS.secondaryDictionaryName);
    }
    activity->setStartPage(startPage);
#endif
    startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
#ifdef CROSSPOINT_DICT_SECONDARY
      if (const auto* switched = std::get_if<DictionarySwitchResult>(&result.data); switched && !result.isCancelled) {
        switchDictionary(switched->page, switched->word);
        return;
      }
#endif
      // An edited word: the finished definition is already freed, so the
      // re-lookup (next loop pass) holds at most one definition at a time. It
      // stays in the dictionary the edited view was showing.
      if (const auto* edited = std::get_if<KeyboardResult>(&result.data); edited && !result.isCancelled) {
        lookupText = edited->text;
        queueRelookup();
        return;
      }
      setResult(ActivityResult{result});
      finish();
    });
    return;
  }
  // Name the failure: a word that WAS found but couldn't be read is a real
  // error — and we distinguish decompression from a low-memory allocation from
  // a generic read error.
  if (!ok) {
    popup = Popup::Error;
    // An index build allocates a scan buffer, so it fails the same way lookups
    // do on a fragmented heap — name that rather than a generic error.
    switch (indexResult) {
      case Dictionary::IndexResult::LowMemory:
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::IndexResult::ReadError:
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
      case Dictionary::IndexResult::Ok:
      default:
        popupMsg = StrId::STR_DICT_ERROR;  // dict.open() failed, not the index
        break;
    }
  } else {
    switch (result) {
      case Dictionary::LookupResult::Decompress:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_DECOMPRESS_ERROR;
        break;
      case Dictionary::LookupResult::LowMemory:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_LOW_MEMORY;
        break;
      case Dictionary::LookupResult::ReadError:
      default:
        popup = Popup::Error;
        popupMsg = StrId::STR_DICT_READ_FAILED;
        break;
    }
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::loop() {
  if (lookupPending) {
    lookupPending = false;
    performLookup();
    return;
  }
  if (popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
#ifdef CROSSPOINT_DICT_SECONDARY
      if (reopenAfterPopup) {
        reopenAfterPopup = false;
        lookupPending = true;  // the previous view, on its page (restorePage)
        return;
      }
#endif
      finish();
    }
  }
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  if (page) page->render(renderer, fontId, marginLeft, marginTop);

  if (popup != Popup::None) {
#ifdef CROSSPOINT_DICT_SECONDARY
    GUI.drawPopup(renderer, popupText[0] != '\0' ? popupText : I18N.get(popupMsg));
#else
    GUI.drawPopup(renderer, I18N.get(popupMsg));
#endif
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Fork: a re-lookup requested from the definition's result handler. The
// handler runs after the definition popped, and
// performLookup() released `page` before pushing it, so the re-render the pop
// requests would otherwise paint an empty white frame (FAST refresh) ahead of
// the lookup's own popup. Put the busy popup up first, under the lock the
// render task reads it with; the lookup itself still runs from the next loop
// pass.
void DictionaryWordSelectActivity::queueRelookup() {
  {
    RenderLock lock;
    popupMsg = StrId::STR_DICT_LOOKING_UP;
    popup = Popup::Busy;
  }
  lookupPending = true;
  requestUpdate();
}
