#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#ifdef CROSSPOINT_BG_IMAGE_DECODE
#include <Epub/converters/ImageToFramebufferDecoder.h>  // pre-decode abort flag
#endif
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalHeapGauge.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#ifdef CROSSPOINT_UC8179_OVERLAP
#include <esp_heap_caps.h>
#endif
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "UsageLog.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

namespace {
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
bool xteinkClassPanel() { return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || BoardConfig::isX4Classic(); }

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

#ifdef CROSSPOINT_UC8179_OVERLAP
// One panel-sized grayscale plane, held in PSRAM for the length of a page
// render. The whole-buffer (non-tiled) AA path has exactly one framebuffer to
// render planes into, so overlapping BOTH plane renders with the base waveform
// needs somewhere to park the first finished plane until the panel is ready to
// take it. PSRAM keeps those 48 KB clear of the internal heap the page render
// is itself competing for; a failed allocation just leaves the page on the
// serial path.
class PsramPlane {
 public:
  explicit PsramPlane(size_t bytes)
      : ptr(bytes != 0 ? static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
                       : nullptr) {}
  ~PsramPlane() { free(ptr); }  // heap_caps_malloc'd memory is free()-compatible
  PsramPlane(const PsramPlane&) = delete;
  PsramPlane& operator=(const PsramPlane&) = delete;
  uint8_t* get() const { return ptr; }
  explicit operator bool() const { return ptr != nullptr; }

 private:
  uint8_t* ptr;
};
#endif

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Ahead of everything, and in particular ahead of the read-folder move below:
  // a parked prebuild holds SD handles in the book's cache dir and its
  // destructor may commit a partial .bin there, so it has to be gone before that
  // directory is moved or removed. onExit() normally got here first (it stops
  // the build task and discards), but a destructor must not depend on that
  // having run -- and the task is already stopped by then either way, so this is
  // a no-op on the ordinary path rather than a second teardown.
  discardPrebuiltSection();
#endif
  ImageBlock::setExtractor(nullptr, nullptr);
  discardOverlayPage();  // free the overlay's page snapshot if one is held

  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

#ifdef CROSSPOINT_PAGE_CACHE
  // ActivityManager destroys the current activity from exitActivity(), which its
  // callers reach with the RenderLock held, so this teardown is locked exactly as
  // the fill and consume paths are. Freed here, ahead of the section/epub teardown
  // and the read-folder move below, rather than left to the member's own
  // destructor after all of that has run with its heap still pinned.
  dropCachedPage();
#endif
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

#ifdef CROSSPOINT_BG_BUILD_TASK
void EpubReaderActivity::onEnter() {
  ReaderActivity::onEnter();
  // Only with a book: a failed load has already called finish(), and the task
  // has nothing to drive.
  if (epub) startBgBuildTask();
}

void EpubReaderActivity::onExit() {
  // Stop the build task before anything else (it dereferences `section` and the
  // epub, both of which this activity's teardown drops); the join is bounded by
  // the task's short wait cadence.
  stopBgBuildTask();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Task is stopped, so nothing can be mid-tick; discard directly. A prebuild
  // still laying itself out is suspended to a partial .bin here, exactly as
  // section.reset() below suspends the current chapter's build.
  discardPrebuiltSection();
#endif
  ReaderActivity::onExit();
}
#endif

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

#ifdef CROSSPOINT_USAGE_LOG
  // book.bin's first byte is BookMetadataCache's format version, and its load()
  // rejects anything else and rebuilds the whole cache. A plain exists() check
  // therefore reports every book as a WARM open for the one boot after a
  // version bump -- exactly the boot whose load times are worth having. One
  // open and one byte separate the three states, in place of the exists() call
  // this replaces. An unreadable or empty book.bin reads as COLD, which is what
  // it behaves like -- the cache gets rebuilt from the zip either way.
  uint8_t cacheVersion = 0;
  bool haveBookBin = false;
  {
    HalFile bookBin;
    if (Storage.openFileForRead("ERS", loadedEpub->getCachePath() + "/book.bin", bookBin)) {
      haveBookBin = bookBin.read(&cacheVersion, 1) == 1;
      bookBin.close();
    }
  }
  const bool staleCache = haveBookBin && cacheVersion != BookMetadataCache::CACHE_VERSION;
  const bool uncached = !haveBookBin;
#else
  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
#endif
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
#ifdef CROSSPOINT_USAGE_LOG
  // Ahead of the load itself, so BOOK_OPEN -> BOOK_RDY brackets the whole cost.
  // lastRenderCompleteMs is still 0 here (nothing of this book has rendered),
  // which is exactly what ulogRenderMsAtLoad wants to compare against.
  usageLog.noteBookOpen(uncached ? UsageLog::CACHE_COLD : staleCache ? UsageLog::CACHE_STALE : UsageLog::CACHE_WARM);
  ulogRenderMsAtLoad = lastRenderCompleteMs.load(std::memory_order_relaxed);
  ulogPendingReady = 1;
  ulogPendingStartMs = millis();
#endif

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (!loaded) {
    LOG_ERR("ERS", "Failed to load EPUB");
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadCachedBookmarks();
  return true;
}

bool EpubReaderActivity::openShortcutMenu() {
  openReaderMenu();
  return true;
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
                         [this](const ActivityResult& result) {
                           const auto& menu = std::get<MenuResult>(result.data);
                           if (SETTINGS.orientation != menu.orientation) {
                             applyOrientation(menu.orientation);
                           }
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = gateFreeHeap();
  const size_t maxBlock = gateMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        orientedMarginLeft, orientedMarginTop),
                         [this](const ActivityResult&) { requestUpdate(); });
}

#ifdef CROSSPOINT_PAGE_CACHE
void EpubReaderActivity::storeCachedPage(const int pageNumber, std::unique_ptr<Page> page) {
  // A still-building section re-numbers pages under the entry, so it is never cached.
  if (!page || !section || section->isBuilding()) return;
  cachedPage = std::move(page);
  cachedPageSpine = currentSpineIndex;
  cachedPageNumber = pageNumber;
  cachedPageGeneration = sectionGeneration;
  cachedPagePageCount = section->pageCount;
  cachedPagePartial = section->isPartial();
}

std::unique_ptr<Page> EpubReaderActivity::takeCachedPage(const int pageNumber) {
  if (!cachedPage) return nullptr;
  if (!section || section->isBuilding() || cachedPageGeneration != sectionGeneration ||
      cachedPageSpine != currentSpineIndex || cachedPageNumber != pageNumber ||
      cachedPagePageCount != section->pageCount || cachedPagePartial != section->isPartial()) {
    // ANY mismatch drops the entry, the page number included (a backward turn, a
    // menu/bookmark re-render). Keeping it "valid for its own page" instead is
    // what would let two deserialized Pages be live at once: the render loads
    // the page it actually wants while the entry still holds the old one, and
    // the idle prewarm that follows allocates its replacement before the
    // assignment frees it. One retained Page is the whole heap budget here.
    dropCachedPage();
    return nullptr;
  }
  auto page = std::move(cachedPage);
  dropCachedPage();  // consumed: clear the key with it, never leave half an entry
  return page;
}

void EpubReaderActivity::dropCachedPage() {
  cachedPage.reset();
  cachedPageSpine = -1;
  cachedPageNumber = -1;
  // The rest of the key too: a stale generation/pageCount/isPartial left behind
  // could match a later section by accident. cachedPage == nullptr is the guard
  // that makes it moot, but a half-cleared key is not a state worth reasoning
  // about every time one of these fields gains a reader.
  cachedPageGeneration = 0;
  cachedPagePageCount = 0;
  cachedPagePartial = false;
}

void EpubReaderActivity::onForcedRefreshLocked() {
  // A forced refresh re-renders the CURRENT page, and the cache entry names the
  // NEXT one, so takeCachedPage() will miss and drop it -- while the prewarm
  // markers still name this position, so loop() would never refill it and the
  // next forward turn would pay a full SD deserialize. Re-arm the prewarm
  // (markers are only ever read as a pair against the live position, so clearing
  // the page number alone is enough). Called by ReaderActivity::handleForcedRefresh
  // under the same RenderLock that loop() takes to move them. The cost of the
  // re-run is one glyph scan whose fonts are already cached; the point is the
  // refill.
  idlePrewarmPage = -1;
}
#endif  // CROSSPOINT_PAGE_CACHE

#ifdef CROSSPOINT_BG_BUILD_TASK
void EpubReaderActivity::bgBuildTaskTrampoline(void* param) {
  static_cast<EpubReaderActivity*>(param)->bgBuildTaskLoop();
}

void EpubReaderActivity::startBgBuildTask() {
  if (bgBuildTaskHandle) return;
  bgBuildStop.store(false, std::memory_order_relaxed);
  bgBuildExited.store(false, std::memory_order_relaxed);
  bgBuildCompleteNotify.store(false, std::memory_order_relaxed);
  bgBuildFailedNotify.store(false, std::memory_order_relaxed);
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // The pre-decode interlock is process-global, not per-task, and a previous
  // stop can leave it dirty: a cancel that timed out returns with bgDecodeActive
  // still true (the decode it gave up on ends by clearing it, but nothing
  // guarantees that happened before the join), and any path that raised the
  // abort flag lowers it in its own scope. Clear both here so a restarted task
  // does not inherit "a decode is running" (every render would then pay a
  // cancel-and-wait) or "abort" (the first pre-decode would bail instantly).
  // Under a RenderLock: onEnter() runs after ActivityManager released its own
  // lock, so a render of this freshly-installed reader can already be in
  // flight — take the lock so these stores keep the "mutated inside one
  // RenderLock scope" invariant the interlock's users rely on.
  {
    RenderLock lock;
    ImageBlock::endBackgroundDecode();
    ImageToFramebufferDecoder::requestAbort(false);
  }
#endif
  // Core 0: WiFi's home core, idle while reading (loopTask and the render task
  // are both pinned to core 1). Priority 1 matches them; the RenderLock is the
  // ordering authority regardless. Stack: the parse/layout path historically
  // ran on the 8 KB loop task; 12 KB gives margin for deeper CSS/parser frames.
  constexpr uint32_t kStack = 12288;
  if (xTaskCreatePinnedToCore(&bgBuildTaskTrampoline, "ErsBgBuild", kStack, this, 1, &bgBuildTaskHandle, 0) != pdPASS) {
    bgBuildTaskHandle = nullptr;
    LOG_ERR("ERS", "Failed to create background build task; falling back to loop ticks");
  }
}

