// Host tests for lib/hal/SoftClock.h -- the pure half of HalClock's
// CROSSPOINT_SOFT_CLOCK backend (FLASHCARD_SPEC.md §7b.2). Built by build.sh
// with g++ -DCROSSPOINT_SOFT_CLOCK; never compiled into the firmware
// (host_tests/ is outside src/ and lib/).
//
// What is testable here and what is not:
//   * TESTED: the plausibility predicate, the three-state validity machine, the
//     epoch -> Rtc::DateTime conversion, the NTP backoff policy over its
//     attempt/success stamps, the persistence-cadence policy, the persisted
//     record's encode/decode (including torn and tampered records), the
//     RFC-822/1123 date parser that turns a server's `Date:` header into the
//     clock, and the post-sync plausibility settle. All of them are pure
//     functions of their arguments in SoftClock.h, which is why they live there
//     rather than inline in HalClock.cpp -- these tests exercise the exact code
//     the device runs, with the time source and the millis() clock passed in.
//   * NOT TESTED: SNTP, WiFi, millis(), the SD file, and the RTC_NOINIT stamps
//     themselves. HalClock.cpp keeps every side effect, needs Arduino, esp_sntp
//     and HalStorage, and has no host stand-in. In particular the claim that
//     RTC_NOINIT memory survives esp_restart and dies with the battery rail is
//     a platform property, verified by reading the IDF bootloader's segment
//     loading, not by anything here.
//
// One honesty note about the host stand-in: fillDateTime() calls gmtime_r, and
// the gmtime_r running under these tests is glibc's while the device runs
// newlib's. They are not the same implementation. That is exactly why the
// round-trip group cross-checks against src/CivilDate.h -- integer arithmetic
// that is identical on both -- and why softclock::daysFromCivil is pinned
// against civil::daysFromCivil below: a divergence in either direction shows up
// as a failure here rather than as a study day that lands on the wrong date.
//
// The round-trip group deliberately closes the loop that actually runs on the
// device: src/RtcClock.h turns HalClock's DateTime back into epoch seconds for
// the study clock, so "gmtime_r out, daysFromCivil back" is system time ->
// DateTime -> fsrs::Now.unixSecs.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "CivilDate.h"
#include "SoftClock.h"

namespace {

// --- tiny harness ------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void beginGroup(const char* name) { g_group = name; }

void record(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    printf("FAIL [%s] line %d: %s\n", g_group, line, expr);
  }
}

#define EXPECT(cond) record((cond), #cond, __LINE__)

