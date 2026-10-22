#include "TrackingMenuActivity.h"

#ifdef CROSSPOINT_TRACKING

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "TrackingEntryActivity.h"
#include "TrackingStore.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

constexpr unsigned long LOGGED_POPUP_MS = 1500;

StrId parseErrorText(const tracking::ParseError error) {
  switch (error) {
    case tracking::ParseError::Empty:
      return StrId::STR_TRACKING_ERR_EMPTY;
    case tracking::ParseError::Mismatch:
      return StrId::STR_TRACKING_ERR_MISMATCH;
    case tracking::ParseError::MissingValue:
      return StrId::STR_TRACKING_ERR_MISSING_VALUE;
    case tracking::ParseError::Truncated:
      return StrId::STR_TRACKING_ERR_TRUNCATED;
    case tracking::ParseError::RootNotObject:
      return StrId::STR_TRACKING_ERR_ROOT;
    case tracking::ParseError::TrailingContent:
      return StrId::STR_TRACKING_ERR_TRAILING;
    case tracking::ParseError::NoItems:
      return StrId::STR_TRACKING_ERR_NO_ITEMS;
    case tracking::ParseError::ItemsNotArray:
      return StrId::STR_TRACKING_ERR_ITEMS_TYPE;
    case tracking::ParseError::TooLarge:
      return StrId::STR_TRACKING_ERR_TOO_LARGE;
    case tracking::ParseError::Syntax:
    case tracking::ParseError::None:
      break;
  }
  return StrId::STR_TRACKING_ERR_SYNTAX;
}

}  // namespace

TrackingMenuActivity::TrackingMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Tracking", renderer, mappedInput) {}

void TrackingMenuActivity::onEnter() {
  UiListActivity::onEnter();
  // Before the first render: the base's update request is deferred to the end
  // of this loop pass, so no frame reads the config while it loads.
  loadConfig();
}

void TrackingMenuActivity::onExit() {
  UiListActivity::onExit();
  config.reset();
}

const char* TrackingMenuActivity::headerTitle() const { return tr(STR_TRACKING); }

int TrackingMenuActivity::listCount() const { return state == State::List ? config->itemCount : 0; }

void TrackingMenuActivity::showMessage(const StrId headline, const char* detail) {
  state = State::Message;
  messageHeadline = headline;
  snprintf(messageDetail, sizeof(messageDetail), "%s", detail);
}

void TrackingMenuActivity::loadConfig() {
  // The SD read runs unlocked into a private Config; only publishing it and
  // the screen state it decides is done under the render lock.
  auto fresh = makeUniqueNoThrow<tracking::Config>();
  tracking::LoadStatus status = tracking::LoadStatus::OutOfMemory;
  tracking::ParseError error = tracking::ParseError::None;
  uint32_t line = 0;
  if (fresh) {
    status = tracking::loadConfig(*fresh, error, line);
  } else {
    LOG_ERR("TRK", "OOM: config (%u bytes)", static_cast<unsigned>(sizeof(tracking::Config)));
  }
  RenderLock lock(*this);
  config = std::move(fresh);
  publishConfig(status, error, line);
}

void TrackingMenuActivity::publishConfig(const tracking::LoadStatus status, const tracking::ParseError error,
                                         const uint32_t line) {
  switch (status) {
    case tracking::LoadStatus::Ok:
      break;
    case tracking::LoadStatus::Missing:
      showMessage(StrId::STR_TRACKING_NO_CONFIG, tr(STR_TRACKING_NO_CONFIG_HINT));
      return;
    case tracking::LoadStatus::ReadFailed:
      showMessage(StrId::STR_TRACKING_CONFIG_ERROR, tr(STR_TRACKING_ERR_READ));
      return;
    case tracking::LoadStatus::OutOfMemory:
      showMessage(StrId::STR_TRACKING_CONFIG_ERROR, tr(STR_TRACKING_ERR_MEMORY));
      return;
    case tracking::LoadStatus::ParseFailed: {
      const char* reason = I18N.get(parseErrorText(error));
      if (line > 0) {
        char detail[sizeof(messageDetail)];
        snprintf(detail, sizeof(detail), tr(STR_TRACKING_ERR_LINE_FORMAT), reason, static_cast<unsigned>(line));
        showMessage(StrId::STR_TRACKING_CONFIG_ERROR, detail);
      } else {
        showMessage(StrId::STR_TRACKING_CONFIG_ERROR, reason);
      }
      return;
    }
  }

  skippedNotice[0] = '\0';
  if (config->skippedCount > 0) {
    snprintf(skippedNotice, sizeof(skippedNotice), tr(STR_TRACKING_SKIPPED_FORMAT),
             static_cast<int>(config->skippedCount));
  }
  if (config->itemCount == 0) {
    showMessage(StrId::STR_TRACKING_NO_ITEMS,
                config->skippedCount > 0 ? skippedNotice : tr(STR_TRACKING_NO_ITEMS_HINT));
    return;
  }
  for (int i = 0; i < config->itemCount; i++) {
    rows[i] = fui::ListItem{};
    rows[i].label = config->items[i].label;
    rows[i].actionValue = static_cast<int16_t>(i);
  }
  // The skipped-items notice rides above the first row, so it is seen on
  // every visit until the file is fixed.
  if (skippedNotice[0] != '\0') rows[0].sectionHeading = skippedNotice;
  state = State::List;
}

void TrackingMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (state == State::Message) {
    auto headline = screen.theme().bodyText;
    headline.bold = true;
    headline.align = fui::TextAlign::Center;
    headline.maxLines = 2;
    auto detail = screen.theme().bodyText;
    detail.align = fui::TextAlign::Center;
    detail.maxLines = 4;
    const int16_t headlineH = screen.target().lineHeight(headline.font);
    const int16_t detailH = screen.target().lineHeight(detail.font);
    const fui::Rect body = screen.body();
    const int16_t pad = screen.theme().spaceLg;
    screen.spacer(static_cast<int16_t>(body.height / 3));
    fui::Rect r = screen.takeTop(static_cast<int16_t>(headlineH * 2), screen.theme().spaceMd);
    r.x = static_cast<int16_t>(r.x + pad);
    r.width = static_cast<int16_t>(r.width - 2 * pad);
    screen.target().text(r, I18N.get(messageHeadline), headline);
    r = screen.takeTop(static_cast<int16_t>(detailH * 4));
    r.x = static_cast<int16_t>(r.x + pad);
    r.width = static_cast<int16_t>(r.width - 2 * pad);
    screen.target().text(r, messageDetail, detail);
    return;
  }
  if (state != State::List) return;

  fui::ListProps props;
  props.items = rows;
  props.count = static_cast<uint16_t>(config->itemCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}

void TrackingMenuActivity::activateIndex(const int index) {
  if (state != State::List || index < 0 || index >= config->itemCount) return;
  nav.selected = index;
  app.clearTapFlash();  // the tap leaves this screen
  auto entry = makeUniqueNoThrow<TrackingEntryActivity>(renderer, mappedInput, *config, config->items[index]);
  if (!entry) {
    LOG_ERR("TRK", "OOM: entry activity (%u bytes)", static_cast<unsigned>(sizeof(TrackingEntryActivity)));
    return;
  }
  lastOpened = index;
  {
    RenderLock lock(*this);
    loggedPopupVisible = false;
  }
  startActivityForResult(std::move(entry), [this](const ActivityResult& result) { onEntryDone(result); });
}

void TrackingMenuActivity::onEntryDone(const ActivityResult& result) {
  if (result.isCancelled || state != State::List || lastOpened < 0 || lastOpened >= config->itemCount) return;
  RenderLock lock(*this);
  snprintf(loggedPopupText, sizeof(loggedPopupText), tr(STR_TRACKING_LOGGED_FORMAT), config->items[lastOpened].label);
  loggedPopupVisible = true;
  loggedPopupShownMs = millis();
}

void TrackingMenuActivity::onBackButton() { onGoHome(HomeMenuItem::TRACKING); }

bool TrackingMenuActivity::handleHomeGesture() {
  onGoHome(HomeMenuItem::TRACKING);
  return true;
}

void TrackingMenuActivity::loop() {
  if (loggedPopupVisible && millis() - loggedPopupShownMs >= LOGGED_POPUP_MS) {
    {
      RenderLock lock(*this);
      loggedPopupVisible = false;
    }
    requestUpdate();
  }
  UiListActivity::loop();
}

void TrackingMenuActivity::drawFooter() {
  const bool list = state == State::List;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), list ? tr(STR_SELECT) : "", list ? tr(STR_DIR_UP) : "",
                                            list ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Drawn last so it sits over the list; render() flips the buffer next.
  if (loggedPopupVisible) GUI.drawPopup(renderer, loggedPopupText);
}

#endif  // CROSSPOINT_TRACKING