void EpubReaderActivity::stopBgBuildTask() {
  if (!bgBuildTaskHandle) return;
  // Wake the task out of its (possibly long) idle wait BEFORE raising the stop
  // flag: at give time the task cannot yet have observed stop=true, so its TCB
  // is provably alive (give-after-store leaves a theoretical window where the
  // task polls the flag, self-deletes, and the give hits a freed TCB). Worst
  // case the woken pass misses the flag and re-sleeps on the short cadence
  // (this caller holds the RenderLock, so its TryAcquire fails -> ~25 ms),
  // keeping the join bounded. It cannot start new work either way: every step
  // is picked under a TryAcquire this caller's lock defeats.
  xTaskNotifyGive(bgBuildTaskHandle);
  bgBuildStop.store(true, std::memory_order_release);
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // A pre-decode in flight would otherwise hold the join for its whole
  // remaining runtime (seconds), so cancel it -- but only AFTER the stop flag
  // is published. The cancel can time out (its 5 s cap covers the un-abortable
  // ZIP extract), and it lowers the abort flag on the way out; with the store
  // after the cancel, a task sitting between the extract and the decode saw
  // neither flag raised and went on to run a full multi-second decode inside
  // the join. Reusing the same cancel-and-wait the render path uses also means
  // the abort flag is raised and lowered inside one RenderLock scope, which is
  // what keeps that global flag from ever touching a render-task decode.
  ImageBlock::cancelBackgroundDecode();
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Same reason and the same ordering constraint as the pre-decode cancel above:
  // a pre-inflate in flight would otherwise hold the join for its whole
  // remaining runtime. Raised only after the stop flag is published, so a task
  // sitting between the two cannot start fresh work that neither flag stops.
  cancelBackgroundHtmlInflate();
#endif
  while (!bgBuildExited.load(std::memory_order_acquire)) {
    delay(1);
  }
  bgBuildTaskHandle = nullptr;
}

void EpubReaderActivity::bgBuildTaskLoop() {
  while (!bgBuildStop.load(std::memory_order_acquire)) {
    bool didWork = false;
    // True when this pass could not rule out imminent work (lock contended, or a
    // build is live but heap-gated this tick) — retry on the short cadence then.
    bool workPlausible = false;
    // Active-section build tick: same per-tick RenderLock scope, heap gate, and
    // page count as the loop pump this replaces — never holds the lock across a
    // chapter, so pending renders interleave exactly as before. TryAcquire, not
    // the blocking ctor: parking here would deadlock an exit path that joins
    // this task while holding the RenderLock (see RenderLock::TryAcquire). All
    // `section` access happens strictly under the lock: the loop-task idiom of
    // an unlocked pre-check is a benign stale read there, but this task runs
    // truly parallel on core 0, where an unlocked deref races ~Section.
    {
      RenderLock lock{RenderLock::TryAcquire{}};
      if (!lock.locked()) {
        workPlausible = true;
      } else if (section && section->isBuilding()) {
        workPlausible = true;
        if (buildTickHeapGate()) {
          if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
            // Reset/redraw belong to the loop task; just report the failure.
            bgBuildFailedNotify.store(true, std::memory_order_release);
          } else {
            didWork = true;
            if (section->isBuildComplete()) {
              bgBuildCompleteNotify.store(true, std::memory_order_release);
            }
          }
        }
      }
    }
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
    // Arming only -- the probe, the park and the layout start. Cheap, one-shot
    // per spine, and it is what actually makes a boundary turn instant, so it
    // keeps the slot the whole prebuild used to hold. Its BUILD ticks are the
    // Pump phase at the bottom of this ladder; see PrebuildPhase.
    if (!didWork && !bgBuildStop.load(std::memory_order_acquire)) {
      didWork = prebuildStep(workPlausible, PrebuildPhase::Arm);
    }
    // Below the prebuild's arming (which this is the precondition for -- an
    // uninflated next spine is exactly what makes the arm defer), above the
    // image pre-decode: this is one-shot per spine and has a deadline -- the
    // boundary turn -- while the pre-decode re-arms every time the reader moves
    // and would otherwise starve it indefinitely on an image-dense chapter.
    // Costing the pre-decode at most one inflate of delay is the cheaper side of
    // that trade.
    if (!didWork && !bgBuildStop.load(std::memory_order_acquire)) {
      didWork = htmlInflateStep(workPlausible);
    }
#endif
#ifdef CROSSPOINT_BG_IMAGE_DECODE
    // It is the only step here that runs for seconds with the lock released, so
    // it goes after everything with a nearer deadline than its own.
    if (!didWork && !bgBuildStop.load(std::memory_order_acquire)) {
      didWork = imageDecodeStep(workPlausible);
    }
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
    // Lowest-priority idle work, and deliberately BELOW the pre-decode: laying a
    // chapter out takes a minute or more of ticks that each report progress, so
    // running it above the pre-decode would starve the images of pages the
    // reader reaches in seconds for the whole of that runway. The prebuild is
    // the one step with a runway to give up (95 s median inside its window), so
    // it yields and the pre-decode does not.
    if (!didWork && !bgBuildStop.load(std::memory_order_acquire)) {
      didWork = prebuildStep(workPlausible, PrebuildPhase::Pump);
    }
#endif
    if (didWork) {
      // Back-to-back ticks while there is work; yield one tick so the render
      // task can take the lock.
      vTaskDelay(1);
    } else {
      // Idle: block on a notification instead of polling, so tickless light
      // sleep isn't held off by this task all session. Every work arrival is
      // notified from INSIDE the same RenderLock scope as the state change it
      // announces: renderBook() after resolving a new render spec (build
      // starts, page turns, settings changes, jumps — and with them the
      // prebuild and pre-decode cursors, which key off exactly that state),
      // loop()'s lazy partial-extension start, and stopBgBuildTask() for exit.
      // The lock scope — not any give/store ordering — is the load-bearing
      // invariant: a pass can't interleave between state and notify, because
      // while the writer holds the lock the pass's TryAcquire fails (25 ms
      // cadence), and once it releases, state and pending notify are both
      // visible. What the long timeout still bounds is a retry of deferrable
      // idle work (a heap-gated or aborted pre-decode), never anything the
      // reader is waiting on, which is what makes it stretchable to the
      // board's sleep window. The short one covers transient lock/heap
      // declines. Index 0 is this task's own slot — nothing else posts to it.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(workPlausible ? 25u : BACKGROUND_IDLE_PARKED_WAIT_MS));
    }
  }
  bgBuildExited.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
#endif  // CROSSPOINT_BG_BUILD_TASK

#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
bool EpubReaderActivity::renderSpecEquals(const ReaderRenderSpec& a, const ReaderRenderSpec& b) {
  return a.fontId == b.fontId && a.lineCompression == b.lineCompression &&
         a.extraParagraphSpacing == b.extraParagraphSpacing && a.paragraphAlignment == b.paragraphAlignment &&
         a.viewportWidth == b.viewportWidth && a.viewportHeight == b.viewportHeight &&
         a.hyphenationEnabled == b.hyphenationEnabled && a.embeddedStyle == b.embeddedStyle &&
         a.imageRendering == b.imageRendering && a.focusReadingEnabled == b.focusReadingEnabled;
}

void EpubReaderActivity::discardPrebuiltSection() {
  prebuiltSection.reset();
  prebuiltSpineIndex = -1;
  prebuildDeclinedSpine.store(-1, std::memory_order_relaxed);
}