#define EXPECT_EQ_I(actual, expected)                                                                               \
  do {                                                                                                              \
    const long long a_ = static_cast<long long>(actual);                                                            \
    const long long e_ = static_cast<long long>(expected);                                                          \
    ++g_checks;                                                                                                     \
    if (a_ != e_) {                                                                                                 \
      ++g_failures;                                                                                                 \
      printf("FAIL [%s] line %d: %s == %s (got %lld, want %lld)\n", g_group, __LINE__, #actual, #expected, a_, e_); \
    }                                                                                                               \
  } while (0)

// Stand-in for Rtc::DateTime: the same field names and widths, so
// fillDateTime() instantiates over the shape the firmware passes it without
// dragging the SDK header (and its Arduino include) onto the host.
struct DateTime {
  uint16_t year = 2000;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t weekday = 0;
};

constexpr int64_t THRESHOLD = softclock::MIN_VALID_EPOCH;  // 2026-01-01T00:00:00Z

using softclock::Validity;

/** Epoch seconds for a UTC calendar instant, computed independently of SoftClock.h. */
int64_t at(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
  return static_cast<int64_t>(civil::daysFromCivil(static_cast<uint32_t>(year), static_cast<uint32_t>(month),
                                                   static_cast<uint32_t>(day))) *
             86400 +
         hour * 3600 + minute * 60 + second;
}

// --- validity ----------------------------------------------------------------

void testValidityThreshold() {
  beginGroup("plausibility threshold");

  // The threshold is a date, not a magic number: pin it to the civil date the
  // spec names, computed independently of the constant it is checking.
  EXPECT_EQ_I(THRESHOLD, at(2026, 1, 1));

  // Cold boot. The C3 comes up at the epoch itself, which is the case the whole
  // threshold exists for.
  EXPECT(!softclock::epochIsValid(0));

  // A pre-epoch reading must compare as too-early rather than wrap into the far
  // future -- the reason the parameter is signed.
  EXPECT(!softclock::epochIsValid(-1));
  EXPECT(!softclock::epochIsValid(-2208988800));  // 1900-01-01, the NTP era base

  // 2025-12-31T23:59:59Z -- one second short.
  EXPECT(!softclock::epochIsValid(THRESHOLD - 1));

  // 2026-01-01T00:00:00Z -- the first valid instant. Inclusive on purpose.
  EXPECT(softclock::epochIsValid(THRESHOLD));
  EXPECT(softclock::epochIsValid(THRESHOLD + 1));

  // Far future: 2100-01-01, and past the 32-bit signed time_t rollover in 2038,
  // which this must not treat as a wrap.
  EXPECT(softclock::epochIsValid(at(2038, 1, 20)));
  EXPECT(softclock::epochIsValid(at(2100, 1, 1)));

  // A whole year of readings either side of the boundary, checked against the
  // date rather than the constant.
  for (int dayOffset = -400; dayOffset <= 400; ++dayOffset) {
    const int64_t epoch = THRESHOLD + static_cast<int64_t>(dayOffset) * 86400;
    EXPECT(softclock::epochIsValid(epoch) == (dayOffset >= 0));
  }
}

// --- the three-state machine -------------------------------------------------

void testValidityStates() {
  beginGroup("validity state machine");

  // Cold boot after a flat battery: system time is at the epoch and the RTC-RAM
  // fix stamp died with the rail. No usable date at all.
  EXPECT(softclock::classify(0, false) == Validity::Invalid);
  EXPECT(softclock::classify(0, true) == Validity::Invalid);  // stamp cannot rescue a 1970 clock
  EXPECT(softclock::classify(THRESHOLD - 1, false) == Validity::Invalid);

  // Restore from the SD record: a plausible epoch with no fix on this rail.
  // This is the STALE state -- the date is right, the lag is unknown.
  EXPECT(softclock::classify(THRESHOLD, false) == Validity::Stale);
  EXPECT(softclock::classify(at(2026, 6, 1, 12), false) == Validity::Stale);

  // A server fix on this rail promotes it to VALID and it stays there for as
  // long as the rail lives (an esp_restart keeps both halves).
  EXPECT(softclock::classify(THRESHOLD, true) == Validity::Valid);
  EXPECT(softclock::classify(at(2026, 6, 1, 12), true) == Validity::Valid);

  // isAvailable() is STALE-or-VALID: existing callers (StudyClock, the status
  // bar, RtcClock) keep seeing exactly one bool and keep working on a restored
  // clock, which is the entire point of adding the middle state.
  EXPECT(!softclock::isUsable(Validity::Invalid));
  EXPECT(softclock::isUsable(Validity::Stale));
  EXPECT(softclock::isUsable(Validity::Valid));

  // The full transition sequence a device actually walks: flat battery ->
  // restore -> feed sync -> flat battery again -> restore.
  int64_t clock = 0;
  bool fixedOnThisRail = false;
  EXPECT(softclock::classify(clock, fixedOnThisRail) == Validity::Invalid);
  int64_t restored = 0;
  uint8_t blob[softclock::RECORD_SIZE];
  softclock::encodeRecord(at(2026, 4, 1, 9, 30), blob);
  EXPECT(softclock::decodeRecord(blob, sizeof(blob), restored));
  clock = restored;
  EXPECT(softclock::classify(clock, fixedOnThisRail) == Validity::Stale);
  clock = at(2026, 4, 3, 18, 5);  // the feed server's Date header
  fixedOnThisRail = true;
  EXPECT(softclock::classify(clock, fixedOnThisRail) == Validity::Valid);
  // Battery dies: system time and the rail stamp go together, never separately.
  clock = 0;
  fixedOnThisRail = false;
  EXPECT(softclock::classify(clock, fixedOnThisRail) == Validity::Invalid);
  softclock::encodeRecord(at(2026, 4, 3, 18, 5), blob);
  EXPECT(softclock::decodeRecord(blob, sizeof(blob), restored));
  EXPECT(softclock::classify(restored, false) == Validity::Stale);
}

// --- post-sync plausibility settle -------------------------------------------

void testSettleAfterSync() {
  beginGroup("post-sync snapshot restore");

  const int64_t good = at(2026, 5, 5, 10);
  const int64_t stale = at(2026, 4, 1);

  // A plausible answer always wins, whatever was there before.
  EXPECT_EQ_I(softclock::settleAfterSync(0, good), good);
  EXPECT_EQ_I(softclock::settleAfterSync(stale, good), good);

  // An implausible answer is discarded and the snapshot goes back. This is the
  // case that matters: SNTP has already called settimeofday() by the time the
  // firmware sees the answer, so without the restore a bad server turns a
  // STALE clock into an INVALID one -- worse than never having asked.
  EXPECT_EQ_I(softclock::settleAfterSync(stale, 0), stale);
  EXPECT_EQ_I(softclock::settleAfterSync(stale, -1), stale);
  EXPECT_EQ_I(softclock::settleAfterSync(stale, THRESHOLD - 1), stale);
  EXPECT(softclock::classify(softclock::settleAfterSync(stale, 0), false) == Validity::Stale);

  // Both implausible: nothing to rescue, and the result stays INVALID rather
  // than pretending otherwise.
  EXPECT_EQ_I(softclock::settleAfterSync(0, 12345), 0);
  EXPECT(softclock::classify(softclock::settleAfterSync(0, 12345), false) == Validity::Invalid);

  // The boundary itself is an acceptable answer.
  EXPECT_EQ_I(softclock::settleAfterSync(stale, THRESHOLD), THRESHOLD);
}

// --- epoch -> DateTime -------------------------------------------------------

void expectDate(int64_t epoch, int year, int month, int day, int hour, int minute, int second, int weekday) {
  DateTime dt;
  EXPECT(softclock::fillDateTime(epoch, dt));
  EXPECT_EQ_I(dt.year, year);
  EXPECT_EQ_I(dt.month, month);
  EXPECT_EQ_I(dt.day, day);
  EXPECT_EQ_I(dt.hour, hour);
  EXPECT_EQ_I(dt.minute, minute);
  EXPECT_EQ_I(dt.second, second);
  EXPECT_EQ_I(dt.weekday, weekday);
}

void testDateTimeConversion() {
  beginGroup("epoch -> DateTime");

  // The threshold instant itself: 2026-01-01 was a Thursday (weekday 4).
  expectDate(THRESHOLD, 2026, 1, 1, 0, 0, 0, 4);

  // Leap day, and the second before/after midnight around it.
  expectDate(at(2028, 2, 29, 23, 59, 59), 2028, 2, 29, 23, 59, 59, 2);
  expectDate(at(2028, 3, 1), 2028, 3, 1, 0, 0, 0, 3);

  // Year boundary.
  expectDate(at(2026, 12, 31, 23, 59, 59), 2026, 12, 31, 23, 59, 59, 4);

  // Invalid readings leave `out` untouched rather than writing 1970 into it --
  // callers that ignore the return value must not silently get a 1970 date.
  DateTime dt;
  EXPECT(!softclock::fillDateTime(0, dt));
  EXPECT_EQ_I(dt.year, 2000);
  EXPECT(!softclock::fillDateTime(THRESHOLD - 1, dt));
  EXPECT_EQ_I(dt.year, 2000);
  EXPECT_EQ_I(dt.month, 1);
}

void testDaysFromCivilAgainstCivilDate() {
  beginGroup("daysFromCivil vs CivilDate");

  // SoftClock.h carries its own copy of days_from_civil because lib/ cannot
  // include src/. Pin the copies together over a long sweep so a typo in either
  // is a test failure rather than a date that is off by a day on the device.
  static const int DAYS[] = {1, 15, 28};
  for (int year = 1970; year <= 2100; ++year) {
    for (int month = 1; month <= 12; ++month) {
      for (const int day : DAYS) {
        EXPECT_EQ_I(softclock::daysFromCivil(year, month, day),
                    static_cast<int64_t>(civil::daysFromCivil(static_cast<uint32_t>(year), static_cast<uint32_t>(month),
                                                              static_cast<uint32_t>(day))));
      }
    }
  }
  // The leap-day cases the sweep above skips.
  EXPECT_EQ_I(softclock::daysFromCivil(2028, 2, 29), static_cast<int64_t>(civil::daysFromCivil(2028, 2, 29)));
  EXPECT_EQ_I(softclock::daysFromCivil(2100, 3, 1), static_cast<int64_t>(civil::daysFromCivil(2100, 3, 1)));
  EXPECT_EQ_I(softclock::daysFromCivil(1970, 1, 1), 0);
}

void testDateTimeRoundTrip() {
  beginGroup("round-trip vs CivilDate");

  // The device path in full: system time -> HalClock::getDateTime ->
  // rtcclock::rtcUnixSecs() -> fsrs::Now.unixSecs. Anything that does not come
  // back bit-for-bit would shift a study day.
  const int64_t samples[] = {
      THRESHOLD,
      THRESHOLD + 1,
      THRESHOLD + 86399,
      at(2026, 3, 8, 4),   // rollover hour
      at(2027, 7, 4, 12),  // mid-year, midday
      at(2028, 2, 29) + 86399,
      at(2038, 1, 19, 3, 14, 8),
      at(2099, 12, 31) + 86399,
  };
  for (const int64_t epoch : samples) {
    DateTime dt;
    EXPECT(softclock::fillDateTime(epoch, dt));
    const int64_t back = static_cast<int64_t>(civil::daysFromCivil(dt.year, dt.month, dt.day)) * 86400 +
                         dt.hour * 3600 + dt.minute * 60 + dt.second;
    EXPECT_EQ_I(back, epoch);
  }

  // Every hour of a leap year, plus a stride over a decade: the conversion is
  // cheap, so sweep rather than spot-check.
  const int64_t yearStart = at(2028, 1, 1);
  for (int64_t epoch = yearStart; epoch < yearStart + 366 * 86400; epoch += 3600) {
    DateTime dt;
    if (!softclock::fillDateTime(epoch, dt)) {
      EXPECT(false);
      break;
    }
    const int64_t back = static_cast<int64_t>(civil::daysFromCivil(dt.year, dt.month, dt.day)) * 86400 +
                         dt.hour * 3600 + dt.minute * 60 + dt.second;
    if (back != epoch) {
      EXPECT_EQ_I(back, epoch);
      break;
    }
  }
  EXPECT(true);  // the sweep completed without breaking out

  for (int64_t epoch = THRESHOLD; epoch < THRESHOLD + 3653LL * 86400; epoch += 86400 + 3607) {
    DateTime dt;
    if (!softclock::fillDateTime(epoch, dt)) {
      EXPECT(false);
      break;
    }
    const int64_t back = static_cast<int64_t>(civil::daysFromCivil(dt.year, dt.month, dt.day)) * 86400 +
                         dt.hour * 3600 + dt.minute * 60 + dt.second;
    if (back != epoch) {
      EXPECT_EQ_I(back, epoch);
      break;
    }
  }
  EXPECT(true);
}

// --- NTP backoff policy ------------------------------------------------------

constexpr uint32_t DAY_MS = softclock::RESYNC_INTERVAL_MS;
constexpr uint32_t FLOOR_MS = softclock::MIN_ATTEMPT_INTERVAL_MS;

softclock::SyncStamps stamps(bool hasAttempt, uint32_t attemptMs, bool hasSuccess, uint32_t successMs) {
  softclock::SyncStamps s = softclock::NO_STAMPS;
  s.hasAttempt = hasAttempt;
  s.lastAttemptMs = attemptMs;
  s.hasSuccess = hasSuccess;
  s.lastSuccessMs = successMs;
  return s;
}

void testBackoffPolicy() {
  beginGroup("NTP backoff policy");

  // The intervals are spelled out so a typo in either constant is caught here
  // rather than as a device that syncs every 24 minutes.
  EXPECT_EQ_I(DAY_MS, 86400000UL);
  EXPECT_EQ_I(FLOOR_MS, 600000UL);

  // Fresh rail, nothing recorded: every state fires. This is the cold boot, and
  // the only thing that can produce a clock.
  EXPECT(softclock::shouldAttemptSync(Validity::Invalid, 0, softclock::NO_STAMPS));
  EXPECT(softclock::shouldAttemptSync(Validity::Stale, 0, softclock::NO_STAMPS));
  EXPECT(softclock::shouldAttemptSync(Validity::Valid, 0, softclock::NO_STAMPS));

  // The failed-attempt floor is the reviewer's M1/M2 case: a device with WiFi
  // but no route to an NTP server must not pay the full 5 s SNTP timeout on
  // every sync flow. It applies in EVERY state, INVALID very much included --
  // "the clock is unset" is exactly the state such a device sits in forever.
  const softclock::SyncStamps failedAt10s = stamps(true, 10000, false, 0);
  EXPECT(!softclock::shouldAttemptSync(Validity::Invalid, 10000, failedAt10s));
  EXPECT(!softclock::shouldAttemptSync(Validity::Invalid, 10000 + 1000, failedAt10s));
  EXPECT(!softclock::shouldAttemptSync(Validity::Invalid, 10000 + FLOOR_MS - 1, failedAt10s));
  EXPECT(softclock::shouldAttemptSync(Validity::Invalid, 10000 + FLOOR_MS, failedAt10s));
  EXPECT(softclock::shouldAttemptSync(Validity::Invalid, 10000 + FLOOR_MS + 1, failedAt10s));
  // Same floor, same boundaries, from the other two states.
  EXPECT(!softclock::shouldAttemptSync(Validity::Stale, 10000 + FLOOR_MS - 1, failedAt10s));
  EXPECT(softclock::shouldAttemptSync(Validity::Stale, 10000 + FLOOR_MS, failedAt10s));
  EXPECT(!softclock::shouldAttemptSync(Validity::Valid, 10000 + FLOOR_MS - 1, failedAt10s));

  // A restored (STALE) clock is worth re-fixing as soon as the floor allows:
  // the date is right but the lag is unknown, and only a server can close it.
  EXPECT(softclock::shouldAttemptSync(Validity::Stale, 10000 + FLOOR_MS, stamps(true, 10000, true, 0)));

  // The 24 h success floor only binds a VALID clock. Two sync flows in one
  // session (RSS then decks) must not cost 5 s twice.
  const softclock::SyncStamps ok = stamps(true, 1000, true, 1000);
  EXPECT(!softclock::shouldAttemptSync(Validity::Valid, 1000 + FLOOR_MS, ok));
  EXPECT(!softclock::shouldAttemptSync(Validity::Valid, 1000 + DAY_MS - 1, ok));
  EXPECT(softclock::shouldAttemptSync(Validity::Valid, 1000 + DAY_MS, ok));
  EXPECT(softclock::shouldAttemptSync(Validity::Valid, 1000 + DAY_MS + 1, ok));

  // A success recorded on a clock that has since gone INVALID (a bad
  // settimeofday, a corrupted counter) does not suppress the retry.
  EXPECT(softclock::shouldAttemptSync(Validity::Invalid, 1000 + FLOOR_MS, ok));

  // millis() wraps at ~49.7 days, and the rail timeline is carried on the same
  // uint32. The unsigned subtraction has to keep giving the true elapsed time
  // across the wrap, or a device left on would either stop re-syncing forever
  // or re-sync on every call. uint32_t and not `unsigned long`, which is
  // 64-bit on this host and would make the wrap untestable.
  const uint32_t nearWrap = 0xFFFFFFFFU - 1000U;
  const softclock::SyncStamps wrapped = stamps(true, nearWrap, true, nearWrap);
  EXPECT(!softclock::shouldAttemptSync(Validity::Valid, nearWrap + 500U, wrapped));  // 500 ms later, no wrap
  EXPECT(!softclock::shouldAttemptSync(Validity::Valid, 500U, wrapped));             // 1501 ms later, wrapped
  EXPECT(softclock::shouldAttemptSync(Validity::Valid, static_cast<uint32_t>(nearWrap + DAY_MS), wrapped));
  // The failure floor across the same wrap.
  const softclock::SyncStamps failedNearWrap = stamps(true, nearWrap, false, 0);
  EXPECT(!softclock::shouldAttemptSync(Validity::Invalid, 500U, failedNearWrap));
  EXPECT(softclock::shouldAttemptSync(Validity::Invalid, static_cast<uint32_t>(nearWrap + FLOOR_MS), failedNearWrap));

  // A rail rebase (HalClock::begin() re-seeds the base from the newest stamp
  // after an esp_restart) leaves elapsed == 0, so the floor is still in force
  // immediately after the reboot. That is the whole reason the stamps are in
  // RTC RAM: the sync flows can silently restart the ESP.
  EXPECT(!softclock::shouldAttemptSync(Validity::Invalid, 10000, stamps(true, 10000, false, 0)));
}

void testBackoffSequence() {
  beginGroup("backoff over a session");

  // Walk a routeless device through a morning: four sync flows in ten minutes
  // must cost exactly one NTP attempt.
  softclock::SyncStamps s = softclock::NO_STAMPS;
  uint32_t now = 5000U;
  int attempts = 0;
  for (int flow = 0; flow < 4; ++flow) {
    if (softclock::shouldAttemptSync(Validity::Invalid, now, s)) {
      ++attempts;
      s.hasAttempt = true;
      s.lastAttemptMs = now;  // the attempt failed: no success stamp
    }
    now += 120000U;  // a sync flow every two minutes
  }
  EXPECT_EQ_I(attempts, 1);

  // Past the floor it tries again, and once a server answers the 24 h floor
  // takes over.
  now = 5000U + FLOOR_MS;
  EXPECT(softclock::shouldAttemptSync(Validity::Invalid, now, s));
  s.hasAttempt = true;
  s.lastAttemptMs = now;
  s.hasSuccess = true;
  s.lastSuccessMs = now;
  for (uint32_t later = now + FLOOR_MS; later < now + DAY_MS; later += DAY_MS / 8) {
    EXPECT(!softclock::shouldAttemptSync(Validity::Valid, later, s));
  }
  EXPECT(softclock::shouldAttemptSync(Validity::Valid, now + DAY_MS, s));
}

// --- persistence cadence -----------------------------------------------------

void testPersistCadence() {
  beginGroup("persistence cadence");

  constexpr uint32_t SIX_H = softclock::PERSIST_INTERVAL_MS;
  EXPECT_EQ_I(SIX_H, 21600000UL);

  // Nothing to write down while the clock is INVALID: a 1970 record would come
  // back as INVALID anyway, and writing it costs an SD transaction per tick.
  EXPECT(!softclock::shouldPersistPeriodically(Validity::Invalid, SIX_H * 4, 0));

  // Not due for the first six hours of a session -- the tick runs on every main
  // loop pass, so this is the answer almost every time it is asked.
  EXPECT(!softclock::shouldPersistPeriodically(Validity::Valid, 0, 0));
  EXPECT(!softclock::shouldPersistPeriodically(Validity::Valid, SIX_H - 1, 0));
  EXPECT(softclock::shouldPersistPeriodically(Validity::Valid, SIX_H, 0));
  EXPECT(softclock::shouldPersistPeriodically(Validity::Valid, SIX_H + 1, 0));

  // A STALE clock is persisted on the same schedule: the record is already on
  // the card, but rewriting it is harmless and keeps one code path.
  EXPECT(softclock::shouldPersistPeriodically(Validity::Stale, SIX_H, 0));

  // Across the millis() wrap, same unsigned arithmetic as the backoff.
  const uint32_t nearWrap = 0xFFFFFFFFU - 1000U;
  EXPECT(!softclock::shouldPersistPeriodically(Validity::Valid, 500U, nearWrap));
  EXPECT(softclock::shouldPersistPeriodically(Validity::Valid, static_cast<uint32_t>(nearWrap + SIX_H), nearWrap));
}

// --- the persisted record ----------------------------------------------------

void testRecordRoundTrip() {
  beginGroup("record encode/decode");

  EXPECT_EQ_I(softclock::RECORD_SIZE, 16);

  const int64_t samples[] = {
      THRESHOLD,
      THRESHOLD + 1,
      at(2026, 4, 1, 9, 30),
      at(2038, 1, 19, 3, 14, 7),  // the 32-bit time_t rollover second
      at(2038, 1, 19, 3, 14, 8),  // and one past it: the record is 64-bit
      at(2099, 12, 31, 23, 59, 59),
  };
  for (const int64_t epoch : samples) {
    uint8_t buf[softclock::RECORD_SIZE];
    softclock::encodeRecord(epoch, buf);
    int64_t out = -1;
    EXPECT(softclock::decodeRecord(buf, sizeof(buf), out));
    EXPECT_EQ_I(out, epoch);
    // The magic is where a decoder looks first, so pin its bytes.
    EXPECT_EQ_I(buf[0], 'S');
    EXPECT_EQ_I(buf[1], 'C');
    EXPECT_EQ_I(buf[2], 'L');
    EXPECT_EQ_I(buf[3], 'K');
    EXPECT_EQ_I(buf[4], softclock::RECORD_VERSION);
    EXPECT_EQ_I(buf[5], 0);
  }
}

void testRecordRejection() {
  beginGroup("record torn/tampered");

  uint8_t good[softclock::RECORD_SIZE];
  const int64_t epoch = at(2026, 4, 1, 9, 30);
  softclock::encodeRecord(epoch, good);

  // Every rejection below must leave the caller's epoch untouched, because
  // HalClock::restoreFromStorage() only writes system time when this returns
  // true and a half-written `out` would be a 1970 clock presented as STALE.
  const int64_t SENTINEL = -777;

  // A missing file, a short read, and an over-long read.
  int64_t out = SENTINEL;
  EXPECT(!softclock::decodeRecord(nullptr, softclock::RECORD_SIZE, out));
  EXPECT(!softclock::decodeRecord(good, 0, out));
  EXPECT(!softclock::decodeRecord(good, softclock::RECORD_SIZE - 1, out));
  EXPECT(!softclock::decodeRecord(good, softclock::RECORD_SIZE + 1, out));
  EXPECT_EQ_I(out, SENTINEL);

  // The classic torn write: the directory entry says 16 bytes but the sector
  // never landed, so the file reads back as zeros. The non-zero checksum seed
  // is what stops that decoding as "1970, valid".
  uint8_t zeros[softclock::RECORD_SIZE] = {};
  out = SENTINEL;
  EXPECT(!softclock::decodeRecord(zeros, sizeof(zeros), out));
  EXPECT_EQ_I(out, SENTINEL);

  // All-0xFF: erased flash / a freshly allocated cluster.
  uint8_t ones[softclock::RECORD_SIZE];
  memset(ones, 0xFF, sizeof(ones));
  out = SENTINEL;
  EXPECT(!softclock::decodeRecord(ones, sizeof(ones), out));
  EXPECT_EQ_I(out, SENTINEL);

  // A single flipped bit anywhere in the record must be caught -- by the magic,
  // the version, the reserved byte or the checksum, depending on where it lands.
  for (size_t byte = 0; byte < softclock::RECORD_SIZE; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      uint8_t buf[softclock::RECORD_SIZE];
      memcpy(buf, good, sizeof(buf));
      buf[byte] = static_cast<uint8_t>(buf[byte] ^ (1u << bit));
      int64_t got = SENTINEL;
      if (softclock::decodeRecord(buf, sizeof(buf), got)) {
        printf("FAIL [%s] byte %zu bit %d decoded as %lld\n", g_group, byte, bit, static_cast<long long>(got));
        ++g_failures;
      }
      ++g_checks;
    }
  }

  // A wrong version is rejected rather than misread: the next format change
  // must not be interpreted by this build.
  uint8_t future[softclock::RECORD_SIZE];
  memcpy(future, good, sizeof(future));
  future[4] = softclock::RECORD_VERSION + 1;
  out = SENTINEL;
  EXPECT(!softclock::decodeRecord(future, sizeof(future), out));

  // A structurally perfect record whose epoch is not plausible -- a device that
  // persisted a 1970 clock through an older build, say -- is INVALID, not a
  // date. The plausibility predicate is applied on the way in as well as out.
  uint8_t old[softclock::RECORD_SIZE];
  softclock::encodeRecord(THRESHOLD - 1, old);
  out = SENTINEL;
  EXPECT(!softclock::decodeRecord(old, sizeof(old), out));
  EXPECT_EQ_I(out, SENTINEL);
  softclock::encodeRecord(0, old);
  EXPECT(!softclock::decodeRecord(old, sizeof(old), out));

  // Byte-order swap of the epoch field: a big-endian reader's record. Caught by
  // the checksum, so this is really a check that the checksum covers the payload.
  uint8_t swapped[softclock::RECORD_SIZE];
  memcpy(swapped, good, sizeof(swapped));
  for (size_t i = 0; i < 4; ++i) {
    const uint8_t t = swapped[6 + i];
    swapped[6 + i] = swapped[13 - i];
    swapped[13 - i] = t;
  }
  out = SENTINEL;
  EXPECT(!softclock::decodeRecord(swapped, sizeof(swapped), out));
}

