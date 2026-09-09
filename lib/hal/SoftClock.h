#pragma once

// Soft clock — the pure half of HalClock's CROSSPOINT_SOFT_CLOCK backend
// (FLASHCARD_SPEC.md §7b.2).
//
// The original ESP32-C3 X4 has no RTC chip: BoardConfig gives it NO_SENSORS, so
// FREEINK_CAP_RTC is off, Rtc::begin() links a stub that returns false, and
// every clock feature on that board is inert today. The soft clock replaces the
// I2C part with ESP system time (settimeofday / gettimeofday via time()).
//
// System time does NOT survive a pocket. HalPowerManager::startDeepSleep drops
// the GPIO13 battery MOSFET on this board, so on battery "sleep" is power-off
// and the whole RTC domain — system time and RTC RAM alike — dies with it. Only
// a USB-powered sleep keeps the rail up. Everything below is built around that:
//
//   * Three validity states. INVALID (no usable time), STALE (restored from the
//     SD record below — approximate, behind by the unknown off duration) and
//     VALID (fixed from a server this power session). isAvailable() is
//     STALE-or-VALID, so existing callers keep seeing one bool.
//   * A 16-byte SD record carries the epoch across the power cut. It is written
//     at sleep entry, after every fix, and at most every 6 h of uptime; a cold
//     boot restores it as STALE. No elapsed-time compensation is possible — the
//     device cannot know how long it was off — so the restored reading is a
//     lower bound, which is the documented limitation.
//   * Time fixes come primarily from the feed server's HTTP `Date:` header
//     (free, LAN-only-friendly, arrives on every sync), with NTP as the
//     fallback. Failed NTP attempts back off against stamps kept in RTC RAM,
//     which dies with the rail at exactly the moment the clock does.
//
// Everything here is a pure function over its arguments — no Arduino, no WiFi,
// no SNTP, no globals — so host_tests/softclock exercises the exact code the
// firmware runs. The side-effecting half (SNTP, millis(), WiFi state, SD, RTC
// RAM) stays in HalClock.cpp.
//
// Header-only and wholly inside the flag: a translation unit that includes it
// without the flag, or with the flag but no call, emits nothing.

