#pragma once

// Usage log -- a CSV of device events on the SD card, so the owner can work out
// usage and battery statistics off-device (pages per charge, awake time per
// session, what the frontlight really costs).
//
// Fork-only and compiled out entirely unless CROSSPOINT_USAGE_LOG is defined:
// the whole header and translation unit are inside the guard and every call
// site is #ifdef'd, so a flags-off build links nothing from here and behaves
// exactly as it did before.
//
// Row format (8 columns):
//   datetime,millis,event,pct,mv,chg,aux,detail
// `aux` is a byte and `detail` a 32-bit unsigned; both are printed unsigned and
// both mean whatever the event below says they mean, 0 where an event has no
// use for them.
//
// Schema migration is automatic: begin() reads the first line of an existing
// usage.csv and, if it is not byte-for-byte the current header, rotates the
// file to usage.old.csv exactly as an oversized log is rotated. The old rows
// are kept, under their own old header, and the new file starts with the
// current one. A card that has been through an older build therefore ends up
// with two files rather than one mixed-schema file, and nothing has to be
// deleted by hand.
//
// Rows written (each carries the battery percent, millivolts and charging flag
// at the time of the event):
//   BOOT       once per boot from begin(), aux = esp_reset_reason()
//   SLEEP      immediately before deep sleep, followed by a forced flush
//   PAGE       a user page turn in a reader. aux encodes what the turn was:
//                1 = single page forward   2 = single page backward
//                3 = skip forward          4 = skip backward
//              A skip is the long-press jump: +/-10 pages in the plain reader,
//              a chapter in the EPUB reader. Counting reading speed off PAGE
//              rows therefore means counting aux 1/2 and not 3/4.
//   FL_ON/OFF  frontlight, aux = brightness percent on the ON row
//   NIGHT_*    night mode (SETTINGS.screenInverted)
//   WIFI_*     WiFi radio up/down
//   CHG_ON/OFF charger connect/disconnect, from the same poll that fills the
//              `chg` column. Charging distorts every battery statistic, so this
//              is what lets an off-device reader cut the charge sessions out.
//              DEBOUNCED: an MCP73832-class STAT pin blinks, and at a 500 ms
//              poll that would be two rows a second flooding the ring, so an
//              edge is recorded only after three consecutive identical samples.
//              A charge session shorter than ~1.5 s therefore has no rows. The
//              `chg` COLUMN on every row keeps the instantaneous sample.
//   FR         refresh requests for a non-FAST mode (see the counter note
//              below -- this is a request count, not a flash count). detail =
//              how many happened since the previous FR row; no row is written
//              for an interval with none.
//   BOOK_OPEN  a book load began. aux = 1 cold (no book.bin: the section index
//              has to be built), 3 stale (book.bin is there but its format
//              version is not the one this firmware accepts, so it is rebuilt
//              exactly like a cold open -- the state every book is in for one
//              boot after a cache-version bump), 2 warm (the cache was there
//              and will be loaded). EPUB only: the TXT reader defers its index
//              build to the render task and the XTC reader has no build at all,
//              so neither has a loop-task point that knows cold from warm.
//   BOOK_RDY   the first page of that book is on the panel. The load time is
//              the millis delta BOOK_OPEN -> BOOK_RDY.
//   CH_START   a section (chapter) load began, because a page turn crossed a
//              section boundary. aux = 1 forward, 2 backward. detail says where
//              the section came from and, when it did not come from the
//              background prebuild, WHY not (UsageLog::SectionSource):
//                1 = adopted from the prebuild, fully laid out
//                2 = built or loaded on demand: nothing was parked at the
//                    boundary (never armed, declined, or discarded). Every
//                    backward crossing is a 2 -- there is no backward prebuild.
//                3 = adopted from the prebuild while its background layout was
//                    still running (the render finishes the pages it needs)
//                4 = one was parked, but for a render spec the reader no longer
//                    uses -- a font/margin/orientation change since it started
//                5 = one was parked for a different spine (the reader jumped)
//                6 = adopted as a partial whose background layout is NOT
//                    running: its pages are real up to the partial watermark,
//                    and the reader's own foreground extension resumes the
//                    rest once it pages near that watermark
//              1, 3 and 6 are the hits (6 the weakest -- it saves the load but
//              not the whole chapter); 2/4/5 all load on demand and are the
//              three distinguishable ways the feature misses. 4 and 5 are now
//              expected to read ZERO: every path that could leave a prebuild
//              parked for a spine or a spec the reader had moved off discards it
//              instead (see EpubReaderActivity's discard-site list). A nonzero
//              count on either is therefore an ALARM -- a discard site was
//              missed -- and not a normal miss mode to be netted off the hit
//              rate. Codes 1 and 2 are
//              unchanged from the first version of this row, so old logs read
//              correctly -- but only READ correctly, they do not COMPARE: the
//              old code 2 was an undifferentiated "not prebuilt" bucket that
//              also swallowed today's 4 and 5. Any cross-flash trend on this
//              row must therefore compare the OLD 2 against the NEW (2+4+5),
//              never old-2 against new-2, and old-1 against new (1+3+6).
//              The EPUB reader decides all of this at the boundary
//              itself, so detail is always known here and CH_RDY leaves it 0.
//              Only page-turn boundary crossings emit the pair: a chapter SKIP
//              (long press) and the TOC / percent / link jumps also load a
//              section, but they never adopt a prebuild and they do not go
//              through the boundary hook, so they are silent rather than
//              half-reported. PAGE aux 3/4 still marks the skips.
//   CH_RDY     the first page of that section is on the panel. aux repeats the
//              direction; detail is 0.
//   LS         light-sleep residency snapshot, written at sleep entry just
//              ahead of SLEEP. aux = residency percent (slept/uptime, 0-100),
//              detail = total light-slept milliseconds this session.
//              pmstats builds only (CROSSPOINT_PM_STATS + the light-sleep flag);
//              every other build simply has no LS rows.
//   LSN        the wake count for that same snapshot, written immediately after
//              LS. detail = light-sleep entries this session, aux = 0. It is a
//              second row rather than another field on LS because the count
//              does not fit in the one-byte aux; detail on LS is already the
//              slept milliseconds. Same build gating as LS.
//   DROP       the ring buffer overflowed while the card was unwritable,
//              aux = entries lost (see the flush notes below)
//
// _START / _RDY pairing: a _RDY row always follows some _START row, but a
// _START can be orphaned. A pending _RDY is discarded when a second turn
// bounces past the boundary before the first render lands, when a TOC / percent
// / link jump takes over the load, and when nothing has reached the panel
// within the 90 s deadline (see noteBookReady() below). Off-device pairing
// should therefore walk BACKWARDS from each _RDY to the nearest preceding
// _START of the same kind, rather than forwards from each _START.
//
// Frontlight, night mode, WiFi and the charger are POLLED from tick() rather
// than hooked at every toggle site: one hook catches the reader menu, Toggle
// Light, the control-center tiles and the power-button blackout alike, and it
// does not have to be re-applied every time one of those paths moves.
//
// Full refreshes are counted, not hooked, for a different reason: they are
// issued from the RENDER task (and from the loop task on a couple of legacy
// direct-displayBuffer paths), while this ring is loop-task only. HalDisplay
// bumps a lock-free atomic at the one point every refresh mode is converted,
// and tick() DRAINS it into a single FR row per poll. The row's timestamp is
// therefore the drain, not the refresh -- up to POLL_INTERVAL_MS late, which is
// immaterial for a refresh count. Cost at the refresh site is one relaxed
// atomic increment.
//
// FR is a LOWER BOUND, not an exact flash count. What is counted is a refresh
// REQUEST for a non-FAST mode at the HAL boundary. Below that boundary the
// display layers promote some FAST requests to a flashing waveform -- night
// mode's inversion forces FAST to HALF, and the UC8179 driver substitutes a
// full clear after a resync or an abandoned gray sequence -- and none of those
// promotions reach the counter. The night-mode ones are recoverable off-device:
// the NIGHT_ON/NIGHT_OFF rows bracket exactly the refreshes that get promoted
// for inversion. See HalDisplay::noteFullRefresh() for the full contract.
//
// Seed behaviour: begin() emits BOOT and then an _ON row for whichever of the
// three is already on at boot, so a session in the log is self-contained and
// does not have to be read against the session before it. Nothing is emitted
// for the ones that start off -- their absence is the initial state.
//
// Interval convention: FL/NIGHT/WIFI are on/off pairs, but a session does not
// end with _OFF rows. A SLEEP row implicitly closes every interval still open
// at that point, and the next boot re-seeds whatever is still on as an _ON row
// (see the seed behaviour above), so an off-device reader should treat SLEEP as
// the closing bracket for any open FL/NIGHT/WIFI interval rather than looking
// for a matching _OFF. Note also that only sleep flushes forcibly: any
// ESP.restart() path (USB Drive mode, firmware update, a crash) loses whatever
// is still sitting in the ring, so an interval can end with no closing row at
// all and the next BOOT is then the only bound on it.
//
// Automatic page turns (the reader's timed advance) are deliberately NOT
// logged as PAGE rows: PAGE is meant to count reading, not a timer running
// against a page. They are NOT silent overall, though -- an automatic turn that
// crosses a section boundary goes through the same boundary hook a manual one
// does and DOES emit CH_START/CH_RDY. That is deliberate: those two are load
// metrics (how long a chapter takes to build or load), and the load costs the
// same whoever asked for the turn. So a session with no PAGE rows can still
// have chapter pairs in it.
//
// Threading: loop-task only. begin(), tick(), the note* hooks and noteSleep()
// all run on the loop task -- the reader page-turn, book-load and section-load
// hooks sit in Activity loop() code, which ActivityManager runs from loop(),
// not on the background build task -- so nothing here takes a lock. The one
// piece of state written off the loop task is HalDisplay's refresh counter,
// which is an atomic this only ever drains (see above).
//
// Cost discipline: events land in a fixed 32-entry static ring buffer holding
// millis() plus the cached battery sample. No heap on the event path (the
// flush's file open does allocate), and on the page-turn path no SD and no I2C
// at all. The wall clock is read ONCE per flush and each row's datetime is
// reconstructed from its millis() delta; the battery comes from a cache tick()
// refreshes every ~500 ms, so an I2C-gauge board never puts a gauge read on a
// page turn either.
//
// The datetime column is UTC: the RTC stores UTC and this writes it out
// unconverted, no timezone maths (the on-device clock display is what applies
// the user's offset, at format time). The column is left empty when the RTC is
// absent or unset -- the millis column still orders the rows.