// --- RFC-822 / HTTP-date parsing ---------------------------------------------

void expectDateParse(const char* text, int64_t expected) {
  int64_t out = -1;
  const bool ok = softclock::parseRfc822Epoch(text, out);
  ++g_checks;
  if (!ok || out != expected) {
    ++g_failures;
    printf("FAIL [%s] \"%s\" -> ok=%d %lld (want %lld)\n", g_group, text, ok ? 1 : 0, static_cast<long long>(out),
           static_cast<long long>(expected));
  }
}

void expectDateReject(const char* text) {
  const int64_t SENTINEL = -999;
  int64_t out = SENTINEL;
  const bool ok = softclock::parseRfc822Epoch(text, out);
  ++g_checks;
  if (ok || out != SENTINEL) {
    ++g_failures;
    printf("FAIL [%s] \"%s\" should have been rejected (ok=%d, out=%lld)\n", g_group, text ? text : "(null)",
           ok ? 1 : 0, static_cast<long long>(out));
  }
}

void testDateHeaderParse() {
  beginGroup("RFC-822 Date parse");

  // The canonical IMF-fixdate every HTTP server sends.
  expectDateParse("Wed, 08 Apr 2026 10:30:00 GMT", at(2026, 4, 8, 10, 30, 0));
  // Day name is optional (RFC-822 proper), and so is the leading zero.
  expectDateParse("8 Apr 2026 10:30:00 GMT", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Wed, 8 Apr 2026 10:30:00 GMT", at(2026, 4, 8, 10, 30, 0));
  // Seconds are optional in RFC-822.
  expectDateParse("Wed, 08 Apr 2026 10:30 GMT", at(2026, 4, 8, 10, 30, 0));

  // The zone spellings the spec names.
  expectDateParse("Wed, 08 Apr 2026 10:30:00 UT", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Wed, 08 Apr 2026 10:30:00 UTC", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Wed, 08 Apr 2026 10:30:00 Z", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Wed, 08 Apr 2026 10:30:00 +0000", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Wed, 08 Apr 2026 10:30:00 -0000", at(2026, 4, 8, 10, 30, 0));
  // Case-insensitive, as servers in the wild are.
  expectDateParse("wed, 08 apr 2026 10:30:00 gmt", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("WED, 08 APR 2026 10:30:00 GMT", at(2026, 4, 8, 10, 30, 0));

  // Numeric offsets move the instant in the right direction: a local wall clock
  // ahead of UTC is an EARLIER epoch.
  expectDateParse("Wed, 08 Apr 2026 12:30:00 +0200", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Wed, 08 Apr 2026 05:00:00 -0530", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Thu, 09 Apr 2026 00:30:00 +1400", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Tue, 07 Apr 2026 22:30:00 -1200", at(2026, 4, 8, 10, 30, 0));
  // Trailing whitespace and a CRLF remnant, which is what a raw header line
  // carries if a client is sloppy about trimming it.
  expectDateParse("Wed, 08 Apr 2026 10:30:00 GMT ", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("Wed, 08 Apr 2026 10:30:00 GMT\r\n", at(2026, 4, 8, 10, 30, 0));
  expectDateParse("  Wed, 08 Apr 2026 10:30:00 GMT", at(2026, 4, 8, 10, 30, 0));

  // Every month name, so a lookup-table typo cannot hide in an untested month.
  expectDateParse("Thu, 01 Jan 2026 00:00:00 GMT", at(2026, 1, 1));
  expectDateParse("Sun, 01 Feb 2026 00:00:00 GMT", at(2026, 2, 1));
  expectDateParse("Sun, 01 Mar 2026 00:00:00 GMT", at(2026, 3, 1));
  expectDateParse("Wed, 01 Apr 2026 00:00:00 GMT", at(2026, 4, 1));
  expectDateParse("Fri, 01 May 2026 00:00:00 GMT", at(2026, 5, 1));
  expectDateParse("Mon, 01 Jun 2026 00:00:00 GMT", at(2026, 6, 1));
  expectDateParse("Wed, 01 Jul 2026 00:00:00 GMT", at(2026, 7, 1));
  expectDateParse("Sat, 01 Aug 2026 00:00:00 GMT", at(2026, 8, 1));
  expectDateParse("Tue, 01 Sep 2026 00:00:00 GMT", at(2026, 9, 1));
  expectDateParse("Thu, 01 Oct 2026 00:00:00 GMT", at(2026, 10, 1));
  expectDateParse("Sun, 01 Nov 2026 00:00:00 GMT", at(2026, 11, 1));
  expectDateParse("Tue, 01 Dec 2026 00:00:00 GMT", at(2026, 12, 1));

  // Boundary dates: the plausibility threshold itself, leap day, year end, and
  // the last second of a day.
  expectDateParse("Thu, 01 Jan 2026 00:00:00 GMT", THRESHOLD);
  expectDateParse("Tue, 29 Feb 2028 12:00:00 GMT", at(2028, 2, 29, 12));
  expectDateParse("Thu, 31 Dec 2026 23:59:59 GMT", at(2026, 12, 31, 23, 59, 59));
  expectDateParse("Fri, 01 Jan 2027 00:00:00 GMT", at(2027, 1, 1));
  // Past the 32-bit time_t rollover: the parser and the record are both 64-bit.
  expectDateParse("Tue, 19 Jan 2038 03:14:08 GMT", at(2038, 1, 19, 3, 14, 8));
  // A leap second folds into the following minute rather than being rejected.
  expectDateParse("Thu, 31 Dec 2026 23:59:60 GMT", at(2027, 1, 1));

  beginGroup("RFC-822 Date rejection");

  // Nothing, empty, and truncations of a valid header.
  expectDateReject(nullptr);
  expectDateReject("");
  expectDateReject("Wed,");
  expectDateReject("Wed, 08 Apr");
  expectDateReject("Wed, 08 Apr 2026");
  expectDateReject("Wed, 08 Apr 2026 10");
  expectDateReject("Wed, 08 Apr 2026 10:30:00");  // no zone: RFC-822 requires one

  // Garbage a proxy or an error page might put in the header slot.
  expectDateReject("not a date at all");
  expectDateReject("0");
  expectDateReject("Wed, XX Apr 2026 10:30:00 GMT");
  expectDateReject("Wed, 08 Xyz 2026 10:30:00 GMT");
  expectDateReject("Wed, 08 Apr 20x6 10:30:00 GMT");
  expectDateReject("Wed, 08 Apr 2026 1a:30:00 GMT");
  expectDateReject("Wed, 08 Apr 2026 10-30-00 GMT");

  // Out-of-range fields.
  expectDateReject("Wed, 00 Apr 2026 10:30:00 GMT");
  expectDateReject("Wed, 32 Apr 2026 10:30:00 GMT");
  expectDateReject("Wed, 31 Apr 2026 10:30:00 GMT");  // April has 30 days
  expectDateReject("Wed, 30 Feb 2026 10:30:00 GMT");
  expectDateReject("Wed, 29 Feb 2026 10:30:00 GMT");  // 2026 is not a leap year
  expectDateReject("Wed, 08 Apr 2026 24:30:00 GMT");
  expectDateReject("Wed, 08 Apr 2026 10:60:00 GMT");
  expectDateReject("Wed, 08 Apr 2026 10:30:61 GMT");

  // 2029 IS a common year and 2028 a leap year: pin both so the leap rule is
  // not accidentally inverted.
  expectDateParse("Tue, 29 Feb 2028 00:00:00 GMT", at(2028, 2, 29));
  expectDateReject("Thu, 29 Feb 2029 00:00:00 GMT");
  expectDateReject("Sat, 29 Feb 2100 00:00:00 GMT");  // century, not a leap year

  // Zones that are not accepted: the obsolete single-letter military zones and
  // the US alphabetics, which RFC-1123 itself calls unreliable. Getting one
  // wrong moves the clock by hours, so they are refused rather than guessed.
  expectDateReject("Wed, 08 Apr 2026 10:30:00 EST");
  expectDateReject("Wed, 08 Apr 2026 10:30:00 PDT");
  expectDateReject("Wed, 08 Apr 2026 10:30:00 A");
  expectDateReject("Wed, 08 Apr 2026 10:30:00 GM");
  expectDateReject("Wed, 08 Apr 2026 10:30:00 GMTX");
  // Malformed numeric offsets.
  expectDateReject("Wed, 08 Apr 2026 10:30:00 +00");
  expectDateReject("Wed, 08 Apr 2026 10:30:00 +00000");
  expectDateReject("Wed, 08 Apr 2026 10:30:00 +1500");  // beyond +14:00
  expectDateReject("Wed, 08 Apr 2026 10:30:00 +0060");  // 60 minutes
  expectDateReject("Wed, 08 Apr 2026 10:30:00 +ab00");

  // Two-digit years (the obsolete RFC-822/850 form) are refused rather than
  // guessed at, and a five-digit year is not a year.
  expectDateReject("Wednesday, 08-Apr-26 10:30:00 GMT");
  expectDateReject("Wed, 08 Apr 26 10:30:00 GMT");
  expectDateReject("Wed, 08 Apr 20260 10:30:00 GMT");

  // asctime()'s format, the third form RFC-2616 tolerates. Not supported, and
  // it must fail cleanly rather than half-parse.
  expectDateReject("Wed Apr  8 10:30:00 2026");

  // Structurally fine but before the plausibility threshold: a server whose own
  // clock is unset must not be allowed to un-set the reader's.
  expectDateReject("Sun, 06 Nov 1994 08:49:37 GMT");
  expectDateReject("Thu, 01 Jan 1970 00:00:00 GMT");
  expectDateReject("Wed, 31 Dec 2025 23:59:59 GMT");
  // ...and the very next second is accepted.
  expectDateParse("Thu, 01 Jan 2026 00:00:00 GMT", THRESHOLD);

  // A zone offset that would drag a just-plausible timestamp back under the
  // threshold is rejected: the predicate is applied to the final epoch, not to
  // the wall-clock fields.
  expectDateReject("Thu, 01 Jan 2026 00:30:00 +0100");
  expectDateParse("Thu, 01 Jan 2026 01:00:00 +0100", THRESHOLD);
}

void testDateHeaderFeedsTheClock() {
  beginGroup("Date header -> validity");

  // The whole primary path, end to end in the pure layer: a header string
  // arrives with a feed response, becomes an epoch, promotes the clock to
  // VALID, and is what gets written to the SD record.
  int64_t epoch = 0;
  EXPECT(softclock::parseRfc822Epoch("Wed, 08 Apr 2026 10:30:00 GMT", epoch));
  EXPECT(softclock::classify(epoch, true) == Validity::Valid);

  uint8_t buf[softclock::RECORD_SIZE];
  softclock::encodeRecord(epoch, buf);
  int64_t restored = 0;
  EXPECT(softclock::decodeRecord(buf, sizeof(buf), restored));
  EXPECT_EQ_I(restored, epoch);
  // ...and after the battery dies, the same epoch comes back as STALE.
  EXPECT(softclock::classify(restored, false) == Validity::Stale);

  // A rejected header leaves the clock exactly as it was, which is what lets
  // the sync flows write `if (!applyServerDate(...)) maybeOpportunisticSync()`.
  int64_t untouched = 4242;
  EXPECT(!softclock::parseRfc822Epoch("Sun, 06 Nov 1994 08:49:37 GMT", untouched));
  EXPECT_EQ_I(untouched, 4242);
  EXPECT(!softclock::parseRfc822Epoch("", untouched));
  EXPECT_EQ_I(untouched, 4242);
}

// --- the policy composed with the validity check ----------------------------

void testPolicyAgainstSystemTime() {
  beginGroup("policy over system time");

  // How HalClock::maybeOpportunisticSync() composes the pieces: validity comes
  // from the live reading plus the rail stamp, not from a cached flag, so the
  // same stamps yield different answers as the clock moves.
  const auto decide = [](int64_t epoch, bool fixedOnThisRail, uint32_t nowMs, const softclock::SyncStamps& s) {
    return softclock::shouldAttemptSync(softclock::classify(epoch, fixedOnThisRail), nowMs, s);
  };

  EXPECT(decide(0, false, 10000, softclock::NO_STAMPS));          // cold boot, first flow: fires
  EXPECT(decide(THRESHOLD, false, 10000, softclock::NO_STAMPS));  // restored STALE: fires
  const softclock::SyncStamps fresh = stamps(true, 9000, true, 9000);
  EXPECT(!decide(THRESHOLD, true, 9000 + FLOOR_MS, fresh));              // fixed and fresh: no-op
  EXPECT(decide(THRESHOLD - 1, true, 9000 + FLOOR_MS, fresh));           // a second short of plausible: fires
  EXPECT(decide(THRESHOLD, true, 9000 + DAY_MS, fresh));                 // a day on: fires
  EXPECT(!decide(0, false, 9000 + 1000, stamps(true, 9000, false, 0)));  // inside the failure floor: no-op
}

}  // namespace

int main() {
  testValidityThreshold();
  testValidityStates();
  testSettleAfterSync();
  testDateTimeConversion();
  testDaysFromCivilAgainstCivilDate();
  testDateTimeRoundTrip();
  testBackoffPolicy();
  testBackoffSequence();
  testPersistCadence();
  testRecordRoundTrip();
  testRecordRejection();
  testDateHeaderParse();
  testDateHeaderFeedsTheClock();
  testPolicyAgainstSystemTime();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    printf("SOFT CLOCK HOST TESTS FAILED\n");
    return 1;
  }
  printf("SOFT CLOCK HOST TESTS PASSED\n");
  return 0;
}
