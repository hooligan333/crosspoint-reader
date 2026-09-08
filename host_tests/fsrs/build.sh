#!/bin/sh
# Build and run the FsrsSched host tests with plain g++ -- no PlatformIO, no
# device toolchain, no test framework. host_tests/ sits outside src/ and lib/,
# so nothing here is ever compiled into the firmware.
#
#   ./build.sh          build and run
#   ./build.sh --build  build only
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LIB="$DIR/../../lib/FsrsSched"
OUT="$DIR/build"
CXX=${CXX:-g++}
CXXFLAGS="-std=gnu++2a -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -fno-exceptions"

mkdir -p "$OUT"

echo "== compiling lib/FsrsSched + tests =="
"$CXX" $CXXFLAGS -DCROSSPOINT_FLASHCARDS -I"$LIB" -I"$DIR" -o "$OUT/fsrs_tests" \
  "$DIR/FsrsSchedTest.cpp" "$LIB/FsrsSched.cpp"

# The flag-off build must produce an empty translation unit: PlatformIO compiles
# everything under lib/ into every environment, and flags-off firmware has to
# stay byte-identical.
echo "== checking the flags-off translation unit is empty =="
"$CXX" $CXXFLAGS -I"$LIB" -c -o "$OUT/fsrs_flags_off.o" "$LIB/FsrsSched.cpp"
if command -v nm >/dev/null 2>&1; then
  SYMBOLS=$(nm --defined-only "$OUT/fsrs_flags_off.o" 2>/dev/null | wc -l)
  if [ "$SYMBOLS" -ne 0 ]; then
    echo "FAIL: FsrsSched.cpp defines $SYMBOLS symbols without CROSSPOINT_FLASHCARDS"
    nm --defined-only "$OUT/fsrs_flags_off.o"
    exit 1
  fi
  echo "ok: no symbols without CROSSPOINT_FLASHCARDS"
fi

if [ "${1:-}" = "--build" ]; then
  exit 0
fi

echo "== running =="
exec "$OUT/fsrs_tests"