#ifdef CROSSPOINT_USAGE_LOG

#include <cstdint>

class HalFile;

class UsageLog;
extern UsageLog usageLog;  // Singleton

class UsageLog {
 public:
  // Call once from setup(), after Storage.begin() succeeded and after SETTINGS
  // and the frontlight have been restored (the seed rows read both). Rotates a
  // log that is oversized OR carries an older schema (see the migration note at
  // the top), then records BOOT plus the initial FL/NIGHT/WIFI/CHG state.
  void begin();

  // Call on every loop() pass; rate-limits itself to POLL_INTERVAL_MS. Refreshes
  // the battery cache, emits FL/NIGHT/WIFI change rows, and flushes once the
  // ring is three-quarters full or the oldest entry has waited FLUSH_AGE_MS.
  void tick();

  // A user page turn. `isSkip` marks the long-press jump (+/-10 pages, or a
  // chapter in the EPUB reader) rather than a single page, which is the
  // difference between aux 3/4 and aux 1/2 on the row.
  // Ring-buffer append only -- no SD, no I2C, no clock read.
  void notePageTurn(bool isForward, bool isSkip = false);

  // What state the on-card book cache was in when a book was opened; the
  // BOOK_OPEN row's aux. STALE is a cache file that exists but carries a format
  // version this firmware rejects, so it is rebuilt from scratch exactly as
  // COLD is -- distinguishing the two is what stops every book reporting a warm
  // open for the one boot after a cache-version bump.
  enum BookCache : uint8_t { CACHE_COLD = 1, CACHE_WARM = 2, CACHE_STALE = 3 };

