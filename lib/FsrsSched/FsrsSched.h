#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <cstddef>
#include <cstdint>

/**
 * FSRS-6 scheduler with an Anki-style learning-step wrapper.
 *
 * Ported from py-fsrs 6.3.2 (`fsrs/scheduler.py`, package `fsrs==6.3.2`); the
 * formulas, clamps and the learning/relearning step machine follow that file
 * statement for statement. See README.md for the deviations and their reasons.
 *
 * Pure C++: no Arduino / SDK / HAL includes, no heap, no globals, no I/O. Every
 * input is passed in, every output is returned by value, so the whole library
 * is host-testable (see `host_tests/fsrs/`).
 *
 * All arithmetic is single precision (`expf`/`powf`/`rintf`) because the target
 * FPU is single precision; py-fsrs computes in double, so ports agree to about
 * 1e-6 relative, not bit-for-bit.
 */
namespace fsrs {

/** Answer buttons, in Anki order. py-fsrs `Rating` == (uint8_t)Grade + 1. */
enum class Grade : uint8_t { Again = 0, Hard = 1, Good = 2, Easy = 3 };

/** Number of answer buttons, and the length of the per-grade arrays below. */
constexpr uint8_t GRADE_COUNT = 4;

/** Card lifecycle phase. The values are the CPST on-disk `state` codes (spec §3). */
enum class CardPhase : uint8_t { New = 0, Learning = 1, Review = 2, Relearning = 3 };

/** `CardState::flags` bits. */
constexpr uint8_t FLAG_SUSPENDED = 0x01;

/** Number of FSRS-6 model weights. */
constexpr uint8_t PARAM_COUNT = 21;

/** Maximum learning / relearning steps a `Params` can hold. */
constexpr uint8_t MAX_STEPS = 4;

/** Local rollover: a new study day starts at 04:00 local time (spec §0). */
constexpr uint32_t ROLLOVER_SECONDS = 4u * 3600u;

/**
 * Per-card scheduling state — byte-identical to the 20-byte payload of a CPST
 * record (spec §3). The record's leading u64 key is *not* part of this struct;
 * the caller keeps it (and passes it to `gradeCard` for the fuzz seed).
 *
 * `due` means local day number when `state == Review`, and unix seconds (UTC)
 * when the card is in an intraday phase (`Learning` / `Relearning`). When
 * `state == New` the field is ignored entirely (spec §3): the scheduler never
 * reads it and overwrites it on the first answer.
 *
 * `lastReviewDay` is a local day number, with **0 as the "never reviewed"
 * sentinel** — not as day 0. A non-New card carrying 0 (a torn record, or a
 * record seeded without a review history) is treated as answered today rather
 * than as ~20000 days overdue, so it earns no overdue stability credit.
 */
struct CardState {
  float stability;         // FSRS S; >= 0.001 once the card has been answered
  float difficulty;        // FSRS D in [1, 10]
  uint32_t due;            // day number (Review) or unix seconds (Learning/Relearning)
  uint16_t lastReviewDay;  // local day number, 0 = never
  CardPhase state;         // stored as one byte, see CardPhase
  uint8_t step;            // index into the learning / relearning step table
  uint16_t reps;           // total answers, saturating
  uint8_t lapses;          // Review-state Again count, saturating
  uint8_t flags;           // FLAG_* bits
};

static_assert(sizeof(CardState) == 20, "CardState must match the 20-byte CPST record payload");
static_assert(offsetof(CardState, stability) == 0, "CPST layout");
static_assert(offsetof(CardState, difficulty) == 4, "CPST layout");
static_assert(offsetof(CardState, due) == 8, "CPST layout");
static_assert(offsetof(CardState, lastReviewDay) == 12, "CPST layout");
static_assert(offsetof(CardState, state) == 14, "CPST layout");
static_assert(offsetof(CardState, step) == 15, "CPST layout");
static_assert(offsetof(CardState, reps) == 16, "CPST layout");
static_assert(offsetof(CardState, lapses) == 18, "CPST layout");
static_assert(offsetof(CardState, flags) == 19, "CPST layout");

/**
 * Everything tunable about the scheduler. Nothing here is read from a global:
 * the caller builds one of these (usually `defaultParams()`, optionally with
 * `w[]` overridden from the deck header) and passes it to every call.
 *
 * Nothing in here is validated on use: a caller that fills `w[]` from a deck
 * file MUST run `validateParams()` first and refuse the deck when it fails.
 * See spec §2.1/§4 — validation is the caller's job, not the scheduler's.
 */
struct Params {
  float w[PARAM_COUNT];               // FSRS-6 model weights
  float desiredRetention;             // target recall probability, default 0.90
  uint32_t learningSteps[MAX_STEPS];  // seconds, strictly ascending
  uint32_t relearningSteps[MAX_STEPS];
  uint32_t maximumIntervalDays;        // review interval ceiling
  uint16_t minGraduatingIntervalDays;  // floor on a Good/Hard graduation, default 0 (off)
  uint16_t minEasyIntervalDays;        // floor on an Easy graduation, default 0 (off)
  uint8_t learningStepCount;           // <= MAX_STEPS; 0 disables the learning phase
  uint8_t relearningStepCount;         // <= MAX_STEPS; 0 keeps lapses in Review
  bool enableFuzz;
};

/**
 * The spec §0.4 defaults: 0.90 retention, 1m/10m steps, 10m relearn, 36500 d
 * cap, fuzz on, and **both graduation floors 0** — graduation intervals come
 * from FSRS stability, which is what Anki does under FSRS. The floors stay in
 * `Params` for a caller that wants Anki's SM-2-era behaviour back; they are
 * applied **before** fuzz, so a nonzero floor can still be undercut by the fuzz
 * band (see README).
 */
Params defaultParams();

/**
 * True when `params` is inside the bounds the scheduler is defined for.
 *
 * `w[]` is checked against py-fsrs 6.3.2's `LOWER_BOUNDS_PARAMETERS` /
 * `UPPER_BOUNDS_PARAMETERS` (the same 21 pairs its `Scheduler._validate_
 * parameters` raises on), plus sane bounds on `desiredRetention`, the step
 * tables and `maximumIntervalDays`. NaN fails every check.
 *
 * Deck-supplied weights (CPDK header flags bit 0) MUST pass this before the
 * deck is opened — spec §2.1 puts it on the refuse list. The library itself
 * never calls it: it is deliberately the caller's job, because the scheduler
 * has no way to report a refusal. Out-of-bounds weights are not merely
 * inaccurate, they are silently degenerate — `w[20] == 0` makes the decay
 * exponent `-0.0`, the curve factor infinite and every interval 1 day forever.
 */
bool validateParams(const Params& params);

/** The moment a review happens: wall clock plus the local day it falls in. */
struct Now {
  uint32_t unixSecs;   // UTC
  uint16_t dayNumber;  // local day number, from dayNumber() below
};

/**
 * Uniform draw in [0, 1) for one fuzz decision, a pure function of `seed` so
 * that the same (card key, reps) always produces the same interval. Tests
 * inject their own; `ctx` is opaque to the library.
 */
using FuzzDrawFn = float (*)(uint64_t seed, void* ctx);

/** splitmix64-based default draw. */
float defaultFuzzDraw(uint64_t seed, void* ctx);

struct FuzzRng {
  FuzzDrawFn draw = &defaultFuzzDraw;
  void* ctx = nullptr;
};

/**
 * Apply `grade` to `card` and return the updated state.
 *
 * `cardKey` is the CPST record key; together with the card's pre-answer `reps`
 * it seeds the fuzz draw, so replaying a session reproduces its intervals.
 * A single draw is reused across the four candidate grades (as Anki does), so
 * the fuzzed intervals stay ordered relative to one another.
 *
 * Deviation from the spec §4 sketch: the sketch's signature omits `cardKey` and
 * the RNG, which the "deterministic fuzz seeded from (key, reps)" and "RNG
 * injectable for tests" requirements both need. The sketch says "roughly".
 */
CardState gradeCard(const Params& params, const CardState& card, Grade grade, Now now, uint64_t cardKey,
                    FuzzRng rng = FuzzRng());

/**
 * Next interval per grade, for the four answer buttons. `intraday[g]` selects
 * the unit: `next[g]` is seconds when true, whole days when false.
 *
 * Fuzz-free by choice: the preview must be stable across redraws of the same
 * card, and a fuzz draw would make the number jump between renders of the same
 * screen. The answer therefore differs from the graded interval by at most the
 * fuzz band (±5–15%). Anki behaves the same way.
 *
 * The `hard < good < easy` rule is applied here too, so a previewed Good or
 * Easy interval can sit one day above the raw FSRS number for the card's new
 * stability. Worth remembering when a preview looks off by a day against a
 * hand-computed FSRS value.
 */
struct Preview {
  uint32_t next[GRADE_COUNT];
  bool intraday[GRADE_COUNT];
};
Preview previewIntervals(const Params& params, const CardState& card, Now now);

/**
 * Local day number for a moment, with the 04:00 rollover:
 * `floor((unix + utcOffset - 4h) / 86400)`, **clamped** into [0, 65535].
 *
 * Spec §4 pins the clamp rather than a wrap (`deck-server/seed_from_revlog.py`
 * is the reference implementation): a corrupt RTC must not alias onto a valid
 * day. Neither end is reachable from a plausible clock — the largest day a u32
 * unix time can produce is 49710 (2106-02-07), and the low end only catches a
 * timestamp before the first rollover of 1970-01-01.
 */
uint16_t dayNumber(uint32_t unixSecs, int32_t utcOffsetSecs);

/** Fuzz seed for a card about to be answered: mixes the record key with `reps`. */
uint64_t fuzzSeed(uint64_t cardKey, uint16_t reps);

/**
 * Internals exposed for the host tests only. Not part of the device-facing API.
 */
namespace detail {
/** Anki fuzz band bounds for a review interval, per py-fsrs `_get_fuzz_range`. */
void fuzzBounds(const Params& params, uint32_t intervalDays, uint32_t& minDays, uint32_t& maxDays);
/** py-fsrs `_get_fuzzed_interval` with the random draw supplied as `u01`. */
uint32_t fuzzInterval(const Params& params, uint32_t intervalDays, float u01);
}  // namespace detail

}  // namespace fsrs

#endif  // CROSSPOINT_FLASHCARDS
