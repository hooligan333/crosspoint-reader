#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"
#include "ReaderRenderSpec.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  HalFile file;

  void writeSectionFileHeader(const ReaderRenderSpec& spec);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

  // Page-offset table entry, kept in RAM while an incremental build is running so
  // already-built pages can be located in the partially-written .bin.
  struct PageLutEntry {
    uint32_t fileOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
    uint32_t visibleTextOffset;
  };
  // Held only while an incremental build is in progress (see startBuild). Carries the
  // live parser plus the strings it references (the parser stores them by reference)
  // and the in-RAM page-offset table.
  struct BuildContext {
    std::unique_ptr<ChapterHtmlSlimParser> parser;
    std::vector<PageLutEntry> lut;
    std::string parsePath;
    std::string contentBase;
    std::string imageBasePath;
    std::string htmlPath;
    std::string tmpHtmlPath;
    bool reusedHtml = false;
    CssParser* cssParser = nullptr;
    // HTML byte progress, for estimating the section's total page count while it's still building.
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    // Exponentially-smoothed page-count estimate (0 = not yet seeded) and the bytesConsumed at its
    // last update. The raw byte-ratio estimate jitters as the build crosses dense/sparse regions;
    // the EMA is stepped once per build advance (not per redraw) to damp that wobble.
    float smoothedEstimate = 0;
    uint32_t smoothedAtConsumed = 0;
  };
  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;
  // Pages laid out by the active build (== build_->lut.size()). Distinct from pageCount,
  // which is the pages *available to read* and also counts a loaded partial file's pages.
  uint16_t builtPageCount_ = 0;
  // A partial section file (suspended build from a previous session) is loaded at filePath.
  // Its pages 0..partialPageCount_-1 are readable while a rebuild extends past them.
  bool partial_ = false;
  uint16_t partialPageCount_ = 0;
  // Parse watermark from the partial's trailer, for estimating the total page count.
  uint32_t partialBytesConsumed_ = 0;
  uint32_t partialTotalBytes_ = 0;
  bool finalizeBuild();
  // Write the LUTs/anchor map (and, for a partial, the watermark trailer), patch the
  // header, stamp the version byte, and swap the tmp .bin over filePath.
  bool commitBuildFile(uint8_t version, uint32_t bytesConsumed, uint32_t totalBytes);
  // Builds write here and are swapped over filePath only on commit, so a prior
  // partial/finalized file stays readable while a rebuild is in progress.
  std::string binTmpPath() const { return filePath + ".part"; }
  std::unique_ptr<Page> loadPageAt(int page) const;
  // Read a page already laid out by the in-progress build (page < build LUT size), from
  // the partially-written tmp .bin without disturbing the build's write cursor.
  std::unique_ptr<Page> loadPageDuringBuild(int page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Constructor and destructor are out-of-line: BuildContext holds a unique_ptr to the
  // forward-declared ChapterHtmlSlimParser, whose full definition is only visible in the .cpp.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(const ReaderRenderSpec& spec);
  bool clearCache() const;
  bool createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr);

  // Incremental build: lay out the section a few pages at a time so a large chapter
  // can show its first page immediately and keep the UI responsive while the rest
  // builds. createSectionFile() above is the one-shot wrapper over these.
  //   if (!startBuild(...)) fail;
  //   each tick: buildSomeMore(N); render up to pageCount; when isBuildComplete() stop.
  bool startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr);
  // Lay out up to maxPages more pages (maxPages <= 0 = build to completion). Returns
  // false on error (the build is abandoned). Sets isBuildComplete() when finished.
  bool buildSomeMore(int maxPages);
  bool isBuilding() const { return static_cast<bool>(build_); }
  bool isBuildComplete() const { return buildComplete_; }
  // Best-known total page count: the exact pageCount once finalized, or a smoothed byte-based
  // estimate (pages so far scaled by totalBytes/bytesConsumed, damped by an EMA) while a giant spine
  // is still building, so "page X of Y" / progress don't read off the small build watermark.
  uint16_t estimatedTotalPages() const;
  void abandonBuild();
  // Persist an in-progress build as a partial section file (version sentinel + LUTs +
  // watermark trailer) instead of discarding it, so the next open of this spine can show
  // its pages instantly and only rebuild in the background. Called by the destructor, so
  // any teardown path (exit, sleep, navigation) keeps the work already done. Keeps a
  // pre-existing partial when it covers more pages than this build reached.
  void suspendBuild();
  // True when a partial file was loaded: pageCount is a watermark, not the chapter total.
  bool isPartial() const { return partial_; }

  // Unified page read: from the active build if it has reached the page, otherwise from
  // the on-disk file (finalized section, or a partial the rebuild hasn't caught up to).
  std::unique_ptr<Page> loadPage(int page);

  std::string getTextFromSectionFile();

  // Resolve an anchor from the in-progress build first, then the on-disk anchor map
  // (covers finalized sections and partials from a previous session).
  std::optional<uint16_t> findAnchor(const std::string& anchor) const;

  // True if this spine's unzipped HTML is already cached, so a build won't pay the (multi-second on a
  // giant spine) zip inflation. Lets the reader skip the indexing popup on a fast reopen/rebuild.
  bool hasHtmlCache() const;

  // --- Chapter HTML cache -----------------------------------------------------
  // The unzipped chapter HTML lives in the per-book cache dir keyed only on the spine index -- never on
  // render settings -- which is why it survives the invalidation that wipes the layout (.bin) caches.
  // These are static so a caller that holds no Section for the spine (the reader's background
  // pre-inflate of the NEXT chapter) derives byte-for-byte the same paths this class does.
  static std::string htmlCacheDir(const Epub& epub);
  static std::string htmlCachePath(const Epub& epub, int spineIndex);
  // Temp file the synchronous build path inflates into before promoting it.
  static std::string htmlBuildTmpPath(const Epub& epub, int spineIndex);
  // Temp file for a background pre-inflate. Deliberately DISTINCT from the build path's temp: the two
  // writers must never share a destination file, so that even a cancel that fails to stop the
  // background inflate in time (see the reader's interlock) can only cost duplicated work -- never a
  // half-written file under the other writer's handle.
  static std::string htmlBackgroundTmpPath(const Epub& epub, int spineIndex);

  enum class HtmlInflate : uint8_t {
    Promoted,  // complete bytes are at htmlCachePath(); the temp is gone (atomic rename)
    TempOnly,  // complete bytes, but the rename failed: they are still at tmpHtmlPath
    Failed,    // no usable output; the temp file has been removed
    Aborted,   // abortFn asked to stop; the temp file has been removed
  };
  // Polled between output chunks and once more at the commit point (immediately before the rename), so
  // a caller running this off the render lock can stop it within one chunk. Return true to abandon.
  using HtmlInflateAbortFn = bool (*)(void* ctx);
  // Stream one spine's HTML out of the EPUB zip into the html cache, via a temp file promoted by an
  // atomic rename. Extracted from startBuild() so the reader's background pre-inflate runs the
  // identical code path instead of a copy of it; with abortFn == nullptr (what the synchronous build
  // passes) this is the original loop, retries and logs included.
  //
  // Thread-safety: everything it touches is either a parameter or immutable Epub state --
  // Epub::readItemContentsToStream is const and builds its own ZipFile (its own HalFile handle) per
  // call, and every file operation is serialized by HalStorage's mutex. It shares no handle with any
  // other task. What it does NOT do is coordinate with a second writer of the same paths; that is the
  // caller's interlock to provide.
  static HtmlInflate inflateHtmlToCache(const Epub& epub, const std::string& localPath, int spineIndex,
                                        const std::string& tmpHtmlPath, HtmlInflateAbortFn abortFn = nullptr,
                                        void* abortCtx = nullptr);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up an anchor among the pages built so far by the in-progress build, so an anchor jump
  // (TOC / chapter select, usually the chapter top = page 0) can resolve without laying out the
  // whole chapter. Returns nullopt if the anchor hasn't been reached yet (build more) or no build.
  std::optional<uint16_t> findAnchorDuringBuild(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Exact zero-based visible Unicode-codepoint offset where a rendered page
  // starts. Available from both an active build and finalized/partial caches.
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;

  // Derive the local page containing an exact visible-text offset. When
  // preferFirstAtOffset is true, ties caused by zero-width content such as an
  // image-only page select the first page at that offset.
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset, bool preferFirstAtOffset = false) const;

  // True once the active build has laid out a page starting at or past `offset`, i.e.
  // getPageForVisibleTextOffset() can resolve it from the build without laying out more.
  // Lets a caller build to a content target instead of a page number, which is what a
  // re-pagination needs: the old page index no longer names the same content.
  // False with no build running -- there is nothing left to wait for, so the on-disk
  // cache answers directly.
  bool buildReachedVisibleTextOffset(uint32_t offset) const {
    return build_ && !build_->lut.empty() && offset <= build_->lut.back().visibleTextOffset;
  }
};
