#pragma once
#ifdef CROSSPOINT_RSS_SYNC

#include "activities/UiListActivity.h"

/**
 * Settings > System > Configure RSS Feed.
 *
 * Two fields, both edited on the on-screen keyboard and persisted straight
 * into CrossPointSettings (the same field-at-a-time save OpdsSettingsActivity
 * uses, so a partially configured feed survives navigation and power loss).
 */
class RssSettingsActivity final : public UiListActivity {
 public:
  explicit RssSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

 private:
  static constexpr int ROW_URL = 0;
  static constexpr int ROW_FOLDER = 1;
  static constexpr int ROW_COUNT = 2;

  int listCount() const override { return ROW_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // Keyboard result for whichever row is selected. A real member (rather than
  // a capturing lambda per field) keeps one trivial std::function handler in
  // flash instead of one full copy of the store logic per field.
  void onFieldEntered(const ActivityResult& result);

  // Labels/actionValues are set once in the constructor; buildScreen() only
  // refreshes the value pointers, which point at the settings fields
  // themselves, so no per-repaint strings are built.
  freeink::ui::ListItem fieldRowItems[ROW_COUNT]{};
};

#endif  // CROSSPOINT_RSS_SYNC
