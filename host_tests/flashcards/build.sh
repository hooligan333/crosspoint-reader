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
CXX=${CXX:-g++}
# -Wconversion/-Wsign-conversion are part of the bar here, not decoration: this
# layer is nothing but width arithmetic over a binary file format, and an
# implicit narrowing is exactly the class of bug the byte layouts hide.
CXXFLAGS="-std=gnu++2a -O2 -Wall -Wextra -Wconversion -Wsign-conversion -Werror \
-Wno-missing-field-initializers -fno-exceptions"

mkdir -p "$OUT" "$WORK"

echo "== compiling src/flashcards + lib/FsrsSched + tests =="
"$CXX" $CXXFLAGS -DCROSSPOINT_FLASHCARDS -DFIXTURE_PATH="\"$DIR/crosspoint-fixture.deck\"" \
  -I"$DIR/stubs" -I"$SRC" -I"$ROOT/lib/FsrsSched" -I"$ROOT/lib/Memory" \
  -o "$OUT/flashcard_tests" \
  "$DIR/FlashcardsTest.cpp" "$SRC/DeckFile.cpp" "$SRC/StateStore.cpp" "$SRC/SessionQueue.cpp" "$SRC/KeySort.cpp" \
  "$ROOT/lib/FsrsSched/FsrsSched.cpp"

# The flags-off build must produce empty translation units: PlatformIO compiles
# everything under src/ into every environment, and flags-off firmware has to
# stay byte-identical.
echo "== checking the flags-off translation units are empty =="
for unit in DeckFile StateStore SessionQueue KeySort StudyClock; do
  "$CXX" $CXXFLAGS -I"$DIR/stubs" -I"$SRC" -I"$ROOT/lib/FsrsSched" -I"$ROOT/lib/Memory" \
    -c -o "$OUT/${unit}_flags_off.o" "$SRC/${unit}.cpp"
  if command -v nm >/dev/null 2>&1; then
    SYMBOLS=$(nm --defined-only "$OUT/${unit}_flags_off.o" 2>/dev/null | wc -l)
    if [ "$SYMBOLS" -ne 0 ]; then
      echo "FAIL: ${unit}.cpp defines $SYMBOLS symbols without CROSSPOINT_FLASHCARDS"
      nm --defined-only "$OUT/${unit}_flags_off.o"
      exit 1
    fi
  fi
done
echo "ok: no symbols without CROSSPOINT_FLASHCARDS"

if [ "${1:-}" = "--build" ]; then
  exit 0
fi

echo "== running =="
exec "$OUT/flashcard_tests" "$WORK"