// One prebuild step, run on the background build task when the active section
// has nothing to build: adopt the next spine's layout cache if it has one, lay
// the chapter out here if it does not, and pump that layout a tick at a time
// until it is parked and ready. Those are two phases at two different ladder
// priorities (see PrebuildPhase); `phase` says which one this call is.
//
// All work — Section construction, loadSectionFile and startBuild (which read
// the shared book.bin metadata handle) and every build tick — happens under the
// (try-acquired) RenderLock, which is both what serializes it against renders
// and what makes the one-live-build-context invariant provable (see the
// header). Returns true if it made progress.
bool EpubReaderActivity::prebuildStep(bool& workPlausible, const PrebuildPhase phase) {
  RenderLock lock{RenderLock::TryAcquire{}};
  if (!lock.locked()) {
    workPlausible = true;
    return false;
  }
  // Drop a stale prebuild: the reader jumped or paged back, the settings
  // changed, or `section` has been released outright. That last one is the
  // catch-all for a sub-activity push — a font picker or the chapter list
  // releases the section and then tears fonts down while this activity sits on
  // the stack, still running this task. Those sites discard explicitly (see the
  // header), so reaching here with a live prebuild and no section means one of
  // them was missed; stopping the pump is the safety net, not the mechanism.
  // The arming gate below keeps its own `!section` test because it dereferences
  // `section`, and this one only fires when something is parked.
  if (prebuiltSection && (!section || prebuiltSpineIndex != currentSpineIndex + 1 || !lastRenderSpecValid ||
                          !renderSpecEquals(prebuiltSpec, lastRenderSpec))) {
    discardPrebuiltSection();
  }
  // The preconditions for STARTING a layout that startBuild() does not supply
  // itself, split in two because only the second half needs a Section to ask.
  // Lambdas rather than inline gates because there are now two callers: the
  // fresh arm at the bottom of this function, and the parked-partial resume
  // just below it.
  //
  // Neither of these needs a Section or touches the card, which is why both
  // callers run them FIRST: an arm that is going to decline must not pay
  // `new Section` + loadSectionFile()'s SD I/O to find that out. It did, on
  // every pass, until this order was fixed -- and on the 25 ms transient cadence
  // that is a measurable idle drain. The cost of hoisting is that a pass which
  // could merely have ADOPTED a finished cache (no layout to start) declines
  // too; that only defers the adoption to a later pass, and the boundary turn
  // loads the very same cache synchronously in the worst case.
  const auto layoutStartDeferred = [&]() -> bool {
    // No workPlausible on the heap gate: a shortfall persists, so the 25 ms
    // cadence would just spend a reading session probing SD against it.
    // htmlInflateStep() and imageDecodeStep() set nothing on their heap gates
    // either; the parked cadence is the right retry for this.
    if (gateFreeHeap() < PREBUILD_BUILD_MIN_FREE_HEAP || gateMaxAllocHeap() < PREBUILD_BUILD_MIN_MAX_ALLOC) return true;
    // The one lock hold on this path that is not tens of ms: SD file setup, the
    // cached CSS load and the chapter parser cost 100-300 ms on a slow card, and
    // the reader is by definition turning pages near a chapter end when this
    // arms. A render already queued goes first. That one IS transient, so it
    // asks for the 25 ms cadence, and the next Arm pass retries -- by re-probing
    // when nothing is parked, or through the resume below when a partial is.
    if (activityManager.hasPendingRender()) {
      workPlausible = true;
      return true;
    }
    return false;
  };
  // Set when a refusal is permanent for this spine (as opposed to worth
  // retrying on a later pass).
  bool settled = false;
  const auto startLayout = [&](Section& target, const int spine) -> bool {
    // The HTML must already be inflated: startBuild() would otherwise stream it
    // out of the zip, seconds long, under this lock. htmlInflateStep() is the
    // next step in the ladder and re-arms the decline memory when it promotes,
    // so this resolves in a pass or two.
    if (!target.hasHtmlCache()) {
      settled = true;
      return false;
    }
    if (!target.startBuild(lastRenderSpec)) {
      LOG_ERR("ERS", "Failed to start prebuild of section %d", spine);
      settled = true;
      return false;
    }
    return true;
  };

  if (prebuiltSection) {
    if (!prebuiltSection->isBuilding()) {
      // Parked. A parked PARTIAL is stopped dead at its watermark because its
      // startLayout() was refused, and nothing else would ever retry it: the arm
      // below never runs while something is parked here. So retry the resume,
      // on the Arm pass, under exactly the gates the fresh arm uses. The
      // staleness drop above has already proved the spine and the render spec
      // still match, so there is nothing else to re-check. A parked section that
      // is NOT partial is finished work -- nothing to do either way.
      if (phase != PrebuildPhase::Arm || !prebuiltSection->isPartial()) return false;
      // This starts a build, so it owes the same ONE LIVE BUILD CONTEXT check
      // the fresh arm makes. Reaching here with a building or partial `section`
      // should be impossible -- every foreground build start discards the
      // prebuild first (see the header) -- which is exactly why the test is
      // written out rather than argued: it keeps the invariant provable from
      // this function alone.
      if (!section || section->isBuilding() || section->isPartial()) return false;
      // The same settled memory the fresh arm consults, for the same reason: a
      // resume already refused for something that will not change must not be
      // re-attempted (SD setup, 100-300 ms under this lock) every idle pass.
      if (prebuildDeclinedSpine.load(std::memory_order_relaxed) == prebuiltSpineIndex) return false;
      if (layoutStartDeferred()) return false;
      if (!startLayout(*prebuiltSection, prebuiltSpineIndex)) {
        // Refused: the partial STAYS parked either way -- its pages are real up
        // to the watermark and the boundary turn still adopts them (CH_START
        // detail 6). All a settled refusal changes is that this stops asking.
        if (settled) prebuildDeclinedSpine.store(prebuiltSpineIndex, std::memory_order_relaxed);
        return false;
      }
      LOG_DBG("ERS", "Resumed prebuild of partial section %d", prebuiltSpineIndex);
      return true;
    }
    if (phase != PrebuildPhase::Pump) return false;  // build ticks run at the bottom of the ladder
    // Still laying itself out: same tick size, heap gate and per-tick lock
    // scope as the active section's ticks above, and never at the same time as
    // one — the arming gate below refuses to start while `section` builds.
    if (!buildTickHeapGate()) {
      workPlausible = true;  // transient: the 25 ms retry, as the active build's tick takes
      return false;
    }
    if (!prebuiltSection->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
      // buildSomeMore has already abandoned the build. A parse error recurs
      // against the same HTML, so stop offering this spine; the boundary turn
      // reports it exactly as it does today.
      LOG_ERR("ERS", "Prebuild of section %d failed", prebuiltSpineIndex);
      const int failedSpine = prebuiltSpineIndex;
      discardPrebuiltSection();
      prebuildDeclinedSpine.store(failedSpine, std::memory_order_relaxed);
      return false;
    }
    return true;
  }
  if (phase != PrebuildPhase::Arm) return false;  // arming is the Arm pass's job
  // Arm ONLY while this reader is the activity on top. A Push does not stop this
  // activity (see the header), so without this the arm keeps probing SD for as
  // long as the menu, the chapter list, the bookmark list, a footnote, the
  // dictionary, the QR view or the go-to-percent dialog sits on top of the
  // reader -- and those screens raise pendingRenders as they scroll, so the
  // deferral above would hold it on the 25 ms cadence the whole time. Pumping a
  // build that is ALREADY armed stays allowed while stacked (above): that work
  // is bounded and the reader still wants it. ActivityManager::isReaderActivity()
  // is the wrong test here -- it walks the stack, so it stays true for exactly
  // the pushed-sub-activity case this excludes.
  if (!activityManager.isCurrentActivity(this)) return false;
  // Consider prebuilding: current chapter fully built, reader near its end.
  if (!section || section->isBuilding() || section->isPartial() || !lastRenderSpecValid) return false;
  if (section->pageCount == 0 || section->currentPage + PREBUILD_NEAR_END_PAGES < static_cast<int>(section->pageCount))
    return false;
  if (currentSpineIndex + 1 >= epub->getSpineItemsCount()) return false;
  const int buildSpine = currentSpineIndex + 1;
  // settled; don't re-probe every idle tick
  if (prebuildDeclinedSpine.load(std::memory_order_relaxed) == buildSpine) return false;
  // Above the probe, not inside the layout start below it: see layoutStartDeferred().
  if (layoutStartDeferred()) return false;

  auto candidate = std::unique_ptr<Section>(new Section(epub, buildSpine, renderer));
  const bool loaded = candidate->loadSectionFile(lastRenderSpec);
  // No layout cache at all — the state every next chapter of a book being read
  // forward for the first time is in — or one that loaded as a PARTIAL, which is
  // now the common artifact: every discarded in-flight prebuild suspends to one.
  // A parked partial stops dead at its watermark, so resume its build here, the
  // same way loop()'s foreground extension resumes the active section's.
  if (!loaded || candidate->isPartial()) {
    if (!startLayout(*candidate, buildSpine) && !loaded) {
      // Nothing loaded, so there is nothing worth parking.
      if (settled) prebuildDeclinedSpine.store(buildSpine, std::memory_order_relaxed);
      return false;
    }
    // A partial that could not resume is still parked: its pages are real up to
    // the watermark, and the boundary turn's foreground extension picks up the
    // rest (CH_START detail 6). A transient refusal is not the end of it -- the
    // resume path above re-attempts it from the next Arm pass onwards.
  }
  prebuiltSpineIndex = buildSpine;
  prebuiltSpec = lastRenderSpec;
  prebuiltSection = std::move(candidate);
  LOG_DBG("ERS", "Prebuilt next section %d (%s)", buildSpine,
          prebuiltSection->isBuilding()  ? "building"
          : prebuiltSection->isPartial() ? "partial"
                                         : "ready");
  return true;
}

bool EpubReaderActivity::htmlInflateAbortRequested(void* ctx) {
  return static_cast<EpubReaderActivity*>(ctx)->bgHtmlInflateAbort.load(std::memory_order_acquire);
}

EpubReaderActivity::HtmlInflateCancel EpubReaderActivity::cancelBackgroundHtmlInflate() {
  if (!bgHtmlInflateActive.load(std::memory_order_acquire)) return HtmlInflateCancel::Idle;
  bgHtmlInflateAbort.store(true, std::memory_order_release);
  const uint32_t start = millis();
  while (bgHtmlInflateActive.load(std::memory_order_acquire)) {
    if (millis() - start >= HTML_INFLATE_CANCEL_TIMEOUT_MS) {
      // Deliberately leaves the abort flag raised (see the header note): the
      // inflate we failed to join can then still not promote its temp file.
      // It DOES still hold an open write handle, which is why destructive
      // callers must treat TimedOut as "decline".
      LOG_ERR("ERS", "Background HTML inflate (spine %d) did not stop within %ums", bgHtmlInflateSpine,
              (unsigned)HTML_INFLATE_CANCEL_TIMEOUT_MS);
      return HtmlInflateCancel::TimedOut;
    }
    delay(1);
  }
  LOG_DBG("ERS", "Background HTML inflate stopped in %ums", (unsigned)(millis() - start));
  return HtmlInflateCancel::Cancelled;
}

// One HTML pre-inflate step, run on the background build task when there is
// nothing left to build and no cached prebuild to adopt. See the design note in
// the header for the two-phase split and the interlock's invariants.
//
// Not gated on lastRenderSpecValid, unlike prebuildStep: the html cache is keyed
// only on the book, never on render settings, so no spec has to match for the
// bytes to stay usable -- which is also why nothing here can be invalidated by a
// settings change while it runs.
bool EpubReaderActivity::htmlInflateStep(bool& workPlausible) {
  std::shared_ptr<Epub> epubRef;
  std::string localPath;
  std::string tmpPath;
  int target = -1;

  {
    RenderLock lock{RenderLock::TryAcquire{}};
    if (!lock.locked()) {
      workPlausible = true;
      return false;
    }
    // Never while a build is running: this is strictly lower priority than the
    // chapter the reader is waiting on, and a build's own parse pump inflates
    // images out of the same zip. A partial section is the same case one step
    // ahead -- loop() starts its extension build as soon as the reader nears
    // the watermark, and that build would immediately cancel this.
    if (!epub || !section || section->isBuilding() || section->isPartial()) return false;
    // Same near-the-end arming window as the cached prebuild: three pages of
    // reading is minutes, an inflate is seconds.
    if (section->pageCount == 0 ||
        section->currentPage + PREBUILD_NEAR_END_PAGES < static_cast<int>(section->pageCount))
      return false;

    target = currentSpineIndex + 1;
    if (target >= epub->getSpineItemsCount()) return false;
    if (htmlInflateDeclinedSpine == target) return false;
    // A parked prebuild means this spine needs nothing from here: it loaded a
    // layout cache (which an inflate must have run for), or it is laying one
    // out, which prebuildStep only starts once the HTML is there. The one state
    // that is neither is a partial parked without a resume -- prebuildStep tries
    // to resume every partial it loads, so that means the resume was refused,
    // and the refusal this step could lift (no HTML cache) is the rarest of them
    // (a partial is written by a build that had the HTML). Left to the reader's
    // own foreground extension rather than special-cased here.
    if (prebuiltSection && prebuiltSpineIndex == target) return false;
    if (gateFreeHeap() < HTML_INFLATE_MIN_FREE_HEAP || gateMaxAllocHeap() < HTML_INFLATE_MIN_MAX_ALLOC) return false;

    if (Storage.exists(Section::htmlCachePath(*epub, target).c_str())) {
      htmlInflateDeclinedSpine = target;  // already inflated by an earlier visit
      return false;
    }
    // Reads the shared book.bin metadata handle, so it belongs in this phase;
    // the unlocked phase only ever sees the resulting value.
    localPath = epub->getSpineItem(target).href;
    if (localPath.empty()) {
      htmlInflateDeclinedSpine = target;
      return false;
    }
    tmpPath = Section::htmlBackgroundTmpPath(*epub, target);

    // A shared_ptr copy for the same reason the pre-decode takes one:
    // launchKOReaderSync() drops the Epub under the RenderLock WITHOUT joining
    // this task, to free RAM for the TLS handshake.
    epubRef = epub;
    bgHtmlInflateSpine = target;
    bgHtmlInflateAbort.store(false, std::memory_order_relaxed);
    bgHtmlInflateActive.store(true, std::memory_order_release);
  }

  // ---- No RenderLock held from here ----------------------------------------
  const unsigned long t0 = millis();
  const Section::HtmlInflate result = Section::inflateHtmlToCache(*epubRef, localPath, target, tmpPath,
                                                                  &EpubReaderActivity::htmlInflateAbortRequested, this);
  // Publish the stop before anything else: a canceller is spinning on this.
  bgHtmlInflateActive.store(false, std::memory_order_release);

  // htmlInflateDeclinedSpine is written from here, off the lock, which is safe
  // because only this task ever touches it (see the header note).
  switch (result) {
    case Section::HtmlInflate::Promoted:
      htmlInflateDeclinedSpine = target;
      // prebuildStep settled this spine because its HTML was not inflated yet
      // (that is why this ran); it is now, so let the next pass lay the chapter
      // out. Written off the lock, unlike every other access to it: it is
      // std::atomic for exactly this one site (see the header), and the only
      // other writer that can run concurrently is the loop task's boundary hook,
      // which writes the same -1 -- so the worst a race can cost is one extra
      // probe.
      if (prebuildDeclinedSpine.load(std::memory_order_relaxed) == target)
        prebuildDeclinedSpine.store(-1, std::memory_order_relaxed);
      LOG_DBG("ERS", "Pre-inflated HTML for spine %d in %lums", target, millis() - t0);
      return true;
    case Section::HtmlInflate::TempOnly:
      // The bytes never became the shared cache, and only the synchronous build
      // parses its own temp -- this one is dead weight on the card.
      Storage.remove(tmpPath.c_str());
      htmlInflateDeclinedSpine = target;
      return false;
    case Section::HtmlInflate::Aborted:
      // A render or build took the zip over, or the reader is leaving. Not a
      // property of the spine, so leave the decline memory unarmed and let a
      // later idle pass start over (the inflate is not resumable -- a zip entry
      // has no restart point -- so this discards the partial work by design).
      LOG_DBG("ERS", "Pre-inflate of spine %d aborted", target);
      return false;
    case Section::HtmlInflate::Failed:
      // Storage or a corrupt entry: a property of this spine, so stop offering
      // it. The boundary turn still inflates it synchronously, exactly as today.
      htmlInflateDeclinedSpine = target;
      return false;
  }
  return false;
}
#endif  // CROSSPOINT_NEXT_SECTION_PREBUILD

