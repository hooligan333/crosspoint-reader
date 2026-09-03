#include "UsageLog.h"

#ifdef CROSSPOINT_USAGE_LOG

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cstdio>

#include "CivilDate.h"
#include "CrossPointSettings.h"

UsageLog usageLog;  // Singleton instance

namespace {

constexpr char LOG_DIR[] = "/.crosspoint";
constexpr char LOG_PATH[] = "/.crosspoint/usage.csv";
constexpr char OLD_PATH[] = "/.crosspoint/usage.old.csv";
constexpr char CSV_HEADER[] = "datetime,millis,event,pct,mv,chg,aux\n";

// Indexed by UsageLog::Event; kept short so a row stays well inside the
// stack buffer in writeRow().
const char* const EVENT_NAMES[] = {"BOOT",     "SLEEP",     "PAGE",    "FL_ON",    "FL_OFF",
                                   "NIGHT_ON", "NIGHT_OFF", "WIFI_ON", "WIFI_OFF", "DROP"};

// Battery access mirrors HalPowerManager/HalGPIO: one function-local static
// monitor for the process, constructed on first use so BoardConfig::ACTIVE is
// already resolved.
const BatteryMonitor& monitor() {
  static const BatteryMonitor battery;
  return battery;
}

constexpr uint32_t SECONDS_PER_DAY = 86400;

// Epoch seconds for the RTC's current time, or 0 when it is missing or unset.
uint32_t readWallClock() {
  Rtc::DateTime dt;
  if (!halClock.getDateTime(dt) || dt.year < 2020) return 0;
  return civil::daysFromCivil(dt.year, dt.month, dt.day) * SECONDS_PER_DAY + dt.hour * 3600u + dt.minute * 60u +
         dt.second;
}

}  // namespace

void UsageLog::begin() {
  // Rotate before anything is appended, so a session never straddles the cut.
  uint32_t size = 0;
  bool exists = false;
  {
    HalFile file = Storage.open(LOG_PATH, O_RDONLY);
    if (file) {
      size = static_cast<uint32_t>(file.fileSize());
      // A zero-length file (an interrupted create, or a header write that never
      // landed) still needs the header, so treat only a non-empty file as
      // existing -- the append below reuses it either way.
      exists = size > 0;
      file.close();
    }
  }
  if (exists && size > MAX_LOG_BYTES) {
    Storage.remove(OLD_PATH);
    if (Storage.rename(LOG_PATH, OLD_PATH)) {
      exists = false;
      LOG_INF("ULOG", "Rotated usage log (%lu KB)", static_cast<unsigned long>(size / 1024));
    }
  }
  needHeader = !exists;
  if (needHeader) Storage.mkdir(LOG_DIR);

  started = true;
  sampleBattery();
  record(EV_BOOT, static_cast<uint8_t>(esp_reset_reason()));

  // Seed the polled state, emitting an _ON row for whatever is already on so
  // the session stands on its own. Nothing is written for the off ones.
  lastFrontlightOn = Frontlight.present() && Frontlight.isOn();
  lastNightMode = SETTINGS.screenInverted != 0;
  lastWifiOn = WiFi.getMode() != WIFI_MODE_NULL;
  if (lastFrontlightOn) record(EV_FL_ON, Frontlight.brightness());
  if (lastNightMode) record(EV_NIGHT_ON, 0);
  if (lastWifiOn) record(EV_WIFI_ON, 0);
}

void UsageLog::tick() {
  if (!started) return;
  const uint32_t now = millis();
  if (now - lastPollMs < POLL_INTERVAL_MS) return;
  lastPollMs = now;

  sampleBattery();
  pollState();

  if (count >= FLUSH_WATERMARK || (count > 0 && now - pendingSinceMs >= FLUSH_AGE_MS)) {
    // A card that just failed is not retried on every poll: without this the
    // watermark condition stays true and each 500 ms tick costs an open() and
    // an error line for as long as the card stays unwritable.
    if (!flushFailed || now - lastFlushFailMs >= FLUSH_RETRY_MS) flush();
  }
}

void UsageLog::notePageTurn(const bool isForward, const bool isSkip) {
  // Ring append only: no SD, no I2C, no clock. See the header for the aux
  // encoding (1/2 single page forward/back, 3/4 skip forward/back).
  const uint8_t aux = isSkip ? (isForward ? 3 : 4) : (isForward ? 1 : 2);
  record(EV_PAGE, aux);
}

void UsageLog::noteSleep() {
  if (!started) return;
  sampleBattery();
  record(EV_SLEEP, 0);
  flush();
}

void UsageLog::record(const uint8_t event, const uint8_t aux) {
  if (!started) return;
  if (count == RING_SIZE) {
    // Card missing or erroring: keep the newest window and say how much went.
    // The counter saturates at 255, so the DROP row's aux is a floor on the
    // number lost, not necessarily the exact count.
    head = static_cast<uint8_t>((head + 1) % RING_SIZE);
    count--;
    if (dropped < UINT8_MAX) dropped++;
    // The oldest entry just changed, and it is what the FLUSH_AGE_MS deadline
    // is measured from.
    pendingSinceMs = entries[head].ms;
  }
  Entry& e = entries[(head + count) % RING_SIZE];
  e.ms = millis();
  e.mv = lastMv;
  e.pct = lastPct;
  e.event = event;
  e.aux = aux;
  e.chg = lastChg ? 1 : 0;
  if (count == 0) pendingSinceMs = e.ms;
  count++;
}

