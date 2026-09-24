#pragma once

#include <cstdint>

// Howard Hinnant's days_from_civil / civil_from_days, narrowed to years >= 1970
// so the negative-era branches drop out. Integer-only on purpose: mktime() and
// localtime_r() drag newlib's timezone machinery in for ~2 KB of IROM, which
// this firmware does not have to spare, and both callers want raw RTC time
// anyway.
//
// Header-only inline: a translation unit that includes it without calling it
// emits nothing, so the flag-gated users below cost the flags-off builds zero.

namespace civil {

inline uint32_t daysFromCivil(uint32_t year, const uint32_t month, const uint32_t day) {
  year -= month <= 2;
  const uint32_t era = year / 400;
  const uint32_t yoe = year - era * 400;
  const uint32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

inline void civilFromDays(uint32_t days, uint16_t& year, uint8_t& month, uint8_t& day) {
  days += 719468;
  const uint32_t era = days / 146097;
  const uint32_t doe = days - era * 146097;
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  day = static_cast<uint8_t>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<uint8_t>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<uint16_t>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

}  // namespace civil
