#include "FsrsSched.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <math.h>

namespace fsrs {
namespace {

// --- py-fsrs 6.3.2 module constants -----------------------------------------

constexpr float STABILITY_MIN = 0.001f;
constexpr float STABILITY_MAX = 1.0e9f;  // not py-fsrs': see clampStability
constexpr float DIFFICULTY_MIN = 1.0f;
constexpr float DIFFICULTY_MAX = 10.0f;

// py-fsrs DEFAULT_PARAMETERS (w[20] is FSRS_DEFAULT_DECAY = 0.1542).
constexpr float DEFAULT_W[PARAM_COUNT] = {0.212f,  1.2931f, 2.3065f, 8.2956f, 6.4133f, 0.8334f, 3.0194f,
                                          0.001f,  1.8722f, 0.1666f, 0.796f,  1.4835f, 0.0614f, 0.2629f,
                                          1.6483f, 0.6014f, 1.8729f, 0.5425f, 0.0912f, 0.0658f, 0.1542f};

// py-fsrs FUZZ_RANGES. The last band is open-ended (math.inf there).
struct FuzzBand {
  float start;
  float end;
  float factor;
};
constexpr FuzzBand FUZZ_BANDS[] = {{2.5f, 7.0f, 0.15f}, {7.0f, 20.0f, 0.1f}, {20.0f, INFINITY, 0.05f}};

// py-fsrs LOWER_BOUNDS_PARAMETERS / UPPER_BOUNDS_PARAMETERS, the pairs its
// `Scheduler._validate_parameters` raises on. w[0..3] share INITIAL_STABILITY_MAX.
constexpr float PARAM_LOWER[PARAM_COUNT] = {0.001f, 0.001f, 0.001f, 0.001f, 1.0f,   0.001f, 0.001f,
                                            0.001f, 0.0f,   0.0f,   0.001f, 0.001f, 0.001f, 0.001f,
                                            0.0f,   0.0f,   1.0f,   0.0f,   0.0f,   0.0f,   0.1f};
constexpr float PARAM_UPPER[PARAM_COUNT] = {100.0f, 100.0f, 100.0f, 100.0f, 10.0f, 4.0f, 4.0f, 0.75f, 4.5f, 0.8f, 3.5f,
                                            5.0f,   0.25f,  0.9f,   4.0f,   1.0f,  6.0f, 2.0f, 2.0f,  0.8f, 0.8f};

// Bounds py-fsrs has no opinion on, chosen so that nothing downstream can turn
// degenerate. Retention matches Anki's own 0.70-0.99 slider; a step must be a
// positive number of seconds no longer than a month; the ceiling is spec §4's.
constexpr float RETENTION_MIN = 0.70f;
constexpr float RETENTION_MAX = 0.99f;
constexpr uint32_t STEP_SECONDS_MAX = 31u * 86400u;
constexpr uint32_t MAXIMUM_INTERVAL_DAYS_CAP = 36500u;

// py-fsrs skips fuzz when the interval is under 2.5 days. It measures whole
// days (timedelta.days), so the first fuzzed interval is 3.
constexpr uint32_t FUZZ_MIN_INTERVAL_DAYS = 3u;

constexpr uint32_t SECONDS_PER_DAY = 86400u;

// --- small numeric helpers ---------------------------------------------------

/**
 * py-fsrs rounds with Python's round(): nearest, ties away from zero avoided in
 * favour of ties-to-even. rintf() under the default FE_TONEAREST does the same,
 * so exact-day comparisons against generated vectors line up.
 */
inline float roundTiesToEven(float value) { return rintf(value); }

// Both clamps are written as failed lower bounds so that a NaN lands on the
// minimum instead of passing through, and stability additionally gets an upper
// bound so that an infinity cannot. py-fsrs has neither -- it never produces a
// non-finite value -- but spec §3 accepts torn state records, so one can arrive
// from the file, and a NaN written back would poison the card forever. For
// every value a real review can produce the behaviour is py-fsrs' exactly:
// STABILITY_MAX is four orders of magnitude past the 36500-day interval ceiling.
inline float clampStability(float stability) {
  if (!(stability >= STABILITY_MIN)) {
    return STABILITY_MIN;
  }
  return stability > STABILITY_MAX ? STABILITY_MAX : stability;
}

inline float clampDifficulty(float difficulty) {
  if (!(difficulty >= DIFFICULTY_MIN)) {
    return DIFFICULTY_MIN;
  }
  if (difficulty > DIFFICULTY_MAX) {
    return DIFFICULTY_MAX;
  }
  return difficulty;
}

/**
 * The state byte as a phase the switches below actually handle. A torn record
 * can hold anything; without this it would fall through every exhaustive switch
 * and leave the outcome uninitialised.
 */
inline CardPhase phaseOf(CardPhase state) {
  switch (state) {
    case CardPhase::New:
    case CardPhase::Learning:
    case CardPhase::Review:
    case CardPhase::Relearning:
      return state;
  }
  return CardPhase::New;
}

/** py-fsrs indexes weights by `rating - 1`; our Grade already is that index. */
inline float gradeIndex(Grade grade) { return static_cast<float>(static_cast<uint8_t>(grade)); }

/** py-fsrs writes `rating - 3`, i.e. 0 for Good. */
inline float gradeOffsetFromGood(Grade grade) { return gradeIndex(grade) - 2.0f; }

/**
 * The forgetting curve's shape, derived from w[20] once per call:
 * py-fsrs `_DECAY` / `_FACTOR`.
 */
struct Curve {
  float decay;
  float factor;
};

inline Curve curveOf(const Params& params) {
  Curve curve;
  curve.decay = -params.w[20];
  curve.factor = powf(0.9f, 1.0f / curve.decay) - 1.0f;
  return curve;
}

// --- FSRS-6 memory state (py-fsrs Scheduler private methods) -----------------

inline float initialStability(const Params& params, Grade grade) {
  return clampStability(params.w[static_cast<uint8_t>(grade)]);
}

/** py-fsrs `_initial_difficulty`; `clamp=False` is only ever used for Easy. */
inline float initialDifficulty(const Params& params, Grade grade) {
  return clampDifficulty(params.w[4] - expf(params.w[5] * gradeIndex(grade)) + 1.0f);
}

inline float retrievability(const Curve& curve, uint32_t elapsedDays, float stability) {
  return powf(1.0f + curve.factor * static_cast<float>(elapsedDays) / stability, curve.decay);
}

inline uint32_t nextIntervalDays(const Params& params, const Curve& curve, float stability) {
  const float raw = (stability / curve.factor) * (powf(params.desiredRetention, 1.0f / curve.decay) - 1.0f);
  float days = roundTiesToEven(raw);
  // Written as a failed lower bound so a NaN (impossible with a clamped
  // stability, but cheap to be sure of) lands on 1 rather than on garbage.
  if (!(days >= 1.0f)) {
    days = 1.0f;
  }
  // Clamped twice: in float first, because that is the only form that survives
  // an infinity, and again in u32, because `(float)maximumIntervalDays` rounds
  // up for values above 2^24 and could otherwise let the result out by one.
  const float maximum = static_cast<float>(params.maximumIntervalDays);
  if (days > maximum) {
    days = maximum;
  }
  uint32_t wholeDays = static_cast<uint32_t>(days);
  if (wholeDays > params.maximumIntervalDays) {
    wholeDays = params.maximumIntervalDays;
  }
  return wholeDays;
}

inline float shortTermStability(const Params& params, float stability, Grade grade) {
  float increase = expf(params.w[17] * (gradeOffsetFromGood(grade) + params.w[18])) * powf(stability, -params.w[19]);
  if (grade != Grade::Again && increase < 1.0f) {
    increase = 1.0f;
  }
  return clampStability(stability * increase);
}

inline float nextDifficulty(const Params& params, float difficulty, Grade grade) {
  // py-fsrs `arg_1`: the unclamped initial difficulty for Easy (rating 4).
  const float target = params.w[4] - expf(params.w[5] * 3.0f) + 1.0f;
  const float deltaDifficulty = -(params.w[6] * gradeOffsetFromGood(grade));
  const float damped = difficulty + (10.0f - difficulty) * deltaDifficulty / 9.0f;
  return clampDifficulty(params.w[7] * target + (1.0f - params.w[7]) * damped);
}

inline float nextForgetStability(const Params& params, float difficulty, float stability, float recall) {
  const float longTerm = params.w[11] * powf(difficulty, -params.w[12]) *
                         (powf(stability + 1.0f, params.w[13]) - 1.0f) * expf((1.0f - recall) * params.w[14]);
  const float shortTerm = stability / expf(params.w[17] * params.w[18]);
  return longTerm < shortTerm ? longTerm : shortTerm;
}

inline float nextRecallStability(const Params& params, float difficulty, float stability, float recall, Grade grade) {
  const float hardPenalty = (grade == Grade::Hard) ? params.w[15] : 1.0f;
  const float easyBonus = (grade == Grade::Easy) ? params.w[16] : 1.0f;
  return stability * (1.0f + expf(params.w[8]) * (11.0f - difficulty) * powf(stability, -params.w[9]) *
                                 (expf((1.0f - recall) * params.w[10]) - 1.0f) * hardPenalty * easyBonus);
}

inline float nextStability(const Params& params, float difficulty, float stability, float recall, Grade grade) {
  const float updated = (grade == Grade::Again) ? nextForgetStability(params, difficulty, stability, recall)
                                                : nextRecallStability(params, difficulty, stability, recall, grade);
  return clampStability(updated);
}

// --- candidate outcomes ------------------------------------------------------

/**
 * One grade's result before the due date is stamped on it. `interval` is whole
 * days when `intraday` is false and seconds when it is true.
 */
struct Outcome {
  CardState card;
  uint32_t interval;
  bool intraday;
};

/** Leave the learning/relearning phase for a day-scale review interval. */
void graduate(const Params& params, const Curve& curve, Grade grade, Outcome& outcome) {
  outcome.card.state = CardPhase::Review;
  outcome.card.step = 0;
  uint32_t days = nextIntervalDays(params, curve, outcome.card.stability);
  // Spec §0.4: graduation intervals come from FSRS stability, so both floors
  // default to 0 and this is a no-op on the device. They stay configurable for
  // a caller that wants Anki's SM-2-era graduating/easy intervals back.
  //
  // The floor is applied PRE-fuzz (computeAllOutcomes fuzzes afterwards), so a
  // nonzero floor is a floor on the FSRS interval, not on the interval the card
  // actually gets: the fuzz band around a floored interval reaches below it.
  // Documented rather than fixed -- moving the floor after fuzz would break the
  // "fuzz reproduces py-fsrs draw for draw" property the vectors rest on.
  const uint32_t floorDays = (grade == Grade::Easy) ? params.minEasyIntervalDays : params.minGraduatingIntervalDays;
  if (days < floorDays) {
    days = floorDays;
  }
  if (days > params.maximumIntervalDays) {
    days = params.maximumIntervalDays;
  }
  outcome.interval = days;
  outcome.intraday = false;
}

/**
 * The Anki learning/relearning step machine, shared by both phases exactly as
 * py-fsrs shares its two identical blocks.
 */
void applyStepMachine(const Params& params, const Curve& curve, const uint32_t* steps, uint8_t stepCount,
                      CardPhase stepPhase, uint8_t currentStep, Grade grade, Outcome& outcome) {
  if (stepCount > MAX_STEPS) {
    stepCount = MAX_STEPS;  // a malformed Params must not index past the table
  }
  // Second clause: a card left behind by a scheduler that had more steps than
  // this one graduates instead of indexing past the table.
  const bool beyondTable = (currentStep >= stepCount) && (grade != Grade::Again);
  if (stepCount == 0 || beyondTable) {
    graduate(params, curve, grade, outcome);
    return;
  }

  outcome.intraday = true;
  outcome.card.state = stepPhase;

  switch (grade) {
    case Grade::Again:
      outcome.card.step = 0;
      outcome.interval = steps[0];
      return;
    case Grade::Hard:
      // The step does not move; Anki's delay for a repeated first step is
      // 1.5x it when it is the only step, else the mean of the first two.
      outcome.card.step = currentStep;
      if (currentStep == 0 && stepCount == 1) {
        // u64 intermediates: an absurd step table would overflow u32 otherwise.
        outcome.interval = static_cast<uint32_t>((static_cast<uint64_t>(steps[0]) * 3u) / 2u);
      } else if (currentStep == 0) {
        outcome.interval =
            static_cast<uint32_t>((static_cast<uint64_t>(steps[0]) + static_cast<uint64_t>(steps[1])) / 2u);
      } else {
        outcome.interval = steps[currentStep];
      }
      return;
    case Grade::Good:
      if (static_cast<uint8_t>(currentStep + 1u) == stepCount) {
        graduate(params, curve, grade, outcome);
        return;
      }
      outcome.card.step = static_cast<uint8_t>(currentStep + 1u);
      outcome.interval = steps[outcome.card.step];
      return;
    case Grade::Easy:
      graduate(params, curve, grade, outcome);
      return;
  }

  // Unreachable: `Grade` has exactly the four enumerators above, and a grade
  // always comes from the UI rather than from a file, so there is no torn-record
  // path here. Written as a fallback anyway (same shape as `phaseOf`) so that a
  // future enumerator cannot leave `outcome.interval` at its zero initialiser:
  // an unknown grade restarts the card, which is the conservative answer.
  outcome.card.step = 0;
  outcome.interval = steps[0];
}

/** Interval for a card that was already in the Review phase. */
void applyReviewInterval(const Params& params, const Curve& curve, Grade grade, Outcome& outcome) {
  if (grade == Grade::Again && params.relearningStepCount > 0) {
    outcome.card.state = CardPhase::Relearning;
    outcome.card.step = 0;
    outcome.interval = params.relearningSteps[0];
    outcome.intraday = true;
    return;
  }
  // With no relearning steps configured, py-fsrs keeps a lapsed card in Review.
  outcome.card.state = CardPhase::Review;
  outcome.card.step = 0;
  outcome.interval = nextIntervalDays(params, curve, outcome.card.stability);
  outcome.intraday = false;
}

void computeOutcome(const Params& params, const Curve& curve, const CardState& card, uint16_t today, Grade grade,
                    Outcome& outcome) {
  outcome.card = card;
  outcome.interval = 0;
  outcome.intraday = true;

  const CardPhase phase = phaseOf(card.state);
  outcome.card.state = phase;
  const uint8_t currentStep = (phase == CardPhase::New) ? 0 : card.step;
  // Sanitised copies of the memory state: see the clamp helpers above.
  const float stability = clampStability(card.stability);
  const float difficulty = clampDifficulty(card.difficulty);

  // Overdue credit rides on this: retrievability decays with elapsed days, so a
  // late answer earns more stability than a punctual one. A clock that jumped
  // backwards (today < lastReviewDay) reads as 0 rather than as a huge number.
  uint32_t elapsedDays = 0;
  if (card.lastReviewDay != 0 && today > card.lastReviewDay) {
    elapsedDays = static_cast<uint32_t>(today - card.lastReviewDay);
  }
  // py-fsrs takes the short-term path when the previous review was under a day
  // ago; in day-number terms that is "on the same rollover day".
  const bool sameDay = (elapsedDays == 0);

  switch (phase) {
    case CardPhase::New:
      outcome.card.stability = initialStability(params, grade);
      outcome.card.difficulty = initialDifficulty(params, grade);
      break;
    case CardPhase::Learning:
    case CardPhase::Review:
    case CardPhase::Relearning:
      outcome.card.stability =
          sameDay ? shortTermStability(params, stability, grade)
                  : nextStability(params, difficulty, stability, retrievability(curve, elapsedDays, stability), grade);
      outcome.card.difficulty = nextDifficulty(params, difficulty, grade);
      break;
  }

  switch (phase) {
    case CardPhase::New:
    case CardPhase::Learning:
      applyStepMachine(params, curve, params.learningSteps, params.learningStepCount, CardPhase::Learning, currentStep,
                       grade, outcome);
      break;
    case CardPhase::Review:
      applyReviewInterval(params, curve, grade, outcome);
      break;
    case CardPhase::Relearning:
      applyStepMachine(params, curve, params.relearningSteps, params.relearningStepCount, CardPhase::Relearning,
                       currentStep, grade, outcome);
      break;
  }

  if (outcome.card.reps < 0xFFFFu) {
    outcome.card.reps = static_cast<uint16_t>(outcome.card.reps + 1u);
  }
  // Anki counts a lapse when a card in the Review phase is failed, and only then.
  if (phase == CardPhase::Review && grade == Grade::Again && outcome.card.lapses < 0xFFu) {
    outcome.card.lapses = static_cast<uint8_t>(outcome.card.lapses + 1u);
  }
}

/**
 * Anki's post-fuzz ordering rule: the day-scale intervals must be strictly
 * increasing across Hard < Good < Easy, or the buttons stop making sense. Only
 * grades that landed on a day-scale interval take part; an intraday step is
 * always shorter than any review interval anyway.
 */
void enforceMonotonicIntervals(const Params& params, Outcome* outcomes) {
  Outcome& hard = outcomes[static_cast<uint8_t>(Grade::Hard)];
  Outcome& good = outcomes[static_cast<uint8_t>(Grade::Good)];
  Outcome& easy = outcomes[static_cast<uint8_t>(Grade::Easy)];

  if (!hard.intraday && !good.intraday && good.interval <= hard.interval) {
    good.interval = hard.interval + 1u;
  }
  if (!good.intraday && !easy.intraday && easy.interval <= good.interval) {
    easy.interval = good.interval + 1u;
  }
  // Re-cap: the bumps above can push a near-ceiling interval past the maximum,
  // in which case the ceiling wins and the last pair ties.
  if (!good.intraday && good.interval > params.maximumIntervalDays) {
    good.interval = params.maximumIntervalDays;
  }
  if (!easy.intraday && easy.interval > params.maximumIntervalDays) {
    easy.interval = params.maximumIntervalDays;
  }
}

void computeAllOutcomes(const Params& params, const CardState& card, Now now, bool withFuzz, uint64_t cardKey,
                        FuzzRng rng, Outcome* outcomes) {
  const Curve curve = curveOf(params);
  for (uint8_t index = 0; index < GRADE_COUNT; ++index) {
    computeOutcome(params, curve, card, now.dayNumber, static_cast<Grade>(index), outcomes[index]);
  }

  if (withFuzz && params.enableFuzz && rng.draw != nullptr) {
    // One draw for all four buttons, as Anki does: the same relative position
    // inside each band keeps the fuzzed intervals ordered.
    float u01 = rng.draw(fuzzSeed(cardKey, card.reps), rng.ctx);
    if (!(u01 >= 0.0f)) {
      u01 = 0.0f;
    }
    if (u01 >= 1.0f) {
      u01 = nextafterf(1.0f, 0.0f);
    }
    for (uint8_t index = 0; index < GRADE_COUNT; ++index) {
      if (!outcomes[index].intraday) {
        outcomes[index].interval = detail::fuzzInterval(params, outcomes[index].interval, u01);
      }
    }
  }

  enforceMonotonicIntervals(params, outcomes);
}

inline uint64_t splitmix64(uint64_t value) {
  uint64_t z = value + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

// --- public API --------------------------------------------------------------

Params defaultParams() {
  Params params;
  for (uint8_t index = 0; index < PARAM_COUNT; ++index) {
    params.w[index] = DEFAULT_W[index];
  }
  params.desiredRetention = 0.90f;
  params.learningSteps[0] = 60u;   // 1 m
  params.learningSteps[1] = 600u;  // 10 m
  params.learningSteps[2] = 0u;
  params.learningSteps[3] = 0u;
  params.learningStepCount = 2;
  params.relearningSteps[0] = 600u;  // 10 m
  params.relearningSteps[1] = 0u;
  params.relearningSteps[2] = 0u;
  params.relearningSteps[3] = 0u;
  params.relearningStepCount = 1;
  params.maximumIntervalDays = 36500u;
  // Spec §0.4: no fixed graduating / easy interval -- FSRS stability decides,
  // as it does in Anki. The floors exist for callers who disagree; off here.
  params.minGraduatingIntervalDays = 0u;
  params.minEasyIntervalDays = 0u;
  params.enableFuzz = true;
  return params;
}

bool validateParams(const Params& params) {
  // Every test is written as a failed range so that a NaN fails rather than
  // slipping through a negated comparison.
  for (uint8_t index = 0; index < PARAM_COUNT; ++index) {
    if (!(params.w[index] >= PARAM_LOWER[index] && params.w[index] <= PARAM_UPPER[index])) {
      return false;
    }
  }
  if (!(params.desiredRetention >= RETENTION_MIN && params.desiredRetention <= RETENTION_MAX)) {
    return false;
  }
  if (!(params.maximumIntervalDays >= 1u && params.maximumIntervalDays <= MAXIMUM_INTERVAL_DAYS_CAP)) {
    return false;
  }
  if (params.learningStepCount > MAX_STEPS || params.relearningStepCount > MAX_STEPS) {
    return false;
  }
  const uint32_t* tables[2] = {params.learningSteps, params.relearningSteps};
  const uint8_t counts[2] = {params.learningStepCount, params.relearningStepCount};
  for (uint8_t table = 0; table < 2; ++table) {
    for (uint8_t index = 0; index < counts[table]; ++index) {
      const uint32_t step = tables[table][index];
      if (step == 0u || step > STEP_SECONDS_MAX) {
        return false;
      }
      if (index > 0 && step <= tables[table][index - 1]) {
        return false;  // the header documents the tables as strictly ascending
      }
    }
  }
  return true;
}

CardState gradeCard(const Params& params, const CardState& card, Grade grade, Now now, uint64_t cardKey, FuzzRng rng) {
  Outcome outcomes[GRADE_COUNT];
  computeAllOutcomes(params, card, now, true, cardKey, rng, outcomes);

  const Outcome& chosen = outcomes[static_cast<uint8_t>(grade)];
  CardState next = chosen.card;
  next.lastReviewDay = now.dayNumber;
  next.due = chosen.intraday ? now.unixSecs + chosen.interval : static_cast<uint32_t>(now.dayNumber) + chosen.interval;
  return next;
}

Preview previewIntervals(const Params& params, const CardState& card, Now now) {
  Outcome outcomes[GRADE_COUNT];
  computeAllOutcomes(params, card, now, false, 0, FuzzRng(), outcomes);

  Preview preview;
  for (uint8_t index = 0; index < GRADE_COUNT; ++index) {
    preview.next[index] = outcomes[index].interval;
    preview.intraday[index] = outcomes[index].intraday;
  }
  return preview;
}

uint16_t dayNumber(uint32_t unixSecs, int32_t utcOffsetSecs) {
  const int64_t shifted =
      static_cast<int64_t>(unixSecs) + static_cast<int64_t>(utcOffsetSecs) - static_cast<int64_t>(ROLLOVER_SECONDS);
  int64_t day = shifted / static_cast<int64_t>(SECONDS_PER_DAY);
  if (shifted < 0 && (shifted % static_cast<int64_t>(SECONDS_PER_DAY)) != 0) {
    day -= 1;  // C++ truncates towards zero; the day number needs a floor.
  }
  // Spec §4 clamps rather than wraps, so a corrupt RTC cannot alias onto a
  // valid day. Neither end is reachable from a plausible u32 clock.
  if (day < 0) {
    return 0u;
  }
  if (day > 0xFFFF) {
    return 0xFFFFu;
  }
  return static_cast<uint16_t>(day);
}

uint64_t fuzzSeed(uint64_t cardKey, uint16_t reps) {
  return splitmix64(cardKey ^ (static_cast<uint64_t>(reps) * 0x9E3779B97F4A7C15ull));
}

float defaultFuzzDraw(uint64_t seed, void*) {
  // Top 24 bits scaled by 2^-24: uniform over [0, 1) with no float rounding to 1.
  return static_cast<float>(splitmix64(seed) >> 40) * (1.0f / 16777216.0f);
}

namespace detail {

void fuzzBounds(const Params& params, uint32_t intervalDays, uint32_t& minDays, uint32_t& maxDays) {
  const float days = static_cast<float>(intervalDays);
  float delta = 1.0f;
  for (const FuzzBand& band : FUZZ_BANDS) {
    const float capped = days < band.end ? days : band.end;
    const float span = capped - band.start;
    if (span > 0.0f) {
      delta += band.factor * span;
    }
  }

  float low = roundTiesToEven(days - delta);
  float high = roundTiesToEven(days + delta);
  if (low < 2.0f) {
    low = 2.0f;
  }
  const float maximum = static_cast<float>(params.maximumIntervalDays);
  if (high > maximum) {
    high = maximum;
  }
  if (low > high) {
    low = high;
  }
  minDays = static_cast<uint32_t>(low);
  maxDays = static_cast<uint32_t>(high);
  // As in nextIntervalDays: the float ceiling rounds up above 2^24, so re-cap.
  if (maxDays > params.maximumIntervalDays) {
    maxDays = params.maximumIntervalDays;
  }
  if (minDays > maxDays) {
    minDays = maxDays;
  }
}

uint32_t fuzzInterval(const Params& params, uint32_t intervalDays, float u01) {
  if (intervalDays < FUZZ_MIN_INTERVAL_DAYS) {
    return intervalDays;
  }

  uint32_t minDays = 0;
  uint32_t maxDays = 0;
  fuzzBounds(params, intervalDays, minDays, maxDays);

  // py-fsrs draws over [min, max + 1) and rounds, so the top of the band can
  // land one day past `maxDays`; only the maximum interval caps it. Kept as-is
  // so the port matches the reference bit for bit.
  float fuzzed = roundTiesToEven(u01 * static_cast<float>(maxDays - minDays + 1u) + static_cast<float>(minDays));
  const float maximum = static_cast<float>(params.maximumIntervalDays);
  if (fuzzed > maximum) {
    fuzzed = maximum;
  }
  uint32_t wholeDays = static_cast<uint32_t>(fuzzed);
  if (wholeDays > params.maximumIntervalDays) {
    wholeDays = params.maximumIntervalDays;
  }
  return wholeDays;
}

}  // namespace detail

}  // namespace fsrs

#endif  // CROSSPOINT_FLASHCARDS
