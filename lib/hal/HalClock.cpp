#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#ifdef CROSSPOINT_SOFT_CLOCK
#include <esp_attr.h>
#include <sys/time.h>

#include "HalStorage.h"
#include "SoftClock.h"
#endif

HalClock halClock;  // Singleton instance

#ifdef CROSSPOINT_SOFT_CLOCK

// --- Soft clock backend (CROSSPOINT_SOFT_CLOCK) ------------------------------
// System time in place of the I2C chip. Whole-function alternates rather than
// branches inside the shared bodies, so the flag-off code below is the code
// that was always there.
//
// On this board deep sleep drops the battery MOSFET, so "sleep" is power-off
// and system time is lost with it. Two stores carry the clock instead:
//
//   * The SD record (/.crosspoint/softclock.bin) crosses the power cut and
//     comes back as STALE.
//   * The RTC-RAM stamps below cross an esp_restart and die with the rail, so
//     they say both "has a server set the time on THIS rail" (VALID vs STALE)
//     and "how recently did we last bother NTP" (the backoff).

namespace {

/** Current system time as epoch seconds; ~0 (1970) until something sets it. */
int64_t systemEpochSecs() { return static_cast<int64_t>(time(nullptr)); }

constexpr char RECORD_DIR[] = "/.crosspoint";
constexpr char RECORD_PATH[] = "/.crosspoint/softclock.bin";

/**
 * The rail-lifetime state, in RTC RAM.
 *
 * RTC_NOINIT_ATTR rather than the RTC_DATA_ATTR the spec names, because
 * RTC_DATA_ATTR does NOT do what is wanted here: the .rtc.data segment is part
 * of the app image and the bootloader reloads it on every boot that is not a
 * deep-sleep wake, so an esp_restart would silently reset the stamps — exactly
 * the case they exist for. .rtc_noinit is never loaded, so it survives
 * esp_restart AND deep sleep, and holds garbage after a power cut. `magic` is
 * what separates the two: a mismatch means the rail died and the stamps (like
 * the clock) start over.
 *
 * Plain aggregate with no member initializers: a dynamic initializer would run
 * at startup and defeat the whole point.
 */
constexpr uint32_t RAIL_MAGIC = 0x53434C4BUL;  // arbitrary sentinel; unset RTC RAM will not match it

struct RailState {
  uint32_t magic;
  uint32_t baseMs;  // rail-uptime at this boot's millis() == 0
  uint32_t attemptMs;
  uint32_t successMs;
  uint8_t flags;
};

constexpr uint8_t RAIL_HAS_ATTEMPT = 1u << 0;
constexpr uint8_t RAIL_HAS_SUCCESS = 1u << 1;

RTC_NOINIT_ATTR RailState g_rail;

/**
 * Uptime on the rail's timeline rather than this boot's.
 *
 * millis() restarts at 0 on every reboot while the stamps do not, so the stamps
 * are carried on a base that begin() re-seeds from the newest of them. That
 * assumes the reboot happened immediately after the last stamp, which
 * UNDER-counts the elapsed time and therefore only ever backs off for longer
 * than asked — the safe direction.
 */
uint32_t railNowMs() { return static_cast<uint32_t>(g_rail.baseMs + static_cast<uint32_t>(millis())); }

softclock::SyncStamps railStamps() {
  softclock::SyncStamps stamps = softclock::NO_STAMPS;
  stamps.lastAttemptMs = g_rail.attemptMs;
  stamps.lastSuccessMs = g_rail.successMs;
  stamps.hasAttempt = (g_rail.flags & RAIL_HAS_ATTEMPT) != 0;
  stamps.hasSuccess = (g_rail.flags & RAIL_HAS_SUCCESS) != 0;
  return stamps;
}

/** Adopt an epoch as the system clock. Callers must have vetted it first. */
void setSystemEpoch(int64_t epochSecs) {
  timeval tv = {};
  tv.tv_sec = static_cast<time_t>(epochSecs);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

}  // namespace

void HalClock::begin() {
  // Nothing to bring up: system time is already running, the only question is
  // whether it means anything yet.
  if (g_rail.magic != RAIL_MAGIC) {
    // Power-on: RTC RAM is uninitialised, so the clock is unset too.
    g_rail.magic = RAIL_MAGIC;
    g_rail.baseMs = 0;
    g_rail.attemptMs = 0;
    g_rail.successMs = 0;
    g_rail.flags = 0;
  } else {
    // Same rail, new boot (esp_restart, or a USB-powered deep-sleep wake):
    // re-base the uptime timeline onto the newest surviving stamp.
    const uint32_t newest = g_rail.attemptMs > g_rail.successMs ? g_rail.attemptMs : g_rail.successMs;
    g_rail.baseMs = newest;
  }
  LOG_INF("CLK", "Soft clock: system time %s", isAvailable() ? "valid" : "unset");
}

softclock::Validity HalClock::validity() const {
  return softclock::classify(systemEpochSecs(), (g_rail.flags & RAIL_HAS_SUCCESS) != 0);
}

bool HalClock::isAvailable() const { return softclock::isUsable(validity()); }

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  Rtc::DateTime dt;
  if (!getDateTime(dt)) return false;
  hour = dt.hour;
  minute = dt.minute;
  return true;
}

