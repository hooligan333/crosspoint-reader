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
//   DROP       the ring buffer overflowed while the card was unwritable,
//              aux = entries lost (see the flush notes below)
//
// Frontlight, night mode and WiFi are POLLED from tick() rather than hooked at
// every toggle site: one hook catches the reader menu, Toggle Light, the
// control-center tiles and the power-button blackout alike, and it does not
// have to be re-applied every time one of those paths moves.
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
// logged: PAGE is meant to count reading, not a timer running against a page.
//
// Threading: loop-task only. begin(), tick(), notePageTurn() and noteSleep()
// all run on the loop task -- the reader page-turn hooks sit in Activity
// loop() code, which ActivityManager runs from loop(), not on the background
// build task -- so nothing here takes a lock.
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
  // and the frontlight have been restored (the seed rows read both). Rotates an
  // oversized log, then records BOOT plus the initial FL/NIGHT/WIFI state.
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

  // Immediately before deep sleep and BEFORE Storage.prepareForDeepSleep():
  // records SLEEP and flushes synchronously, because there is no later tick().
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
  };

  // 12 bytes each; 32 of them is one flush's worth of a busy reading minute.
  struct Entry {
    uint32_t ms;
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

  void record(uint8_t event, uint8_t aux);
  void sampleBattery();
  void pollState();
  bool flush();
  // Formats one row from `e` (overriding its event/aux, which is how the
  // synthetic DROP row reuses an entry's timestamp and battery reading) and
  // appends it. `flushWall` is epoch seconds at `flushMs`, or 0 when the RTC
  // could not be read. Returns false on a short write.
  static bool writeRow(HalFile& file, const Entry& e, uint8_t event, uint8_t aux, uint32_t flushWall, uint32_t flushMs);

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
};

#endif  // CROSSPOINT_USAGE_LOG