  // A book load is starting (the reader's loadBook()). Pair it with
  // noteBookReady().
  void noteBookOpen(BookCache state);

  // The first page of the book just opened is on the panel. Call from loop-task
  // code once per open; the BOOK_OPEN -> BOOK_RDY millis delta is the load time.
  //
  // The caller owns the "is it on the panel yet" test, and it is stamped on
  // every path that actually paints a frame -- including the error screens
  // (empty chapter, page out of bounds, index build failed), which end the load
  // just as much as a rendered page does. Two backstops keep a row from being
  // fabricated out of a much later frame: a pending _RDY is dropped if nothing
  // paints within 90 s (see EpubReaderActivity's ULOG_READY_DEADLINE_MS), and
  // dropped again whenever a newer load supersedes it. The consequence of the
  // deadline is that a cold index build genuinely slower than 90 s loses its
  // BOOK_RDY row rather than reporting one; 90 s is well above any measured
  // cold build on this hardware, and losing the row is the intended trade
  // against publishing an invented load time.
  void noteBookReady();

  // Where the section a boundary crossing installs came from; the CH_START
  // row's detail. The three ON_DEMAND-equivalent codes are kept apart because
  // "no prebuild was parked" and "one was parked but did not match" are
  // different bugs -- and the two MISMATCH codes are kept even though they
  // should now never be emitted: they are the assertion that the discard sites
  // are complete, so reading one in a log means a site was missed. See the
  // CH_START note at the top for the full table.
  enum SectionSource : uint8_t {
    SECTION_PREBUILT = 1,
    SECTION_ON_DEMAND = 2,
    SECTION_PREBUILT_BUILDING = 3,
    SECTION_SPEC_MISMATCH = 4,
    SECTION_SPINE_MISMATCH = 5,
    SECTION_PREBUILT_PARTIAL = 6,
  };

