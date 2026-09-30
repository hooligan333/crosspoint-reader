#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "components/UITheme.h"

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

void DictionaryWordSelectActivity::performLookup() {
  {
    RenderLock lock;
    popupMsg = StrId::STR_DICT_LOOKING_UP;
    popup = Popup::Busy;
  }
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
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
    startActivityForResult(std::move(activity), [this](const ActivityResult& result) {
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
    GUI.drawPopup(renderer, I18N.get(popupMsg));
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