#ifdef CROSSPOINT_BG_IMAGE_DECODE
// One image pre-decode step, run on the background build task when there is
// nothing left to build or prebuild. First view of an in-book image costs a
// 0.5-3 s decode today, taken inside the render behind a placeholder pass; this
// moves it off the page-turn critical path by decoding the images of upcoming
// pages into their .pxc files ahead of time.
//
// Two phases, and the split is the whole design:
//
// UNDER the (try-acquired) RenderLock -- everything that touches reader state:
// pick the nearest lookahead page not already ruled out, deserialize it,
// find the first image with no usable cache, and capture its path, source
// href and geometry AS VALUES. Also captured: a shared_ptr copy of the Epub,
// the decoder pointer (resolving it touches ImageDecoderFactory's non-
// thread-safe lazy init, which only the lock serializes), and the "a background
// decode is running" flag plus the image it is running on (published here,
// before the lock is released, so no render can start without seeing them).
//
// WITHOUT the lock: extract the image out of the book if needed, then decode it
// in cacheOnly mode. Both steps touch only the SD card through Storage (which
// is mutex'd per operation), the pixel cache, and the heap -- no Section, no
// Epub member, no renderer.
//
// Why a shared_ptr copy of the Epub rather than ImageBlock's extractor hook:
// that hook's context is a raw Epub*, and launchKOReaderSync() drops the Epub
// under the RenderLock WITHOUT joining this task, to free RAM for the TLS
// handshake. (onExit's clear is safe -- it happens after stopBgBuildTask()
// joins -- but the sync path's is not.) A shared_ptr copy keeps the object
// alive for the length of this step no matter which path runs.
//
// Staleness is a non-issue: a .pxc is keyed by the image, not by the page it
// appears on, so re-pagination cannot invalidate one. The only correctness
// requirement is that two decoders never write the same file, which the
// ImageBlock interlock handles (see the note there).
bool EpubReaderActivity::imageDecodeStep(bool& workPlausible) {
  std::shared_ptr<Epub> epubRef;
  // Resolved under the lock and carried into the unlocked phase: the factory's
  // lazy singleton init is not thread-safe, so it must not be reached from
  // there (see ImageBlock::backgroundDecoderFor).
  ImageToFramebufferDecoder* decoder = nullptr;
  std::string imagePath;
  std::string srcPath;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int screenWidth = 0;
  int screenHeight = 0;
  int offset = 0;

  {
    RenderLock lock{RenderLock::TryAcquire{}};
    if (!lock.locked()) {
      workPlausible = true;
      return false;
    }
    // A building section re-numbers pages under the scan, and the margins
    // snapshot only exists once a render has happened.
    if (!epub || !section || section->isBuilding() || !lastRenderMarginsValid) return false;

    // The cursor belongs to one reading position; the reader moving re-arms the
    // whole window. Only this task touches these three.
    if (imageDecodeSpine != currentSpineIndex || imageDecodeBasePage != section->currentPage) {
      imageDecodeSpine = currentSpineIndex;
      imageDecodeBasePage = section->currentPage;
      imageDecodeDeclined = 0;
    }
    if (gateFreeHeap() < IMAGE_DECODE_MIN_FREE_HEAP || gateMaxAllocHeap() < IMAGE_DECODE_MIN_MAX_ALLOC) return false;

    std::unique_ptr<Page> page;
    // The page this scan reads: either `page` above, or the resident cache entry
    // borrowed in place (see below). Valid only inside this locked scope.
    const Page* scanPage = nullptr;
    for (int i = 1; i <= IMAGE_DECODE_LOOKAHEAD; i++) {
      if (imageDecodeDeclined & (1u << i)) continue;
      const int pageNumber = section->currentPage + i;
      if (pageNumber >= static_cast<int>(section->pageCount)) {
        imageDecodeDeclined |= (1u << i);  // past the end of the chapter
        continue;
      }
      offset = i;
#if defined(CROSSPOINT_PAGE_CACHE) && defined(CROSSPOINT_BG_IMAGE_DECODE)
      // Composition hook for PR #3050 (CROSSPOINT_PAGE_CACHE), whose one-entry
      // page cache usually holds exactly this page (currentPage + 1 is the first
      // lookahead slot); deserializing a second copy of it would cost the SD read
      // that cache exists to avoid, plus a second live Page. Borrow it instead:
      // the scan below is read-only (const element access), so nothing here can
      // disturb the entry the next forward turn is going to consume. Full key
      // check, same fields and same lock as takeCachedPage -- minus the consume:
      // the entry is left in place, and any mismatch just falls through to the
      // deserialize. Undefined on this branch; compiled in whenever both flags
      // are on, in either merge order.
      if (cachedPage && cachedPageGeneration == sectionGeneration && cachedPageSpine == currentSpineIndex &&
          cachedPageNumber == pageNumber && cachedPagePageCount == section->pageCount &&
          cachedPagePartial == section->isPartial()) {
        scanPage = cachedPage.get();
        break;
      }
#endif
      page = section->loadPage(pageNumber);
      scanPage = page.get();
      break;
    }
    if (offset == 0) return false;  // window exhausted until the reader moves
    if (!scanPage) {
      imageDecodeDeclined |= (1u << offset);
      return false;
    }

    screenWidth = renderer.getScreenWidth();
    screenHeight = renderer.getScreenHeight();
    for (const auto& element : scanPage->elements) {
      if (element->getTag() != TAG_PageImage) continue;
      const auto& pageImage = static_cast<const PageImage&>(*element);
      const ImageBlock& block = pageImage.getImageBlock();
      if (!block.needsDecode()) continue;
      // Exactly the geometry ImageBlock::render() would use, including its
      // bounds check: an image the render would reject is not worth decoding.
      const int px = pageImage.xPos + lastRenderMarginLeft;
      const int py = pageImage.yPos + lastRenderMarginTop;
      if (px < 0 || py < 0 || px + block.getWidth() > screenWidth || py + block.getHeight() > screenHeight) continue;
      decoder = ImageBlock::backgroundDecoderFor(block.getImagePath());
      if (!decoder) continue;  // no decoder for this format
      imagePath = block.getImagePath();
      srcPath = block.getSourcePath();
      x = px;
      y = py;
      width = block.getWidth();
      height = block.getHeight();
      break;
    }
    if (imagePath.empty()) {
      imageDecodeDeclined |= (1u << offset);  // nothing left to decode on this page
      return false;
    }

    epubRef = epub;
    ImageBlock::beginBackgroundDecode(imagePath);
  }

  // ---- No RenderLock held from here ----------------------------------------
  if (!srcPath.empty() && !Storage.exists(imagePath.c_str())) {
    // The one step of this that cannot be aborted; it is bounded by the
    // image's stored size (see BG_DECODE_CANCEL_TIMEOUT_MS).
    if (!epubRef->extractItemToFile(srcPath, imagePath)) {
      LOG_DBG("ERS", "Pre-decode extraction failed: %s", srcPath.c_str());
    }
  }

  bool decoded = false;
  // Whoever wanted this decode stopped raised the abort flag while we were in
  // the extract above, which is the one step that cannot honor it. Starting the
  // decode anyway is the bad case: the canceller may give up waiting and lower
  // the flag (BG_DECODE_CANCEL_TIMEOUT_MS) precisely because the extract took
  // that long, and the decode would then run to completion -- as a second
  // writer on the file the render task is about to produce, or for seconds
  // inside an exit join.
  bool aborted = ImageToFramebufferDecoder::abortRequested();
  if (!aborted && !bgBuildStop.load(std::memory_order_acquire)) {
    const unsigned long t0 = millis();
    decoded =
        ImageBlock::decodeToCacheOnly(renderer, decoder, imagePath, x, y, width, height, screenWidth, screenHeight);
    if (decoded) {
      LOG_DBG("ERS", "Pre-decoded %s in %lums", imagePath.c_str(), millis() - t0);
    } else {
      // Read BEFORE endBackgroundDecode(): a canceller only lowers the flag
      // once it observes the decode has stopped, so while the "active" flag is
      // still up the abort flag cannot go back down under this read.
      aborted = ImageToFramebufferDecoder::abortRequested();
    }
  }
  ImageBlock::endBackgroundDecode();

  if (aborted) {
    // Not a property of the image: the render task took it over (or the reader
    // is leaving). Declining here would disarm this lookahead slot until the
    // reader moves, and on an image-dense chapter the takeover happens on
    // exactly the pages worth pre-decoding. Leave the cursor alone and retry.
    LOG_DBG("ERS", "Pre-decode aborted: %s", imagePath.c_str());
    return false;
  }
  if (!decoded) {
    // Heap, format, or a corrupt file -- a property of this image, so stop
    // offering this page: the render path still decodes whatever it needs, and
    // retrying here every 25 ms would not help.
    imageDecodeDeclined |= (1u << offset);
    return false;
  }
  // Deliberately NOT declined on success: the next pass re-scans the same page
  // for a second image, and declines it once there is nothing left.
  return true;
}
#endif  // CROSSPOINT_BG_IMAGE_DECODE

