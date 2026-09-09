#pragma once
// Host stand-in for lib/Logging. The real header pulls in Arduino and a
// HardwareSerial; the units under test only use the three macros.
//
// Silent by default so a passing run is quiet. Build with -DFLASHCARD_TEST_LOGS
// to see what the layer complains about while a test is being written.
//
// The silent form is NOT an empty macro. An expanded-to-nothing log leaves any
// variable that exists only to be logged looking unused, and this suite builds
// with -Werror -- so a perfectly good diagnostic in the layer under test would
// have to be deleted, or papered over with a (void) cast, to keep the host
// build quiet. Instead the arguments are consumed inside an UNEVALUATED sizeof:
// nothing runs, nothing is printed, but every argument counts as used and the
// format string is still type-checked against them.

#include <cstdio>

#ifdef FLASHCARD_TEST_LOGS
#define LOG_ERR(origin, format, ...) printf("[ERR %s] " format "\n", origin, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) printf("[INF %s] " format "\n", origin, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) printf("[DBG %s] " format "\n", origin, ##__VA_ARGS__)
#else
#define FLASHCARD_LOG_SINK(origin, format, ...) \
  ((void)sizeof(printf("%s" format, (origin), ##__VA_ARGS__)))  // NOLINT: unevaluated, see above
#define LOG_ERR(origin, format, ...) FLASHCARD_LOG_SINK(origin, format, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) FLASHCARD_LOG_SINK(origin, format, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) FLASHCARD_LOG_SINK(origin, format, ##__VA_ARGS__)
#endif