  // A section (chapter) load is starting because a page turn crossed a section
  // boundary. The EPUB reader knows `source` at the boundary itself, so both
  // halves of the row are filled in here and noteSectionReady() only repeats
  // the direction.
  void noteSectionStart(bool isForward, SectionSource source);

  // The first page of that new section is on the panel.
  void noteSectionReady(bool isForward);

  // Immediately before deep sleep and BEFORE Storage.prepareForDeepSleep():
  // records LS and LSN (pmstats builds), then SLEEP, and flushes synchronously,
  // because there is no later tick().
  void noteSleep();

 private:
  enum Event : uint8_t {
    EV_BOOT,
    EV_SLEEP,
    EV_PAGE,
    EV_FL_ON,
    EV_FL_OFF,
    EV_NIGHT_ON,
    EV_NIGHT_OFF,
    EV_WIFI_ON,
    EV_WIFI_OFF,
    EV_DROP,
    EV_CHG_ON,
    EV_CHG_OFF,
    EV_FR,
    EV_BOOK_OPEN,
    EV_BOOK_RDY,
    EV_CH_START,
    EV_CH_RDY,
    EV_LS,
    EV_LSN,
  };

  // 16 bytes each; 32 of them is one flush's worth of a busy reading minute.
  struct Entry {
    uint32_t ms;
    uint32_t detail;
    uint16_t mv;
    uint8_t pct;
    uint8_t event;
    uint8_t aux;
    uint8_t chg;
  };

  static constexpr uint8_t RING_SIZE = 32;
  static constexpr uint8_t FLUSH_WATERMARK = 24;  // 3/4 full
  static constexpr uint32_t FLUSH_AGE_MS = 5UL * 60UL * 1000UL;
  static constexpr uint32_t POLL_INTERVAL_MS = 500;
  static constexpr uint32_t MAX_LOG_BYTES = 4UL * 1024UL * 1024UL;
  // A missing or unwritable card would otherwise cost an open() and a LOG_ERR
  // on every poll once the ring is at the watermark; back off instead. The
  // forced flush from noteSleep() ignores this gate.
  static constexpr uint32_t FLUSH_RETRY_MS = 30UL * 1000UL;
  // Consecutive identical samples a charger transition needs before it earns a
  // CHG row. See the CHG_ON/OFF note at the top.
  static constexpr uint8_t CHG_DEBOUNCE_SAMPLES = 3;

  void record(uint8_t event, uint8_t aux, uint32_t detail = 0);
  void sampleBattery();
  void pollState();
  // Drain HalDisplay's flashing-refresh counter into one FR row. Called from
  // tick() and once more from noteSleep(), so the sleep screen's own refresh is
  // in the log before the card goes away.
  void pollRefreshes();
  bool flush();
  // Formats one row from `e` (overriding its event/aux/detail, which is how the
  // synthetic DROP row reuses an entry's timestamp and battery reading) and
  // appends it. `flushWall` is epoch seconds at `flushMs`, or 0 when the RTC
  // could not be read. Returns false on a short write.
  static bool writeRow(HalFile& file, const Entry& e, uint8_t event, uint8_t aux, uint32_t detail, uint32_t flushWall,
                       uint32_t flushMs);

  Entry entries[RING_SIZE] = {};
  uint8_t head = 0;
  uint8_t count = 0;
  uint8_t dropped = 0;

  uint32_t lastPollMs = 0;
  uint32_t pendingSinceMs = 0;
  uint32_t lastFlushFailMs = 0;

  // Battery cache, refreshed by tick() and before BOOT/SLEEP. Only a successful
  // read replaces a field, so a transient gauge I2C failure repeats the last
  // good sample instead of writing a fabricated 0%/0 mV row. The zero init is
  // what the first rows carry if no read has succeeded yet.
  uint16_t lastMv = 0;
  uint8_t lastPct = 0;
  bool lastChg = false;

  bool started = false;
  bool flushFailed = false;
  bool needHeader = false;
  bool lastFrontlightOn = false;
  bool lastNightMode = false;
  bool lastWifiOn = false;
  // The charger state the last CHG_ON/CHG_OFF row reported. Separate from
  // lastChg, which every row carries in its `chg` column and is refreshed on
  // every sample.
  bool lastChgLogged = false;
  // Consecutive polls whose charger sample has disagreed with lastChgLogged.
  // The edge is recorded at CHG_DEBOUNCE_SAMPLES; a single disagreeing sample
  // resets it, which is what filters a blinking STAT pin.
  uint8_t chgStableCount = 0;
};

#endif  // CROSSPOINT_USAGE_LOG