void EpubReaderActivity::loop() {
  if (!epub) {
    finish();
    return;
  }

#ifdef CROSSPOINT_USAGE_LOG
  // See ulogPendingReady in the header: the render task has just put the first
  // page of the book / section it belongs to on the panel.
  if (ulogPendingReady != 0) {
    if (lastRenderCompleteMs.load(std::memory_order_relaxed) != ulogRenderMsAtLoad) {
      if (ulogPendingReady == 1) {
        usageLog.noteBookReady();
      } else {
        usageLog.noteSectionReady(ulogPendingReady == 2);
      }
      ulogPendingReady = 0;
    } else if (millis() - ulogPendingStartMs > ULOG_READY_DEADLINE_MS) {
      // Nothing painted in 90 s. Drop the pending rather than let it answer a
      // frame from some unrelated, much later render. See the deadline note on
      // ULOG_READY_DEADLINE_MS.
      ulogPendingReady = 0;
    }
  }
#endif

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  const uint32_t lastRenderMs = lastRenderCompleteMs.load(std::memory_order_relaxed);
  if (section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() && lastRenderMs != 0 &&
      millis() - lastRenderMs > IDLE_PREWARM_DEBOUNCE_MS && gateFreeHeap() > RENDER_MIN_FREE_HEAP &&
      gateMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock;
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        auto p = section->loadPage(nextPage);
        if (p) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
#ifdef CROSSPOINT_PAGE_CACHE
          // Keep the deserialized page instead of discarding it: the next forward
          // turn is the render that asks for exactly this (spine, page). The scan
          // pass does not mutate it (Page::render is const, and the only render-time
          // state is ImageBlock's static payload cache), so the retained object is
          // equivalent to a fresh load. Still under the prewarm's RenderLock.
          storeCachedPage(nextPage, std::move(p));
#endif
        }
      }
    }
  }

  // section is owned by the RenderLock: the render task resets or replaces it on its
  // failure/load paths while holding the lock, so even eligibility peeks must not
  // dereference it unlocked -- a concurrent reset would leave this task reading a freed
  // Section mid-expression. Only lock-free state is checked outside. The acquire is
  // NON-BLOCKING: this outer gate passes on virtually every pass, and a blocking
  // acquire that loses the peek→acquire race would park the loop task behind a whole
  // page render, stalling input polling. Deferrable work retries next pass instead.
  if (!RenderLock::peek() && buildViewportWidth > 0 && !partialRebuildStartFailed) {
    RenderLock lock{RenderLock::TryAcquire{}};
    if (lock.locked() && section && !section->isBuilding() && section->isPartial() &&
        section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
#ifdef CROSSPOINT_BG_IMAGE_DECODE
      // Same reason as the renderBook()-site cancel: this build's parse pump (the
      // buildSomeMore ticks that follow, wherever they run) extracts images out of
      // the book to probe their dimensions, and a pre-decode in flight extracts
      // too, with no lock -- both derive the same destination path from the
      // book-internal href, so the two would be writing the SAME file. Stop it
      // before the build exists, and only once the re-check says there IS going
      // to be a build: the wait is bounded by the cancel timeout and there is no
      // reason to pay it for a start that a render just made moot. On a timeout
      // the overlap stays open exactly as it does at the render site.
      ImageBlock::cancelBackgroundDecode();
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
      // And for the same file-level reason, one spine over: the section this is
      // about to extend can be a prebuild adopted at a chapter boundary, i.e.
      // exactly the spine a pre-inflate started for while the reader was still
      // in the previous chapter. startBuild() would then be inflating the same
      // zip entry into the same html cache from the other core.
      cancelBackgroundHtmlInflate();
      // Second foreground build start, so the same one-live-build-context rule
      // as the render site: drop a prebuild that is laying out a chapter of its
      // own before this one takes the Epub's CssParser. Reachable only via an
      // adopted partial (a prebuild is armed only while the current section is
      // complete and non-partial), which is exactly the pairing the cancel
      // above exists for; the discard is a line and keeps the rule local.
      discardPrebuiltSection();
#endif
      const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
      if (!section->startBuild(buildSpec)) {
        partialRebuildStartFailed = true;
        LOG_ERR("ERS", "Failed to start deferred partial extension build");
      } else {
        LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
                section->pageCount);
#ifdef CROSSPOINT_BG_BUILD_TASK
        // The one build start that doesn't flow through a render: wake the build
        // task directly so pickup isn't left to its long idle-wait fallback.
        if (bgBuildTaskHandle) xTaskNotifyGive(bgBuildTaskHandle);
#endif
      }
    }
  }

#ifdef CROSSPOINT_BG_BUILD_TASK
  // The core-0 build task owns the pump. Consume its completion/failure
  // notifications here in the loop task, so section reset, reposition, and
  // redraw run in their usual context; the loop-tick pump below stays only as
  // a runtime fallback for the (never observed) case that task creation failed.
  if (bgBuildFailedNotify.exchange(false, std::memory_order_acq_rel)) {
    RenderLock lock;
    LOG_ERR("ERS", "Background section build failed");
    section.reset();
    requestUpdate();
  }
  if (bgBuildCompleteNotify.exchange(false, std::memory_order_acq_rel)) {
    RenderLock lock;
    // cppcheck-suppress knownConditionTrueFalse
    if (section && section->isBuildComplete() && applyDeferredReposition()) {
      requestUpdate();
    }
  }
  // Same locking rule (and same non-blocking acquire) as above; the heap gate is
  // re-checked under the lock because state can shift between gate and acquire.
  // bgBuildTaskHandle is this task's own state (created and cleared here), so it
  // stays outside.
  if (bgBuildTaskHandle == nullptr && !RenderLock::peek() && buildTickHeapGate()) {
#else
  // Same locking rule (and same non-blocking acquire) as above; the heap gate is
  // re-checked under the lock because state can shift between gate and acquire.
  if (!RenderLock::peek() && buildTickHeapGate()) {
#endif
    RenderLock lock{RenderLock::TryAcquire{}};
    if (lock.locked() && section && section->isBuilding() &&
        (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
        buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        requestUpdate();
      }
    }
  }

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      requestUpdate();
      return;
    }
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }
  const bool endOfBookMenuOpen = endOfBookMenuActive();

  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmLongPressed) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (launchKOReaderSync()) {
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        openDictionaryWordSelect();
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Home-key boards have no front Confirm button, so a Home-key hold runs the
  // same user-selected long-press action. The SDK emits this event once per
  // hold and suppresses the short Home tap for the same contact.
  if (mappedInput.wasHomeKeyHold() && !endOfBookMenuOpen) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case CrossPointSettings::LP_MENU_KOSYNC:
        launchKOReaderSync();
        return;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (!showDictionaryMessage) {
          openDictionaryWordSelect();
        }
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
        if (usesToolbarMenu() && section) {
          openOverlay(Overlay::Toolbar);
        } else {
          openReaderMenu();
        }
        return;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Link taps take priority over the reader-menu and page-turn zones.
  if (!atEndOfBook && !currentPageLinks.empty() && SETTINGS.touchReaderControls && mappedInput.hasTouch()) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      const auto* link = EpubReaderUtils::linkAtPoint(currentPageLinks, touchX, touchY, currentPageLinkMarginLeft,
                                                      currentPageLinkMarginTop);
      if (link) {
        navigateToHref(link->href, true);
        return;
      }
    }
  }

  if (confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      pendingManualTurn = 0;
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
  }

  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    if (pageTurn(forward)) {
#ifdef CROSSPOINT_USAGE_LOG
      usageLog.notePageTurn(forward);
#endif
    }
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (skipPages(nextTriggered ? 1 : -1)) {
#ifdef CROSSPOINT_USAGE_LOG
      // A chapter skip, not a page turn: the log records it as such.
      usageLog.notePageTurn(nextTriggered, true);
#endif
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    return;
  }

  // One call so the usage log's PAGE hook sees the direction and whether the
  // page actually moved; the empty body is what a flags-off build compiles to.
  const bool forward = !prevTriggered;
  if (pageTurn(forward)) {
#ifdef CROSSPOINT_USAGE_LOG
    usageLog.notePageTurn(forward);
#endif
  }
  requestUpdate();
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) return;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock;
        clearDeferredReposition();
        if (section && currentSpineIndex == sync.spineIndex) {
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);
          section.reset();
        }
        requestUpdate();
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      RenderLock lock;
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
      requestUpdate();
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
        // A Push does not stop this activity: the build task keeps pumping while
        // the chapter list is up, and that list clears the font cache out from
        // under the renderer a prebuild measures text through. Drop it here,
        // under the lock already held. (See the header's discard-site list.)
        discardPrebuiltSection();
