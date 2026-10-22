#pragma once
#ifdef CROSSPOINT_TRACKING

#include <I18n.h>
#include <TrackingConfig.h>

#include <cstdint>
#include <memory>

#include "TrackingStore.h"
#include "activities/UiListActivity.h"

/**
 * Home > Tracking: one row per item in /tracking/config.json, in file order.
 * The file is re-read on every entry so an edit made on a PC always applies.
 * A row opens TrackingEntryActivity for that item; a confirmed entry comes back
 * as a non-cancelled result and is acknowledged with a short "Logged" popup.
 * A missing or broken config replaces the list with a message saying what
 * failed.
 */
class TrackingMenuActivity final : public UiListActivity {
 public:
  TrackingMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool handleHomeGesture() override;

 private:
  enum class State : uint8_t { Loading, List, Message };

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  void loadConfig();
  // Caller holds the RenderLock: sets everything the render task reads.
  void publishConfig(tracking::LoadStatus status, tracking::ParseError error, uint32_t line);
  void showMessage(StrId headline, const char* detail);
  void onEntryDone(const ActivityResult& result);

  // ~7 KB, allocated once per visit and freed on exit: the file is re-read on
  // every entry by design, so nothing is cached across visits.
  std::unique_ptr<tracking::Config> config;
  // Labels point into `config`; rebuilt only when the config is (re)loaded.
  freeink::ui::ListItem rows[tracking::MAX_ITEMS]{};

  State state = State::Loading;
  StrId messageHeadline = StrId::STR_NONE_OPT;
  char messageDetail[96] = {};
  char skippedNotice[48] = {};

  int lastOpened = -1;
  bool loggedPopupVisible = false;
  unsigned long loggedPopupShownMs = 0;
  char loggedPopupText[tracking::MAX_LABEL_BYTES + 24] = {};
};

#endif  // CROSSPOINT_TRACKING
