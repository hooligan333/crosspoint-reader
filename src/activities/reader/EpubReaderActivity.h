#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>
#if defined(CROSSPOINT_NEXT_SECTION_PREBUILD) && !defined(CROSSPOINT_BG_BUILD_TASK)
#error "CROSSPOINT_NEXT_SECTION_PREBUILD requires CROSSPOINT_BG_BUILD_TASK: the prebuild runs on that task."
#endif
#if defined(CROSSPOINT_BG_IMAGE_DECODE) && !defined(CROSSPOINT_BG_BUILD_TASK)
#error "CROSSPOINT_BG_IMAGE_DECODE requires CROSSPOINT_BG_BUILD_TASK: it runs as that task's idle work"
#endif

#if defined(CROSSPOINT_BG_IDLE_STRETCH_MS) && !defined(CROSSPOINT_BG_BUILD_TASK)
#error "CROSSPOINT_BG_IDLE_STRETCH_MS requires CROSSPOINT_BG_BUILD_TASK: it stretches that task's parked fallback"
#endif
#if defined(CROSSPOINT_BG_BUILD_TASK) && !defined(CROSSPOINT_BG_IDLE_STRETCH_MS)
#define CROSSPOINT_BG_IDLE_STRETCH_MS 250
#endif

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
#ifdef CROSSPOINT_USAGE_LOG
// For UsageLog::SectionSource, the CH_START hook's argument type. That header
// is entirely inside its own flag guard, so a flags-off build gets nothing.
#include "UsageLog.h"
#endif

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
  // Stamped by renderBook() on every path that puts a frame on the panel (the
  // rendered page and the error screens alike). Written by the RENDER task and
  // read from the loop task by the idle prewarm below and, in usage-log builds,
  // by the _RDY hook -- so a relaxed atomic: on the LX7 that compiles to the
  // same plain aligned 32-bit load/store the unsynchronised member was, and no
  // other state is published through it, since both readers only ask "has this
  // moved since I looked?".
  std::atomic<uint32_t> lastRenderCompleteMs{0};
#ifdef CROSSPOINT_USAGE_LOG
  // Usage-log book/chapter load profiling. The _RDY half of each pair is "the
  // first page is physically on the panel", which only the RENDER task knows --
  // and the usage log's ring is loop-task only. So the start hook parks the
  // then-current lastRenderCompleteMs here, and loop() emits the _RDY row on the
  // first pass where the render task has moved that stamp on. Nothing is
  // recorded off the loop task, and no new cross-task state is introduced:
  // lastRenderCompleteMs is already read this way by the idle prewarm.
  uint32_t ulogRenderMsAtLoad = 0;
  uint8_t ulogPendingReady = 0;     // 0 none, 1 BOOK_RDY, 2 CH_RDY forward, 3 CH_RDY back
  uint32_t ulogPendingStartMs = 0;  // millis() when that pending _RDY was armed
  // Belt and braces against a fabricated row. Every frame-painting path in
  // renderBook() stamps lastRenderCompleteMs, error screens included, so a
  // pending _RDY is normally answered within the load. Where it is not -- the
  // spine-past-the-end early return, which paints nothing and leaves the panel
  // to the base class's end-of-book screen -- the pending is DROPPED here
  // instead of surviving to fire minutes later against a stale _START and
  // publish an absurd load time. 90 s sits well above any plausible cold index
  // build; the cost is that a build slower than that loses its _RDY row.
  static constexpr uint32_t ULOG_READY_DEADLINE_MS = 90UL * 1000UL;
  // Arms that pending _RDY and writes the CH_START row. `source` is one of
  // UsageLog's SECTION_* codes -- where the section came from, and when it did
  // not come from the prebuild, why not.
  void ulogNoteSectionStart(bool isForward, UsageLog::SectionSource source);