bool HalClock::getDateTime(Rtc::DateTime& out) const { return softclock::fillDateTime(systemEpochSecs(), out); }

void HalClock::restoreFromStorage() {
  uint8_t buf[softclock::RECORD_SIZE];
  HalFile file = Storage.open(RECORD_PATH, O_RDONLY);
  if (!file) return;  // no record yet: the clock stays INVALID, as after a flash
  const int read = file.read(buf, sizeof(buf));
  file.close();

  int64_t epoch = 0;
  if (read != static_cast<int>(sizeof(buf)) || !softclock::decodeRecord(buf, sizeof(buf), epoch)) {
    LOG_ERR("CLK", "Soft clock record unusable, clock stays unset");
    return;
  }
  // Restored, not fixed: the rail success flag stays clear, so this reads back
  // as STALE. No elapsed-time compensation is possible — the device cannot know
  // how long it was off — so the restored reading is a lower bound on now.
  setSystemEpoch(epoch);
  _lastPersistMs = railNowMs();  // it is on the card already; don't rewrite it
  Rtc::DateTime dt;
  if (getDateTime(dt)) {
    LOG_INF("CLK", "Soft clock restored (stale) %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour,
            dt.minute, dt.second);
  }
}

void HalClock::persistNow() {
  if (validity() == softclock::Validity::Invalid) return;  // nothing worth writing down

  uint8_t buf[softclock::RECORD_SIZE];
  softclock::encodeRecord(systemEpochSecs(), buf);

  Storage.ensureDirectoryExists(RECORD_DIR);
  // Plain overwrite (O_TRUNC): the record is one sector, and a torn write fails
  // the checksum, which decodes as INVALID — the same recoverable state as no
  // record at all.
  HalFile file = Storage.open(RECORD_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("CLK", "Cannot write %s", RECORD_PATH);
    return;
  }
  const size_t written = file.write(buf, sizeof(buf));
  file.close();
  if (written != sizeof(buf)) {
    LOG_ERR("CLK", "Short write to %s", RECORD_PATH);
    return;
  }
  _lastPersistMs = railNowMs();
}

void HalClock::tick() {
  if (!softclock::shouldPersistPeriodically(validity(), railNowMs(), _lastPersistMs)) return;
  persistNow();
}

bool HalClock::applyServerDate(const char* rfc822) {
  int64_t epoch = 0;
  if (!softclock::parseRfc822Epoch(rfc822, epoch)) return false;

  setSystemEpoch(epoch);
  g_rail.successMs = railNowMs();
  g_rail.flags = static_cast<uint8_t>(g_rail.flags | RAIL_HAS_SUCCESS);
  Rtc::DateTime dt;
  if (getDateTime(dt)) {
    LOG_INF("CLK", "Clock set from server Date header: %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day,
            dt.hour, dt.minute, dt.second);
  }
  persistNow();
  return true;
}

