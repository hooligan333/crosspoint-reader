#!/bin/sh
# Build and run the flashcard study-logic host tests with plain g++ -- no
# PlatformIO, no device toolchain, no test framework. host_tests/ sits outside
# src/ and lib/, so nothing here is ever compiled into the firmware.
#
#   ./build.sh          build and run
#   ./build.sh --build  build only
#
# The units under test reach the SD card only through HalStorage / HalFile, so
# stubs/ supplies a stdio-backed pair of those (and a silent Logging.h) and the
# real DeckFile / StateStore / SessionQueue / KeySort sources compile unchanged.
#
# TWO CONFIGURATIONS (FLASHCARD_SPEC.md §7b.4). The suite is built and run both
# without and with -DCROSSPOINT_FLASHCARDS_C3, and must pass in both: the C3
# variant swaps the resident card index for on-demand seeks and the merge's
# payload buffer for per-record reads, and the point of running the SAME
# behavioural suite over both is that those are implementation changes, not
# behavioural ones. The two documented divergences (a 2000-card cap and a
# skipped duplicate-key check) are asserted per configuration inside the suite.
# The C3 build also picks up stubs/HalHeapGauge.h, which lets the gated
# allocations be observed refusing.
#
# I/O FAULTS. stubs/HalStorage.h can schedule a short read, a refused seek, a
# short write or a seek that reports success and lands elsewhere, on files
# chosen by path suffix, and counts every operation by file kind. The C3 variant
# turns index access into I/O, so its error paths only mean something if they
# can be executed (§7b.4) -- and the merge's read-ahead is a claim about op
# counts until something counts them. Both configurations run those groups; the
# expectations differ only where the Pro's resident index makes a deck fault
# unreachable, and that difference is asserted rather than skipped.
#
# CROSS-VARIANT COMPATIBILITY. A deck/state pair written by one build must be
# fully usable by the other -- the on-disk formats are identical and only the
# in-RAM strategy differs. That cannot be checked inside one process, so each
# binary is run once with --produce (writing a deck, a state file with known
# schedules, and a v2 deck for the merge path, all under build/card/xvariant/)
# before either runs its suite; each suite then CONSUMES the other
# configuration's artifacts. Hence: produce, produce, run, run.
#
# StudyClock.cpp is NOT linked: it needs the RTC, the settings blob and the DST
# rule, none of which have a host stand-in. Its one piece of real arithmetic is
# a pure inline in StudyClock.h and is tested through that; the .cpp still goes
# through the flags-off zero-symbol check below like every other unit.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$DIR/../.."
SRC="$ROOT/src/flashcards"
OUT="$DIR/build"
WORK="$OUT/card"
XDIR="$WORK/xvariant"
CXX=${CXX:-g++}
# -Wconversion/-Wsign-conversion are part of the bar here, not decoration: this
# layer is nothing but width arithmetic over a binary file format, and an
# implicit narrowing is exactly the class of bug the byte layouts hide.
CXXFLAGS="-std=gnu++2a -O2 -Wall -Wextra -Wconversion -Wsign-conversion -Werror \
-Wno-missing-field-initializers -fno-exceptions"

SOURCES="$DIR/FlashcardsTest.cpp $SRC/DeckFile.cpp $SRC/StateStore.cpp $SRC/SessionQueue.cpp $SRC/KeySort.cpp \
$ROOT/lib/FsrsSched/FsrsSched.cpp"
INCLUDES="-I$DIR/stubs -I$SRC -I$ROOT/lib/FsrsSched -I$ROOT/lib/Memory"

mkdir -p "$OUT" "$WORK"

build_config() {
  name=$1
  extra=$2
  echo "== compiling src/flashcards + lib/FsrsSched + tests ($name) =="
  # shellcheck disable=SC2086 -- the flag lists are deliberately word-split
  "$CXX" $CXXFLAGS -DCROSSPOINT_FLASHCARDS $extra -DFIXTURE_PATH="\"$DIR/crosspoint-fixture.deck\"" \
    $INCLUDES -o "$OUT/flashcard_tests_$name" $SOURCES
}

build_config default ""
build_config c3 "-DCROSSPOINT_FLASHCARDS_C3"

# The flags-off build must produce empty translation units: PlatformIO compiles
# everything under src/ into every environment, and flags-off firmware has to
# stay byte-identical. Checked with the C3 flag set as well as unset, because
# CROSSPOINT_FLASHCARDS_C3 is defined on an env where CROSSPOINT_FLASHCARDS is
# too -- but nothing may make it able to emit code on its own.
echo "== checking the flags-off translation units are empty =="
for unit in DeckFile StateStore SessionQueue KeySort StudyClock; do
  for off in "" "-DCROSSPOINT_FLASHCARDS_C3"; do
    # shellcheck disable=SC2086
    "$CXX" $CXXFLAGS $off $INCLUDES -c -o "$OUT/${unit}_flags_off.o" "$SRC/${unit}.cpp"
    if command -v nm >/dev/null 2>&1; then
      SYMBOLS=$(nm --defined-only "$OUT/${unit}_flags_off.o" 2>/dev/null | wc -l)
      if [ "$SYMBOLS" -ne 0 ]; then
        echo "FAIL: ${unit}.cpp defines $SYMBOLS symbols without CROSSPOINT_FLASHCARDS (${off:-no extra flags})"
        nm --defined-only "$OUT/${unit}_flags_off.o"
        exit 1
      fi
    fi
  done
done
echo "ok: no symbols without CROSSPOINT_FLASHCARDS"

if [ "${1:-}" = "--build" ]; then
  exit 0
fi

# Both configurations lay down their cross-variant artifacts before either
# consumes the other's; a missing file here would otherwise show up as a
# silently skipped compatibility group rather than a failure.
echo "== producing cross-variant artifacts =="
rm -rf "$XDIR"
"$OUT/flashcard_tests_default" "$WORK" --produce
"$OUT/flashcard_tests_c3" "$WORK" --produce
for tag in std c3; do
  for leaf in "$XDIR/$tag.deck" "$XDIR/$tag-v2.deck" "$XDIR/.state/$tag.deck.state"; do
    if [ ! -f "$leaf" ]; then
      echo "FAIL: cross-variant artifact missing: $leaf"
      exit 1
    fi
  done
done

STATUS=0
echo
echo "== running (default: resident index, 40000-card cap) =="
"$OUT/flashcard_tests_default" "$WORK" || STATUS=1
echo
echo "== running (CROSSPOINT_FLASHCARDS_C3: on-demand index, 2000-card cap) =="
"$OUT/flashcard_tests_c3" "$WORK" || STATUS=1
exit "$STATUS"
