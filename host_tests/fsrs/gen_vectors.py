#!/usr/bin/env python3
"""Generate FSRS-6 reference vectors for the C++ port in lib/FsrsSched.

Runs the real py-fsrs scheduler over scripted review sequences and emits a
C++ include (`FsrsVectors.inc`) that the host test replays against the port.

Usage (from this directory, with py-fsrs installed):

    python3 -m venv .venv && .venv/bin/pip install 'fsrs==6.3.2'
    .venv/bin/python gen_vectors.py > FsrsVectors.inc

Two vector families are produced:

* Scenario vectors -- fuzzing disabled, so every interval is the raw formula
  output. Each step records the grade, the seconds to advance the clock, and
  the resulting state/step/stability/difficulty/interval.
* Fuzz vectors -- py-fsrs `_get_fuzzed_interval` with `random()` stubbed to a
  fixed value, so the C++ fuzz path can be compared draw for draw.

The device works in local day numbers, py-fsrs in datetimes. The generator
asserts, for every step, that the day-number delta (04:00 rollover, UTC offset
0) equals py-fsrs' `days_since_last_review`; the vectors are only meaningful
while that holds, so a scenario that drifts across a rollover fails here rather
than silently disagreeing in C++.
"""

from __future__ import annotations

import sys
from datetime import datetime, timedelta, timezone

import fsrs
from fsrs import Card, Rating, Scheduler, State

ROLLOVER_SECONDS = 4 * 3600
SECONDS_PER_DAY = 86400

# Noon UTC on 2026-03-02. Mid-day keeps every intraday step inside one rollover
# day, which is what the day-number assertion below checks.
BASE = datetime(2026, 3, 2, 12, 0, 0, tzinfo=timezone.utc)


def day_number(moment: datetime, utc_offset_secs: int = 0) -> int:
    """The device's dayNumber(), in Python -- unmasked, see MAX_DAY_NUMBER."""
    shifted = int(moment.timestamp()) + utc_offset_secs - ROLLOVER_SECONDS
    return shifted // SECONDS_PER_DAY


# The device stores day numbers in a u16. A vector must never walk a card past
# the wrap, or the C++ elapsed-days subtraction would not match py-fsrs.
MAX_DAY_NUMBER = 0xFFFF


def scheduler() -> Scheduler:
    return Scheduler(enable_fuzzing=False)


# --- scenario description ----------------------------------------------------
#
# A step is (rating, advance) where advance is one of:
#   ("due", extra_days)  -- move the clock to the card's due date, plus extra
#   ("secs", n)          -- move the clock forward n seconds
#   ("days", n)          -- move the clock forward n whole days

DUE = ("due", 0)


def due_plus(days: int):
    return ("due", days)


def after(seconds: int):
    return ("secs", seconds)


def after_days(days: int):
    return ("days", days)


ALL_RATINGS = (Rating.Again, Rating.Hard, Rating.Good, Rating.Easy)

# Prefixes that park a card in each state.
PREFIX_NEW: list = []
PREFIX_LEARN_STEP1 = [(Rating.Good, after(0))]
PREFIX_REVIEW = [(Rating.Good, after(0)), (Rating.Good, DUE)]
PREFIX_RELEARNING = PREFIX_REVIEW + [(Rating.Again, DUE)]