#ifdef CROSSPOINT_SOFT_CLOCK

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace softclock {

/**
 * Epoch seconds at 2026-01-01T00:00:00Z — the plausibility threshold.
 *
 * Chosen (spec §7b.2) as a date this firmware could not plausibly predate: the
 * feature shipped in 2026, so any earlier reading is an unset clock rather than
 * a user who set the wrong year. It is deliberately NOT the 2020 floor
 * RtcClock.h uses for hardware RTCs — that one screens a battery-backed chip
 * whose power-on default is 2000, this one screens a counter that starts at 0.
 */
constexpr int64_t MIN_VALID_EPOCH = 1767225600;  // 2026-01-01T00:00:00Z

/**
 * The plausibility predicate. True when a reading is a real wall clock rather
 * than an unset counter — used on system time, on a restored SD record, and on
 * anything a server hands us (NTP answer, HTTP Date header) before it is
 * allowed to become the clock.
 *
 * Signed on purpose: time_t is signed and a pre-epoch reading must compare as
 * too-early, not wrap into the far future.
 */
constexpr bool epochIsValid(int64_t epochSecs) { return epochSecs >= MIN_VALID_EPOCH; }

// --- validity ----------------------------------------------------------------

/**
 * How much the clock can be trusted. Ordered by increasing confidence so a
 * caller that only wants "is there a usable date at all" can compare, but the
 * two callers that care about the difference switch on it exhaustively.
 */
enum class Validity : uint8_t {
  Invalid = 0,  // no usable time: cold boot with no (or a torn) record
  Stale = 1,    // restored from SD: right date, unknown lag, no fix yet this rail
  Valid = 2,    // fixed from a server on this power session
};

/**
 * Classify a system-time reading.
 *
 * `fixedOnThisRail` is the RTC-RAM success stamp, not a RAM flag: RTC RAM
 * survives esp_restart (the sync flows' silent reboot) and dies with the
 * battery rail at the same instant system time does, so it means exactly "the
 * time now in the counter was put there by a server, not by a restore".
 */
constexpr Validity classify(int64_t epochSecs, bool fixedOnThisRail) {
  if (!epochIsValid(epochSecs)) return Validity::Invalid;
  return fixedOnThisRail ? Validity::Valid : Validity::Stale;
}

/** isAvailable(): a STALE clock is still a usable date. */
constexpr bool isUsable(Validity state) { return state != Validity::Invalid; }

/**
 * Which reading should stand once a sync has run: the server's answer if it is
 * plausible, otherwise the snapshot taken before the server was allowed to
 * write system time.
 *
 * SNTP calls settimeofday() itself, so by the time this firmware can inspect an
 * answer the clock has already moved. Without the snapshot a server answering
 * 1970 (or a garbage packet) would turn a restored STALE clock into an INVALID
 * one — leaving the device worse off than if it had never asked.
 */
constexpr int64_t settleAfterSync(int64_t snapshot, int64_t answer) { return epochIsValid(answer) ? answer : snapshot; }

// --- sync backoff ------------------------------------------------------------

/**
 * Floor between NTP attempts that did NOT produce a time.
 *
 * Without it a device with no route to an NTP server pays the full ~5 s SNTP
 * timeout on every sync flow, forever — and the sync flows can silently restart
 * the ESP, which is why the stamps live in RTC RAM rather than in the object.
 */
constexpr uint32_t MIN_ATTEMPT_INTERVAL_MS = 10UL * 60UL * 1000UL;  // 10 min

/**
 * Floor between SUCCESSFUL NTP syncs. Day-granularity scheduling is what the
 * clock feeds, so a day is the natural period.
 */
constexpr uint32_t RESYNC_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;  // 24 h

/**
 * The attempt/success stamps, on the rail-uptime timeline (see HalClock.cpp:
 * millis() restarts at 0 on a reboot, so the stamps are carried on a base that
 * is re-seeded from them). Plain aggregate with no member initializers on
 * purpose — the firmware places one of these in RTC RAM, which must not be
 * touched by a dynamic initializer at startup.
 */
struct SyncStamps {
  uint32_t lastAttemptMs;
  uint32_t lastSuccessMs;
  bool hasAttempt;
  bool hasSuccess;
};

/** A cleared stamp set: no attempt, no success. */
constexpr SyncStamps NO_STAMPS = {0, 0, false, false};

/**
 * Whether an NTP attempt is worth making now (spec §7b.2 backoff).
 *
 * Applying an HTTP Date header does NOT come through here — it costs nothing
 * over the fetch that was happening anyway, so it is exempt from the backoff
 * and is always applied when one arrives.
 *
 * All the arithmetic is unsigned subtraction so it stays correct across the
 * ~49.7-day millis() wrap; uint32_t rather than `unsigned long` (millis()' own
 * type) because they are the same width on the ESP32 but not on a 64-bit host,
 * and the wrap has to be reproducible in the host tests.
 */
constexpr bool shouldAttemptSync(Validity state, uint32_t nowMs, const SyncStamps& stamps) {
  // The failure floor comes first and applies in every state, INVALID included:
  // "the clock is unset" is precisely the state a server-less device sits in
  // forever, and it must not turn every sync into a 5 s stall.
  if (stamps.hasAttempt && static_cast<uint32_t>(nowMs - stamps.lastAttemptMs) < MIN_ATTEMPT_INTERVAL_MS) return false;
  // No usable time at all, or a restored reading of unknown lag: worth the 5 s.
  if (state != Validity::Valid) return true;
  return !stamps.hasSuccess || static_cast<uint32_t>(nowMs - stamps.lastSuccessMs) >= RESYNC_INTERVAL_MS;
}

// --- persistence -------------------------------------------------------------

/**
 * How much uptime may pass before the SD record is refreshed opportunistically.
 *
 * The record is also written at sleep entry and after every fix; this only
 * covers the device that dies on a flat battery without ever reaching the sleep
 * path, and 6 h keeps that loss under a day.
 */
constexpr uint32_t PERSIST_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;  // 6 h

/** Whether the periodic (tick-driven) persistence write is due. */
constexpr bool shouldPersistPeriodically(Validity state, uint32_t nowMs, uint32_t lastPersistMs) {
  if (state == Validity::Invalid) return false;  // nothing worth writing down
  return static_cast<uint32_t>(nowMs - lastPersistMs) >= PERSIST_INTERVAL_MS;
}

/**
 * The persisted record: 16 fixed bytes, little-endian, no padding assumptions
 * (encoded byte by byte rather than memcpy'd off a struct).
 *
 *   [0..3]   magic
 *   [4]      version
 *   [5]      reserved, must be 0
 *   [6..13]  epoch seconds, signed 64-bit
 *   [14..15] checksum over bytes 0..13
 *
 * A plain overwrite is used rather than write-to-temp-and-rename: the record is
 * one FAT sector write and a torn one fails the checksum, which decodes as
 * INVALID — the same state a missing record gives, and a state the device is
 * built to recover from on the next sync.
 */
constexpr size_t RECORD_SIZE = 16;
constexpr uint32_t RECORD_MAGIC = 0x4B4C4353UL;  // "SCLK" little-endian
constexpr uint8_t RECORD_VERSION = 1;

/**
 * Additive 16-bit checksum with a non-zero seed, so a run of zero bytes (the
 * classic short/torn write) does not check out as valid.
 */
inline uint16_t recordChecksum(const uint8_t* bytes, size_t count) {
  uint16_t sum = 0xA5A5;
  for (size_t i = 0; i < count; ++i) {
    sum = static_cast<uint16_t>(sum + bytes[i]);
    sum = static_cast<uint16_t>((sum << 1) | (sum >> 15));  // rotate: order matters
  }
  return sum;
}

/** Serialise `epochSecs` into exactly RECORD_SIZE bytes of `out`. */
inline void encodeRecord(int64_t epochSecs, uint8_t* out) {
  out[0] = static_cast<uint8_t>(RECORD_MAGIC & 0xFF);
  out[1] = static_cast<uint8_t>((RECORD_MAGIC >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((RECORD_MAGIC >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((RECORD_MAGIC >> 24) & 0xFF);
  out[4] = RECORD_VERSION;
  out[5] = 0;
  const uint64_t bits = static_cast<uint64_t>(epochSecs);
  for (size_t i = 0; i < 8; ++i) out[6 + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
  const uint16_t sum = recordChecksum(out, 14);
  out[14] = static_cast<uint8_t>(sum & 0xFF);
  out[15] = static_cast<uint8_t>((sum >> 8) & 0xFF);
}

/**
 * Parse a record. Returns false — leaving `outEpoch` untouched — for a short
 * read, a wrong magic or version, a bad checksum, or an epoch that fails the
 * plausibility predicate. Every one of those is the INVALID state.
 */
inline bool decodeRecord(const uint8_t* in, size_t len, int64_t& outEpoch) {
  if (in == nullptr || len != RECORD_SIZE) return false;
  const uint32_t magic = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
  if (magic != RECORD_MAGIC) return false;
  if (in[4] != RECORD_VERSION || in[5] != 0) return false;
  const uint16_t sum = recordChecksum(in, 14);
  if (in[14] != static_cast<uint8_t>(sum & 0xFF) || in[15] != static_cast<uint8_t>((sum >> 8) & 0xFF)) return false;
  uint64_t bits = 0;
  for (size_t i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(in[6 + i]) << (8 * i);
  const int64_t epoch = static_cast<int64_t>(bits);
  if (!epochIsValid(epoch)) return false;
  outEpoch = epoch;
  return true;
}

// --- calendar ----------------------------------------------------------------

/**
 * Howard Hinnant's days_from_civil, narrowed to years >= 1970 so the negative-
 * era branches drop out. Duplicated from src/CivilDate.h rather than included:
 * lib/ cannot include src/ headers, and pulling timegm() in instead would bet
 * the build on a newlib GNU extension. The host tests pin the two against each
 * other, so a drift between the copies is a test failure, not a field bug.
 */
constexpr int64_t daysFromCivil(int32_t year, int32_t month, int32_t day) {
  year -= month <= 2 ? 1 : 0;
  const int32_t era = year / 400;
  const int32_t yoe = year - era * 400;
  const int32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + doe - 719468;
}

/**
 * Epoch seconds to a broken-down UTC calendar value, in whatever DateTime shape
 * the caller carries (HalClock passes Rtc::DateTime so its callers stay
 * backend-agnostic; the host tests pass their own struct).
 *
 * gmtime_r rather than the integer arithmetic above: HalClock.cpp already calls
 * gmtime_r in syncFromNTP(), so it is in every image regardless, and unlike
 * localtime_r/mktime it pulls in no timezone machinery. The weekday field is
 * the other reason — daysFromCivil does not produce one.
 *
 * Returns false — leaving `out` untouched — when the reading is not plausible.
 */
template <typename DateTimeT>
inline bool fillDateTime(int64_t epochSecs, DateTimeT& out) {
  if (!epochIsValid(epochSecs)) return false;
  const time_t t = static_cast<time_t>(epochSecs);
  struct tm parts;
  if (gmtime_r(&t, &parts) == nullptr) return false;
  out.year = static_cast<uint16_t>(parts.tm_year + 1900);
  out.month = static_cast<uint8_t>(parts.tm_mon + 1);
  out.day = static_cast<uint8_t>(parts.tm_mday);
  out.hour = static_cast<uint8_t>(parts.tm_hour);
  out.minute = static_cast<uint8_t>(parts.tm_min);
  out.second = static_cast<uint8_t>(parts.tm_sec);
  out.weekday = static_cast<uint8_t>(parts.tm_wday);
  return true;
}

// --- RFC-822 / HTTP-date parsing ---------------------------------------------

namespace detail {

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

inline const char* skipSpaces(const char* p) {
  while (*p == ' ' || *p == '\t') ++p;
  return p;
}

/** Read 1..maxDigits digits into `out`. Returns nullptr when none are there. */
inline const char* readNumber(const char* p, int maxDigits, int& out) {
  int value = 0;
  int digits = 0;
  while (digits < maxDigits && isDigit(*p)) {
    value = value * 10 + (*p - '0');
    ++p;
    ++digits;
  }
  if (digits == 0) return nullptr;
  out = value;
  return p;
}

/** Exactly `count` digits, no more and no fewer — for the year and the zone. */
inline const char* readFixedNumber(const char* p, int count, int& out) {
  int value = 0;
  for (int i = 0; i < count; ++i) {
    if (!isDigit(p[i])) return nullptr;
    value = value * 10 + (p[i] - '0');
  }
  out = value;
  return p + count;
}

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/** Case-insensitive match of a 3-letter month abbreviation; 0 when unknown. */
inline int monthFromName(const char* p) {
  if (p[0] == '\0' || p[1] == '\0' || p[2] == '\0') return 0;
  static const char* const NAMES = "janfebmaraprmayjunjulaugsepoctnovdec";
  const char a = lowerAscii(p[0]);
  const char b = lowerAscii(p[1]);
  const char c = lowerAscii(p[2]);
  for (int m = 0; m < 12; ++m) {
    if (NAMES[m * 3] == a && NAMES[m * 3 + 1] == b && NAMES[m * 3 + 2] == c) return m + 1;
  }
  return 0;
}

/**
 * The zone suffix. Only the forms the spec names are accepted: numeric offsets
 * and the three spellings of zero. The obsolete single-letter military zones
 * are deliberately rejected — RFC-1123 calls them unreliable, and getting one
 * wrong moves the clock by hours.
 */
inline bool parseZoneOffsetSecs(const char* p, int32_t& outSecs) {
  if (*p == '+' || *p == '-') {
    const bool negative = *p == '-';
    int hhmm = 0;
    const char* end = readFixedNumber(p + 1, 4, hhmm);
    if (end == nullptr) return false;
    if (isDigit(*end)) return false;  // a fifth digit is not an offset
    const int hours = hhmm / 100;
    const int minutes = hhmm % 100;
    if (hours > 14 || minutes > 59) return false;
    const int32_t secs = static_cast<int32_t>(hours) * 3600 + static_cast<int32_t>(minutes) * 60;
    outSecs = negative ? -secs : secs;
    return true;
  }
  // Named zones. The comparison walks in step so it never reads past the
  // terminator: a '\0' in the input can never equal a lowercase letter, so the
  // loop stops there and the delimiter test below sees the '\0' itself.
  const char* const NAMES[] = {"gmt", "utc", "ut", "z"};
  for (const char* name : NAMES) {
    size_t i = 0;
    while (name[i] != '\0' && lowerAscii(p[i]) == name[i]) ++i;
    if (name[i] != '\0') continue;
    const char after = p[i];
    if (after == '\0' || after == ' ' || after == '\t' || after == '\r' || after == '\n') {
      outSecs = 0;
      return true;
    }
  }
  return false;
}

/** Calendar length of a month, so 31 February is rejected rather than folded. */
inline int daysInMonth(int year, int month) {
  static const uint8_t LENGTHS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
  }
  return LENGTHS[month - 1];
}

}  // namespace detail

/**
 * Parse an RFC-822/1123 date-time — the shape of both an HTTP `Date:` response
 * header and an RSS `<pubDate>` — into epoch seconds.
 *
 *   [Www, ]D[D] Mon YYYY HH:MM[:SS] (GMT|UT|UTC|Z|+hhmm|-hhmm)
 *
 * Stricter than lib/RssParser's rssShortDate(), which only needs the day and
 * month for a display label: this answer becomes the device's clock, so every
 * field is range-checked, the zone is mandatory, and the result must pass the
 * plausibility predicate. Two-digit years (the obsolete RFC-822/850 form) are
 * rejected rather than guessed at.
 *
 * Returns false — leaving `outEpoch` untouched — on anything that does not
 * parse cleanly.
 */
inline bool parseRfc822Epoch(const char* text, int64_t& outEpoch) {
  if (text == nullptr) return false;
  const char* p = detail::skipSpaces(text);

  // "Sun, 06 Nov 1994 ...": the day name is optional, so start after the comma
  // when there is one (same shape as rssShortDate). A comma anywhere else is
  // not valid RFC-822, and skipping to it would just fail the fields below.
  for (const char* q = p; *q != '\0' && q - p < 10; ++q) {
    if (*q == ',') {
      p = detail::skipSpaces(q + 1);
      break;
    }
  }

  int day = 0;
  p = detail::readNumber(p, 2, day);
  if (p == nullptr || day < 1 || day > 31) return false;

  p = detail::skipSpaces(p);
  const int month = detail::monthFromName(p);
  if (month == 0) return false;
  p = detail::skipSpaces(p + 3);

  int year = 0;
  p = detail::readFixedNumber(p, 4, year);
  if (p == nullptr || year < 1970) return false;
  // A 5-digit year is not a year; catch it before the spaces are skipped.
  if (detail::isDigit(*p)) return false;

  p = detail::skipSpaces(p);
  int hour = 0;
  p = detail::readFixedNumber(p, 2, hour);
  if (p == nullptr || *p != ':') return false;
  int minute = 0;
  p = detail::readFixedNumber(p + 1, 2, minute);
  if (p == nullptr) return false;
  int second = 0;
  if (*p == ':') {
    p = detail::readFixedNumber(p + 1, 2, second);
    if (p == nullptr) return false;
  }
  // 60 is a leap second, which the counter below folds into the next minute.
  if (hour > 23 || minute > 59 || second > 60) return false;

  p = detail::skipSpaces(p);
  int32_t zoneSecs = 0;
  if (!detail::parseZoneOffsetSecs(p, zoneSecs)) return false;

  // daysFromCivil folds an impossible day silently (31 Feb comes out as 3 Mar),
  // so the day is checked against the month before it is converted.
  if (day > detail::daysInMonth(year, month)) return false;

  const int64_t days = daysFromCivil(year, month, day);
  const int64_t epoch = days * 86400 + hour * 3600 + minute * 60 + second - zoneSecs;
  if (!epochIsValid(epoch)) return false;
  outEpoch = epoch;
  return true;
}

}  // namespace softclock

#endif  // CROSSPOINT_SOFT_CLOCK
