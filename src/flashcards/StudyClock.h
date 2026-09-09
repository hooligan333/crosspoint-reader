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
 * Local means the clock's configured timezone (upstream #3562: a POSIX TZ
 * rule, daylight saving included), i.e. the wall time the status-bar clock
 * shows.
 *
 * Without an RTC there is no honest day number, so `isAvailable()` is false and
 * the study screen must offer Cram only: cram touches no scheduling state and
 * therefore needs no clock.
 */
namespace flashcards {

/**
 * Largest value the retired `CrossPointSettings::clockUtcOffsetQ` may hold:
 * quarter-hours biased by 48, so 104 is UTC+14 (the real maximum, Line Islands).
 */
constexpr uint8_t CLOCK_UTC_OFFSET_Q_MAX = 104;

/**
 * Biased quarter-hour offset (48 = UTC+0) to seconds east of UTC, with a
 * corrupt value clamped down to UTC+14 rather than turned into a wild day
 * number. Kept as a pure, host-tested helper for the quarter-hour encoding;
 * since the r5 rebase the device derives its offset from the timezone rule
 * instead (localUtcOffsetSecs()).
 */
constexpr int32_t utcOffsetSecsFromQuarters(uint8_t offsetQ) {
  const uint8_t clamped = offsetQ > CLOCK_UTC_OFFSET_Q_MAX ? CLOCK_UTC_OFFSET_Q_MAX : offsetQ;
  return (static_cast<int32_t>(clamped) - 48) * 15 * 60;
}

/** True when the RTC is present AND holds a plausible date. */
bool clockIsAvailable();

/** Fills `out` with the current moment. False when the clock is unusable. */
bool studyNow(fsrs::Now& out);

/** Local offset from UTC in seconds right now, per the clock timezone (DST included). */
int32_t localUtcOffsetSecs();

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