def build_scenarios() -> list[tuple[str, list]]:
    scenarios: list[tuple[str, list]] = []

    # Every grade out of every state, answered on the due date.
    for rating in ALL_RATINGS:
        scenarios.append((f"new_{rating.name.lower()}", PREFIX_NEW + [(rating, after(0))]))
    for rating in ALL_RATINGS:
        scenarios.append(
            (f"learning_step1_{rating.name.lower()}", PREFIX_LEARN_STEP1 + [(rating, DUE)])
        )
    for rating in ALL_RATINGS:
        scenarios.append((f"review_{rating.name.lower()}", PREFIX_REVIEW + [(rating, DUE)]))
    for rating in ALL_RATINGS:
        scenarios.append(
            (f"relearning_{rating.name.lower()}", PREFIX_RELEARNING + [(rating, DUE)])
        )

    # Same-day re-review of a Review card: the short-term stability path.
    for rating in ALL_RATINGS:
        scenarios.append(
            (f"review_sameday_{rating.name.lower()}", PREFIX_REVIEW + [(rating, after(600))])
        )

    # Overdue: elapsed far past the interval, credited through retrievability.
    for extra in (10, 60, 200, 2000):
        for rating in ALL_RATINGS:
            scenarios.append(
                (
                    f"overdue_{extra}d_{rating.name.lower()}",
                    PREFIX_REVIEW + [(rating, due_plus(extra))],
                )
            )

    # Learning-step machine corners.
    scenarios.append(("learn_hard_first_step", [(Rating.Hard, after(0))]))
    scenarios.append(
        ("learn_hard_second_step", [(Rating.Good, after(0)), (Rating.Hard, DUE)])
    )
    scenarios.append(
        ("learn_again_from_step1", [(Rating.Good, after(0)), (Rating.Again, DUE)])
    )
    scenarios.append(
        (
            "learn_full_walk",
            [
                (Rating.Again, after(0)),
                (Rating.Hard, DUE),
                (Rating.Good, DUE),
                (Rating.Good, DUE),
                (Rating.Easy, DUE),
            ],
        )
    )

    # Learn today, answer tomorrow -- the most common real path there is, and
    # the only one that walks a Learning card through the long-term stability
    # formula. Every other learning scenario above stays inside one rollover day.
    for rating in ALL_RATINGS:
        scenarios.append(
            (
                f"learning_nextday_{rating.name.lower()}",
                [(Rating.Good, after(0)), (rating, after_days(1))],
            )
        )

    # Lapse and recovery.
    scenarios.append(
        (
            "lapse_and_recover",
            PREFIX_REVIEW
            + [
                (Rating.Again, DUE),
                (Rating.Good, DUE),
                (Rating.Good, DUE),
                (Rating.Good, DUE),
            ],
        )
    )

    # A card lapsed repeatedly until its stability collapses, then failed again
    # after a very long overdue gap. This is the only shape that drives the
    # `min(longTerm, shortTerm)` cap in `nextForgetStability` onto its shortTerm
    # arm: it needs a low stability AND a retrievability far below 0.9, which
    # only a many-hundred-day gap on a sub-day-stability card produces. Without
    # it the cap is dead weight in the vectors (a `return longTerm` mutant
    # survives), and it masks up to ~28% of stability on the steps below.
    scenarios.append(
        (
            "forget_cap_overdue_lapse",
            PREFIX_REVIEW
            + [
                (Rating.Again, DUE),
                (Rating.Again, after(600)),
                (Rating.Good, after(600)),
                (Rating.Again, due_plus(300)),
                (Rating.Again, after(600)),
                (Rating.Good, after(600)),
                (Rating.Again, due_plus(900)),  # shortTerm arm wins here
                (Rating.Again, after(600)),
                (Rating.Good, after(600)),
                (Rating.Again, due_plus(1500)),  # ...and here, by more
            ],
        )
    )
    # The same cap reached from the learning phase rather than through a lapse.
    scenarios.append(
        (
            "forget_cap_learning_overdue",
            [
                (Rating.Again, after(0)),
                (Rating.Again, after_days(200)),
                (Rating.Again, after_days(600)),
                (Rating.Again, after_days(1500)),
            ],
        )
    )

    # Long chains: 25+ answers, always on the due date, with grade patterns that
    # walk in and out of relearning.
    pattern_a = [
        Rating.Good,
        Rating.Good,
        Rating.Hard,
        Rating.Good,
        Rating.Easy,
        Rating.Good,
        Rating.Again,
        Rating.Good,
    ]
    scenarios.append(
        ("chain_mixed_28", [(pattern_a[i % len(pattern_a)], DUE) for i in range(28)])
    )
    # Following the due date with nothing but Good runs the interval up to the
    # 36500 d ceiling; stop before the cumulative elapsed days leave the u16.
    scenarios.append(("chain_good_due_11", [(Rating.Good, DUE) for _ in range(11)]))
    # Fixed-spacing chains stay inside the u16 while still running 24+ answers.
    scenarios.append(
        ("chain_good_30d", [(Rating.Good, after_days(30)) for _ in range(30)])
    )
    scenarios.append(
        ("chain_easy_90d", [(Rating.Easy, after_days(90)) for _ in range(24)])
    )
    scenarios.append(("chain_hard_24", [(Rating.Hard, DUE) for _ in range(24)]))
    # A chain answered late every time, so retrievability stays far below 0.9.
    # The periodic Again keeps the interval (and so the elapsed-day total) inside
    # the u16 while still running past 20 answers.
    pattern_b = [Rating.Good, Rating.Good, Rating.Again, Rating.Hard, Rating.Good]
    scenarios.append(
        (
            "chain_overdue_25",
            [(pattern_b[i % len(pattern_b)], due_plus(2 + (i % 5))) for i in range(25)],
        )
    )

    return scenarios


MAXIMUM_INTERVAL = 36500

