#pragma once
// Host stand-in for lib/Logging. The real header pulls in Arduino and a
// HardwareSerial; the units under test only use the three macros.
//
// Silent by default so a passing run is quiet. Build with -DFLASHCARD_TEST_LOGS
// to see what the layer complains about while a test is being written.

#ifdef FLASHCARD_TEST_LOGS
#include <cstdio>
#define LOG_ERR(origin, format, ...) printf("[ERR %s] " format "\n", origin, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) printf("[INF %s] " format "\n", origin, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) printf("[DBG %s] " format "\n", origin, ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#define LOG_DBG(origin, format, ...)
#endif
