#include "ClockDst.h"

#ifdef CROSSPOINT_CLOCK_DST

#include <Arduino.h>
#include <HalClock.h>

#include "CivilDate.h"

namespace {

constexpr uint8_t MAX_OFFSET_Q = 104;     // UTC+14:00, the largest offset the clock accepts
constexpr uint8_t DST_STEP_Q = 4;         // every rule below is a one-hour shift
constexpr uint32_t RECOMPUTE_MS = 60000;  // the clock itself only shows whole minutes
constexpr int32_t MINUTES_PER_DAY = 1440;

// A "nth Sunday of month, at minute-of-day" transition pair. `nth == 0` means
// the LAST Sunday of the month.
//
// The minute fields are stated in the rule's own reference frame, which is what
// makes the comparison below a single signed integer test:
//   - EU law states both instants in UTC, so utcReference skips the local
//     conversion entirely.
//   - US and AU law states the spring instant in local standard time and the
//     autumn instant in local DST time. Both are written here in local STANDARD
//     time, so the autumn minute is one hour EARLIER than the statute's wording
//     (US 02:00 DST -> 01:00 standard, AU 03:00 DST -> 02:00 standard).
struct DstRule {
  uint8_t startMonth;
  uint8_t startNth;
  uint16_t startMinute;
  uint8_t endMonth;
  uint8_t endNth;
  uint16_t endMinute;
  bool utcReference;
};

// Indexed by CrossPointSettings::CLOCK_DST_RULE minus one.
constexpr DstRule RULES[] = {
    // US: 2nd Sunday of March 02:00 local standard -> 1st Sunday of November
    // 02:00 local DST. Northern hemisphere, so the window sits inside the year.
    {3, 2, 120, 11, 1, 60, false},
    // EU: last Sunday of March 01:00 UTC -> last Sunday of October 01:00 UTC.
    {3, 0, 60, 10, 0, 60, true},
    // AU (southeast: NSW/VIC/TAS/ACT/SA): 1st Sunday of October 02:00 local
    // standard -> 1st Sunday of April 03:00 local DST. SOUTHERN hemisphere, so
    // startMonth > endMonth and the window wraps the new year — handled by the
    // start/end ordering test in inDstWindow() rather than a separate flag.
    {10, 1, 120, 4, 1, 120, false},
};

// Day-of-month of the nth Sunday of a month, or of the last one when nth == 0.
// 1970-01-01 (day 0) was a Thursday, so weekday = (days + 4) % 7 with 0 = Sunday.
uint8_t sundayOfMonth(const uint16_t year, const uint8_t month, const uint8_t nth) {
  const uint32_t first = civil::daysFromCivil(year, month, 1);
  if (nth == 0) {
    const uint32_t length = civil::daysFromCivil(year, month + 1u, 1) - first;
    return static_cast<uint8_t>(length - (first + length + 3) % 7);
  }
  return static_cast<uint8_t>(1 + (7 - (first + 4) % 7) % 7 + 7 * (nth - 1));
}

// Minutes from the start of `year` to a date/time inside it.
int32_t minutesIntoYear(const uint16_t year, const uint8_t month, const uint8_t day, const int32_t minuteOfDay) {
  const int32_t days = static_cast<int32_t>(civil::daysFromCivil(year, month, day)) -
                       static_cast<int32_t>(civil::daysFromCivil(year, 1, 1));
  return days * MINUTES_PER_DAY + minuteOfDay;
}

// Everything is converted to minutes from the start of the CURRENT UTC YEAR, in
// the rule's reference frame, and compared as signed integers. `now` may land
// slightly outside [0, year length) when the local date and the UTC date differ
// across New Year; that is harmless, because no transition is anywhere near
// January 1 and the wrap test below still resolves those hours correctly.
bool inDstWindow(const DstRule& rule, const Rtc::DateTime& dt, const int32_t offsetMinutes) {
  const int32_t now = minutesIntoYear(dt.year, dt.month, dt.day, dt.hour * 60 + dt.minute) + offsetMinutes;
  const int32_t start = minutesIntoYear(dt.year, rule.startMonth,
                                        sundayOfMonth(dt.year, rule.startMonth, rule.startNth), rule.startMinute);
  const int32_t end =
      minutesIntoYear(dt.year, rule.endMonth, sundayOfMonth(dt.year, rule.endMonth, rule.endNth), rule.endMinute);
  // Southern-hemisphere rules run Oct -> Apr, so the window is the complement.
  return start < end ? (now >= start && now < end) : (now >= start || now < end);
}

uint8_t computeEffective(const uint8_t baseQ, const uint8_t rule) {
  Rtc::DateTime dt;
  if (!halClock.getDateTime(dt) || dt.year < 2020) return baseQ;

  const DstRule& r = RULES[rule - 1];
  const int32_t offsetMinutes = r.utcReference ? 0 : (static_cast<int32_t>(baseQ) - 48) * 15;
  if (!inDstWindow(r, dt, offsetMinutes)) return baseQ;

  const uint16_t shifted = static_cast<uint16_t>(baseQ) + DST_STEP_Q;
  return shifted > MAX_OFFSET_Q ? MAX_OFFSET_Q : static_cast<uint8_t>(shifted);
}

}  // namespace

uint8_t effectiveUtcOffsetQ(const uint8_t baseQ, const uint8_t rule) {
  // Rule 0 (and any out-of-range persisted value) is the passthrough: no RTC
  // read, and the cache is left alone so re-enabling a rule recomputes at once.
  if (rule == 0 || rule > sizeof(RULES) / sizeof(RULES[0])) return baseQ;

  // Accepted limitation: the switch can lag a real DST transition by up to
  // RECOMPUTE_MS (60 s) while a stale entry is still live, and the minute that
  // is eventually drawn comes from HalClock's own separate 10 s poll cache.
  // Both are bounded and purely cosmetic on a display that only shows whole
  // minutes and refreshes on demand, so neither is chased.
  //
  // 0xFF is never a valid rule, so the first call always misses.
  static uint8_t cachedRule = 0xFF;
  static uint8_t cachedBase = 0;
  static uint8_t cachedResult = 0;
  static uint32_t cachedAtMs = 0;

  const uint32_t now = millis();
  if (rule == cachedRule && baseQ == cachedBase && (now - cachedAtMs) < RECOMPUTE_MS) return cachedResult;

  cachedRule = rule;
  cachedBase = baseQ;
  cachedResult = computeEffective(baseQ, rule);
  cachedAtMs = now;
  return cachedResult;
}

#endif  // CROSSPOINT_CLOCK_DST
