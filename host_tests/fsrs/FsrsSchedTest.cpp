// Host tests for lib/FsrsSched. Built by build.sh with g++ -DCROSSPOINT_FLASHCARDS;
// never compiled into the firmware (host_tests/ is outside src/ and lib/).
//
// Two layers:
//   * replay of vectors generated from py-fsrs 6.3.2 (FsrsVectors.inc), and
//   * hand-written property tests for the parts that are ours rather than
//     py-fsrs': the day-number function, the fuzz plumbing, the post-fuzz
//     monotonicity clamp, parameter validation, the counters and the clamps.
//
// The two layers do not overlap, and the split matters when reading a failure.
// gen_vectors.py MODELS the port's non-py-fsrs interval rules (hard < good <
// easy, the graduation floors) and asserts that its own day-number deltas match
// py-fsrs', so the vectors are evidence for the FSRS formulas and nothing else.
// testMonotonicity, testGraduationFloors and testRolloverDeviation are the
// authoritative checks for those three rules -- never cite a vector for them.

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "FsrsSched.h"

using fsrs::CardPhase;
using fsrs::CardState;
using fsrs::Grade;
using fsrs::Now;
using fsrs::Params;

namespace {

// --- tiny harness ------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void beginGroup(const char* name) { g_group = name; }

void record(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    printf("FAIL [%s] line %d: %s\n", g_group, line, expr);
  }
}

#define EXPECT(cond) record((cond), #cond, __LINE__)

