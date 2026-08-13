#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group).
  void clearCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Hot group: last decompressed group (byte-aligned) for non-prewarmed fallback path.
  // Kept in byte-aligned format; individual glyphs are compacted on demand into hotGlyphBuf.
  // Nothrow high-water malloc buffers, NOT std::vector: getBitmap() runs on the render path,
  // and under -fno-exceptions a vector resize that hits OOM abort()s the firmware instead of
  // failing (field crash: hotGroup.resize() -> std::bad_alloc -> abort with ~11 KB free).
  // ensureCapacity() returns false on OOM so the caller can skip the glyph gracefully.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  uint8_t* hotGroup = nullptr;  // owned; freed in freeHotGroup()/dtor
  uint32_t hotGroupCapacity = 0;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call. Same ownership/OOM contract as hotGroup.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphBufCapacity = 0;

#ifdef CROSSPOINT_RESIDENT_FONT_GROUPS
  // Session-persistent cache of decompressed glyph groups (byte-aligned form),
  // PSRAM-ONLY. On PSRAM boards (X4 Pro) this makes group decompression a
  // once-per-session cost instead of once-per-page: prewarmCache() and the
  // getBitmap() fallback read the resident copy instead of re-inflating, while
  // the per-page page slots / hot-group machinery (and clearCache()) behave
  // exactly as before — only the DEFLATE step disappears after first touch.
  // Builtin EpdFontData lives in flash for the app's lifetime, so entries are
  // keyed by (fontData, groupIndex) and never invalidated; the budget bounds
  // total residency (all builtin faces ≈ 3 MB decompressed). On budget
  // exhaustion, PSRAM allocation failure, or absent PSRAM, getResidentGroup()
  // returns nullptr and callers take the existing temp-buffer/hot-group path —
  // deliberately NO internal-heap fallback, so the cache can never capture
  // session-lifetime internal RAM the render path needs.
  //
  // Interaction with the heap-critical release path
  // (FontCacheManager::releaseSdFontCaches(), used by #3035's WiFi + web-server
  // start and #3093's transparent sleep-overlay decode): resident groups are
  // EXEMPT. That path calls FontDecompressor::clearCache(), which still frees the
  // page slots and hot group — every internal-heap byte this class held before the
  // flag existed — so the protection it provides is unchanged with the flag on.
  // What it is protecting is the INTERNAL heap (its own instrumentation reads
  // ESP.getFreeHeap(), and the sleep decode's scanline/region buffers are what
  // must fit), and resident payloads are PSRAM-only, so releasing them would hand
  // back nothing where the pressure actually is. It would, however, throw away the
  // entire once-per-session inflate saving on every sleep entry — the exact
  // per-page DEFLATE cost this flag removes. The only internal-heap residency the
  // flag adds is the ResidentGroup index array below (12 B/entry, sub-KB at real
  // entry counts), which is the cache's identity and cannot be dropped without
  // dropping the cache. Payload residency is bounded instead by
  // RESIDENT_GROUP_BUDGET_BYTES, leaving >= 4 MB of an 8 MB part free for the
  // decode paths that may spill to PSRAM under SPIRAM_USE_MALLOC.
  struct ResidentGroup {  // 12 bytes on the 32-bit target
    const EpdFontData* fontData;
    uint16_t groupIndex;
    uint8_t* data;  // owned; PSRAM (heap_caps), freed in freeResidentGroups()
  };
  static constexpr uint32_t RESIDENT_GROUP_BUDGET_BYTES = 4u * 1024u * 1024u;
  static constexpr uint32_t RESIDENT_GROUP_MAX_ENTRIES = 4096;  // far above any real font set
  ResidentGroup* residentGroups = nullptr;                      // grow-by-realloc array (internal heap; a few KB)
  uint32_t residentGroupCount = 0;
  uint32_t residentGroupCapacity = 0;
  uint32_t residentGroupBytes = 0;
  bool residentBudgetWarned = false;
  // Returns the decompressed byte-aligned group, caching it on first request;
  // nullptr when over budget / allocation failed / decompression failed.
  const uint8_t* getResidentGroup(const EpdFontData* fontData, uint16_t groupIndex);
  void freeResidentGroups();
#endif

  // Grow (never shrink) an owned buffer to at least `needed` bytes; false on OOM, buffer freed.
  static bool ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed);

  void freePageBuffer();
  void freeHotGroup();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
