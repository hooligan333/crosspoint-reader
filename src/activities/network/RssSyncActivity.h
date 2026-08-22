#pragma once
#ifdef CROSSPOINT_RSS_SYNC

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

struct RssItem;

/**
 * Sync RSS Feed: mirrors a personal LAN RSS 2.0 feed's enclosures into a
 * folder on the SD card.
 *
 * The feed's <item> enclosures are ready-made .epub files (the server does any
 * conversion); this screen only downloads and deletes. The checkbox list is
 * the DESIRED END STATE for the items in the current feed, so unticking a row
 * whose file is already on the card removes it — files in the folder that the
 * feed does not mention are never touched.
 *
 * State machine, modelled on FontDownloadActivity (the in-tree "Wi-Fi, then a
 * list of downloadables" screen): WifiSelectionActivity handles the radio,
 * both LIST and SUMMARY are FreeInkUI lists driven by the UiListActivity base
 * (touch + physical keys, viewport paging), and the blocking states render
 * straight through GfxRenderer.
 */
class RssSyncActivity final : public UiListActivity {
 public:
  explicit RssSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  // Every state: FETCHING and SYNCING block the loop (so this is not polled
  // while they run), and LIST/SUMMARY/ERROR must survive long enough for the
  // user to act — sleeping there would drop the radio and the fetched list.
  // Matches OpdsBookBrowserActivity.
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State : uint8_t { WIFI_SELECTION, FETCHING, LIST, SYNCING, SUMMARY, ERROR };

  // One feed item resolved against the destination folder. `filename` and
  // `present` are computed once, when the list is built, and not re-derived.
  struct Row {
    std::string title;
    std::string url;
    std::string filename;  // leaf name inside destFolder, collision-suffixed
    char date[12] = {};    // "12 Aug", from the item's pubDate
    bool checked = true;   // the list opens fully checked, by design
    bool present = false;  // file already in destFolder at list-entry time
  };

  // The three action rows sit above the items so both are reachable without
  // paging to the end of a 100-item feed.
  static constexpr int ACTION_ROW_COUNT = 3;
  static constexpr int ROW_SELECT_ALL = 0;
  static constexpr int ROW_UNSELECT_ALL = 1;
  static constexpr int ROW_SYNC = 2;
  // SUMMARY's own two rows.
  static constexpr int ROW_BROWSE = 0;
  static constexpr int ROW_DONE = 1;

  State state = State::WIFI_SELECTION;
  std::string destFolder;
  std::string errorMessage;
  std::vector<Row> rows;

  // Row buffer for the LIST screen, rebuilt only when the data behind it
  // changes (fetch, select-all, a toggle), never on every repaint.
  std::vector<freeink::ui::ListItem> rowItems;
  // SUMMARY's two rows: fixed count, so no vector.
  freeink::ui::ListItem summaryRowItems[2]{};
  std::string syncLabel;  // "Sync (X new, Y remove)" / "Up to date"
  int newCount = 0;       // checked && absent
  int removeCount = 0;    // unchecked && present

  // SYNCING progress.
  int actionIndex = 0;  // 1-based, over the items that actually need work
  int actionTotal = 0;
  std::string activeTitle;
  size_t fileProgress = 0;
  size_t fileTotal = 0;
  // Set from the progress callback's input pump: finish the CURRENT item, then
  // skip the rest. Deliberately NOT passed to HttpDownloader as its cancel
  // flag, which would abort mid-file and leave nothing to show for it.
  bool cancelRequested = false;
  bool goHomeRequested = false;

  // SUMMARY counters.
  int downloadedCount = 0;
  int deletedCount = 0;
  int skippedCount = 0;
  int failedCount = 0;

  // --- UiListActivity contract ---
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;

  // --- flow ---
  void onWifiSelectionComplete(bool connected);
  void startFetch();
  bool fetchFeed();
  void buildRows(std::vector<RssItem>&& items);
  void recountAndRelabel();
  void rebuildRowItems();
  void runSync();
  bool downloadRow(const Row& row);
  bool deleteRow(const Row& row);
  void failWith(const char* message);
  std::string pathFor(const Row& row) const;
  bool nameTaken(const std::string& filename) const;
  // Drop the radio before leaving to somewhere other than home: onExit's
  // silentRestart() (the defrag path OPDS/Fonts use) always reboots into the
  // home screen, which would discard a file-browser destination.
  void shutdownWifi();

  void buildListScreen(UiScreen& screen);
  void buildSummaryScreen(UiScreen& screen);
};

#endif  // CROSSPOINT_RSS_SYNC