#define EXPECT_EQ_U(actual, expected)                                                                               \
  do {                                                                                                              \
    const unsigned long long a_ = static_cast<unsigned long long>(actual);                                          \
    const unsigned long long e_ = static_cast<unsigned long long>(expected);                                        \
    ++g_checks;                                                                                                     \
    if (a_ != e_) {                                                                                                 \
      ++g_failures;                                                                                                 \
      printf("FAIL [%s] line %d: %s == %s (got %llu, want %llu)\n", g_group, __LINE__, #actual, #expected, a_, e_); \
    }                                                                                                               \
  } while (0)

#define EXPECT_NEAR_REL(actual, expected, tol)                                                                       \
  do {                                                                                                               \
    const double a_ = static_cast<double>(actual);                                                                   \
    const double e_ = static_cast<double>(expected);                                                                 \
    const double scale_ = fabs(e_) > 1e-6 ? fabs(e_) : 1.0;                                                          \
    ++g_checks;                                                                                                      \
    if (!(fabs(a_ - e_) / scale_ <= (tol))) {                                                                        \
      ++g_failures;                                                                                                  \
      printf("FAIL [%s] line %d: %s ~= %s (got %.9g, want %.9g, rel %.3g)\n", g_group, __LINE__, #actual, #expected, \
             a_, e_, fabs(a_ - e_) / scale_);                                                                        \
    }                                                                                                                \
  } while (0)

// --- generated vectors -------------------------------------------------------

struct VecStep {
  uint8_t grade;
  uint32_t advanceSecs;
  uint8_t outState;
  int8_t outStep;
  float outStability;
  float outDifficulty;
  uint32_t outIntervalSecs;     // with the hard < good < easy ordering applied
  uint32_t outRawIntervalSecs;  // py-fsrs, untouched
};

struct VecScenario {
  const char* name;
  uint32_t baseUnix;
  const VecStep* steps;
  uint16_t count;
};

struct FuzzVec {
  uint32_t days;
  float draw;
  uint32_t expected;
};

#include "FsrsVectors.inc"

constexpr double VECTOR_TOLERANCE = 1e-4;
constexpr uint32_t SECONDS_PER_DAY = 86400u;
constexpr uint64_t TEST_KEY = 0x0123456789ABCDEFull;

CardState newCard() {
  CardState card = {};
  card.state = CardPhase::New;
  return card;
}

Params testParams() {
  Params params = fsrs::defaultParams();
  params.enableFuzz = false;  // vectors were generated with fuzzing disabled
  return params;
}

Now momentAt(uint32_t unixSecs) {
  Now now;
  now.unixSecs = unixSecs;
  now.dayNumber = fsrs::dayNumber(unixSecs, 0);
  return now;
}

// --- 1. py-fsrs scenario vectors ---------------------------------------------

double relativeError(float actual, float expected) {
  const double scale = fabs(static_cast<double>(expected)) > 1e-6 ? fabs(static_cast<double>(expected)) : 1.0;
  return fabs(static_cast<double>(actual) - static_cast<double>(expected)) / scale;
}

void testScenarioVectors() {
  beginGroup("py-fsrs scenario vectors");
  const Params params = testParams();
  int orderingAdjusted = 0;
  double worstError = 0.0;

  for (const VecScenario& scenario : VEC_SCENARIOS) {
    CardState card = newCard();
    uint32_t unixSecs = scenario.baseUnix;
    uint16_t expectedReps = 0;

    for (uint16_t index = 0; index < scenario.count; ++index) {
      const VecStep& step = scenario.steps[index];
      unixSecs += step.advanceSecs;
      const Now now = momentAt(unixSecs);
      const uint8_t previousState = static_cast<uint8_t>(card.state);

      card = fsrs::gradeCard(params, card, static_cast<Grade>(step.grade), now, TEST_KEY);
      ++expectedReps;

      if (static_cast<uint8_t>(card.state) != step.outState) {
        ++g_checks;
        ++g_failures;
        printf("FAIL [%s] %s step %u: state %u, want %u\n", g_group, scenario.name, index,
               static_cast<unsigned>(card.state), step.outState);
        break;
      }
      ++g_checks;

      if (step.outStep >= 0) {
        EXPECT_EQ_U(card.step, static_cast<uint8_t>(step.outStep));
      }
      EXPECT_NEAR_REL(card.stability, step.outStability, VECTOR_TOLERANCE);
      EXPECT_NEAR_REL(card.difficulty, step.outDifficulty, VECTOR_TOLERANCE);
      worstError = fmax(worstError, relativeError(card.stability, step.outStability));
      worstError = fmax(worstError, relativeError(card.difficulty, step.outDifficulty));
      EXPECT_EQ_U(card.reps, expectedReps);
      EXPECT_EQ_U(card.lastReviewDay, now.dayNumber);

      if (card.state == CardPhase::Review) {
        // py-fsrs day intervals are whole days by construction.
        EXPECT_EQ_U(step.outIntervalSecs % SECONDS_PER_DAY, 0u);
        const uint32_t days = card.due - now.dayNumber;
        if (days != step.outIntervalSecs / SECONDS_PER_DAY) {
          ++g_checks;
          ++g_failures;
          printf("FAIL [%s] %s step %u: interval %u d, want %u d (S=%.6f)\n", g_group, scenario.name, index, days,
                 step.outIntervalSecs / SECONDS_PER_DAY, static_cast<double>(card.stability));
        } else {
          ++g_checks;
        }
      } else {
        EXPECT_EQ_U(card.due - now.unixSecs, step.outIntervalSecs);
        EXPECT_EQ_U(step.outIntervalSecs, step.outRawIntervalSecs);
      }

      if (step.outIntervalSecs != step.outRawIntervalSecs) {
        ++orderingAdjusted;
        // The ordering rule and the graduation floors may only lengthen an
        // interval, and only for Good or Easy.
        EXPECT(step.outIntervalSecs > step.outRawIntervalSecs);
        EXPECT(step.grade == static_cast<uint8_t>(Grade::Good) || step.grade == static_cast<uint8_t>(Grade::Easy));
      }

      // Lapses are counted by us, not by py-fsrs, so check the transition here.
      if (previousState == static_cast<uint8_t>(CardPhase::Review) &&
          step.grade == static_cast<uint8_t>(Grade::Again)) {
        EXPECT(card.lapses > 0);
      }
    }
  }
  printf("  (%u scenarios; ordering/floor rules moved %d steps; worst float-vs-double error %.2e)\n",
         static_cast<unsigned>(sizeof(VEC_SCENARIOS) / sizeof(VEC_SCENARIOS[0])), orderingAdjusted, worstError);
}

// --- 2. fuzz ------------------------------------------------------------------

void testFuzzVectors() {
  beginGroup("py-fsrs fuzz vectors");
  Params params = fsrs::defaultParams();
  for (const FuzzVec& vec : FUZZ_VECTORS) {
    const uint32_t got = fsrs::detail::fuzzInterval(params, vec.days, vec.draw);
    if (got != vec.expected) {
      ++g_checks;
      ++g_failures;
      printf("FAIL [%s] days=%u draw=%.9g: got %u, want %u\n", g_group, vec.days, static_cast<double>(vec.draw), got,
             vec.expected);
    } else {
      ++g_checks;
    }
  }
}

void expectBounds(uint32_t days, uint32_t wantLow, uint32_t wantHigh, const Params& params) {
  uint32_t low = 0;
  uint32_t high = 0;
  fsrs::detail::fuzzBounds(params, days, low, high);
  EXPECT_EQ_U(low, wantLow);
  EXPECT_EQ_U(high, wantHigh);
}

void testFuzzBands() {
  beginGroup("fuzz bands");
  Params params = fsrs::defaultParams();

  // delta = 1 + 0.15*[2.5,7) + 0.10*[7,20) + 0.05*[20,inf), bounds rounded.
  //
  // days=3 is where the lower bound comes closest to py-fsrs' `max(2, ...)`
  // floor: delta is 1.075, so the bound rounds to exactly 2 on its own. With
  // this band table the floor can never actually bind (delta >= 1 and fuzzing
  // starts at 3 days), so it is carried for fidelity, not for effect.
  expectBounds(3, 2, 4, params);           // delta 1.075 -> 1.925 / 4.075
  expectBounds(7, 5, 9, params);           // delta 1.675, first band fully accumulated
  expectBounds(10, 8, 12, params);         // delta 1.975, second band opens
  expectBounds(20, 17, 23, params);        // delta 2.975, second band fully accumulated
  expectBounds(100, 93, 107, params);      // delta 6.975, third band
  expectBounds(3650, 3466, 3834, params);  // delta 184.475

  // Under 3 whole days nothing is fuzzed at all (py-fsrs' 2.5 day threshold).
  for (uint32_t days = 0; days < 3; ++days) {
    EXPECT_EQ_U(fsrs::detail::fuzzInterval(params, days, 0.0f), days);
    EXPECT_EQ_U(fsrs::detail::fuzzInterval(params, days, 0.75f), days);
  }

  // Band widths grow with the interval but shrink as a fraction of it.
  uint32_t low = 0;
  uint32_t high = 0;
  fsrs::detail::fuzzBounds(params, 5, low, high);
  const uint32_t width5 = high - low;
  fsrs::detail::fuzzBounds(params, 50, low, high);
  const uint32_t width50 = high - low;
  fsrs::detail::fuzzBounds(params, 500, low, high);
  const uint32_t width500 = high - low;
  EXPECT(width5 < width50 && width50 < width500);
  EXPECT(static_cast<float>(width5) / 5.0f > static_cast<float>(width500) / 500.0f);

  // The ceiling clamps the upper bound, and the lower bound follows it down.
  Params capped = fsrs::defaultParams();
  capped.maximumIntervalDays = 5;
  expectBounds(100, 5, 5, capped);
  EXPECT_EQ_U(fsrs::detail::fuzzInterval(capped, 100, 0.99f), 5u);

  // Every draw in [0,1) stays inside [low, high+1]: py-fsrs draws over
  // [low, high+1) and rounds, so the top of the band can reach high + 1.
  fsrs::detail::fuzzBounds(params, 30, low, high);
  for (int i = 0; i <= 100; ++i) {
    const float draw = static_cast<float>(i) / 101.0f;
    const uint32_t fuzzed = fsrs::detail::fuzzInterval(params, 30, draw);
    EXPECT(fuzzed >= low && fuzzed <= high + 1u);
  }
}

float stubDraw(uint64_t, void* ctx) { return *static_cast<const float*>(ctx); }

void testFuzzIsDeterministicAndSeeded() {
  beginGroup("fuzz determinism");
  Params params = fsrs::defaultParams();

  CardState card = {};
  card.state = CardPhase::Review;
  card.stability = 40.0f;
  card.difficulty = 5.0f;
  card.lastReviewDay = 20000;
  card.reps = 7;
  const Now now = momentAt(1772452800u);

  const CardState a = fsrs::gradeCard(params, card, Grade::Good, now, TEST_KEY);
  const CardState b = fsrs::gradeCard(params, card, Grade::Good, now, TEST_KEY);
  EXPECT_EQ_U(a.due, b.due);

  // A different key, or a different rep count, must be able to land elsewhere.
  bool keyChanged = false;
  bool repsChanged = false;
  for (uint64_t key = 1; key < 40 && !keyChanged; ++key) {
    keyChanged = fsrs::gradeCard(params, card, Grade::Good, now, key).due != a.due;
  }
  for (uint16_t reps = 1; reps < 40 && !repsChanged; ++reps) {
    CardState variant = card;
    variant.reps = reps;
    repsChanged = fsrs::gradeCard(params, variant, Grade::Good, now, TEST_KEY).due != a.due;
  }
  EXPECT(keyChanged);
  EXPECT(repsChanged);

  // The default draw must stay in [0,1).
  for (uint64_t seed = 0; seed < 2000; ++seed) {
    const float draw = fsrs::defaultFuzzDraw(fsrs::fuzzSeed(seed * 2654435761ull, seed & 0xFFFFu), nullptr);
    EXPECT(draw >= 0.0f && draw < 1.0f);
  }

  // An injected RNG is actually used: the extremes of the band are reachable.
  float value = 0.0f;
  fsrs::FuzzRng rng;
  rng.draw = &stubDraw;
  rng.ctx = &value;
  const uint32_t lowDue = fsrs::gradeCard(params, card, Grade::Good, now, TEST_KEY, rng).due;
  value = 0.99f;
  const uint32_t highDue = fsrs::gradeCard(params, card, Grade::Good, now, TEST_KEY, rng).due;
  EXPECT(lowDue < highDue);

  // Out-of-range draws are clamped rather than escaping the band.
  uint32_t low = 0;
  uint32_t high = 0;
  const uint32_t unfuzzed = fsrs::previewIntervals(params, card, now).next[static_cast<uint8_t>(Grade::Good)];
  fsrs::detail::fuzzBounds(params, unfuzzed, low, high);
  value = -5.0f;
  EXPECT_EQ_U(fsrs::gradeCard(params, card, Grade::Good, now, TEST_KEY, rng).due - now.dayNumber, low);
  value = 5.0f;
  const uint32_t clampedHigh = fsrs::gradeCard(params, card, Grade::Good, now, TEST_KEY, rng).due - now.dayNumber;
  EXPECT(clampedHigh >= low && clampedHigh <= high + 1u);

  // Fuzzing off means the raw formula interval.
  Params noFuzz = fsrs::defaultParams();
  noFuzz.enableFuzz = false;
  EXPECT_EQ_U(fsrs::gradeCard(noFuzz, card, Grade::Good, now, TEST_KEY).due - now.dayNumber, unfuzzed);
}

// --- 3. post-fuzz monotonicity ------------------------------------------------

void testMonotonicity() {
  beginGroup("post-fuzz monotonicity");
  Params params = fsrs::defaultParams();

  float value = 0.0f;
  fsrs::FuzzRng rng;
  rng.draw = &stubDraw;
  rng.ctx = &value;

  const float draws[] = {0.0f, 0.13f, 0.5f, 0.87f, 0.999f};
  const float stabilities[] = {0.01f, 0.5f, 1.0f, 2.0f, 2.6f, 3.0f, 4.0f, 7.5f, 21.0f, 400.0f, 90000.0f};
  const float difficulties[] = {1.0f, 3.3f, 5.0f, 8.8f, 10.0f};

  for (float draw : draws) {
    value = draw;
    for (float stability : stabilities) {
      for (float difficulty : difficulties) {
        for (uint32_t elapsed = 0; elapsed <= 30; elapsed += 10) {
          CardState card = {};
          card.state = CardPhase::Review;
          card.stability = stability;
          card.difficulty = difficulty;
          card.lastReviewDay = 20000;
          card.reps = 5;
          const Now now = momentAt(1772452800u + elapsed * SECONDS_PER_DAY);

          const CardState hard = fsrs::gradeCard(params, card, Grade::Hard, now, TEST_KEY, rng);
          const CardState good = fsrs::gradeCard(params, card, Grade::Good, now, TEST_KEY, rng);
          const CardState easy = fsrs::gradeCard(params, card, Grade::Easy, now, TEST_KEY, rng);
          // All three stay in Review, so all three dues are day numbers. The
          // one place strictness cannot hold is the interval ceiling, where
          // everything piles up on the same day (Anki behaves the same way).
          const uint32_t ceiling = static_cast<uint32_t>(now.dayNumber) + params.maximumIntervalDays;
          const bool ordered = (hard.due < good.due || (hard.due == ceiling && good.due == ceiling)) &&
                               (good.due < easy.due || (good.due == ceiling && easy.due == ceiling));
          if (!ordered) {
            ++g_checks;
            ++g_failures;
            printf("FAIL [%s] S=%g D=%g elapsed=%u draw=%g: %u / %u / %u\n", g_group, static_cast<double>(stability),
                   static_cast<double>(difficulty), elapsed, static_cast<double>(draw), hard.due - now.dayNumber,
                   good.due - now.dayNumber, easy.due - now.dayNumber);
          } else {
            ++g_checks;
          }
        }
      }
    }
  }

  // The clamp has to actually fire somewhere, or the test above proves nothing:
  // a low-stability review card collides on Hard/Good before the clamp.
  CardState card = {};
  card.state = CardPhase::Review;
  card.stability = 1.4f;
  card.difficulty = 9.0f;
  card.lastReviewDay = 20000;
  card.reps = 3;
  const Now now = momentAt(1772452800u);
  Params noFuzz = fsrs::defaultParams();
  noFuzz.enableFuzz = false;
  fsrs::Preview raw = fsrs::previewIntervals(noFuzz, card, now);
  EXPECT(raw.next[static_cast<uint8_t>(Grade::Hard)] < raw.next[static_cast<uint8_t>(Grade::Good)]);
  EXPECT(raw.next[static_cast<uint8_t>(Grade::Good)] < raw.next[static_cast<uint8_t>(Grade::Easy)]);
}

// --- 4. preview ---------------------------------------------------------------

void testPreview() {
  beginGroup("previewIntervals");
  Params params = testParams();  // fuzz off, so preview and grade must agree
  const Now now = momentAt(1772452800u);

  CardState fresh = newCard();
  const fsrs::Preview newPreview = fsrs::previewIntervals(params, fresh, now);
  EXPECT(newPreview.intraday[0] && newPreview.intraday[1] && newPreview.intraday[2]);
  EXPECT(!newPreview.intraday[3]);        // Easy graduates straight to days
  EXPECT_EQ_U(newPreview.next[0], 60u);   // Again -> first learning step
  EXPECT_EQ_U(newPreview.next[1], 330u);  // Hard -> mean(1m, 10m)
  EXPECT_EQ_U(newPreview.next[2], 600u);  // Good -> second learning step
  EXPECT_EQ_U(newPreview.next[3], 8u);    // Easy -> round(w[3]) days

  const CardState states[] = {fresh,
                              [] {
                                CardState card = {};
                                card.state = CardPhase::Learning;
                                card.step = 1;
                                card.stability = 2.3f;
                                card.difficulty = 5.1f;
                                card.lastReviewDay = 20500;
                                card.reps = 1;
                                return card;
                              }(),
                              [] {
                                CardState card = {};
                                card.state = CardPhase::Review;
                                card.stability = 33.0f;
                                card.difficulty = 6.2f;
                                card.lastReviewDay = 20480;
                                card.reps = 9;
                                return card;
                              }(),
                              [] {
                                CardState card = {};
                                card.state = CardPhase::Relearning;
                                card.stability = 4.5f;
                                card.difficulty = 8.0f;
                                card.lastReviewDay = 20510;
                                card.reps = 14;
                                card.lapses = 2;
                                return card;
                              }()};

  for (const CardState& card : states) {
    const fsrs::Preview preview = fsrs::previewIntervals(params, card, now);
    for (uint8_t index = 0; index < fsrs::GRADE_COUNT; ++index) {
      const CardState graded = fsrs::gradeCard(params, card, static_cast<Grade>(index), now, TEST_KEY);
      const bool intraday = graded.state != CardPhase::Review;
      EXPECT(preview.intraday[index] == intraday);
      const uint32_t interval = intraday ? graded.due - now.unixSecs : graded.due - now.dayNumber;
      EXPECT_EQ_U(preview.next[index], interval);
    }
  }
}

// --- 5. day numbers -----------------------------------------------------------

void testDayNumbers() {
  beginGroup("dayNumber");

  // 2026-03-02, UTC. 03:59:59 is still the previous study day; 04:00:00 is not.
  EXPECT_EQ_U(fsrs::dayNumber(1772423999u, 0), 20513u);
  EXPECT_EQ_U(fsrs::dayNumber(1772424000u, 0), 20514u);
  EXPECT_EQ_U(fsrs::dayNumber(1772452800u, 0), 20514u);  // noon, same day

  // The same local wall clock at +05:30 and at -08:00 crosses at the same
  // local instant, not at the same UTC instant.
  EXPECT_EQ_U(fsrs::dayNumber(1772404199u, 19800), 20513u);
  EXPECT_EQ_U(fsrs::dayNumber(1772404200u, 19800), 20514u);
  EXPECT_EQ_U(fsrs::dayNumber(1772452799u, -28800), 20513u);
  EXPECT_EQ_U(fsrs::dayNumber(1772452800u, -28800), 20514u);

  // Whole days step by exactly one, all day long.
  for (uint32_t hour = 0; hour < 24; ++hour) {
    const uint32_t base = 1772400000u + hour * 3600u;
    EXPECT_EQ_U(static_cast<uint16_t>(fsrs::dayNumber(base + SECONDS_PER_DAY, 0) - fsrs::dayNumber(base, 0)), 1u);
    EXPECT_EQ_U(
        static_cast<uint16_t>(fsrs::dayNumber(base + 30u * SECONDS_PER_DAY, 19800) - fsrs::dayNumber(base, 19800)),
        30u);
  }

  // Before the first rollover the floor goes negative; spec §4 pins a CLAMP to
  // [0, 65535] rather than a wrap, so a corrupt RTC cannot alias onto a valid
  // day. 1970-01-01 00:00 UTC is mathematically study day -1 and reads as 0.
  EXPECT_EQ_U(fsrs::dayNumber(0u, 0), 0u);
  EXPECT_EQ_U(fsrs::dayNumber(4u * 3600u - 1u, 0), 0u);
  EXPECT_EQ_U(fsrs::dayNumber(4u * 3600u, 0), 0u);
  EXPECT_EQ_U(fsrs::dayNumber(0u, -28800), 0u);
  EXPECT_EQ_U(fsrs::dayNumber(SECONDS_PER_DAY, -28800), 0u);  // day 0 starts later at -08:00
  // The largest UTC offset anyone runs (+14:00) cannot push a zero clock above
  // the first rollover either, so the clamp is one-directional at this end.
  EXPECT_EQ_U(fsrs::dayNumber(0u, 14 * 3600), 0u);

  // u16 sanity: a u32 unix clock cannot reach the top of the range, so the
  // upper clamp is unreachable in practice. The largest day number a u32
  // timestamp can produce is 49710 (2106-02-07), well inside the u16, and
  // day + the 36500 d ceiling still fits the u32 `due` field.
  EXPECT_EQ_U(fsrs::dayNumber(0xFFFFFFFFu, 0), 49710u);
  EXPECT(49710u + 36500u < 0x1FFFFu);
  EXPECT(fsrs::dayNumber(0xFFFFFFFFu, 14u * 3600u) < 0xFFFFu);
}

// The port counts elapsed time in local day numbers, py-fsrs in a rolling 24 h
// window (README deviation 1). The generator refuses to emit a vector where the
// two disagree, so the disagreement itself has to be pinned by hand: a card
// answered at 23:00 and again at 05:00 is "the same day" to py-fsrs and "one day
// later" here, which is the long-term stability path rather than the short-term
// one. This is the behaviour Anki has and the spec §4 requires.
void testRolloverDeviation() {
  beginGroup("day-number deviation from py-fsrs");
  const Params params = testParams();

  // 2026-03-02 23:00 UTC, and 06:00 later (05:00 the next morning, past the
  // 04:00 rollover). Six hours apart: py-fsrs would call that zero days.
  const uint32_t evening = 1772492400u;  // 2026-03-02 23:00 UTC
  const uint32_t morning = evening + 6u * 3600u;
  const Now atEvening = momentAt(evening);
  const Now atMorning = momentAt(morning);
  EXPECT_EQ_U(atMorning.dayNumber - atEvening.dayNumber, 1u);
  EXPECT_EQ_U((morning - evening) / SECONDS_PER_DAY, 0u);  // py-fsrs would see 0 days

  CardState learning = {};
  learning.state = CardPhase::Learning;
  learning.step = 1;
  learning.stability = 2.3065f;
  learning.difficulty = 2.1181f;
  learning.lastReviewDay = atEvening.dayNumber;
  learning.reps = 1;

  // Same evening, four hours on: still the same study day, short-term path.
  const CardState sameDay = fsrs::gradeCard(params, learning, Grade::Good, momentAt(evening + 4u * 3600u), TEST_KEY);
  // Next morning: one elapsed day, long-term path, and a much larger stability.
  const CardState nextMorning = fsrs::gradeCard(params, learning, Grade::Good, atMorning, TEST_KEY);
  EXPECT(nextMorning.stability > sameDay.stability * 2.0f);

  // ...and it is exactly the interday answer, not something in between: the
  // learning_nextday_good vector graded a whole 24 h later lands on the same
  // stability, because both are "one elapsed day".
  const CardState fullDay =
      fsrs::gradeCard(params, learning, Grade::Good, momentAt(evening + SECONDS_PER_DAY), TEST_KEY);
  EXPECT_NEAR_REL(nextMorning.stability, fullDay.stability, 1e-6);

  // The short-term path is what an answer before the rollover gets, right up to
  // 03:59:59 the following morning.
  const CardState beforeRollover =
      fsrs::gradeCard(params, learning, Grade::Good, momentAt(evening + 5u * 3600u - 1u), TEST_KEY);
  EXPECT_NEAR_REL(beforeRollover.stability, sameDay.stability, 1e-6);
}

// --- 6. same-day path, overdue credit, clock jumps ----------------------------

CardState reviewCard(float stability, float difficulty, uint16_t lastDay) {
  CardState card = {};
  card.state = CardPhase::Review;
  card.stability = stability;
  card.difficulty = difficulty;
  card.lastReviewDay = lastDay;
  card.reps = 6;
  return card;
}

void testElapsedPaths() {
  beginGroup("short-term / overdue / clock jump");
  const Params params = testParams();

  const CardState card = reviewCard(10.0f, 5.0f, 20514);
  const CardState sameDay = fsrs::gradeCard(params, card, Grade::Good, momentAt(1772452800u), TEST_KEY);
  const CardState nextDay =
      fsrs::gradeCard(params, card, Grade::Good, momentAt(1772452800u + SECONDS_PER_DAY), TEST_KEY);
  const CardState lateDay =
      fsrs::gradeCard(params, card, Grade::Good, momentAt(1772452800u + 60u * SECONDS_PER_DAY), TEST_KEY);

  // The short-term path must not be the long-term formula with t = 0: it gives
  // a much smaller increase than even a single elapsed day.
  EXPECT(sameDay.stability < nextDay.stability);
  // Overdue answers earn more stability, because retrievability decayed.
  EXPECT(nextDay.stability < lateDay.stability);
  // ...and it is a real difference, not float noise.
  EXPECT(lateDay.stability > nextDay.stability * 1.05f);

  // A clock that jumped backwards must read as "same day", never as a huge
  // negative elapsed count.
  const CardState jumped =
      fsrs::gradeCard(params, card, Grade::Good, momentAt(1772452800u - 40u * SECONDS_PER_DAY), TEST_KEY);
  EXPECT_NEAR_REL(jumped.stability, sameDay.stability, 1e-6);

  // An unreviewed card (lastReviewDay == 0) in a non-new state must not be read
  // as ~20000 days overdue.
  CardState orphan = reviewCard(10.0f, 5.0f, 0);
  const CardState orphanGraded = fsrs::gradeCard(params, orphan, Grade::Good, momentAt(1772452800u), TEST_KEY);
  EXPECT_NEAR_REL(orphanGraded.stability, sameDay.stability, 1e-6);
}

// --- 7. counters, clamps, saturation -------------------------------------------

void testCountersAndClamps() {
  beginGroup("counters and clamps");
  Params params = testParams();

  // Lapses count Review-state Agains only.
  CardState card = reviewCard(20.0f, 5.0f, 20500);
  const Now now = momentAt(1772452800u);
  const CardState lapsed = fsrs::gradeCard(params, card, Grade::Again, now, TEST_KEY);
  EXPECT_EQ_U(lapsed.lapses, 1u);
  EXPECT(lapsed.state == CardPhase::Relearning);
  EXPECT_EQ_U(lapsed.step, 0u);
  EXPECT_EQ_U(lapsed.due - now.unixSecs, 600u);
  // Failing again while relearning is not a second lapse.
  EXPECT_EQ_U(fsrs::gradeCard(params, lapsed, Grade::Again, now, TEST_KEY).lapses, 1u);
  // Neither is failing a learning card.
  CardState learning = newCard();
  EXPECT_EQ_U(fsrs::gradeCard(params, learning, Grade::Again, now, TEST_KEY).lapses, 0u);

  // Saturation rather than wrap.
  CardState saturated = reviewCard(20.0f, 5.0f, 20500);
  saturated.lapses = 255;
  saturated.reps = 65535;
  const CardState still = fsrs::gradeCard(params, saturated, Grade::Again, now, TEST_KEY);
  EXPECT_EQ_U(still.lapses, 255u);
  EXPECT_EQ_U(still.reps, 65535u);

  // Difficulty stays in [1, 10] and stability above the floor under abuse.
  CardState abused = newCard();
  uint32_t clock = 1772452800u;
  for (int i = 0; i < 400; ++i) {
    const Grade grade = (i % 5 == 0) ? Grade::Easy : Grade::Again;
    abused = fsrs::gradeCard(params, abused, grade, momentAt(clock), TEST_KEY);
    clock += SECONDS_PER_DAY;
    EXPECT(abused.difficulty >= 1.0f && abused.difficulty <= 10.0f);
    EXPECT(abused.stability >= 0.001f);
  }

  // The interval ceiling holds from an absurd stability. Fuzz-free it lands
  // exactly on the ceiling; fuzzed it may sit a band-width below, never above.
  CardState huge = reviewCard(1.0e9f, 1.0f, 20000);
  EXPECT_EQ_U(fsrs::gradeCard(params, huge, Grade::Easy, now, TEST_KEY).due - now.dayNumber, 36500u);
  Params fuzzed = fsrs::defaultParams();
  const CardState capped = fsrs::gradeCard(fuzzed, huge, Grade::Easy, now, TEST_KEY);
  EXPECT(capped.due - now.dayNumber <= 36500u);
  uint32_t ceilingLow = 0;
  uint32_t ceilingHigh = 0;
  fsrs::detail::fuzzBounds(fuzzed, 36500u, ceilingLow, ceilingHigh);
  EXPECT(capped.due - now.dayNumber >= ceilingLow);
  EXPECT_EQ_U(ceilingHigh, 36500u);
  const fsrs::Preview cappedPreview = fsrs::previewIntervals(fuzzed, huge, now);
  for (uint8_t index = 1; index < fsrs::GRADE_COUNT; ++index) {
    EXPECT_EQ_U(cappedPreview.next[index], 36500u);  // preview is fuzz-free
  }

  // A custom ceiling is honoured everywhere.
  Params tight = testParams();
  tight.maximumIntervalDays = 30;
  EXPECT_EQ_U(fsrs::gradeCard(tight, huge, Grade::Easy, now, TEST_KEY).due - now.dayNumber, 30u);
}

// --- 8. step-table configuration ------------------------------------------------

void testStepTables() {
  beginGroup("step tables");
  const Now now = momentAt(1772452800u);

  // A single learning step: Hard repeats it at 1.5x, Good graduates.
  Params single = testParams();
  single.learningSteps[0] = 60u;
  single.learningStepCount = 1;
  CardState fresh = newCard();
  EXPECT_EQ_U(fsrs::gradeCard(single, fresh, Grade::Hard, now, TEST_KEY).due - now.unixSecs, 90u);
  const CardState graduated = fsrs::gradeCard(single, fresh, Grade::Good, now, TEST_KEY);
  EXPECT(graduated.state == CardPhase::Review);
  EXPECT_EQ_U(graduated.due - now.dayNumber, 2u);  // round(w[2]) with retention 0.9

  // Three steps: Good walks up the table one step at a time.
  Params three = testParams();
  three.learningSteps[0] = 60u;
  three.learningSteps[1] = 600u;
  three.learningSteps[2] = 3600u;
  three.learningStepCount = 3;
  CardState walking = newCard();
  walking = fsrs::gradeCard(three, walking, Grade::Good, now, TEST_KEY);
  EXPECT_EQ_U(walking.step, 1u);
  EXPECT_EQ_U(walking.due - now.unixSecs, 600u);
  walking = fsrs::gradeCard(three, walking, Grade::Good, now, TEST_KEY);
  EXPECT_EQ_U(walking.step, 2u);
  EXPECT_EQ_U(walking.due - now.unixSecs, 3600u);
  // Hard past the first step repeats the current step verbatim.
  EXPECT_EQ_U(fsrs::gradeCard(three, walking, Grade::Hard, now, TEST_KEY).due - now.unixSecs, 3600u);
  walking = fsrs::gradeCard(three, walking, Grade::Good, now, TEST_KEY);
  EXPECT(walking.state == CardPhase::Review);

  // A card stranded past the end of a shortened table graduates instead of
  // reading off the end; Again still restarts it at step 0.
  CardState stranded = newCard();
  stranded.state = CardPhase::Learning;
  stranded.step = 3;
  stranded.stability = 2.0f;
  stranded.difficulty = 5.0f;
  stranded.lastReviewDay = 20500;
  stranded.reps = 2;
  Params two = testParams();
  EXPECT(fsrs::gradeCard(two, stranded, Grade::Good, now, TEST_KEY).state == CardPhase::Review);
  const CardState restarted = fsrs::gradeCard(two, stranded, Grade::Again, now, TEST_KEY);
  EXPECT(restarted.state == CardPhase::Learning);
  EXPECT_EQ_U(restarted.step, 0u);

  // No relearning steps: a failed review card stays in Review, per py-fsrs.
  Params noRelearn = testParams();
  noRelearn.relearningStepCount = 0;
  const CardState stillReview = fsrs::gradeCard(noRelearn, reviewCard(20.0f, 5.0f, 20500), Grade::Again, now, TEST_KEY);
  EXPECT(stillReview.state == CardPhase::Review);
  EXPECT(stillReview.due - now.dayNumber >= 1u);

  // No learning steps at all: a new card goes straight to Review.
  Params noLearn = testParams();
  noLearn.learningStepCount = 0;
  EXPECT(fsrs::gradeCard(noLearn, fresh, Grade::Again, now, TEST_KEY).state == CardPhase::Review);
}

void testGraduationFloors() {
  beginGroup("graduation floors");
  const Now now = momentAt(1772452800u);
  CardState fresh = newCard();

  // Spec §0.4: graduation intervals come from FSRS stability, so both floors
  // are 0 by default and a graduation is the plain FSRS interval -- 2 d for
  // Good, 8 d for Easy with the default weights.
  EXPECT_EQ_U(fsrs::defaultParams().minGraduatingIntervalDays, 0u);
  EXPECT_EQ_U(fsrs::defaultParams().minEasyIntervalDays, 0u);
  Params defaults = testParams();
  defaults.learningStepCount = 1;
  EXPECT_EQ_U(fsrs::gradeCard(defaults, fresh, Grade::Good, now, TEST_KEY).due - now.dayNumber, 2u);
  EXPECT_EQ_U(fsrs::gradeCard(defaults, fresh, Grade::Easy, now, TEST_KEY).due - now.dayNumber, 8u);

  // At floor 0 the shortest graduation FSRS can produce is 1 d, and Easy out of
  // relearning then lands on 2 d -- from the hard < good < easy rule, not from
  // FSRS and not from a floor. (This is the VEC_RELEARNING_EASY step; the
  // vectors model the rule, so this is where it is actually checked.)
  CardState relearning = {};
  relearning.state = CardPhase::Relearning;
  relearning.stability = 0.6077f;
  relearning.difficulty = 7.392f;
  relearning.lastReviewDay = 20514;
  relearning.reps = 3;
  relearning.lapses = 1;
  const fsrs::Preview outOfRelearning = fsrs::previewIntervals(defaults, relearning, now);
  EXPECT_EQ_U(outOfRelearning.next[static_cast<uint8_t>(Grade::Good)], 1u);
  EXPECT_EQ_U(outOfRelearning.next[static_cast<uint8_t>(Grade::Easy)], 2u);

  // Raised, they do bind -- and only on graduation, not on later reviews.
  Params floored = defaults;
  floored.minGraduatingIntervalDays = 30;
  floored.minEasyIntervalDays = 50;
  EXPECT_EQ_U(fsrs::gradeCard(floored, fresh, Grade::Good, now, TEST_KEY).due - now.dayNumber, 30u);
  EXPECT_EQ_U(fsrs::gradeCard(floored, fresh, Grade::Easy, now, TEST_KEY).due - now.dayNumber, 50u);
  const CardState small = reviewCard(1.0f, 5.0f, 20513);
  EXPECT(fsrs::gradeCard(floored, small, Grade::Hard, now, TEST_KEY).due - now.dayNumber < 30u);

  // A floor above the ceiling still respects the ceiling.
  Params conflicting = floored;
  conflicting.maximumIntervalDays = 10;
  EXPECT_EQ_U(fsrs::gradeCard(conflicting, fresh, Grade::Easy, now, TEST_KEY).due - now.dayNumber, 10u);
}

// --- 9. corrupt records ----------------------------------------------------------

void testCorruptRecords() {
  beginGroup("corrupt records");
  // Spec §3 accepts that a torn sector can leave garbage in a state record, so
  // the scheduler has to survive one without producing NaN, a wild interval or
  // an out-of-range read.
  const Params params = testParams();
  const Now now = momentAt(1772452800u);

  CardState garbage[6] = {};
  // Zeroed memory state in a non-new phase (the common torn-write result).
  garbage[0].state = CardPhase::Review;
  garbage[0].lastReviewDay = 20500;
  // NaN and infinity.
  garbage[1] = garbage[0];
  garbage[1].stability = NAN;
  garbage[1].difficulty = NAN;
  garbage[2] = garbage[0];
  garbage[2].stability = INFINITY;
  garbage[2].difficulty = -INFINITY;
  // Negative and absurd values.
  garbage[3] = garbage[0];
  garbage[3].stability = -1000.0f;
  garbage[3].difficulty = 4000.0f;
  // An unknown state code and a step past the end of the table.
  garbage[4] = garbage[0];
  garbage[4].state = static_cast<CardPhase>(200);
  garbage[4].step = 250;
  garbage[4].stability = 5.0f;
  garbage[4].difficulty = 5.0f;
  garbage[5] = garbage[0];
  garbage[5].state = CardPhase::Learning;
  garbage[5].step = 250;
  garbage[5].stability = 5.0f;
  garbage[5].difficulty = 5.0f;

  for (const CardState& card : garbage) {
    for (uint8_t index = 0; index < fsrs::GRADE_COUNT; ++index) {
      const CardState graded = fsrs::gradeCard(params, card, static_cast<Grade>(index), now, TEST_KEY);
      EXPECT(!isnan(graded.stability) && !isinf(graded.stability));
      EXPECT(graded.stability >= 0.001f);
      EXPECT(graded.difficulty >= 1.0f && graded.difficulty <= 10.0f);
      EXPECT(static_cast<uint8_t>(graded.state) <= static_cast<uint8_t>(CardPhase::Relearning));
      if (graded.state == CardPhase::Review) {
        const uint32_t days = graded.due - now.dayNumber;
        EXPECT(days >= 1u && days <= 36500u);
      } else {
        const uint32_t secs = graded.due - now.unixSecs;
        EXPECT(secs >= 60u && secs <= 3600u);
      }
    }
    const fsrs::Preview preview = fsrs::previewIntervals(params, card, now);
    for (uint8_t index = 0; index < fsrs::GRADE_COUNT; ++index) {
      EXPECT(preview.next[index] > 0u);
    }
  }

  // Spec §4: an unknown state byte reads as New and the card RESTARTS. Sanity
  // is not enough here -- the outcome has to be exactly what a new card gets,
  // or "restart" could quietly mean "graduate".
  CardState unknownState = {};
  unknownState.state = static_cast<CardPhase>(200);
  unknownState.step = 250;
  unknownState.stability = 40.0f;  // a Review-grade memory state, to be discarded
  unknownState.difficulty = 5.0f;
  unknownState.lastReviewDay = 20000;
  unknownState.reps = 30;
  unknownState.lapses = 4;
  const CardState fromNew = fsrs::gradeCard(params, newCard(), Grade::Good, now, TEST_KEY);
  const CardState restarted = fsrs::gradeCard(params, unknownState, Grade::Good, now, TEST_KEY);
  EXPECT(restarted.state == CardPhase::Learning);
  EXPECT_EQ_U(restarted.step, 1u);
  EXPECT_EQ_U(restarted.due - now.unixSecs, 600u);
  // The initial-stability table, not the 40 d the torn record claimed.
  EXPECT_NEAR_REL(restarted.stability, fromNew.stability, 1e-6);
  EXPECT_NEAR_REL(restarted.difficulty, fromNew.difficulty, 1e-6);
  // Counters are history, not memory state: they are kept, not reset.
  EXPECT_EQ_U(restarted.reps, 31u);
  EXPECT_EQ_U(restarted.lapses, 4u);

  // A New card whose step byte is garbage must enter the table at step 0, not
  // read the garbage and "graduate" off the end of it.
  CardState newWithStep = newCard();
  newWithStep.step = 250;
  const CardState entered = fsrs::gradeCard(params, newWithStep, Grade::Good, now, TEST_KEY);
  EXPECT(entered.state == CardPhase::Learning);
  EXPECT_EQ_U(entered.step, 1u);
  EXPECT_EQ_U(entered.due - now.unixSecs, 600u);
  const CardState enteredAgain = fsrs::gradeCard(params, newWithStep, Grade::Again, now, TEST_KEY);
  EXPECT(enteredAgain.state == CardPhase::Learning);
  EXPECT_EQ_U(enteredAgain.step, 0u);
  EXPECT_EQ_U(enteredAgain.due - now.unixSecs, 60u);

  // A New card's stored `due` is ignored entirely (spec §3): it is overwritten
  // from the answer, never read.
  CardState newWithDue = newCard();
  newWithDue.due = 0xDEADBEEFu;
  const CardState overwritten = fsrs::gradeCard(params, newWithDue, Grade::Good, now, TEST_KEY);
  EXPECT_EQ_U(overwritten.due, now.unixSecs + 600u);

  // A Params with a step count past the table must not read off the end: keep
  // pressing Good and the card has to graduate inside MAX_STEPS, not walk on.
  Params broken = testParams();
  broken.learningStepCount = 250;
  broken.relearningStepCount = 250;
  CardState walking = newCard();
  bool graduated = false;
  for (int i = 0; i < 8 && !graduated; ++i) {
    walking = fsrs::gradeCard(broken, walking, Grade::Good, now, TEST_KEY);
    EXPECT(walking.step < fsrs::MAX_STEPS);
    graduated = walking.state == CardPhase::Review;
  }
  EXPECT(graduated);
}

// --- 10. record layout ----------------------------------------------------------

void testRecordLayout() {
  beginGroup("CPST record layout");
  // The static_asserts in the header do the real work; this keeps the numbers
  // visible in the test output as well.
  EXPECT_EQ_U(sizeof(CardState), 20u);
  EXPECT_EQ_U(sizeof(CardPhase), 1u);
  EXPECT_EQ_U(static_cast<uint8_t>(CardPhase::New), 0u);
  EXPECT_EQ_U(static_cast<uint8_t>(CardPhase::Learning), 1u);
  EXPECT_EQ_U(static_cast<uint8_t>(CardPhase::Review), 2u);
  EXPECT_EQ_U(static_cast<uint8_t>(CardPhase::Relearning), 3u);
  EXPECT_EQ_U(static_cast<uint8_t>(Grade::Again), 0u);
  EXPECT_EQ_U(static_cast<uint8_t>(Grade::Easy), 3u);

  // Defaults are the spec §0 numbers.
  const Params params = fsrs::defaultParams();
  EXPECT_NEAR_REL(params.desiredRetention, 0.90f, 1e-9);
  EXPECT_EQ_U(params.learningStepCount, 2u);
  EXPECT_EQ_U(params.learningSteps[0], 60u);
  EXPECT_EQ_U(params.learningSteps[1], 600u);
  EXPECT_EQ_U(params.relearningStepCount, 1u);
  EXPECT_EQ_U(params.relearningSteps[0], 600u);
  EXPECT_EQ_U(params.maximumIntervalDays, 36500u);
  EXPECT_EQ_U(params.minGraduatingIntervalDays, 0u);  // spec §0.4: FSRS decides
  EXPECT_EQ_U(params.minEasyIntervalDays, 0u);
  EXPECT(params.enableFuzz);
}

// --- 11. parameter validation -----------------------------------------------

// Spec §2.1/§4: deck-supplied w[21] must pass validateParams() at deck open or
// the deck is refused. The library never calls it itself, so these bounds are
// the only thing standing between a hand-edited deck header and a scheduler
// that quietly stops working.
void testValidateParams() {
  beginGroup("validateParams");
  const Params defaults = fsrs::defaultParams();
  EXPECT(fsrs::validateParams(defaults));

  // py-fsrs 6.3.2 LOWER_BOUNDS_PARAMETERS / UPPER_BOUNDS_PARAMETERS.
  const float lower[fsrs::PARAM_COUNT] = {0.001f, 0.001f, 0.001f, 0.001f, 1.0f,   0.001f, 0.001f,
                                          0.001f, 0.0f,   0.0f,   0.001f, 0.001f, 0.001f, 0.001f,
                                          0.0f,   0.0f,   1.0f,   0.0f,   0.0f,   0.0f,   0.1f};
  const float upper[fsrs::PARAM_COUNT] = {100.0f, 100.0f, 100.0f, 100.0f, 10.0f, 4.0f, 4.0f, 0.75f, 4.5f, 0.8f, 3.5f,
                                          5.0f,   0.25f,  0.9f,   4.0f,   1.0f,  6.0f, 2.0f, 2.0f,  0.8f, 0.8f};
  for (uint8_t index = 0; index < fsrs::PARAM_COUNT; ++index) {
    // The bounds themselves are inclusive and must pass.
    Params atLower = defaults;
    atLower.w[index] = lower[index];
    EXPECT(fsrs::validateParams(atLower));
    Params atUpper = defaults;
    atUpper.w[index] = upper[index];
    EXPECT(fsrs::validateParams(atUpper));

    // One ulp outside either bound must not.
    Params belowLower = defaults;
    belowLower.w[index] = nextafterf(lower[index], -INFINITY);
    EXPECT(!fsrs::validateParams(belowLower));
    Params aboveUpper = defaults;
    aboveUpper.w[index] = nextafterf(upper[index], INFINITY);
    EXPECT(!fsrs::validateParams(aboveUpper));

    // NaN fails every bound, in both directions.
    Params notANumber = defaults;
    notANumber.w[index] = NAN;
    EXPECT(!fsrs::validateParams(notANumber));
  }

  // The motivating case: w[20] = 0 makes the decay exponent -0.0 and the curve
  // factor infinite, so every raw interval collapses to the 1-day clamp (the
  // buttons then read 1/2/3 d only because the ordering rule spreads them) --
  // a 400-day-stability card would be shown again tomorrow, forever, with no
  // other symptom. Nothing downstream can detect that; the bound has to.
  Params zeroDecay = defaults;
  zeroDecay.w[20] = 0.0f;
  EXPECT(!fsrs::validateParams(zeroDecay));
  const Now now = momentAt(1772452800u);
  const CardState big = reviewCard(400.0f, 5.0f, 20000);
  const fsrs::Preview collapsed = fsrs::previewIntervals(zeroDecay, big, now);
  EXPECT(!collapsed.intraday[static_cast<uint8_t>(Grade::Easy)]);
  EXPECT_EQ_U(collapsed.next[static_cast<uint8_t>(Grade::Hard)], 1u);
  EXPECT(collapsed.next[static_cast<uint8_t>(Grade::Easy)] <= 3u);
  EXPECT(fsrs::gradeCard(defaults, big, Grade::Good, now, TEST_KEY).due - now.dayNumber > 100u);

  // Retention: Anki's own 0.70-0.99 range, inclusive.
  const float retentions[] = {0.70f, 0.90f, 0.99f};
  for (float retention : retentions) {
    Params ok = defaults;
    ok.desiredRetention = retention;
    EXPECT(fsrs::validateParams(ok));
  }
  const float badRetentions[] = {0.0f, 0.5f, 0.699f, 0.991f, 1.0f, 2.0f, -1.0f, NAN};
  for (float retention : badRetentions) {
    Params bad = defaults;
    bad.desiredRetention = retention;
    EXPECT(!fsrs::validateParams(bad));
  }

  // Maximum interval: spec §4's [1, 36500].
  const uint32_t goodCeilings[] = {1u, 30u, 36500u};
  for (uint32_t ceiling : goodCeilings) {
    Params ok = defaults;
    ok.maximumIntervalDays = ceiling;
    EXPECT(fsrs::validateParams(ok));
  }
  const uint32_t badCeilings[] = {0u, 36501u, 0xFFFFFFFFu};
  for (uint32_t ceiling : badCeilings) {
    Params bad = defaults;
    bad.maximumIntervalDays = ceiling;
    EXPECT(!fsrs::validateParams(bad));
  }

  // Step tables: counts inside MAX_STEPS, entries positive, strictly ascending,
  // and no longer than a month (a step is meant to be an intraday delay).
  Params tooManyLearning = defaults;
  tooManyLearning.learningStepCount = fsrs::MAX_STEPS + 1;
  EXPECT(!fsrs::validateParams(tooManyLearning));
  Params tooManyRelearning = defaults;
  tooManyRelearning.relearningStepCount = 250;
  EXPECT(!fsrs::validateParams(tooManyRelearning));

  Params zeroStep = defaults;
  zeroStep.learningSteps[1] = 0u;
  EXPECT(!fsrs::validateParams(zeroStep));
  Params descending = defaults;
  descending.learningSteps[0] = 600u;
  descending.learningSteps[1] = 60u;
  EXPECT(!fsrs::validateParams(descending));
  Params equalSteps = defaults;
  equalSteps.learningSteps[1] = equalSteps.learningSteps[0];
  EXPECT(!fsrs::validateParams(equalSteps));
  Params hugeStep = defaults;
  hugeStep.relearningSteps[0] = 32u * 86400u;
  EXPECT(!fsrs::validateParams(hugeStep));

  // Garbage past the in-use part of a table is ignored, and an empty table is
  // legal (0 learning steps sends new cards straight to Review).
  Params trailingGarbage = defaults;
  trailingGarbage.learningSteps[2] = 0u;
  trailingGarbage.learningSteps[3] = 5u;  // descending, but out of range
  EXPECT(fsrs::validateParams(trailingGarbage));
  Params noSteps = defaults;
  noSteps.learningStepCount = 0;
  noSteps.relearningStepCount = 0;
  EXPECT(fsrs::validateParams(noSteps));
}

// --- 12. flags and fuzzed intraday grades ------------------------------------

// The device runs with defaultParams(), where enableFuzz is true. Fuzz must
// never touch an intraday interval: a 10-minute learning step that came back
// as 9 or 11 days would be a scheduling bug, not a nudge.
void testFuzzNeverTouchesIntraday() {
  beginGroup("fuzz leaves intraday grades alone");
  Params params = fsrs::defaultParams();  // fuzz ON, as on the device
  EXPECT(params.enableFuzz);
  const Now now = momentAt(1772452800u);

  float value = 0.0f;
  fsrs::FuzzRng rng;
  rng.draw = &stubDraw;
  rng.ctx = &value;

  const float draws[] = {0.0f, 0.05f, 0.25f, 0.5f, 0.75f, 0.999f};
  for (float draw : draws) {
    value = draw;
    const CardState fresh = newCard();
    // A new card: Again 1 m, Hard mean(1 m, 10 m) = 330 s, Good the second step.
    const CardState again = fsrs::gradeCard(params, fresh, Grade::Again, now, TEST_KEY, rng);
    const CardState hard = fsrs::gradeCard(params, fresh, Grade::Hard, now, TEST_KEY, rng);
    const CardState good = fsrs::gradeCard(params, fresh, Grade::Good, now, TEST_KEY, rng);
    EXPECT(again.state == CardPhase::Learning && hard.state == CardPhase::Learning &&
           good.state == CardPhase::Learning);
    EXPECT_EQ_U(again.due - now.unixSecs, 60u);
    EXPECT_EQ_U(hard.due - now.unixSecs, 330u);
    EXPECT_EQ_U(good.due - now.unixSecs, 600u);

    // Same for a lapse out of Review, which lands on the relearning step.
    const CardState lapsed = fsrs::gradeCard(params, reviewCard(30.0f, 5.0f, 20500), Grade::Again, now, TEST_KEY, rng);
    EXPECT(lapsed.state == CardPhase::Relearning);
    EXPECT_EQ_U(lapsed.due - now.unixSecs, 600u);
  }

  // The same draws do move the day-scale grades, so the assertions above are
  // about the intraday guard and not about a dead RNG.
  value = 0.0f;
  const uint32_t lowEasy = fsrs::gradeCard(params, newCard(), Grade::Easy, now, TEST_KEY, rng).due;
  value = 0.999f;
  const uint32_t highEasy = fsrs::gradeCard(params, newCard(), Grade::Easy, now, TEST_KEY, rng).due;
  EXPECT(lowEasy < highEasy);
}

// R3's suspend feature reads this bit back after every answer.
void testFlagsArePreserved() {
  beginGroup("flags survive gradeCard");
  const Params params = testParams();
  const Now now = momentAt(1772452800u);

  const CardState states[] = {newCard(), reviewCard(20.0f, 5.0f, 20500)};
  for (const CardState& base : states) {
    CardState card = base;
    card.flags = fsrs::FLAG_SUSPENDED;
    for (uint8_t index = 0; index < fsrs::GRADE_COUNT; ++index) {
      EXPECT_EQ_U(fsrs::gradeCard(params, card, static_cast<Grade>(index), now, TEST_KEY).flags, fsrs::FLAG_SUSPENDED);
    }
    // Unknown bits are carried too: the scheduler owns none of this byte.
    CardState reserved = base;
    reserved.flags = 0xFEu;
    EXPECT_EQ_U(fsrs::gradeCard(params, reserved, Grade::Good, now, TEST_KEY).flags, 0xFEu);
    CardState clear = base;
    EXPECT_EQ_U(fsrs::gradeCard(params, clear, Grade::Good, now, TEST_KEY).flags, 0u);
  }
}

}  // namespace

int main() {
  testScenarioVectors();
  testFuzzVectors();
  testFuzzBands();
  testFuzzIsDeterministicAndSeeded();
  testMonotonicity();
  testPreview();
  testDayNumbers();
  testElapsedPaths();
  testCountersAndClamps();
  testStepTables();
  testGraduationFloors();
  testCorruptRecords();
  testRecordLayout();
  testValidateParams();
  testFuzzNeverTouchesIntraday();
  testFlagsArePreserved();
  testRolloverDeviation();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    printf("FSRS HOST TESTS FAILED\n");
    return 1;
  }
  printf("FSRS HOST TESTS PASSED\n");
  return 0;
}