#endif
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
  // the prewarm that fills the entry until the render that consumes it. Never a
  // second RETAINED one: ANY key mismatch, page number included, drops the entry
  // on the spot, so the next prewarm can't allocate a second Page beside a
  // resident one -- which is also why a hit-for-its-own-page-later policy is
  // deliberately not offered.
  //
  // One transient exception, with CROSSPOINT_BG_IMAGE_DECODE: the bg task's
  // lookahead scan (imageDecodeStep) deserializes a Page of its own to look for
  // undecoded images, so a second Page is live for the length of that scan while
  // an entry is resident. It is scoped to the task's locked phase and freed
  // there, never stored. The scan reuses the resident entry read-only when the
  // keys match, so the two only coexist for a lookahead page the cache is not
  // already holding.
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
  // Fallback wake for the fully-parked idle wait (see bgBuildTaskLoop): every
  // work arrival is notified, so this only bounds a retry of deferrable idle
  // work. On a tickless-idle board it is also this task's last residual wake
  // source, so -DCROSSPOINT_BG_IDLE_STRETCH_MS=1000 trades that bound for
  // second-long in-reader sleep windows. (Default is defined beside the flag
  // checks at the top of this header.)
  static constexpr uint32_t BACKGROUND_IDLE_PARKED_WAIT_MS = CROSSPOINT_BG_IDLE_STRETCH_MS;
  // A zero/short value would make ulTaskNotifyTake non-blocking: this task would
  // free-spin on core 0 at priority 1, starve IDLE0, and trip the task WDT panic
  // ~5 s into any book.
  static_assert(BACKGROUND_IDLE_PARKED_WAIT_MS >= 25,
                "CROSSPOINT_BG_IDLE_STRETCH_MS must not undercut the 25 ms transient cadence");
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Prebuild of the NEXT spine's Section by the background build task while the
  // reader sits near the end of a fully-built chapter, so the forward
  // chapter-boundary turn swaps a ready Section in instead of paying the
  // synchronous load. Two ways to get one, tried in that order: ADOPT an
  // existing layout cache (tens of ms), or LAY THE CHAPTER OUT here. The second
  // is what covers a book being read forward for the first time, where the next
  // spine has no .bin at all -- on-device logging measured that case as
  // essentially every forward transition. Of 57 logged CH_START pairs, 52 were
  // forward and 5 backward; 5 of those 52 forward crossings adopted a cache,
  // and all five were returns across a boundary the reader had just crossed
  // backwards.
  //
  // Everything -- construction, loadSectionFile, startBuild, every build tick,
  // park, discard, consume -- runs under the try-acquired RenderLock, so the
  // shared book.bin metadata handle and all reader state are only ever touched
  // serialized, and a prebuild tick can never overlap a render.
  //
  // The lock holds are NOT all one size, and the outlier is worth naming:
  // loadSectionFile and a build tick are both tens of ms, but startBuild() opens
  // SD files, loads the cached CSS and constructs the chapter parser -- 100-300
  // ms on a slow card, and it fires exactly when the reader is turning pages
  // near a chapter end. So the ARMING call that would run it is additionally
  // deferred while activityManager.hasPendingRender() is true (the same
  // requestedUpdate/pendingRenders count the loop's idle tail reads): a render
  // already queued goes first, and the arm retries on the next pass, on the
  // 25 ms transient cadence. That check and the arm's heap floor are both taken
  // BEFORE the candidate Section is constructed and its cache probed, so a pass
  // that is going to decline costs no SD I/O at all. Nothing else on this path
  // defers -- once a layout is armed, its ticks are the same size as the active
  // section's.
  //
  // ONE LIVE BUILD CONTEXT AT A TIME, and that is the load-bearing invariant: a
  // Section's build holds the Epub's single CssParser (hydrated at startBuild,
  // cleared at finalize/suspend/abandon) and the parser keeps its rules by
  // reference, so two builds would corrupt each other whatever the locking. It
  // holds in both directions:
  //  * this task never STARTS one while `section` is building or partial (the
  //    arming gate in prebuildStep, unchanged), and only ever pumps one build
  //    per idle pass -- the active section's first, this one only when that has
  //    nothing to do (see PrebuildPhase for where in the ladder each half sits);
  //  * every foreground build start first calls discardPrebuiltSection() under
  //    the RenderLock it already holds -- renderBook()'s chapter-load branch and
  //    loop()'s deferred partial extension. No wait is needed, unlike the
  //    pre-inflate's cancel, precisely because a prebuild tick never runs
  //    outside the lock: holding it is proof no tick is in flight.
  //
  // The same discard is what keeps an in-flight layout from OUTLIVING the reader
  // that armed it. A Push does not stop this activity: ActivityManager moves it
  // to the stack without calling onExit(), so the build task keeps running while
  // a sub-activity is up -- and the font pickers and the chapter list tear fonts
  // down (SdFontSystem::ensureLoaded, FontCacheManager::clearCache) under a
  // renderer this task measures text through. So every site that releases
  // `section` before pushing, or abandons the spec the prebuild was armed for,
  // discards it too, under the RenderLock it already holds: the two TEXT_SETTINGS
  // branches (classic menu and toolbar), applyReaderTextSettings (the return
  // path both of those take, plus the Text panel's own live edits),
  // SELECT_CHAPTER, applyOrientation, toggleAutoPageTurn, and the backward
  // chapter-boundary arm. The two font teardowns themselves were moved INSIDE a
  // RenderLock for the same reason.
  // Belt and braces on top of that: prebuildStep drops the prebuild whenever
  // `section` is null, so anything that releases it without discarding still
  // stops the pump on the next background pass. And nothing new is ARMED while
  // this activity is not the current one (activityManager.isCurrentActivity),
  // so a sub-activity that keeps its own section resident cannot have a prebuild
  // started underneath it -- nor have the arm probe SD for one every pass. The
  // pump stays live while stacked on purpose: an armed build is bounded work the
  // reader still wants when it comes back.
  // A discarded in-flight prebuild is not lost work -- ~Section suspends it to a
  // partial .bin, which the eventual visit to that spine resumes from, and which
  // a later prebuild of the same spine resumes in the background.
  std::unique_ptr<Section> prebuiltSection;
  int prebuiltSpineIndex = -1;
  // Spine whose prebuild is settled (started, or refused for a reason that will
  // not change) so idle iterations don't re-probe SD every wake. A refusal for
  // "the next spine's HTML is not inflated yet" IS expected to change, and the
  // pre-inflate below clears this when it promotes.
  // std::atomic, unlike the members beside it, for one site: the pre-inflate
  // clears it from its UNLOCKED phase (the only access not under the RenderLock),
  // which would otherwise be a plain data race against the loop task's boundary
  // hook. Relaxed everywhere -- nothing is published through this value, the
  // worst a lost update can cost is one extra SD probe, and relaxed int loads
  // and stores are the same instructions the plain int compiled to.
  std::atomic<int> prebuildDeclinedSpine{-1};
  ReaderRenderSpec prebuiltSpec{};
  // Spec snapshot for the task; written by renderBook() under the RenderLock.
  ReaderRenderSpec lastRenderSpec{};
  bool lastRenderSpecValid = false;
  static bool renderSpecEquals(const ReaderRenderSpec& a, const ReaderRenderSpec& b);
  // The prebuild runs as two steps at DIFFERENT priorities in the idle ladder,
  // because they cost different things and serve deadlines that are minutes
  // apart:
  //   Arm  -- drop a stale prebuild, probe the next spine's cache, park it or
  //           start its layout -- and resume a parked partial whose own start
  //           was refused. Cheap, one-shot per spine, and its deadline is
  //           the boundary turn, so it sits where the old single step did:
  //           straight after the active section's build tick.
  //   Pump -- one buildSomeMore() tick of that layout. This is the LOWEST
  //           priority work in the ladder, below the image pre-decode: a
  //           chapter's layout runs for a minute or more and returns didWork
  //           every pass, so pumping it above the pre-decode starved the images
  //           of pages the reader reaches in seconds for the whole runway. The
  //           prebuild's own runway is 95 s median, which is what makes it the
  //           one that can afford to yield.
  enum class PrebuildPhase : uint8_t { Arm, Pump };
  // `workPlausible` follows htmlInflateStep's convention: set when this pass
  // could not rule out IMMINENT work (contended lock, a build tick's heap gate,
  // deferred for a queued render) so the task retries on the 25 ms cadence
  // instead of parking. Deliberately NOT set by the arm's own heap floor: a heap
  // shortfall persists, and spinning against it is the one way this step can
  // cost real idle power -- the sibling steps' heap gates set nothing either.
  bool prebuildStep(bool& workPlausible, PrebuildPhase phase);
  // Drop whatever is parked (suspending its build, if any) and re-arm the probe.
  void discardPrebuiltSection();
  // Start prebuilding when the reader is within this many pages of chapter end.
  // Measured runway inside that window: 95 s median, 4.6 s minimum -- ample for
  // a layout that costs the boundary turn ~1.6 s today.
  static constexpr int PREBUILD_NEAR_END_PAGES = 3;
  // Free-heap floor for STARTING a prebuild's layout (its ticks are then gated
  // by buildTickHeapGate(), exactly as the active section's are). The same
  // allocations -- BuildContext, chapter parser, hydrated CSS rules -- are made
  // with no floor at all when the boundary turn builds the chapter itself, so
  // this only has to stop the OPPORTUNISTIC copy from taking blocks the render
  // task is about to need on the other core.
  static constexpr size_t PREBUILD_BUILD_MIN_FREE_HEAP = 64 * 1024;
  static constexpr size_t PREBUILD_BUILD_MIN_MAX_ALLOC = 32 * 1024;

  // --- Background HTML pre-inflate of the next spine ---------------------------
  // A next chapter that has never been visited has no layout cache, and the
  // boundary turn then pays the whole cold cost: inflating the chapter HTML out
  // of the zip (multi-second on a large spine), parsing CSS, laying out page
  // one. This step moves the first of those three off the critical path -- it
  // produces a file, not reader state, and Section::startBuild already has a
  // fast path for finding that file present (`reusedHtml`). So when the reader
  // is near the end of a chapter and the next spine's HTML is NOT cached,
  // inflate it here, on the background task's idle path.
  //
  // It is also the PRECONDITION for the prebuild above laying that chapter out:
  // prebuildStep refuses to start a build whose startBuild() would inflate,
  // because that inflate is seconds long and would run under the RenderLock.
  // The two therefore run in sequence over a couple of idle passes -- inflate
  // here, then build there -- which is why a promotion re-arms
  // prebuildDeclinedSpine.
  //
  // Two phases, copied from the image pre-decode below because the hazard is
  // the same shape:
  //   UNDER the (try-acquired) RenderLock -- decide the target and capture
  //   everything needed AS VALUES (spine index, its zip-local href, its temp
  //   path, a shared_ptr copy of the Epub), then publish bgHtmlInflateActive
  //   BEFORE releasing the lock, so no render can start without seeing it.
  //   WITHOUT the lock -- the zip stream, the temp write and the atomic rename.
  //   Nothing there touches `section`, `epub` (the member), or the renderer.
  //
  // A PARALLEL interlock, not the image pre-decode's: that one lives in
  // ImageBlock and only exists under CROSSPOINT_BG_IMAGE_DECODE, which this
  // feature does not (and must not) require, and its cancel-side abort flag is
  // ImageToFramebufferDecoder's, consumed inside the JPEG/PNG row callbacks.
  // The two are never in flight at once anyway -- one background task, one work
  // item per idle pass -- so a second flag pair costs nothing and keeps the two
  // cancels independently readable. Invariants:
  //  * bgHtmlInflateActive is raised only in the locked phase and cleared only
  //    by the inflate itself, wherever it ends.
  //  * bgHtmlInflateAbort is raised by a canceller and lowered ONLY by the next
  //    inflate's locked phase. That is the deliberate difference from
  //    ImageBlock::cancelBackgroundDecode, which must lower it on the way out
  //    because that flag is shared with render-path decodes: this one is
  //    private to this single background activity, so leaving it raised after a
  //    cancel that timed out is both safe and useful -- the runaway inflate
  //    still refuses to promote its temp file at the commit point.
  //  * bgHtmlInflateSpine is plain, not atomic: written in the locked phase and
  //    read only by cancellers, all of which hold the RenderLock (see the
  //    call-site list on cancelBackgroundHtmlInflate below).
  //  * the inflate must never be running while a FrameBufferLoan is: the
  //    inflater claims the lent framebuffer bytes for its 43 KB of state
  //    (buildscratch::claim), and the loan takes them back and draws over them
  //    with no synchronization. Both loans live inside renderBook()'s
  //    chapter-load branch, behind that branch's cancel, and the locked phase
  //    cannot start an inflate while a render holds the lock.
  // Returns true when an inflate completed and was promoted.
  bool htmlInflateStep(bool& workPlausible);
  // Stop an in-flight background inflate and wait (bounded) for it to
  // acknowledge. Every caller holds the RenderLock, which is what keeps a
  // cancel from racing the locked phase that starts one. The complete set of
  // call sites, each because the foreground is about to touch something the
  // inflate is holding:
  //  * renderBook()'s chapter-load branch -- it inflates this spine's HTML and
  //    lends the framebuffer to whichever inflater claims it (see the note
  //    there for why no other render needs one);
  //  * loop()'s deferred partial-extension start -- same inflate, for a spine
  //    that can be a prebuild just adopted at a boundary;
  //  * the reader menu's DELETE_CACHE -- it removes the tree being written to;
  //  * stopBgBuildTask() -- so the join is not held for a whole inflate.
  // Idle = nothing was running; Cancelled = stopped within the timeout;
  // TimedOut = the inflate is STILL RUNNING (abort stays latched, so it cannot
  // promote its temp — but it still holds an open write handle in the cache
  // tree, so destructive callers must decline).
  enum class HtmlInflateCancel : uint8_t { Idle, Cancelled, TimedOut };
  HtmlInflateCancel cancelBackgroundHtmlInflate();
  static bool htmlInflateAbortRequested(void* ctx);
  std::atomic<bool> bgHtmlInflateActive{false};
  std::atomic<bool> bgHtmlInflateAbort{false};
  int bgHtmlInflateSpine = -1;
  // Spine whose pre-inflate is settled (promoted, already cached, or failed) so
  // idle passes stop re-probing SD for it -- the counterpart of
  // prebuildDeclinedSpine. Touched ONLY by the background task, which is why it
  // is not cleared from the reader's spine-change sites the way that one is: it
  // is always compared against currentSpineIndex + 1, so moving to another
  // chapter re-arms it by itself.
  int htmlInflateDeclinedSpine = -1;
  // Free-heap floor for starting one. A streaming inflate costs ~11 KB of tinfl
  // state plus a 32 KB window plus 2x8 KB of zip buffers, and the render task
  // can be lazily extracting an image (its own inflate, same size) on the other
  // core while this one runs -- and a render-path extract that loses an
  // allocation race marks its image failed for the whole session. Leave room
  // for both rather than win a race for the last block; pre-inflating is pure
  // opportunism and declining costs only today's behavior.
  // ~59 KB for THIS inflate plus the same again for a concurrent render-path
  // lazy image extract, with margin — 96 KB only covered one of the pair, and
  // a render extract that loses the allocation race marks its image failed for
  // the whole session.
  static constexpr size_t HTML_INFLATE_MIN_FREE_HEAP = 128 * 1024;
  // The 32 KB inflate window is a single contiguous allocation, and the
  // render's own window must still fit after ours is carved out.
  static constexpr size_t HTML_INFLATE_MIN_MAX_ALLOC = 80 * 1024;
  // Cap for the cancel wait. The abort is polled between output chunks and
  // before the commit rename, but the span BEFORE the first output byte is not
  // interruptible and is not small on many-spine books: a fresh ZipFile scans
  // the central directory linearly under the storage mutex, so a 1000+-entry
  // EPUB contending with foreground SD traffic can plausibly exceed this cap.
  // A timeout is therefore an expected rare event, not a bug signal: callers
  // proceed (destructive ones must DECLINE instead — see DELETE_CACHE) and the
  // still-raised abort flag keeps the runaway inflate from promoting.
  static constexpr uint32_t HTML_INFLATE_CANCEL_TIMEOUT_MS = 3000;
