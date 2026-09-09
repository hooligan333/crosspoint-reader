#!/bin/sh
# Build and run the soft-clock host tests with plain g++ -- no PlatformIO, no
# device toolchain, no test framework. host_tests/ sits outside src/ and lib/,
# so nothing here is ever compiled into the firmware.
#
#   ./build.sh          build and run
#   ./build.sh --build  build only
#
# The unit under test is lib/hal/SoftClock.h, which is header-only and pure by
# construction: it takes the epoch reading and the millis() value as arguments
# rather than calling time() and millis() itself, so the tests drive the exact
# code the device runs. HalClock.cpp keeps every side effect (SNTP, WiFi,
# millis()) and is not built here -- it needs Arduino and esp_sntp.
#
# The second half of this script is the flag-off guarantee. CROSSPOINT_SOFT_CLOCK
# is default-off and the X4 Pro combo images must stay byte-identical without it
# (FLASHCARD_SPEC.md §0.5/§7b), so it checks two things:
#   1. SoftClock.h with the flag undefined is an empty translation unit.
#   2. No file that gained a guarded block has any soft-clock code left once the
#      CROSSPOINT_SOFT_CLOCK conditionals are resolved as undefined. A residue
#      here is what a broken guard looks like.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$DIR/../.."
OUT="$DIR/build"
CXX=${CXX:-g++}
# -Wconversion/-Wsign-conversion are part of the bar here for the same reason as
# host_tests/flashcards: this is width arithmetic (int64 epoch down to uint8
# calendar fields) and an implicit narrowing is exactly the bug the field widths
# would hide.
CXXFLAGS="-std=gnu++2a -O2 -Wall -Wextra -Wconversion -Wsign-conversion -Werror \
-Wno-missing-field-initializers -fno-exceptions"

mkdir -p "$OUT"

echo "== compiling lib/hal/SoftClock.h + tests =="
"$CXX" $CXXFLAGS -DCROSSPOINT_SOFT_CLOCK \
  -I"$ROOT/lib/hal" -I"$ROOT/src" \
  -o "$OUT/softclock_tests" "$DIR/SoftClockTest.cpp"

echo "== checking the flag-off translation unit is empty =="
printf '#include "SoftClock.h"\n' >"$OUT/flags_off.cpp"
"$CXX" $CXXFLAGS -I"$ROOT/lib/hal" -c -o "$OUT/softclock_flags_off.o" "$OUT/flags_off.cpp"
if command -v nm >/dev/null 2>&1; then
  SYMBOLS=$(nm --defined-only "$OUT/softclock_flags_off.o" 2>/dev/null | wc -l)
  if [ "$SYMBOLS" -ne 0 ]; then
    echo "FAIL: SoftClock.h defines $SYMBOLS symbols without CROSSPOINT_SOFT_CLOCK"
    nm --defined-only "$OUT/softclock_flags_off.o"
    exit 1
  fi
  echo "ok: no symbols without CROSSPOINT_SOFT_CLOCK"
fi

echo "== checking no soft-clock code survives with the flag undefined =="
if command -v python3 >/dev/null 2>&1; then
  python3 - "$ROOT" <<'PY' || exit 1
import re
import sys

ROOT = sys.argv[1]
MACRO = "CROSSPOINT_SOFT_CLOCK"
FILES = [
    "lib/hal/HalClock.h",
    "lib/hal/HalClock.cpp",
    "src/main.cpp",
    "src/network/HttpDownloader.h",
    "src/network/HttpDownloader.cpp",
    "src/activities/network/RssSyncActivity.cpp",
    "src/activities/network/FlashcardSyncActivity.cpp",
    "src/activities/settings/StatusBarSettingsActivity.cpp",
    "src/activities/settings/StatusBarSettingsActivity.h",
]
# Anything the flag introduced. A `#if defined(...)` test of an undefined macro
# is inert, so directive lines are exempt; nothing else is.
RESIDUE = re.compile(
    r"softclock|SoftClock|SOFT_CLOCK"
    r"|maybeOpportunisticSync|applyServerDate|restoreFromStorage|persistNow"
    r"|lastResponseDate|noteResponseDate|g_rail|RAIL_"
    r"|_lastPersistMs|halClock\.tick"
)

def resolve(lines):
    """Drop the CROSSPOINT_SOFT_CLOCK branches, leaving other conditionals alone."""
    out, stack = [], []
    for line in lines:
        s = line.strip()
        m = re.match(r"#(ifdef|ifndef)\s+(\w+)\s*$", s)
        if m and m.group(2) == MACRO:
            stack.append(("ours", m.group(1) == "ifndef"))
            continue
        if s.startswith("#if"):
            stack.append(("theirs", None))
        elif s.startswith("#else") and stack and stack[-1][0] == "ours":
            stack[-1] = ("ours", not stack[-1][1])
            continue
        elif s.startswith("#endif"):
            if stack and stack.pop()[0] == "ours":
                continue
        if all(kind != "ours" or keep for kind, keep in stack):
            out.append(line)
    return out

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    # Backslash continuations are one logical line: a multi-line `#if` must be
    # judged as the directive it is, not as a bare fragment.
    text = re.sub(r"\\\n\s*", " ", text)
    return [re.sub(r"//.*$", "", ln) for ln in text.split("\n")]

failed = False

# The C3a review removed the broad WifiSelectionActivity auto-sync hook (M3/S1):
# font, OTA and OPDS flows must not pay for the clock, and the file is back at
# its pre-flag state. Assert it stays that way.
with open(f"{ROOT}/src/activities/network/WifiSelectionActivity.cpp") as fh:
    if MACRO in fh.read():
        print("FAIL: WifiSelectionActivity.cpp mentions CROSSPOINT_SOFT_CLOCK; the hook was removed on purpose")
        failed = True

for rel in FILES:
    with open(f"{ROOT}/{rel}") as fh:
        body = resolve(fh.read().split("\n"))
    for n, line in enumerate(strip_comments("\n".join(body)), 1):
        if RESIDUE.search(line) and not line.lstrip().startswith(("#if", "#elif")):
            print(f"FAIL: {rel}: soft-clock code survives the flag being off: {line.strip()}")
            failed = True
sys.exit(1 if failed else 0)
PY
  echo "ok: guarded blocks leave nothing behind"
else
  echo "skipped: python3 not available"
fi

if [ "${1:-}" = "--build" ]; then
  exit 0
fi

echo "== running =="
exec "$OUT/softclock_tests"