#endif
      }
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, spineIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 openReaderMenu();
                                 return;
                               }
                               const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                               navigateToHref(footnoteResult.href, true);
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      // Release the section BEFORE the settings screen opens, like
      // SELECT_CHAPTER above, not in the result handler: an external pop (the
      // Home-key "Go Back" action) skips a no-result handler, and a section
      // released only there survives a font change — renderBook() then draws
      // the old layout's word positions with the new size's glyphs (spacing
      // collapses or gapes until something else rebuilds the section).
      // Resetting up front means every return path rebuilds against the
      // settings as they are then, and the settings screen's font previews get
      // the section's tens-of-KB footprint, as the chapter list already does.
      {
        RenderLock lock;
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
        // Same as SELECT_CHAPTER above, and more sharply: the settings screen
        // loads and unloads SD fonts under the renderer this task measures text
        // through, while the Push leaves the task running. Drop the prebuild
        // under the lock already held.
        discardPrebuiltSection();
#endif
      }
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) { openReaderMenu(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult&) { openReaderMenu(); });
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          // abandonBuild() before the reset, for the reason the prebuild's own
          // teardown below states: this section can be mid-build (a chapter
          // still laying itself out, or a partial the reader's extension is
          // resuming), and ~Section's default teardown is suspendBuild(), which
          // commits the pages so far as a partial .bin into the directory the
          // clearCache() below deletes. It is a no-op when no build is active.
          section->abandonBuild();
          section.reset();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
          // The tree about to be deleted is where a background pre-inflate
          // writes. Stop it first — and if it will not stop (cancel timeout),
          // DECLINE the deletion: removeDir would free the cluster chain under
          // the inflate's open write handle (SdFat has no open-file table),
          // cross-linking the filesystem. The user can simply retry.
          if (cancelBackgroundHtmlInflate() == HtmlInflateCancel::TimedOut) {
            LOG_ERR("ERS", "Cache clear declined: background inflate still running");
            onGoHome();
            return;
          }
          // A prebuild that is BUILDING holds an open write handle on the next
          // spine's .bin.tmp, in that same tree, so it has to go ahead of the
          // removal for the reason the cancel above states. (A prebuild that is
          // merely parked holds no handle at all: loadSectionFile() closes the
          // .bin on every path it can return through.) Unlike the inflate it
          // cannot fail to stop: it only ever runs under the lock held here.
          //
          // abandonBuild() FIRST, and only then the discard: ~Section's default
          // teardown is suspendBuild(), which writes the pages laid out so far
          // back out as a partial .bin — an SD commit into a directory the very
          // next line deletes. abandonBuild drops the build context instead
          // (closing and removing the tmp file), which both skips that commit
          // and leaves the Section plainly destructible: with build_ cleared,
          // the destructor's suspendBuild() returns immediately. It is a no-op
          // on a prebuild that only loaded a cache and never started a build.
          if (prebuiltSection) prebuiltSection->abandonBuild();
          discardPrebuiltSection();
#endif
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (SETTINGS.longPressMenuFunction) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
    case CrossPointSettings::LP_MENU_DICTIONARY:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case CrossPointSettings::LP_MENU_KOSYNC:
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case CrossPointSettings::LP_MENU_READER_MENU:
    case CrossPointSettings::LP_MENU_DISABLED:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock;
    if (section) {
      nextPageNumber = section->currentPage;
    }
    ImageBlock::setExtractor(nullptr, nullptr);
#ifdef CROSSPOINT_PAGE_CACHE
    // Freeing RAM for the handshake is the entire point of this block, and a
    // retained Page is tens of KB of it. Dropping it here also keeps the
    // "heap after" log below honest.
    dropCachedPage();
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
    // Freeing RAM for the handshake is the entire point of this block, and a
    // parked prebuild is both a Section of its own and a shared_ptr keeping the
    // Epub alive past the reset below. Dropping it here also keeps the "heap
    // after" log honest.
    discardPrebuiltSection();
#endif
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // The viewport just changed, so the parked layout is for a spec nothing will
  // use again. Drop it under the lock held here rather than leaving it to the
  // task's own staleness check, which would let it keep building meanwhile.
  discardPrebuiltSection();
#endif
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
    // The status bar changed height, so the viewport did: same spec-abandon as
    // applyOrientation above.
    discardPrebuiltSection();
#endif
  }
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!section) return false;
  {
    RenderLock lock;
    clearDeferredReposition();
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
#ifdef CROSSPOINT_USAGE_LOG
      // Without the prebuild compiled in, every forward boundary loads on
      // demand; the arm below refines this when it is.
      UsageLog::SectionSource chSource = UsageLog::SECTION_ON_DEMAND;
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
      const bool adoptPrebuild = prebuiltSection && prebuiltSpineIndex == currentSpineIndex && lastRenderSpecValid &&
                                 renderSpecEquals(prebuiltSpec, lastRenderSpec);
#ifdef CROSSPOINT_USAGE_LOG
      // Read before the adoption moves the state it describes. When the
      // prebuild is not taken, this is the whole diagnosis of why -- which is
      // what makes the hit rate readable off the log rather than guessable.
      if (adoptPrebuild) {
        // Three shapes of hit, and the difference matters: a live build finishes
        // the pages the render needs, a finalized cache already has them all,
        // and a parked PARTIAL has neither -- it stops at its watermark until
        // loop()'s foreground extension resumes it.
        chSource = prebuiltSection->isBuilding()  ? UsageLog::SECTION_PREBUILT_BUILDING
                   : prebuiltSection->isPartial() ? UsageLog::SECTION_PREBUILT_PARTIAL
                                                  : UsageLog::SECTION_PREBUILT;
      } else if (!prebuiltSection) {
        chSource = UsageLog::SECTION_ON_DEMAND;
      } else if (prebuiltSpineIndex != currentSpineIndex) {
        chSource = UsageLog::SECTION_SPINE_MISMATCH;
      } else {
        chSource = UsageLog::SECTION_SPEC_MISMATCH;
      }
#endif
      if (adoptPrebuild) {
        // Swap the prebuilt Section in: renderBook() sees a loaded section and
        // skips the synchronous load-or-build entirely. Mirror the cache-hit
        // side effects of renderBook()'s construction path; a still-building
        // prebuild continues via the existing incremental machinery.
        section = std::move(prebuiltSection);
        prebuiltSpineIndex = -1;
        prebuildDeclinedSpine.store(-1, std::memory_order_relaxed);
        section->currentPage = 0;
        cachedChapterTotalPageCount = 0;
        cachedVisibleTextOffset.reset();
        partialRebuildStartFailed = false;
#ifdef CROSSPOINT_PAGE_CACHE
        // Composition hook for PR #3050 (CROSSPOINT_PAGE_CACHE), which keys its
        // one-entry page cache on sectionGeneration and documents renderBook()
        // as the only site that installs a `section`. This adoption is the
        // second such site, and the entry it would leave behind names the
        // previous chapter's pagination. Undefined on this branch; compiled in
        // whenever both flags are on, in either merge order.
        sectionGeneration++;
        dropCachedPage();
#endif
      } else {
        discardPrebuiltSection();
        section.reset();
      }
#else
      section.reset();
#endif
#ifdef CROSSPOINT_USAGE_LOG
      // Decided at the boundary itself, so the CH_START row can state the
      // source (and, when it is not the prebuild, the reason) without waiting
      // for the render.
      ulogNoteSectionStart(true, chSource);
#endif
      lastPageTurnTime = millis();
      return true;
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
      lastPageTurnTime = millis();
      return true;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
      // Backwards over a boundary: whatever is parked was armed for the spine
      // AFTER the one being left, which the reader is now two away from. Drop it
      // under the lock held here so it stops building immediately, rather than
      // one bg pass later when the staleness check notices.
      discardPrebuiltSection();
#endif
#ifdef CROSSPOINT_USAGE_LOG
      // There is no backward prebuild: a back-boundary turn always loads.
      ulogNoteSectionStart(false, UsageLog::SECTION_ON_DEMAND);
#endif
      lastPageTurnTime = millis();
      return true;
    }
  }
  return false;
}

#ifdef CROSSPOINT_USAGE_LOG
void EpubReaderActivity::ulogNoteSectionStart(const bool isForward, const UsageLog::SectionSource source) {
  usageLog.noteSectionStart(isForward, source);
  ulogRenderMsAtLoad = lastRenderCompleteMs.load(std::memory_order_relaxed);
  ulogPendingReady = isForward ? 2 : 3;
  ulogPendingStartMs = millis();
}
#endif