#endif
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // Pre-decode the images on upcoming pages from the background build task's
  // idle work, so the first view of an image page finds a ready .pxc instead of
  // paying a 0.5-3 s decode on the page-turn critical path (today that decode
  // happens inside the render, behind a placeholder pass). One page is examined
  // and at most one image decoded per idle pass; the decode itself runs with
  // the RenderLock RELEASED, which is what the ImageBlock interlock and the
  // cacheOnly decode mode exist to make safe. Returns true when a decode ran.
  bool imageDecodeStep(bool& workPlausible);
  // How far ahead to look. Three pages is roughly the runway a decode needs at
  // reading pace, and keeps the declined mask a byte.
  static constexpr int IMAGE_DECODE_LOOKAHEAD = 3;
  // Cursor state, touched only by the background task. The declined mask
  // records which lookahead offsets have already been examined and have nothing
  // left to decode, so idle passes stop re-reading their pages; it is reset
  // whenever the reader moves (which is also what re-arms the window).
  int imageDecodeSpine = -1;
  int imageDecodeBasePage = -1;
  uint8_t imageDecodeDeclined = 0;
  // Free-heap floor for starting a pre-decode. The PNG decoder object is ~44 KB
  // (JPEG ~20 KB) and the render task may start its own decode, or a build-time
  // image header probe, on the other core while this one runs; leave room for
  // both rather than win a race for the last block. A render-path decode that
  // loses one marks its image failed for the whole session, so this floor is
  // deliberately generous -- pre-decoding is pure opportunism.
  static constexpr size_t IMAGE_DECODE_MIN_FREE_HEAP = 96 * 1024;
  // The decoder object is a single contiguous allocation.
  static constexpr size_t IMAGE_DECODE_MIN_MAX_ALLOC = 48 * 1024;
  // Page origin of the last render, captured under the RenderLock: a
  // pre-decode must place the image exactly where a render would, because both
  // the screen clip and the dither phase depend on absolute position.
  //
  // Implicit invariant, worth stating because it is load-bearing and unenforced:
  // these describe the layout the CURRENT `section` was paginated with. It holds
  // because every path that changes the layout (settings, orientation, margins,
  // status-bar height, auto-turn indicator) resets `section` in the same lock
  // scope, so the pre-decode -- which refuses to run without a section -- can
  // never pair a new page with an old origin. A future re-pagination that
  // changes the origin WITHOUT resetting the section would break it silently:
  // the .pxc files written from here would be dithered and clipped for the old
  // origin, and nothing downstream would notice, because the header check only
  // compares dimensions. Reset lastRenderMarginsValid in any such path.
  int16_t lastRenderMarginLeft = 0;
  int16_t lastRenderMarginTop = 0;
  bool lastRenderMarginsValid = false;
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
  // Activity override: the reader's contextual menu is what a Home-key
  // long press opens (CrossInk-style configurable long-press action).
  bool openShortcutMenu() override;
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