void UsageLog::sampleBattery() {
  // Keep the last good values: readMillivolts() returns 0 and
  // readPercentageChecked() returns false on a transient gauge I2C failure, and
  // a fabricated 0%/0 mV row would read as a dead battery off-device.
  // The percent comes from readPercentageChecked() rather than the millivolt
  // curve so a gauge board logs the same SoC the on-screen indicator shows;
  // ADC boards fall back to the curve inside it.
  const uint16_t mv = monitor().readMillivolts();
  if (mv != 0) lastMv = mv;
  uint16_t pct;
  if (monitor().readPercentageChecked(pct)) lastPct = static_cast<uint8_t>(pct);
  lastChg = monitor().isCharging();
}

void UsageLog::pollState() {
  const bool frontlightOn = Frontlight.present() && Frontlight.isOn();
  if (frontlightOn != lastFrontlightOn) {
    lastFrontlightOn = frontlightOn;
    record(frontlightOn ? EV_FL_ON : EV_FL_OFF, frontlightOn ? Frontlight.brightness() : 0);
  }

  const bool nightMode = SETTINGS.screenInverted != 0;
  if (nightMode != lastNightMode) {
    lastNightMode = nightMode;
    record(nightMode ? EV_NIGHT_ON : EV_NIGHT_OFF, 0);
  }

  const bool wifiOn = WiFi.getMode() != WIFI_MODE_NULL;
  if (wifiOn != lastWifiOn) {
    lastWifiOn = wifiOn;
    record(wifiOn ? EV_WIFI_ON : EV_WIFI_OFF, 0);
  }
}

bool UsageLog::flush() {
  if (count == 0) return true;

  HalFile file = Storage.open(LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    // Card pulled or unwritable: everything stays buffered for a later tick.
    LOG_ERR("ULOG", "Cannot append to %s", LOG_PATH);
    lastFlushFailMs = millis();
    flushFailed = true;
    return false;
  }
  if (needHeader) {
    // Only clear the flag once the header is actually on the card, so a failed
    // write does not leave a headerless file that never gets one.
    if (file.write(CSV_HEADER, sizeof(CSV_HEADER) - 1) != sizeof(CSV_HEADER) - 1) {
      LOG_ERR("ULOG", "Short header write to %s", LOG_PATH);
      file.close();
      lastFlushFailMs = millis();
      flushFailed = true;
      return false;
    }
    needHeader = false;
  }

  // One clock read for the whole batch; per-row times come off the millis()
  // delta, which is why the event path never touches the RTC.
  const uint32_t flushMs = millis();
  const uint32_t flushWall = readWallClock();

  // Anything lost while the card was unwritable is reported once, ahead of the
  // batch, borrowing the oldest surviving entry's timestamp and battery sample.
  if (dropped > 0 && !writeRow(file, entries[head], EV_DROP, dropped, flushWall, flushMs)) {
    file.close();
    lastFlushFailMs = millis();
    flushFailed = true;
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    const Entry& e = entries[(head + i) % RING_SIZE];
    if (!writeRow(file, e, e.event, e.aux, flushWall, flushMs)) {
      // Nothing is dropped on a short write: the whole batch stays buffered and
      // a later tick retries it. A card that fails mid-batch can therefore leave
      // the rows it did take duplicated in the file -- cheaper to de-duplicate
      // off-device than to track a partial write here.
      file.close();
      lastFlushFailMs = millis();
      flushFailed = true;
      return false;
    }
  }

  file.close();
  head = 0;
  count = 0;
  dropped = 0;
  pendingSinceMs = 0;
  flushFailed = false;
  return true;
}

bool UsageLog::writeRow(HalFile& file, const Entry& e, const uint8_t event, const uint8_t aux, const uint32_t flushWall,
                        const uint32_t flushMs) {
  char stamp[20] = "";
  if (flushWall != 0) {
    // The entry's own wall time, walked back from the one clock read by its
    // millis() delta.
    const uint32_t wall = flushWall - (flushMs - e.ms) / 1000;
    const uint32_t secondOfDay = wall % SECONDS_PER_DAY;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    civil::civilFromDays(wall / SECONDS_PER_DAY, year, month, day);
    snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u", year, month, day, secondOfDay / 3600,
             (secondOfDay / 60) % 60, secondOfDay % 60);
  }

  // 80 bytes is not a truncation risk: every field is fixed-width by type, and
  // the longest possible row (19 stamp + 10 millis + 9 NIGHT_OFF + 3 + 5 + 3 + 3
  // + 6 commas + newline) is 58 bytes, so `len` is always the full row.
  char line[80];
  const int len = snprintf(line, sizeof(line), "%s,%lu,%s,%u,%u,%u,%u\n", stamp, static_cast<unsigned long>(e.ms),
                           EVENT_NAMES[event], e.pct, e.mv, e.chg, aux);
  if (len <= 0 || file.write(line, static_cast<size_t>(len)) != static_cast<size_t>(len)) {
    LOG_ERR("ULOG", "Short write to %s", LOG_PATH);
    return false;
  }
  return true;
}

#endif  // CROSSPOINT_USAGE_LOG
