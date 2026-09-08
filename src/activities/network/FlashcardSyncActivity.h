#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <cstdint>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "util/DeckGuid.h"

struct RssItem;

/**
 * Sync Decks: mirrors a personal LAN deck feed's enclosures into a folder on
 * the SD card.
 *
 * Structurally a clone of RssSyncActivity — the deck feed is the same RSS 2.0
 * document with a different <enclosure type> (DECK_SERVER_SPEC.md §2), so the
 * same expat parser, the same desired-end-state checkbox list, the same
 * duplicate-leaf refusal and the same temp+rename download apply. The deltas
 * are all downstream of "a deck is not a book":
 *
 *  - the leaf filename is forced to `.deck`, not `.epub`;
 *  - unticking a deck deletes the deck file AND its scheduling state file
 *    (`<destFolder>/.state/<leaf>.state`), and never touches a book cache;
 *  - a completed download is header-checked against CPDK v1 before it is
 *    renamed into place, so a wrong-format or forward-version file is reported
 *    instead of being left behind as a deck the picker cannot open;
 *  - a deck that is ALREADY present is not automatically up to date. Decks are
 *    mutable: the same leaf is re-exported whenever its cards change. A checked
 *    present row is therefore compared against the feed's content hash and
 *    re-downloaded on a mismatch, keeping its scheduling state
 *    (FLASHCARD_SPEC.md §1, util/DeckGuid.h).
 */
class FlashcardSyncActivity final : public UiListActivity {
 public:
  explicit FlashcardSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  // Every state: FETCHING and SYNCING block the loop (so this is not polled
  // while they run), and LIST/SUMMARY/ERROR must survive long enough for the
  // user to act — sleeping there would drop the radio and the fetched list.
  // Matches RssSyncActivity.
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State : uint8_t { WIFI_SELECTION, FETCHING, LIST, SYNCING, SUMMARY, ERROR };
  // Outcome of one row's work. Invalid is separated from Failed because the two
  // ask the user for different things: retry the transfer, versus re-export the
  // deck on the server.
  enum class ItemResult : uint8_t { Ok, Failed, Invalid };

  // One feed item resolved against the destination folder. `filename` and
  // `present` are computed once, when the list is built, and not re-derived.
  struct Row {
    std::string title;
    std::string url;
    std::string filename;  // leaf name inside destFolder, unique across the feed
    // Everything the feed claims about the file itself: the guid's content hash
    // (staleness) and the enclosure length (which also drives the progress bar
    // when the response carries no Content-Length).
    DeckFeedStamp feed;
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
  // Latched only when the run actually stopped short, so a cancel that lands
  // during the last item (which still completes) reads as a finished sync.
  bool cancelled = false;

  // SUMMARY counters.
  int downloadedCount = 0;
  int updatedCount = 0;  // present but stale: replaced in place, state kept
  int deletedCount = 0;
  int skippedCount = 0;
  int failedCount = 0;
  int invalidCount = 0;    // downloaded, but not a CPDK v1 file this build reads
  int remainingCount = 0;  // actions a cancel skipped; 0 on a full run

  // --- UiListActivity contract ---
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  const char* headerTitle() const override;

  // --- flow ---
  void onWifiSelectionComplete(bool connected);
  void startFetch();
  bool fetchFeed();
  // False when the feed cannot be mirrored (duplicate filenames); the error
  // state is already set.
  bool buildRows(std::vector<RssItem>&& items);
  void recountAndRelabel();
  void rebuildRowItems();
  void runSync();
  ItemResult downloadRow(const Row& row);
  bool deleteRow(const Row& row);
  void failWith(std::string message);
  std::string pathFor(const Row& row) const;
  std::string statePathFor(const Row& row) const;
  const Row* rowWithFilename(const std::string& filename) const;
  // Drop the radio before leaving to somewhere other than home; see
  // RssSyncActivity::shutdownWifi for the full reasoning.
  void shutdownWifi();

  void buildListScreen(UiScreen& screen);
  void buildSummaryScreen(UiScreen& screen);
};

#endif  // CROSSPOINT_FLASHCARDS
