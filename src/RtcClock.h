#pragma once

// One reading of the RTC as epoch seconds, shared by the two features that want
// raw UTC out of it: the usage log's row timestamps and the flashcard study
// session's day number.
//
// It lives next to CivilDate.h and follows the same rules: header-only inline,
// integer-only (no newlib timezone machinery), and compiled only for the builds
// that carry one of its two callers. Both had a verbatim copy of this function
// before; the copies are the reason it is here.
//
// The RTC keeps UTC. Local time is the caller's business — the usage log
// records UTC deliberately, and the study clock applies the settings offset (and
// the DST rule) itself.

#if defined(CROSSPOINT_USAGE_LOG) || defined(CROSSPOINT_FLASHCARDS)

#include <HalClock.h>

#include <cstdint>

#include "CivilDate.h"

namespace rtcclock {

constexpr uint32_t SECONDS_PER_DAY = 86400;
// An RTC that has never been set reads as its power-on default, which is well
// before this. Both callers treat "implausible" and "absent" the same way.
constexpr uint16_t EARLIEST_PLAUSIBLE_YEAR = 2020;

/** Epoch seconds off the RTC, or 0 when it is missing or holds no plausible date. */
inline uint32_t rtcUnixSecs() {
  Rtc::DateTime dt;
  if (!halClock.getDateTime(dt) || dt.year < EARLIEST_PLAUSIBLE_YEAR) return 0;
  return civil::daysFromCivil(dt.year, dt.month, dt.day) * SECONDS_PER_DAY + dt.hour * 3600u + dt.minute * 60u +
         dt.second;
}

}  // namespace rtcclock

#endif  // CROSSPOINT_USAGE_LOG || CROSSPOINT_FLASHCARDS
