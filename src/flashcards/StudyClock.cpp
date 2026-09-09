#include "StudyClock.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <HalClock.h>

#include <ctime>

#include "CivilDate.h"
#include "RtcClock.h"

namespace flashcards {

namespace {
// Seconds east of UTC in force at `unixSecs`. The clock's zone is a POSIX TZ
// rule that HalClock::setTimezone() installs process-wide (upstream #3562), so
// newlib's localtime_r() yields the wall time for this instant with the zone's
// daylight-saving rule already applied — the same wall time the status-bar
// clock shows. Reading the local fields back through the UTC civil-date maths
// turns that into an offset without needing tm_gmtoff or timegm().
int32_t offsetAt(const uint32_t unixSecs) {
  const time_t t = static_cast<time_t>(unixSecs);
  struct tm local;
  if (localtime_r(&t, &local) == nullptr || local.tm_year + 1900 < 1970) return 0;
  const int64_t localAsUtc = static_cast<int64_t>(civil::daysFromCivil(static_cast<uint32_t>(local.tm_year + 1900),
                                                                       static_cast<uint32_t>(local.tm_mon + 1),
                                                                       static_cast<uint32_t>(local.tm_mday))) *
                                 rtcclock::SECONDS_PER_DAY +
                             local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
  const int64_t offset = localAsUtc - static_cast<int64_t>(unixSecs);
  // Real zones span UTC-12..UTC+14; anything else is a broken TZ string.
  return (offset < -12 * 3600 || offset > 14 * 3600) ? 0 : static_cast<int32_t>(offset);
}
}  // namespace

int32_t localUtcOffsetSecs() {
  const uint32_t unixSecs = rtcclock::rtcUnixSecs();
  return unixSecs == 0 ? 0 : offsetAt(unixSecs);
}

bool clockIsAvailable() { return halClock.isAvailable() && rtcclock::rtcUnixSecs() != 0; }

bool studyNow(fsrs::Now& out) {
  const uint32_t unixSecs = rtcclock::rtcUnixSecs();
  if (unixSecs == 0) return false;
  out.unixSecs = unixSecs;
  // dayNumber() applies the 04:00 rollover and clamps into [0, 65535] — a
  // corrupt clock must never alias onto a valid study day (spec §4).
  out.dayNumber = fsrs::dayNumber(unixSecs, offsetAt(unixSecs));
  return true;
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
