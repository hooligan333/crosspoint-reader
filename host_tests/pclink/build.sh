#!/bin/sh
# Build and run the PC Link protocol host tests with plain g++ -- no PlatformIO,
# no device toolchain, no test framework. host_tests/ sits outside src/ and
# lib/, so nothing here is ever compiled into the firmware.
#
#   ./build.sh          build and run
#   ./build.sh --build  build only
#
# The unit under test is lib/PcLink (framing, CRC, parser, flow-control math,
# wear policy, BLIT conversion). Its DEFLATE path is the firmware's own
# lib/InflateReader + lib/uzlib, built here unchanged. When the host has zlib,
# the suite also round-trips real raw-DEFLATE streams (the exact settings
# pclink.py uses) and cross-checks the CRC against zlib's.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$DIR/../.."
LIB="$ROOT/lib/PcLink"
OUT="$DIR/build"
CXX=${CXX:-g++}
CC=${CC:-gcc}
CXXFLAGS="-std=gnu++2a -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -fno-exceptions"
INCS="-I$LIB -I$ROOT/lib/InflateReader -I$ROOT/lib/uzlib/src"

mkdir -p "$OUT"

# zlib is optional. Probe plainly first, then under an active conda prefix
# (whose cross-named compilers do not search it by default).
ZLIB_FLAGS=""
ZLIB_LIBS=""
zlib_probe() {
  printf '#include <zlib.h>\nint main(void){return zlibVersion()[0] == 0;}\n' |
    "$CC" -x c - $1 -lz -o "$OUT/zlib_probe" 2>/dev/null
}
if zlib_probe ""; then
  ZLIB_FLAGS="-DPCLINK_TEST_ZLIB"
  ZLIB_LIBS="-lz"
elif [ -n "${CONDA_PREFIX:-}" ] &&
  zlib_probe "-I$CONDA_PREFIX/include -L$CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib"; then
  ZLIB_FLAGS="-DPCLINK_TEST_ZLIB -I$CONDA_PREFIX/include"
  ZLIB_LIBS="-L$CONDA_PREFIX/lib -Wl,-rpath,$CONDA_PREFIX/lib -lz"
fi
if [ -n "$ZLIB_FLAGS" ]; then
  echo "(zlib found: real DEFLATE + CRC cross-checks enabled)"
else
  echo "(zlib not found: stored-block DEFLATE checks only)"
fi

echo "== compiling lib/PcLink + tests =="
# Section GC drops tinflate.c's checksum wrapper, whose uzlib_crc32/adler32 the
# firmware never links either (lib/uzlib vendors inflate only).
"$CC" -O2 -ffunction-sections -c -I"$ROOT/lib/uzlib/src" -o "$OUT/tinflate.o" "$ROOT/lib/uzlib/src/tinflate.c"
"$CXX" $CXXFLAGS $INCS -c -o "$OUT/InflateReader.o" "$ROOT/lib/InflateReader/InflateReader.cpp"
# -Wconversion/-Wsign-conversion are part of the bar for the unit itself: it is
# byte-packing and bit-twiddling code, where a silent narrowing is the bug.
"$CXX" $CXXFLAGS -Wconversion -Wsign-conversion -DCROSSPOINT_PC_LINK $INCS -c -o "$OUT/PcLinkProtocol.o" \
  "$LIB/PcLinkProtocol.cpp"
"$CXX" $CXXFLAGS -DCROSSPOINT_PC_LINK $ZLIB_FLAGS $INCS -o "$OUT/pclink_tests" "$DIR/PcLinkTest.cpp" \
  "$OUT/PcLinkProtocol.o" "$OUT/InflateReader.o" "$OUT/tinflate.o" -Wl,--gc-sections $ZLIB_LIBS

# The flag-off build must produce an empty translation unit: PlatformIO compiles
# everything under lib/ into every environment, and flags-off firmware has to
# stay byte-identical.
echo "== checking the flags-off translation unit is empty =="
"$CXX" $CXXFLAGS $INCS -c -o "$OUT/pclink_flags_off.o" "$LIB/PcLinkProtocol.cpp"
if command -v nm >/dev/null 2>&1; then
  SYMBOLS=$(nm --defined-only "$OUT/pclink_flags_off.o" 2>/dev/null | wc -l)
  if [ "$SYMBOLS" -ne 0 ]; then
    echo "FAIL: PcLinkProtocol.cpp defines $SYMBOLS symbols without CROSSPOINT_PC_LINK"
    nm --defined-only "$OUT/pclink_flags_off.o"
    exit 1
  fi
  echo "ok: no symbols without CROSSPOINT_PC_LINK"
fi

if [ "${1:-}" = "--build" ]; then
  exit 0
fi

echo "== running =="
exec "$OUT/pclink_tests"