bool EpubReaderActivity::skipPages(int amount) {
  if (!section) return false;
  if (amount > 0) {
    RenderLock lock;
    nextPageNumber = 0;
    currentSpineIndex++;
    section.reset();
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

bool EpubReaderActivity::skipLoopDelay() {
#ifdef CROSSPOINT_BG_BUILD_TASK
  // With the core-0 build task, loop() no longer needs fast full-clock ticks to
  // pump the build (the battery cost the loop-driven design paid); only fall
  // back to loop pacing if the task failed to start. Builds may then run at the
  // idle CPU clock — slower per page but still seconds per chapter, off-core.
  // The short-circuit also keeps buildHeapPaused a single-writer field: with the
  // task alive, only that task calls buildTickHeapGate() — its active-section
  // ticks and its prebuild ticks alike, both on the same thread.
  if (bgBuildTaskHandle != nullptr) return false;
#endif
  return section && section->isBuilding() && !buildHeapPaused &&
         (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
}

void EpubReaderActivity::renderBook() {
  currentPageLinks.clear();
  if (!epub) return;

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
    // drawPopup() displays: a frame IS on the panel, so this is a render
    // completion like any other. See lastRenderCompleteMs in the header.
    lastRenderCompleteMs.store(millis(), std::memory_order_relaxed);
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Snapshot for the prebuild task (renderBook runs under the RenderLock here;
  // the task only reads these under the same lock). Also invalidates stale
  // prebuilds: the task compares its captured spec against this every pass.
  lastRenderSpec = renderSpec;
  lastRenderSpecValid = true;
#endif
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // Snapshot for the image pre-decode (same lock, same reason as above): these
  // are the offsets renderContents hands to Page::render, so an image's on-page
  // origin is xPos/yPos plus these. The pre-decode must match them exactly --
  // the screen clip and the dither phase both key off absolute position.
  lastRenderMarginLeft = static_cast<int16_t>(orientedMarginLeft);
  lastRenderMarginTop = static_cast<int16_t>(orientedMarginTop);
  lastRenderMarginsValid = true;
#endif
#ifdef CROSSPOINT_BG_BUILD_TASK
  // Arm the build task: nearly every state change it cares about (build
  // started, page turned, spec changed) flows through a render, so one notify
  // here retires its need to idle-poll — the lazy partial-extension start in
  // loop() is the exception and notifies directly. The task wakes, fails the
  // TryAcquire while this render holds the lock, and settles on the short retry
  // cadence until the lock frees.
  if (bgBuildTaskHandle) xTaskNotifyGive(bgBuildTaskHandle);
#endif

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
#ifdef CROSSPOINT_BG_IMAGE_DECODE
    // Everything below -- the Section, its loadSectionFile, and the build this
    // may start -- can run the chapter parser, which extracts images out of the
    // book to probe their dimensions. A pre-decode in flight extracts too, with
    // no lock, and the two would be writing the SAME file (both derive the path
    // from the book-internal href). Stop it first; the wait is bounded by the
    // cancel timeout and this is already the slow path (a chapter load), not a
    // page turn. On a timeout the overlap stays open exactly as it is today --
    // there is no useful way for a render to decline loading its chapter -- but
    // that cap is a liveness backstop, not an expected path.
    ImageBlock::cancelBackgroundDecode();
#endif
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
    // The chapter being loaded here is, at a forward boundary turn, the very
    // spine a background pre-inflate may still be streaming. Two things then
    // collide: startBuild() would inflate the same zip entry (each writer has
    // its own temp path, so the loser can at worst waste its work -- but the
    // promotion rename must not land under the parser's open handle), and the
    // FrameBufferLoan taken further down lends the framebuffer's bytes to
    // whichever inflater claims them first, then takes them back and draws over
    // them. Stopping the pre-inflate here settles both, and this is already the
    // slow path (a chapter load), not a page turn.
    //
    // This is the only render that has to cancel, and the argument is worth
    // stating because the trailing partial-extension loop in this function also
    // starts builds, with a section already installed: a pre-inflate only ever
    // runs while the CURRENT section is complete and NON-partial (see
    // htmlInflateStep), and isPartial() is fixed when a Section loads its file,
    // so that loop's isPartial() guard cannot pass while one is in flight.
    // Adopting a prebuilt PARTIAL section at a boundary can create the pairing,
    // and that is exactly what loop()'s deferred extension start cancels for.
    // Keeping cancels off the ordinary page-turn path matters: those are the
    // turns that happen while a pre-inflate is running, and a cancelled inflate
    // has to start over from zero.
    cancelBackgroundHtmlInflate();
    // And the same one-live-build-context rule the header states: this branch
    // is the reader's own build start, so a prebuild laying out its chapter has
    // to go first — both would otherwise hold the Epub's single CssParser. No
    // wait is needed (unlike the inflate above): prebuild ticks run only under
    // the RenderLock this render holds, so holding it is proof none is in
    // flight. The pages it did lay out are suspended to a partial .bin that the
    // eventual visit to that spine resumes from. The other foreground build
    // start — the partial-extension loop further down — needs no discard of its
    // own: it requires section->isPartial(), which is fixed when a Section
    // loads its file, and a prebuild is only ever armed while the current
    // section is complete and NON-partial.
    discardPrebuiltSection();
#endif
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
#ifdef CROSSPOINT_PAGE_CACHE
    // New Section object: anything cached against the old one names a pagination
    // that no longer exists. This is the ONLY site that installs a `section`, so
    // it is the single funnel for every reset path (settings, spine, orientation,
    // footnote nav, jumps, auto-turn toggle, error recovery) -- all of them
    // section.reset() and come back through here.
    sectionGeneration++;
    dropCachedPage();
#endif
    partialRebuildStartFailed = false;

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    // A frame went to the panel, so this counts as a render completion even
    // though no page rendered. Without it the idle prewarm keeps its stale
    // stamp and a pending usage-log _RDY would be answered by a much later
    // frame. See lastRenderCompleteMs in the header.
    lastRenderCompleteMs.store(millis(), std::memory_order_relaxed);
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    lastRenderCompleteMs.store(millis(), std::memory_order_relaxed);  // frame on the panel; see above
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
#ifdef CROSSPOINT_PAGE_CACHE
    // First, the idle prewarm's already-deserialized page, when it names this exact
    // (spine, page) under an unchanged section -- the forward turn then touches SD
    // not at all. Consumed here and nowhere else, under the RenderLock that the fill
    // and every build tick also take: that lock is what keeps the KEY FIELDS
    // (generation, spine, pageCount, isPartial) from moving between the check and the
    // use. It does not cover section->currentPage, which pageTurn() moves from the
    // loop task unlocked -- but that only selects which page is asked for, so a stale
    // read costs a miss, never a wrong page. A miss falls through.
    // Note this is a consume, not a fill: renderContents() below moves the Page (and
    // currentPageFootnotes steals its footnotes first), so the render path cannot
    // hand a copy back to the cache without a second deserialize. The prewarm is the
    // only fill site, which is what keeps the cost at one retained Page.
    auto p = takeCachedPage(section->currentPage);
    if (!p) p = section->loadPage(section->currentPage);
#else
    auto p = section->loadPage(section->currentPage);
#endif
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        lastRenderCompleteMs.store(millis(), std::memory_order_relaxed);  // frame on the panel; see above
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);
    currentPageLinks = std::move(p->links);
    currentPageLinkMarginLeft = orientedMarginLeft;
    currentPageLinkMarginTop = orientedMarginTop;

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs.store(millis(), std::memory_order_relaxed);
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  std::optional<uint32_t> offset;
  if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  // Scan the status bar too: a CJK book/chapter title redirected to the SD
  // fallback font joins the page's single batch prewarm instead of triggering
  // its own SD pass after the scope ends.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
#ifdef CROSSPOINT_IMG_FACTORY_GRAY
  // Single-activation absolute 4-gray for image pages. The panel's OEM factory
  // waveform paints a finished 4-level frame from ANY prior screen state, so
  // both the B/W base activation and the slow refinement activation (whose
  // wash / inverse-hold / snap phases read as several visible passes) collapse
  // into one. Gated on the driver capability so UC8279-X4 device batches keep
  // today's path; on night mode being off (the SDK's grayscale entry points
  // no-op while inverted, but the intent is explicit here); and on there being
  // no manual refresh gesture — that one keeps the old path for its HALF clean.
  const bool factoryGrayPage = pageHasImages && tiledGrayscale && renderer.supportsFactoryGrayscale() &&
                               SETTINGS.screenInverted == 0 && !manualRefreshPending;
#endif
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  // Tiled grayscale band machinery, shared by the refinement path below and the
  // factory single-pass path: render one plane band-by-band into `bandScratch`,
  // streaming each finished band straight to controller RAM. `pass` draws the
  // frame and is re-run per band, clipped by the strip target.
  constexpr int STRIP_ROWS = 80;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  const auto renderPlaneStrips = [&](const bool lsbPlane, uint8_t* bandScratch, auto&& pass) {
    renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
    for (int y = 0; y < gh; y += STRIP_ROWS) {
      const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
      renderer.beginStripTarget(bandScratch, y, rows);
      renderer.clearScreen(0x00);
      pass();
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, bandScratch, y, rows);
    }
  };

#ifdef CROSSPOINT_UC8179_OVERLAP
  // Overlap the whole-buffer (non-tiled) AA plane renders with the B/W base
  // waveform. That path is fully serial today: the base transition runs to
  // completion with the CPU idle (666 ms measured on UC8179 for stock's
  // XTF_PRE_BW_MID, 1492 ms when the page's turn on the clean cadence comes up),
  // then the CPU renders both AA planes with the panel idle, then the 296 ms
  // gray activation. Starting the base asynchronously lets both plane renders
  // hide inside its waveform, so the page turn costs whichever of the two is
  // longer instead of their sum.
  //
  // Safe because the panel refreshes from CONTROLLER RAM: both planes were
  // streamed before the activation started, so rewriting the host framebuffer
  // while the waveform runs cannot disturb it. The driver keeps its own PSRAM
  // snapshot of the base frame for the post-waveform work that used to read the
  // framebuffer.
  //
  // Tiled panels have their own overlap (overlapRefresh above) and render
  // planes into separate band buffers, so this is exclusive with it.
  //
  // asyncRefreshKeepsOwnFrame() is the load-bearing gate, not
  // supportsAsyncRefresh(): every deferring driver here can start a waveform and
  // return, but most of them re-read the framebuffer in their post-waveform
  // baseline restore, which reusing it as plane scratch would poison. It is a
  // RUNTIME capability — this one image also drives SSD1677 and UC8279-X4
  // batches — and it is false in night mode and with the sunlight fading fix,
  // both of which therefore collapse back to the serial path.
#if defined(CROSSPOINT_UC8179_OVERLAP) && !defined(FREEINK_UC8179_OVERLAP_BASE)
#error "CROSSPOINT_UC8179_OVERLAP requires FREEINK_UC8179_OVERLAP_BASE (SDK half of the overlap feature)"
#endif
  const PsramPlane overlapPlane(
      needsAnyGrayscale && !tiledGrayscale && renderer.asyncRefreshKeepsOwnFrame() ? renderer.getBufferSize() : 0);
  const bool overlapBaseTransition = static_cast<bool>(overlapPlane);
  // Set only by the branches that actually pass async=true below; the scratch
  // being allocated is not the same fact as a waveform being in flight, and
  // the wait/upload epilogue must key on the latter.
  bool baseStartedAsync = false;
#endif

#ifdef CROSSPOINT_IMG_FACTORY_GRAY
  if (factoryGrayPage) {
    auto factoryScratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!factoryScratch) {
      // Nothing has reached the panel yet on this path (the B/W base is
      // deliberately never activated), so bailing silently would leave the user
      // staring at the OLD page. Display the intact B/W framebuffer instead and
      // skip the grays.
      LOG_ERR("ERS", "OOM: factory gray strip scratch (%d bytes); B/W only this page", gwBytes * STRIP_ROWS);
      // HALF when a clean is due: the previous page was almost always itself a
      // factory-gray image page (this path sets pagesUntilFullRefresh = 1), and
      // a FAST differential cannot erase its 4-gray content — the fallback
      // would land under heavy ghosting, defeating its purpose.
      renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh = 1;
      return;
    }
    // Under the absolute encoding an unmarked pixel is DRIVEN white, so each
    // plane pass has to carry the whole frame — page content and status bar —
    // not just the pixels a refinement pass would touch. The B/W render above
    // still stands in the framebuffer: cleanupGrayscaleWithFrameBuffer() needs
    // it to re-seed the RED-RAM differential baseline.
    const auto renderFullFrame = [&]() {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderStatusBar();
    };

    renderer.setGrayscaleFactoryEncoding(true);
    renderPlaneStrips(true, factoryScratch.get(), renderFullFrame);
    const auto tGrayLsb = millis();
    renderPlaneStrips(false, factoryScratch.get(), renderFullFrame);
    const auto tGrayMsb = millis();

    renderer.setRenderMode(GfxRenderer::BW);
    renderer.setGrayscaleFactoryEncoding(false);
    // One activation, absolute waveform. It self-powers the panel down when it
    // finishes, exactly as the 0xCC refinement activation already does.
    renderer.displayGrayBufferFactory();
    const auto tGrayDisplay = millis();

    // RED RAM still holds the MSB plane; re-seed it from the B/W framebuffer so
    // the next differential page turn diffs against a valid baseline.
    renderer.cleanupGrayscaleWithFrameBuffer();
    const auto tEnd = millis();
    // Same bookkeeping image pages get today: the panel now carries 4-gray
    // content a differential update cannot fully erase.
    pagesUntilFullRefresh = 1;

    LOG_DBG("ERS",
            "Page render (factory gray): prewarm=%lums bw_render=%lums gray_lsb=%lums gray_msb=%lums "
            "gray_display=%lums cleanup=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tGrayLsb - tBwRender, tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb,
            tEnd - tGrayDisplay, tEnd - t0);
    return;
  }
#endif

  if (pageHasImages) {
#ifdef CROSSPOINT_IMG_NO_FORCED_CLEAN
    if (renderer.fastAfterGrayscaleSafe()) {
      // This panel's driver substitutes a gray-exit transition for the plain
      // differential update whenever a FAST paint follows a grayscale pass, so
      // the page after an image does not need a clean base to erase the gray
      // charge — it is re-driven per pixel by that transition. Forcing one
      // anyway made every turn in an image-dense book pay the OTP GC waveform
      // (1491 ms measured on UC8179, vs 559 ms for FAST and 296 ms for the
      // grayscale pass itself). Run the ordinary page cadence instead, so the
      // periodic ghost-clear still lands on SETTINGS.getRefreshFrequency().
#ifdef FREEINK_UC8179_DOUBLE_GRAY_PRE
      // That gray-exit transition drives a was-white->black pixel for 25
      // saturating frames but gives a was-black->black pixel only the 4-frame
      // corrective, so black lands at two depths — and the AA gray drive is
      // ~2 frames off black. On a text page the difference hides in the glyphs;
      // on an image page it reads as the OUTGOING page's text ghosting through
      // the incoming photo's grays. Ask the driver to equalize black depth as
      // part of this transition. One-shot and armed here only, so text pages
      // never pay its second activation (~666 ms).
      renderer.requestDeepGrayEqualize();
#endif
#ifdef CROSSPOINT_UC8179_OVERLAP
      // The equalize pass rides inside the transition's own finish half, so it
      // stays blocking and image pages keep today's ordering: base start ‖ plane
      // renders, then finish (including the second activation), then uploads.
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapBaseTransition,
                                           manualRefreshPending);
      baseStartedAsync = overlapBaseTransition;
#else
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/false, manualRefreshPending);
#endif
    } else
#endif
    {
      // Image pages use one base refresh before the grayscale pass. FAST leaves
      // the panel receptive to the gray waveform; pending cleanup still honors
      // the scheduled/manual HALF refresh.
      renderer.displayBuffer((manualRefreshPending || (cleanImageBasePending && SETTINGS.screenInverted == 0))
                                 ? HalDisplay::HALF_REFRESH
                                 : HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh = 1;
    }
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh, manualRefreshPending);
  } else {
#ifdef CROSSPOINT_UC8179_OVERLAP
    // Exactly one of the two can be set: overlapRefresh is the tiled panels'
    // band-buffer overlap, overlapBaseTransition the whole-buffer one.
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh || overlapBaseTransition,
                                         manualRefreshPending);
    baseStartedAsync = overlapBaseTransition;
