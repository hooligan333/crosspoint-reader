#pragma once

#include <Arduino.h>
#include <Rtc.h>

// CROSSPOINT_SOFT_CLOCK (FLASHCARD_SPEC.md §7b.2) swaps the backend of this
// class — I2C RTC out, ESP system time in — for the original ESP32-C3 X4, which
// has no RTC chip. It changes nothing else: the public surface, Rtc::DateTime
// as the calendar DTO, the POSIX-TZ local-time path and the caller-side
// debounce convention all stay put, so StudyClock, RtcClock.h and the status
// bar are backend-agnostic and unmodified. SoftClock.h holds the pure policy
// (plausibility predicate, validity states, backoff, the persisted record,
// RFC-822 parsing); the flag-off preprocessed output of this file is identical
// to what it was before the flag existed.

#ifdef CROSSPOINT_SOFT_CLOCK
#include "SoftClock.h"
#endif

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
#ifdef CROSSPOINT_SOFT_CLOCK
  // Validity is read live off system time, so nothing is cached — a
  // gettimeofday() is a memory read, not the I2C round trip the cache below
  // exists to avoid. The sync stamps that decide backoff and VALID-vs-STALE do
  // not live here either: they are in RTC RAM (HalClock.cpp), because they must
  // survive an esp_restart and die with the battery rail exactly when system
  // time does. What is left is the persistence bookkeeping, which is per-boot.
  uint32_t _lastPersistMs = 0;  // rail-uptime ms of the last SD record write
#else
  bool _available = false;
  mutable Rtc _sdkRtc;
  // The RTC keeps UTC; local time comes from newlib's localtime_r under the
  // POSIX TZ rule set via setTimezone(), so zones with DST are correct
  // year-round. Cached as a UTC epoch to keep the RTC bus quiet.
  mutable time_t _cachedUtc = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds
#endif

 public:
  // Call after BoardConfig has selected the active device.
  //
  // CROSSPOINT_SOFT_CLOCK: there is no chip to probe and BoardConfig does not
  // matter — this only re-seeds the RTC-RAM sync stamps for the new boot. The
  // SD record is restored separately, by restoreFromStorage(), because it
  // cannot run until the card is mounted.
  void begin();

#ifdef CROSSPOINT_SOFT_CLOCK
  // How much the clock can be trusted right now, read live off system time and
  // the RTC-RAM fix stamp. INVALID (nothing usable), STALE (restored from the
  // SD record across a power cut — right date, unknown lag) or VALID (fixed
  // from a server this power session).
  softclock::Validity validity() const;

  // True for STALE and VALID alike: a restored clock is still a usable date,
  // and study scheduling absorbs hours of lag. Unlike the RTC build this is NOT
  // fixed at begin() — it flips as fixes land and as records are restored, and
  // a cold boot with no record starts false. Callers that gate UI on it must
  // not hide the path that sets the clock — see StatusBarSettingsActivity.
  bool isAvailable() const;

  // Restore the persisted epoch as a STALE clock. Call once at boot, after
  // Storage.begin() and before anything timestamps anything. A missing or torn
  // record leaves the clock INVALID. On a warm boot, where system time survived
  // and is newer than the record, it is left alone: the clock never moves back.
  void restoreFromStorage();

  // Write the persisted record now. Called at deep-sleep entry — on this board
  // sleep drops the battery rail, so this is the last chance to save the time.
  // No-op while the clock is INVALID (there is nothing worth writing down).
  void persistNow();

  // Main-loop tick: rewrites the persisted record at most every 6 h of uptime,
  // for the device that dies flat without ever reaching the sleep path. Costs a
  // millis() compare on every other pass, like usageLog.tick().
  void tick();

  // Adopt a time reported by a server as an RFC-822 date string — the HTTP
  // `Date:` response header on a feed fetch, which is the PRIMARY time source
  // here: it costs no extra round trip, needs no route off the LAN, and arrives
  // on every sync. Rejects anything that does not parse or does not pass the
  // plausibility predicate. Exempt from the NTP backoff (it is free), so call
  // it after every feed fetch. Returns true when the clock was set.
  bool applyServerDate(const char* rfc822);

  // NTP FALLBACK for a flow that already has WiFi up and no usable Date header.
  // Backs off against RTC-RAM stamps: ≥10 min after a failed attempt, ≥24 h
  // after a successful one; otherwise returns false without touching the radio.
  // When it does fire it is syncFromNTP(), with the same ~5 s worst case — call
  // it where a progress screen is already showing.
  // Returns true only if a sync actually ran and succeeded.
  bool maybeOpportunisticSync();
#else
  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }
#endif

  // Set the POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") applied to every
  // read. nullptr/empty falls back to UTC. Drops the read cache so the change
  // shows immediately.
  void setTimezone(const char* posixTz);

  // Current wall-clock time in the configured timezone.
  // Returns false if RTC is not available.
  bool localTime(struct tm& out) const;

  // Get current local hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

// The soft-clock term is on this declaration but NOT on the definition in
// HalClock.cpp, which is the one asymmetry in the guarded-alternate scheme. It
// is deliberate: the soft backend's getDateTime() is its whole UTC calendar
// path, so it lives unconditionally inside that backend's region, while the RTC
// backend still only defines it for the features that need a date. Adding the
// term here and not there is what makes both true at once.
#if defined(CROSSPOINT_USAGE_LOG) || defined(CROSSPOINT_FLASHCARDS) || defined(CROSSPOINT_SOFT_CLOCK)
  // Full calendar date and time (UTC), straight off the RTC. Unlike getTime()
  // this does not go through the 10 s cache: callers read it rarely (once per
  // usage log flush, once per study-clock reading) and need a date, which the
  // cache does not keep.
  // Returns false if the RTC is not available or the read failed.
  //
  // CROSSPOINT_SOFT_CLOCK: served as UTC from system time, and false whenever
  // isAvailable() is — the Rtc::DateTime shape is kept so callers need no
  // knowledge of which backend is underneath. The cache remark above describes
  // the RTC backend only; the soft backend caches nothing, because reading
  // system time is a memory read rather than an I2C round trip.
  bool getDateTime(Rtc::DateTime& out) const;
#endif

  // Format the local time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  //
  // CROSSPOINT_SOFT_CLOCK: SNTP already writes system time, so this sets no
  // chip; it waits for the same completion signal and records the sync in RAM.
  // Same blocking shape, same caller-side debounce convention (and the same
  // display-TZ save/restore around configTzTime) — plus
  // maybeOpportunisticSync() above for the flows that want the debounce done
  // for them.
  bool syncFromNTP();
};