# FLASHCARD_SPEC.md §0.4: graduation intervals come from FSRS stability, so the
# port's floors default to 0 and this modelling is inert. Kept (rather than
# deleted) so that raising Params::minGraduatingIntervalDays in a future round
# only needs these two numbers changed to regenerate matching vectors.
GRADUATING_FLOOR_DAYS = 0
EASY_FLOOR_DAYS = 0

ORDERED_RATINGS = (Rating.Hard, Rating.Good, Rating.Easy)


def apply_graduation_floor(previous_state: State, rating: Rating, card: Card, days: int) -> int:
    if previous_state == State.Review or card.state != State.Review:
        return days
    floor = EASY_FLOOR_DAYS if rating == Rating.Easy else GRADUATING_FLOOR_DAYS
    return min(max(days, floor), MAXIMUM_INTERVAL)


def candidate_days(sched: Scheduler, card: Card, cursor: datetime, rating: Rating):
    """One grade's day interval, or None when the grade stays intraday."""
    previous_state = card.state
    reviewed, _ = sched.review_card(card=card, rating=rating, review_datetime=cursor)
    if reviewed.state != State.Review:
        return None
    days = (reviewed.due - cursor).days
    return apply_graduation_floor(previous_state, rating, reviewed, days)


def ordered_intervals(sched: Scheduler, card: Card, cursor: datetime) -> dict:
    """Day intervals for Hard/Good/Easy after the port's monotonicity rule.

    py-fsrs has no such rule; Anki does, and FLASHCARD_SPEC.md §4 requires it
    ("enforce hard < good < easy AFTER fuzz"). Modelling it here keeps the
    vectors comparable to the port without weakening the formula checks: the
    stabilities and difficulties stay pure py-fsrs, only the interval a grade
    turns into can be nudged.
    """
    out = {rating: candidate_days(sched, card, cursor, rating) for rating in ORDERED_RATINGS}
    if out[Rating.Hard] is not None and out[Rating.Good] is not None:
        out[Rating.Good] = max(out[Rating.Good], out[Rating.Hard] + 1)
    if out[Rating.Good] is not None and out[Rating.Easy] is not None:
        out[Rating.Easy] = max(out[Rating.Easy], out[Rating.Good] + 1)
    for rating in (Rating.Good, Rating.Easy):
        if out[rating] is not None:
            out[rating] = min(out[rating], MAXIMUM_INTERVAL)
    return out


