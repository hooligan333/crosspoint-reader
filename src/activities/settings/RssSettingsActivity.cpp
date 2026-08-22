#include "RssSettingsActivity.h"

#ifdef CROSSPOINT_RSS_SYNC

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/RssFolder.h"

namespace fui = freeink::ui;

RssSettingsActivity::RssSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("RssSettings", renderer, mappedInput) {
  fieldRowItems[ROW_URL].label = I18N.get(StrId::STR_RSS_FEED_URL);
  fieldRowItems[ROW_FOLDER].label = I18N.get(StrId::STR_RSS_DEST_FOLDER);
  for (int i = 0; i < ROW_COUNT; i++) {
    fieldRowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* RssSettingsActivity::headerTitle() const { return tr(STR_RSS_CONFIGURE); }

// Both fields go through ONE keyboard launch and ONE result handler, keyed on
// the selected row: a lambda per field would mint a full copy of the store
// logic in flash, and this screen's whole job is two strings.
void RssSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens the keyboard; a lingering flash would gray an unrelated
  // row on the way back.
  app.clearTapFlash();

  const bool isUrl = index == ROW_URL;
  const char* const field = isUrl ? SETTINGS.rssFeedUrl : SETTINGS.rssDestFolder;
  // Plain http only (see RssSyncActivity::fetchFeed), so an empty URL field
  // prefills http://, not https://.
  const std::string prefill = (isUrl && field[0] == '\0') ? "http://" : std::string(field);

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(
          renderer, mappedInput, I18N.get(isUrl ? StrId::STR_RSS_FEED_URL : StrId::STR_RSS_DEST_FOLDER), prefill,
          (isUrl ? sizeof(SETTINGS.rssFeedUrl) : sizeof(SETTINGS.rssDestFolder)) - 1,
          isUrl ? InputType::Url : InputType::Text),
      [this](const ActivityResult& result) { onFieldEntered(result); });
}

void RssSettingsActivity::onFieldEntered(const ActivityResult& result) {
  if (result.isCancelled) return;
  const std::string& text = std::get<KeyboardResult>(result.data).text;

  const bool isUrl = nav.selected == ROW_URL;
  char* const field = isUrl ? SETTINGS.rssFeedUrl : SETTINGS.rssDestFolder;
  const size_t size = isUrl ? sizeof(SETTINGS.rssFeedUrl) : sizeof(SETTINGS.rssDestFolder);
  // A bare scheme left over from the prefill means "not set".
  const std::string value =
      isUrl ? (text == "http://" || text == "https://" ? std::string() : text) : normalizeRssFolder(text);

  strncpy(field, value.c_str(), size - 1);
  field[size - 1] = '\0';
  SETTINGS.saveToFile();
  requestUpdate();
}

void RssSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Values point straight at the settings fields — no per-repaint strings.
  fieldRowItems[ROW_URL].value = SETTINGS.rssFeedUrl[0] ? SETTINGS.rssFeedUrl : tr(STR_NOT_SET);
  fieldRowItems[ROW_FOLDER].value = SETTINGS.rssDestFolder;

  fui::ListProps props;
  props.items = fieldRowItems;
  props.count = static_cast<uint16_t>(ROW_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // CROSSPOINT_RSS_SYNC
