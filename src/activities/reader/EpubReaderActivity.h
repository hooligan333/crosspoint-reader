#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>
#ifdef CROSSPOINT_BG_BUILD_TASK
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#ifdef CROSSPOINT_PAGE_CACHE
// Section.h only forward-declares Page; the cache holds one by value-owning pointer.
#include <Epub/Page.h>
#endif

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "components/OptionPopup.h"

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
#ifdef CROSSPOINT_PAGE_CACHE
  // One-entry deserialized-page cache, filled by the idle prewarm above. That
  // prewarm already reads (spine, currentPage+1) off SD and deserializes it just
  // to scan its glyphs, then throws the Page away -- and the very next forward
  // turn re-opens section.bin, seeks the page LUT and deserializes the same
  // bytes again, on the page-turn critical path. Keeping the Page instead makes
  // the forward turn SD-free. Owned here rather than by Section because Section
  // objects are destroyed and rebuilt constantly, which is exactly what the
  // cache has to outlive. Owned by EpubReaderActivity rather than ReaderActivity
  // because the prewarm that fills it is EPUB-only; the TXT and XTC readers that
  // share the base have no Section and no prewarm.
  //
  // Steady-state cost: ONE retained Page (tens of KB of text blocks), live from
  // the prewarm that fills the entry until the render that consumes it. Never
  // two: ANY key mismatch, page number included, drops the entry on the spot, so
  // the next prewarm can't allocate a second Page beside a resident one -- which
  // is also why a hit-for-its-own-page-later policy is deliberately not offered.
  //
  // A hit requires ALL of: same section generation, same spine, same page
  // number, same pageCount, same isPartial(), and a section that is not
  // building. Invalidation triggers, enumerated:
  //  * Section replaced -- settings change, spine change, orientation change,
  //    footnote navigation (navigateToHref / restoreSavedPosition), page-load
  //    error recovery, percent/TOC/sync/bookmark jumps, auto-page-turn toggle.
  //    Every one funnels through section.reset() (24 call sites) plus the ONE
  //    site that assigns `section` -- renderBook()'s construction path -- which
  //    bumps sectionGeneration and drops the entry. A reset with no reinstall
  //    yet is covered too: takeCachedPage() drops on !section.
  //  * Section re-paginated in place -- only a build does that, so a fill
  //    requires !isBuilding() and a hit re-checks it. A partial's extension
  //    build starting (loop() or renderBook()) invalidates immediately, as does
  //    build progress that re-numbers pages.
  //  * pageCount / isPartial() moving under an otherwise unchanged Section
  //    (a build finalizing) -- both are part of the key.
  // Fill and consume both run under the RenderLock, which is what makes those
  // key fields coherent. The lock covers the KEY only -- the page number handed
  // to takeCachedPage comes from section->currentPage, which pageTurn() moves
  // from the loop task unlocked, so a stale read there can cost a miss and
  // nothing else.
  std::unique_ptr<Page> cachedPage;
  int cachedPageSpine = -1;
  int cachedPageNumber = -1;
  uint32_t cachedPageGeneration = 0;
  uint16_t cachedPagePageCount = 0;
  bool cachedPagePartial = false;
  // Bumped every time `section` is installed, so an entry can never be matched
  // against a different Section object -- a fresh Section can land on the freed
  // address of the old one, so a pointer alone is not a usable key.
  uint32_t sectionGeneration = 0;
  // All three require the RenderLock (see above).
  void storeCachedPage(int pageNumber, std::unique_ptr<Page> page);
  std::unique_ptr<Page> takeCachedPage(int pageNumber);
  void dropCachedPage();
  void onForcedRefreshLocked() override;
#endif
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More };
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
#ifdef CROSSPOINT_BG_BUILD_TASK
  // X4 Pro (dual-core S3): drive section builds from a dedicated task pinned to
  // core 0 (the Arduino loopTask and the render task both live on core 1)
  // instead of 2-page ticks stolen from loop(). Same RenderLock discipline and
  // tick size as the loop pump — the lock is held per 2-page tick, never across
  // a chapter — but ticks run back-to-back on an otherwise idle core, so a
  // chapter finalizes in seconds of background time instead of tracking reading
  // pace, and the BUILD_WINDOW_AHEAD throttle (which exists to ration loop-task
  // time on a single core) does not apply. Completion/failure are handed back
  // to the loop task via atomics so reposition/reset/requestUpdate keep running
  // in their usual task context. If task creation fails, the loop-tick pump
  // remains as a runtime fallback (gated on bgBuildTaskHandle == nullptr).
  //
  // The idle wait is notification-driven, not a poll: a permanent 25 ms tick
  // would be 40 wakes/s on core 0 for the whole reading session and would hold
  // the chip out of automatic light sleep, which nothing else about reading
  // prevents. See bgBuildTaskLoop() for the two cadences and the arming points.
  TaskHandle_t bgBuildTaskHandle = nullptr;
  std::atomic<bool> bgBuildStop{false};
  std::atomic<bool> bgBuildExited{false};
  std::atomic<bool> bgBuildCompleteNotify{false};
  std::atomic<bool> bgBuildFailedNotify{false};
  static void bgBuildTaskTrampoline(void* param);
  void bgBuildTaskLoop();
  void startBgBuildTask();
  void stopBgBuildTask();
#endif
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openDictionaryWordSelect();
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  bool loadBook() override;
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

#ifdef CROSSPOINT_BG_BUILD_TASK
  void onEnter() override;
  void onExit() override;
#endif
  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