def run_scenario(name: str, steps: list) -> dict:
    sched = scheduler()
    card = Card(card_id=1, due=BASE)
    cursor = BASE
    previous_day = day_number(BASE)
    out_steps = []

    for rating, advance in steps:
        kind, amount = advance
        if kind == "due":
            target = card.due + timedelta(days=amount)
            if target < cursor:
                raise AssertionError(f"{name}: due date moved backwards")
        elif kind == "secs":
            target = cursor + timedelta(seconds=amount)
        elif kind == "days":
            target = cursor + timedelta(days=amount)
        else:
            raise AssertionError(f"{name}: bad advance {advance!r}")

        advance_secs = int((target - cursor).total_seconds())
        if advance_secs < 0:
            raise AssertionError(f"{name}: negative advance")
        cursor = target

        # The port derives elapsed days from day numbers; py-fsrs from the
        # datetime delta. Refuse to emit a vector where they disagree.
        expected_elapsed = (
            (cursor - card.last_review).days if card.last_review is not None else None
        )
        this_day = day_number(cursor)
        if this_day > MAX_DAY_NUMBER:
            raise AssertionError(
                f"{name}: day number {this_day} leaves the u16 range; shorten the scenario"
            )
        if expected_elapsed is not None and this_day - previous_day != expected_elapsed:
            raise AssertionError(
                f"{name}: day-number delta {this_day - previous_day} != py-fsrs "
                f"days_since_last_review {expected_elapsed}"
            )
        previous_day = this_day

        ordered = ordered_intervals(sched, card, cursor)
        previous_state = card.state
        card, _ = sched.review_card(card=card, rating=rating, review_datetime=cursor)
        raw_interval_secs = int((card.due - cursor).total_seconds())

        interval_secs = raw_interval_secs
        if card.state == State.Review:
            if rating in ORDERED_RATINGS:
                if ordered[rating] is None:
                    raise AssertionError(f"{name}: candidate/actual state disagree")
                interval_secs = ordered[rating] * SECONDS_PER_DAY
            else:  # Again only reaches Review with no relearning steps configured
                days = apply_graduation_floor(previous_state, rating, card, raw_interval_secs // SECONDS_PER_DAY)
                interval_secs = days * SECONDS_PER_DAY

        out_steps.append(
            {
                "grade": int(rating) - 1,
                "advance": advance_secs,
                "state": int(card.state),
                "step": -1 if card.step is None else card.step,
                "stability": card.stability,
                "difficulty": card.difficulty,
                "interval": interval_secs,
                "raw_interval": raw_interval_secs,
            }
        )

    return {"name": name, "base": int(BASE.timestamp()), "steps": out_steps}


# --- fuzz vectors ------------------------------------------------------------

# Values that are exactly representable as binary32, so the C++ side draws the
# identical number and the two roundings cannot diverge.
FUZZ_DRAWS = (0.0, 0.25, 0.5, 0.75, 1.0 - 2.0**-24)
FUZZ_INTERVALS = (1, 2, 3, 4, 5, 6, 7, 8, 10, 15, 20, 21, 30, 60, 100, 365, 3650, 36490, 36500)


def fuzz_vectors() -> list[dict]:
    sched = Scheduler(enable_fuzzing=True)
    original = fsrs.scheduler.random
    rows = []
    try:
        for draw in FUZZ_DRAWS:
            fsrs.scheduler.random = lambda value=draw: value
            for days in FUZZ_INTERVALS:
                fuzzed = sched._get_fuzzed_interval(interval=timedelta(days=days))
                rows.append({"days": days, "draw": draw, "expected": fuzzed.days})
    finally:
        fsrs.scheduler.random = original
    return rows


# --- emission ----------------------------------------------------------------


def cxx_float(value: float) -> str:
    text = f"{value:.9g}"
    if "." not in text and "e" not in text and "E" not in text:
        text += ".0"  # "1f" is not a float literal; "1.0f" is
    return text + "f"


def emit(scenarios: list[dict], fuzz: list[dict], out) -> None:
    print("// Generated by host_tests/fsrs/gen_vectors.py -- do not edit.", file=out)
    print(f"// Reference implementation: py-fsrs {fsrs.__version__ if hasattr(fsrs, '__version__') else '6.3.2'}"
          " (package `fsrs==6.3.2`)", file=out)
    print("// Scheduler defaults: retention 0.90, steps 1m/10m, relearn 10m,", file=out)
    print("//   maximum interval 36500 d, fuzzing DISABLED for scenario vectors.", file=out)
    print("// Grades are 0=Again 1=Hard 2=Good 3=Easy; states 1=Learning 2=Review 3=Relearning.", file=out)
    print("// `outStep` is -1 where py-fsrs leaves step as None (Review state).", file=out)
    print("// `outIntervalSecs` has the spec's hard < good < easy ordering and the", file=out)
    print(f"//   graduation floors ({GRADUATING_FLOOR_DAYS} d / {EASY_FLOOR_DAYS} d easy) applied;", file=out)
    print("//   `outRawIntervalSecs` is py-fsrs untouched.", file=out)
    adjusted = sum(
        1 for s in scenarios for step in s["steps"] if step["interval"] != step["raw_interval"]
    )
    total = sum(len(s["steps"]) for s in scenarios)
    print(f"// Those two rules lengthened {adjusted} of {total} steps.", file=out)
    print("// NOTE: both rules are MODELLED here, so the vectors cannot be evidence", file=out)
    print("//   for them -- FsrsSchedTest.cpp's testMonotonicity / testGraduationFloors", file=out)
    print("//   are the authoritative checks.", file=out)
    print(file=out)

    for scenario in scenarios:
        name = scenario["name"]
        print(f"static const VecStep VEC_{name.upper()}[] = {{", file=out)
        for step in scenario["steps"]:
            print(
                "    {{{grade}, {advance}u, {state}, {step}, {stability}, {difficulty}, {interval}u, "
                "{raw}u}},".format(
                    grade=step["grade"],
                    advance=step["advance"],
                    state=step["state"],
                    step=step["step"],
                    stability=cxx_float(step["stability"]),
                    difficulty=cxx_float(step["difficulty"]),
                    interval=step["interval"],
                    raw=step["raw_interval"],
                ),
                file=out,
            )
        print("};", file=out)

    print(file=out)
    print("static const VecScenario VEC_SCENARIOS[] = {", file=out)
    for scenario in scenarios:
        name = scenario["name"]
        print(
            f'    {{"{name}", {scenario["base"]}u, VEC_{name.upper()}, '
            f'{len(scenario["steps"])}u}},',
            file=out,
        )
    print("};", file=out)

    print(file=out)
    print("static const FuzzVec FUZZ_VECTORS[] = {", file=out)
    for row in fuzz:
        print(
            f'    {{{row["days"]}u, {cxx_float(row["draw"])}, {row["expected"]}u}},',
            file=out,
        )
    print("};", file=out)


def main() -> int:
    scenarios = [run_scenario(name, steps) for name, steps in build_scenarios()]
    emit(scenarios, fuzz_vectors(), sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
