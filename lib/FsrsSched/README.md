# FsrsSched — FSRS-6 scheduler for CROSSPOINT_FLASHCARDS

A pure C++ port of the FSRS-6 spaced-repetition scheduler plus the Anki-style
learning/relearning step machine that wraps it. Everything in this library is
gated on `CROSSPOINT_FLASHCARDS`: both files are wrapped whole in the guard, so
the translation unit is empty in every other build (PlatformIO compiles all of
`lib/` into every environment).

- **No** Arduino / SDK / HAL / FreeRTOS includes. `<cstdint>`, `<cstddef>`,
  `<math.h>`, nothing else.
- **No** allocation, no globals, no I/O, no time source. `Now` and `Params` are
  passed in; every function returns by value. Stack use is a few hundred bytes
  (four candidate `Outcome`s, 28 B each).
- **Single precision only** (`expf`/`powf`/`rintf`), because the target FPU is.

## Ported from

`py-fsrs` **6.3.2** (PyPI package `fsrs==6.3.2`), file `fsrs/scheduler.py`. The
formula helpers, the clamps, the fuzz bands and the two step-machine blocks
follow that file statement for statement; `FsrsSched.cpp` names the py-fsrs
method each helper corresponds to. Default weights are py-fsrs'
`DEFAULT_PARAMETERS` (w[20] = `FSRS_DEFAULT_DECAY` = 0.1542).

The weights, the formulas and the parameter bounds `validateParams()` enforces
all come straight from the installed py-fsrs source, which is what
`FLASHCARD_SPEC.md` §4's first sentence asks for.

## API

```cpp
fsrs::Params  params = fsrs::defaultParams();     // spec §0.4 defaults
// Deck-supplied w[21] (CPDK flags bit 0) — the DEVICE validates, and refuses
// the deck when this returns false (spec §2.1/§4). The library never calls it.
if (deckHasParams && !fsrs::validateParams(params)) { /* refuse the deck */ }
fsrs::Now     now{unixSecs, fsrs::dayNumber(unixSecs, utcOffsetSecs)};
fsrs::CardState next = fsrs::gradeCard(params, card, fsrs::Grade::Good, now, cardKey);
fsrs::Preview   hint = fsrs::previewIntervals(params, card, now);
```

