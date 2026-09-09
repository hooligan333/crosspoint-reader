#!/bin/sh
# Build and run the flashcard REVIEW LOOP host tests with plain g++ -- no
# PlatformIO, no device toolchain, no test framework. host_tests/ sits outside
# src/ and lib/, so nothing here is ever compiled into the firmware.
#
#   ./build.sh          build and run
#   ./build.sh --build  build only
#
# StudyLoopTest.cpp mirrors the review half of
# src/activities/flashcards/FlashcardStudyActivity.cpp (the reveal/page/grade
# tap machine and the in-session requeue queue), which cannot be linked on the
# host: it reaches UITheme, GfxRenderer, GUI, OptionPopup, StateStore, DeckFile
# and the ActivityManager singleton. lib/FsrsSched IS the real library, so the
# scheduling half of every scenario is the firmware's own arithmetic.
#
# TWO CONFIGURATIONS, and both are meaningful:
#   * "shipped" builds the mirror as the code stands BEFORE the fix and asserts
#     the two field-reported symptoms -- the answer side of a paginated card
#     having no page navigation, and a Good-graded new card resurfacing after
#     three cards instead of after its 10-minute step. It is the reproduction,
#     and it must keep failing to compile away: if someone re-breaks the
#     activity these assertions are what says what the old behaviour was.
#   * "fixed" (-DSTUDY_LOOP_FIXED) builds the mirror as the code stands AFTER
#     the fix and asserts the corrected behaviour, plus the invariants the fix
#     had to preserve: Again still requeues behind >= 3 cards, the session still
#     ends, and cram still persists nothing.
# Both binaries must exit 0.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$DIR/../.."
OUT="$DIR/build"
CXX=${CXX:-g++}
CXXFLAGS="-std=gnu++2a -O2 -Wall -Wextra -Wconversion -Wsign-conversion -Werror \
-Wno-missing-field-initializers -fno-exceptions"

SOURCES="$DIR/StudyLoopTest.cpp $ROOT/lib/FsrsSched/FsrsSched.cpp"
INCLUDES="-I$ROOT/lib/FsrsSched"

mkdir -p "$OUT"

echo "== compiling the review-loop mirror + lib/FsrsSched (shipped) =="
# shellcheck disable=SC2086 -- the flag lists are deliberately word-split
"$CXX" $CXXFLAGS -DCROSSPOINT_FLASHCARDS $INCLUDES -o "$OUT/study_loop_shipped" $SOURCES

echo "== compiling the review-loop mirror + lib/FsrsSched (fixed) =="
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS -DCROSSPOINT_FLASHCARDS -DSTUDY_LOOP_FIXED $INCLUDES -o "$OUT/study_loop_fixed" $SOURCES

if [ "${1:-}" = "--build" ]; then
  exit 0
fi

STATUS=0
echo
"$OUT/study_loop_shipped" || STATUS=1
echo
"$OUT/study_loop_fixed" || STATUS=1
exit "$STATUS"
