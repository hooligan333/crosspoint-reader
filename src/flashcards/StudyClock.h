#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <FsrsSched.h>

#include <cstdint>

/**
 * The one clock reading a study session needs (FLASHCARD_SPEC.md §5).
 *
 * `fsrs::Now` is UTC unix seconds plus the local day number the 04:00 rollover
 * puts them in, and every scheduling decision — what is due, which day a review
 * lands on, whether a re-review is same-day — is made from that pair.
 *
 * Local means the settings UTC offset, plus the automatic daylight-saving
 * adjustment when CROSSPOINT_CLOCK_DST is also on (both are combo-env flags, so
 * on the builds that carry flashcards they are both present).
 *
 * Without an RTC there is no honest day number, so `isAvailable()` is false and
 * the study screen must offer Cram only: cram touches no scheduling state and
 * therefore needs no clock.
 */
namespace flashcards {

/**
 * Largest value `CrossPointSettings::clockUtcOffsetQ` may hold: quarter-hours
 * biased by 48, so 104 is UTC+14 (the real maximum, Line Islands).
 */
constexpr uint8_t CLOCK_UTC_OFFSET_Q_MAX = 104;

/**
 * The offset composition, as a pure function so it can be host-tested without
 * an RTC, a settings blob or a DST rule: a biased quarter-hour offset (48 =
 * UTC+0) becomes seconds east of UTC, with a corrupt persisted value clamped
 * down to UTC+14 rather than turned into a wild day number.
 *
 * DST enters through `offsetQ`, not through this function: the caller passes
 * the STANDARD offset already run through `effectiveUtcOffsetQ()` when
 * CROSSPOINT_CLOCK_DST is on. That keeps the one part with a real arithmetic
 * trap in it (the −48 bias and the ×900) free of every dependency, and leaves
 * the RTC/settings seam in localUtcOffsetSecs() as thin as it can be.
 */
constexpr int32_t utcOffsetSecsFromQuarters(uint8_t offsetQ) {
  const uint8_t clamped = offsetQ > CLOCK_UTC_OFFSET_Q_MAX ? CLOCK_UTC_OFFSET_Q_MAX : offsetQ;
  return (static_cast<int32_t>(clamped) - 48) * 15 * 60;
}

/** True when the RTC is present AND holds a plausible date. */
bool clockIsAvailable();

/** Fills `out` with the current moment. False when the clock is unusable. */
bool studyNow(fsrs::Now& out);

/** Local offset from UTC in seconds, DST rule applied when that flag is on. */
int32_t localUtcOffsetSecs();

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
