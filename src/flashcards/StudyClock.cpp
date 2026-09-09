#include "StudyClock.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <HalClock.h>

#include "ClockDst.h"
#include "CrossPointSettings.h"
#include "RtcClock.h"

namespace flashcards {

int32_t localUtcOffsetSecs() {
  uint8_t offsetQ = SETTINGS.clockUtcOffsetQ;
#ifdef CROSSPOINT_CLOCK_DST
  // With a DST rule set, the stored offset is STANDARD time and this adds the
  // hour inside the rule's window — the same call the status-bar clock makes.
  offsetQ = effectiveUtcOffsetQ(offsetQ, SETTINGS.clockDstRule);
#endif
  // The clamp and the quarter-hour arithmetic are in the header, pure and
  // host-tested; this function is only the settings/DST seam.
  return utcOffsetSecsFromQuarters(offsetQ);
}

bool clockIsAvailable() { return halClock.isAvailable() && rtcclock::rtcUnixSecs() != 0; }

bool studyNow(fsrs::Now& out) {
  const uint32_t unixSecs = rtcclock::rtcUnixSecs();
  if (unixSecs == 0) return false;
  out.unixSecs = unixSecs;
  // dayNumber() applies the 04:00 rollover and clamps into [0, 65535] — a
  // corrupt clock must never alias onto a valid study day (spec §4).
  out.dayNumber = fsrs::dayNumber(unixSecs, localUtcOffsetSecs());
  return true;
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