`validateParams()` carries py-fsrs 6.3.2's `LOWER_BOUNDS_PARAMETERS` /
`UPPER_BOUNDS_PARAMETERS` (the 21 pairs its `Scheduler._validate_parameters`
raises on) plus bounds py-fsrs has no opinion on: `desiredRetention` in
[0.70, 0.99] (Anki's own slider range), step tables positive, strictly
ascending and at most a month per step, `maximumIntervalDays` in [1, 36500].
NaN fails everything. It exists because out-of-bounds weights are not merely
inaccurate — `w[20] == 0` gives a `-0.0` decay exponent, an infinite curve
factor and a 1-day interval for every card forever, with no other symptom.

`CardState` is byte-identical to the 20-byte payload of a CPST record
(`FLASHCARD_SPEC.md` §3); the record's leading u64 key lives with the caller and
is passed to `gradeCard` for the fuzz seed. `static_assert`s in the header pin
every field offset, so a layout change breaks the build rather than the file
format.

`due` is a **local day number** while the card is in `Review` and **unix
seconds** while it is in `Learning`/`Relearning`, matching the spec.

## Deviations from py-fsrs, and why

1. **Elapsed time is counted in local day numbers, not datetimes.** py-fsrs uses
   `(now - last_review).days`, a rolling 24 h window; this port uses
   `today − lastReviewDay` with the 04:00 rollover, which is what Anki does and
   what the spec's day-number `due` field requires. The two agree whenever
   reviews are a whole number of days apart — the host-test generator asserts
   exactly that for every vector it emits, which also means **no vector can
   cover the disagreement**; `testRolloverDeviation` pins it by hand. They
   differ for a review at 23:00 followed by one at 05:00 (py-fsrs: same day,
   short-term path; here: one day elapsed, long-term path). The rollover
   behaviour is the correct one for a daily-queue scheduler.
2. **`hard < good < easy` is enforced after fuzz.** py-fsrs has no such rule;
   `FLASHCARD_SPEC.md` §4 requires it and Anki does it. Only Good and Easy are
   ever moved, and only upwards. Consequence: at the 36500-day ceiling the three
   can tie, because the ceiling wins over strictness (Anki has the same corner).
   `gen_vectors.py` *models* the rule so that the vectors stay comparable, so
   the vectors are not evidence for it — `testMonotonicity` is the check that
   proves it, over a stability × difficulty × elapsed × draw sweep.
3. **Graduation floors, defaulting to 0 (off).** Spec §0.4 pins Anki's
   behaviour: under FSRS the graduation interval is the FSRS interval for the
   card's new stability, and the SM-2-era graduating/easy-interval settings are
   ignored. `Params::minGraduatingIntervalDays` / `minEasyIntervalDays` remain
   for a caller who wants them back; at their 0 default they never fire. Two
   consequences worth knowing:
   - they are applied **pre-fuzz**, so a nonzero floor floors the FSRS interval,
     not the interval the card is actually given — the fuzz band around a
     floored interval reaches below it. Moving them after fuzz would break the
     draw-for-draw fuzz equivalence with py-fsrs, so this is documented rather
     than fixed;
   - at floor 0, Easy out of relearning on a low-stability card comes out at
     2 days. That 2 is the `hard < good < easy` rule bumping it over Good's
     1 day — it is not FSRS's number and it is not a floor.
4. **One fuzz draw per answer, shared by all four grades**, seeded from
   (card key, pre-answer reps) — Anki's scheme. It keeps the fuzzed intervals in
   proportion so the ordering rule almost never has to fire, and it makes a
   session reproducible for the host tests. py-fsrs calls `random()` per grade.
5. **`previewIntervals` is fuzz-free** (documented in the header). A fuzzed
   preview would change every time the answer screen is redrawn. The number on
   the button can therefore differ from the interval actually assigned by up to
   the fuzz band (±5–15%).
6. **Float rather than double.** Worst observed disagreement with py-fsrs on
   stability and difficulty across the 282 vector steps is 4.4e-6 relative,
   three orders of magnitude inside the 1e-4 test tolerance.

   Day intervals are compared **exactly** and agree on every vector step, but
   that is not a guarantee: the port rounds a single-precision interval where
   py-fsrs rounds a double one, so whenever the pre-round value sits within a
   float ULP of `.5` the two can land a day apart. Measured over the
   differential fuzzer, that is ~0.02% of steps at the default weights and
   ~0.3% with arbitrary in-bounds weights. A one-day difference on one review
   is scheduling noise well inside the ±5–15% fuzz band, so it is accepted
   rather than worked around; do not read "exact" as "provably identical".
7. **Corrupt input is sanitised, not propagated.** Spec §3 accepts that a torn
   sector can leave garbage in a state record, so an unknown `state` byte reads
   as `New`, a step past the end of the table graduates, and stability and
   difficulty are clamped on the way in as well as on the way out. Stability also
   gets an upper clamp (1e9, four orders of magnitude past the interval ceiling)
   that py-fsrs does not have, so that an infinity read from a torn record cannot
   be written back and poison the card permanently. None of this is reachable
   from a value a real review can produce.
8. **Sub-second truncation.** A "Hard" repeat of a single learning step is
   `step × 1.5` and a repeat of the first of several is `mean(step0, step1)`;
   both are computed in whole seconds, where py-fsrs would keep a half second.
   With the 1m/10m defaults both are exact (90 s, 330 s).

Two py-fsrs quirks are reproduced deliberately rather than fixed:

- the fuzz draw spans `[low, high + 1)` and rounds, so a fuzzed interval can
  land one day above the computed upper bound (only the maximum interval caps
  it), and
- the `max(2, ...)` floor on the fuzz lower bound is unreachable with the
  standard band table (fuzzing starts at 3 days and delta ≥ 1). It is kept for
  fidelity; the host test documents that it cannot bind.

## Graduating / easy interval: settled

Anki ignores the SM-2-era graduating- and easy-interval settings once FSRS is
enabled — under FSRS the graduation interval *is* the FSRS interval for the
card's new stability (2 d for Good, 8 d for Easy at the default weights).
`FLASHCARD_SPEC.md` §0.4 pins that behaviour for this device too.

The floors therefore **exist but default to 0**:
`Params::minGraduatingIntervalDays` and `Params::minEasyIntervalDays` are
lower bounds applied on a graduation only, left in place for a caller who wants
Anki's pre-FSRS behaviour back. Nothing on the device sets them, and
`gen_vectors.py` models them from the same two constants, so raising either one
is a two-file change (`defaultParams()` plus the generator's
`GRADUATING_FLOOR_DAYS` / `EASY_FLOOR_DAYS`) and a vector regeneration.

See deviation 3 above for the two things to know before raising them: the floor
is applied pre-fuzz, and at floor 0 an Easy graduation out of relearning still
reads 2 d because of the ordering rule rather than because of a floor.

## Host tests

Not built by PlatformIO (`host_tests/` is neither `src/` nor `lib/`):

```sh
cd host_tests/fsrs
./build.sh                       # compiles with g++ -DCROSSPOINT_FLASHCARDS and runs
```

`build.sh` also compiles the library **without** the flag and asserts the object
file defines no symbols, which is the guard requirement.

To regenerate the vectors (only needed when bumping the py-fsrs version):

```sh
cd host_tests/fsrs
python3 -m venv .venv && .venv/bin/pip install 'fsrs==6.3.2'
.venv/bin/python gen_vectors.py > FsrsVectors.inc
```

The suite covers every grade out of every state, graduation, learn-today /
answer-tomorrow, lapse into relearning, same-day re-review, overdue reviews (up
to 2000 days late), a lapse chain overdue enough to drive `nextForgetStability`
onto its `min()` short-term arm, chains of 24–30 answers, the fuzz bands and
their accumulation, the intraday-grade fuzz guard, RNG injection and
determinism, post-fuzz monotonicity across a stability/difficulty/draw sweep,
`validateParams` at and just outside every bound, the 04:00 rollover with
positive and negative UTC offsets, the day-number clamp, the deliberate
rollover deviation from py-fsrs, flag preservation, corrupt-record restarts,
the counters and every clamp.

What the vectors can and cannot prove: the generator *models* the port's two
non-py-fsrs interval rules (`hard < good < easy` and the graduation floors) and
asserts that its day-number deltas match py-fsrs', so the vectors are evidence
for the FSRS formulas only. The rules themselves, and the day-number deviation,
are checked by `testMonotonicity`, `testGraduationFloors` and
`testRolloverDeviation` — those are authoritative, not the vectors.