bool HalClock::maybeOpportunisticSync() {
  if (!softclock::shouldAttemptSync(validity(), railNowMs(), railStamps())) return false;
  // Cheap no-op for every flow that calls this without a radio up. Checked
  // after the backoff policy so a fresh clock never even reads WiFi state.
  if (WiFi.status() != WL_CONNECTED) return false;
  LOG_INF("CLK", "Opportunistic clock sync (WiFi already up, no usable Date header)");
  return syncFromNTP();
}

bool HalClock::syncFromNTP() {
  // No _available guard, unlike the RTC build: an invalid soft clock is exactly
  // the case a sync exists to fix.
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  // Every exit below is an attempt, successful or not: the backoff floor exists
  // to stop a device with no NTP route from paying 5 s on every sync flow.
  g_rail.attemptMs = railNowMs();
  g_rail.flags = static_cast<uint8_t>(g_rail.flags | RAIL_HAS_ATTEMPT);

  // Snapshot before SNTP is allowed to write system time. A server that answers
  // with something implausible would otherwise leave the clock worse than it
  // was — a restored STALE reading turned into 1970 — so the snapshot goes back.
  const int64_t snapshot = systemEpochSecs();

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds). sntp_get_sync_status()
  // consumes the COMPLETED state on read, so a second call in the same boot
  // waits for a genuinely new sample rather than seeing the old one.
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      // SNTP has already written system time via settimeofday(); there is no
      // chip to set. Read it back rather than trust the callback: a server that
      // answered with something before the threshold leaves the clock unusable,
      // and reporting success for that would strand every caller.
      const int64_t answer = systemEpochSecs();
      if (!softclock::epochIsValid(answer)) {
        LOG_ERR("CLK", "NTP answered but system time is still not plausible");
        esp_sntp_stop();
        setSystemEpoch(softclock::settleAfterSync(snapshot, answer));
        return false;
      }
      Rtc::DateTime dt;
      if (!getDateTime(dt)) return false;  // unreachable: the reading above is plausible
      g_rail.successMs = railNowMs();
      g_rail.flags = static_cast<uint8_t>(g_rail.flags | RAIL_HAS_SUCCESS);
      LOG_INF("CLK", "System time set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour,
              dt.minute, dt.second);
      persistNow();
      return true;
    }
    delay(100);
  }

  // Stop SNTP before returning: a late answer arriving mid-study would step the
  // clock out from under an open session, and nothing is waiting for it now.
  esp_sntp_stop();
  // An answer may still have landed between the last poll and esp_sntp_stop(); keep
  // it if it is plausible, and otherwise undo whatever it wrote.
  const int64_t afterTimeout = systemEpochSecs();
  const int64_t settled = softclock::settleAfterSync(snapshot, afterTimeout);
  if (settled != afterTimeout) setSystemEpoch(settled);
  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}

#else  // CROSSPOINT_SOFT_CLOCK — I2C RTC backend.

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

#if defined(CROSSPOINT_USAGE_LOG) || defined(CROSSPOINT_CLOCK_DST) || defined(CROSSPOINT_FLASHCARDS)
bool HalClock::getDateTime(Rtc::DateTime& out) const {
  if (!_available) return false;
  return _sdkRtc.now(out);
}
#endif

#endif  // CROSSPOINT_SOFT_CLOCK

// --- Shared by both backends -------------------------------------------------
// formatTime() is pure presentation over getTime(), so it is written once. It
// stays in its original position between the two guarded regions: the flag-off
// preprocessed output must be the file as it was, definition order included.

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

#ifndef CROSSPOINT_SOFT_CLOCK

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}

#endif  // !CROSSPOINT_SOFT_CLOCK