#else
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh, manualRefreshPending);
#endif
  }
  const auto tDisplay = millis();

  if (tiledGrayscale) {
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return gateFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             gateMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        renderPlaneStrips(true, scratch.get(), renderGrayscalePass);
        const auto tGrayLsb = millis();

        renderPlaneStrips(false, scratch.get(), renderGrayscalePass);
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
#ifdef CROSSPOINT_UC8179_OVERLAP
        // The base transition is still running on the panel and owns controller
        // RAM until it is waited out; nothing below this early return will do
        // it, and the driver's post-waveform baseline restore lives there.
        if (baseStartedAsync) renderer.waitRefreshComplete();
#endif
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
#ifdef CROSSPOINT_UC8179_OVERLAP
      if (baseStartedAsync) {
        // Controller RAM belongs to the running base waveform, so this plane
        // cannot be uploaded yet — park it in PSRAM and hand the framebuffer
        // straight back to the MSB pass below.
        memcpy(overlapPlane.get(), renderer.getFrameBuffer(), renderer.getBufferSize());
      } else
#endif
      {
        renderer.copyGrayscaleLsbBuffers();
      }
      const auto tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
#ifdef CROSSPOINT_UC8179_OVERLAP
      const auto tMsbRendered = millis();
      unsigned long overlapWaitMs = 0;
      if (baseStartedAsync) {
        // Both planes are composed; ride out whatever is left of the base
        // waveform and upload them back to back. LSB has to go first: the
        // driver folds it into its retained B/W base to rebuild stock's
        // absolute plane0, which the MSB upload then XORs against.
        renderer.waitRefreshComplete();
        overlapWaitMs = millis() - tMsbRendered;
        renderer.copyGrayscaleLsbBuffers(overlapPlane.get());
      }
#endif
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
#ifdef CROSSPOINT_UC8179_OVERLAP
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums wait=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, overlapWaitMs, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
#else
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
#endif
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section ? section->currentPage + 1 : 1;
  const float pageCount = section ? section->estimatedTotalPages() : 1;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = tocItem.title;
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
constexpr StrId kTextRowNames[] = {StrId::STR_FONT, StrId::STR_FONT_SIZE, StrId::STR_LINE_SPACING,
                                   StrId::STR_PARA_ALIGNMENT, StrId::STR_FOCUS_READING};
constexpr StrId kSpacingIds[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kSpacingIds) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Touch-first chrome: button boards always get the classic list menu, even
  // if a settings file (e.g. an SD card moved from a touch board) says Toolbar.
  return mappedInput.hasTouch() && SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS};
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    case 1:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case 2:
      return I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    case 3:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    case 4:
      return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
void EpubReaderActivity::showTextRowPopup(const int row) {
  switch (row) {
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        SETTINGS.fontPointSize = sizes[idx];
        applyTextSettingLive();
      });
      break;
    }
    case 2:
      overlayPopup.show(StrId::STR_LINE_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case 3:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

void EpubReaderActivity::openOverlay(Overlay target) {
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
  if (previous == Overlay::None) toolbarUi->begin();
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents:
      panelIndex = std::max(0, epub->getTocIndexForSpineIndex(currentSpineIndex));
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    case Overlay::Text:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  //
  // Refresh mode: FAST for every overlay paint, first open included. The AA
  // pass only grays glyph edges, and residue a FAST differential leaves under
  // the sheet has not shown in practice; it also self-heals on the
  // Xteink-class panels, whose close path re-renders the page. If text or
  // images ever visibly ghost through the chrome, restore a HALF cleanup on
  // the first open (see #2190 for the mechanism).
  if (section) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Close the overlay back to the reading page. Boards without the Xteink
// grayscale-AA pass restore the page snapshot and push one FAST refresh -- no
// re-render, no flash; Xteink boards re-render to restore the AA planes.
void EpubReaderActivity::closeOverlayToPage() {
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
  if (!epub || !section || !toolbarUi) return;

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    const int pageCount = section->estimatedTotalPages();
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(section->currentPage + 1) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page + button hints.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.bottomReserve = UITheme::getInstance().getMetrics().buttonHintsHeight;
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  };

  // Jump to another spine item (chapter scrub). The overlay stays up and is
  // re-drawn over the new page by renderBook().
  const auto gotoSpine = [this](int target) {
    const int spineCount = epub->getSpineItemsCount();
    target = std::clamp(target, 0, spineCount - 1);
    if (target != currentSpineIndex) {
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      currentSpineIndex = target;
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    return tool == 0 ? Overlay::Contents : (tool == 1 ? Overlay::Text : Overlay::More);
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoSpine(currentSpineIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoSpine(currentSpineIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoSpine(static_cast<int>((static_cast<float>(routed.permille) / 1000.0f) *
                                       static_cast<float>(epub->getSpineItemsCount() - 1) +
                                   0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 2) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoSpine(currentSpineIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents ? epub->getTocItemsCount()
                    : overlay == Overlay::Text   ? kTextRowCount
                                                 : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
        // Full font picker (built-in + SD fonts, live preview) -- the same
        // screen Settings uses; a popup cannot scroll a long font list.
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        // Release the section BEFORE the picker opens, the same way the classic
        // menu's TEXT_SETTINGS branch does. applyReaderTextSettings() below
        // still resets it, but that runs only when the result handler runs, and
        // an EXTERNAL pop (the Home-key "Go Back" action) skips a handler whose
        // result is monostate — so a font or size change made here would leave
        // the old layout's baked word positions to be drawn with the new size's
        // glyphs. Resetting up front means every return path rebuilds against
        // the settings as they are then, and frees the section's tens of KB for
        // the picker's font previews.
        {
          RenderLock lock;
          if (section) {
            rememberCurrentContentOffset();
            cachedSpineIndex = currentSpineIndex;
            cachedChapterTotalPageCount = section->pageCount;
            nextPageNumber = section->currentPage;
          }
          section.reset();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
          // Same as the classic menu's TEXT_SETTINGS branch: the Push leaves
          // this activity's build task running while the picker loads and
          // unloads SD fonts under the shared renderer.
          discardPrebuiltSection();
#endif
        }
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 applyReaderTextSettings();
                                 overlay = Overlay::Text;  // back to the Text panel
                                 panelIndex = 0;
                                 if (toolbarUi) toolbarUi->begin();  // the picker drew its own FUI screen
                                 requestUpdate();                    // re-render page + Text panel
                               });
      } else if (panelIndex == 4) {
        // Focus Reading is a genuine on/off: a tap toggles and applies live.
        SETTINGS.focusReadingEnabled = SETTINGS.focusReadingEnabled ? 0 : 1;
        applyTextSettingLive();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      const auto item = epub->getTocItem(panelIndex);
      if (item.spineIndex != -1) {
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  overlayPopup.render(renderer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubReaderActivity::applyReaderTextSettings() {
  SETTINGS.saveToFile();
  RenderLock lock;
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  //
  // INSIDE the lock, not before it: this swaps fonts on the shared renderer, and
  // both the render task and the background build task measure text through it.
  // That was already a live hazard on the applyTextSettingLive() path, where the
  // reader stays on screen with a section resident and a render can be in
  // flight; the background prebuild only widened it. This function is reached
  // from result handlers and from panel callbacks, both of which run after
  // ActivityManager has released its own lock (see the unlock() before the
  // handler dispatch), and the blocking acquire below was already unconditional
  // here -- so no caller can be holding it, and there is no new deadlock edge.
  sdFontSystem.ensureLoaded(renderer);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();  // force re-pagination with the new settings
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // The spec the prebuild was armed for is the one just abandoned, and the
  // fonts it measures text through were swapped above. Both push sites already
  // discard before opening their picker, but this function is also reached
  // straight from the Text panel's callbacks (no push at all), and it is the one
  // return path they all share -- so drop it here too, under the lock held.
  discardPrebuiltSection();
#endif
}

// The More panel carries everything the classic list menu offers except the
// two entries that have their own tool (chapters -> Contents, text -> Text).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty());
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    case MA::NIGHT_MODE:
      return SETTINGS.screenInverted ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::FRONTLIGHT:
      return Frontlight.isOn() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  const auto action = moreItems[row].action;
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(idx));
      });
      paintOverlayPopup();
      return;
    }
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  discardOverlayPage();
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;
    return;
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
