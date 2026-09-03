#pragma once

// Automatic daylight-saving adjustment for the status-bar clock.
//
// Fork-only and compiled out entirely unless CROSSPOINT_CLOCK_DST is defined:
// with the flag off nothing here exists and every call site falls back to the
// raw SETTINGS.clockUtcOffsetQ it used before.
//
// When a rule is selected, SETTINGS.clockUtcOffsetQ is read as the STANDARD
// time offset and the display offset gains one hour inside the rule's DST
// window. The usage log deliberately does not use this: it records raw UTC.
//
// This lives app-side rather than in HalClock so the HAL stays free of any
// app-layer settings dependency (see the note on HalClock::syncFromNTP).

#ifdef CROSSPOINT_CLOCK_DST

#include <cstdint>

// Display offset for `baseQ` (a biased quarter-hour standard-time offset,
// 48 = UTC+0) under `rule` (CrossPointSettings::CLOCK_DST_RULE). Returns baseQ
// unchanged for rule 0/out-of-range, and whenever the RTC has no plausible
// date. The DST result is clamped to the 0..104 the clock accepts.
//
// Reads the RTC at most once a minute and caches the answer, so drawing the
// status-bar clock does not turn into an I2C transaction per frame.
//
// Task contract: almost every call is on the render task — the status-bar draw
// (BaseTheme::drawStatusBar, resolved there rather than in statusBarSpec() so
// the snapshot stays a side-effect-free byte read) and ClockOffsetActivity's
// live preview, both reached only from Activity::render(). The one exception is
// ClockSyncActivity::runSync(), which runs on the loop task after a manual NTP
// sync, so the cache is NOT strictly single-task.
//
// That race is benign by construction: the cache is four plain scalar statics,
// no pointers and no allocation, so a torn read can only mix a stale field with
// a fresh one. The worst outcome is a status bar showing the wrong hour for up
// to the 60 s until the next recompute — never memory unsafety. Keep it that
// way: do not add a pointer, a handle or an owning type to the cache.
uint8_t effectiveUtcOffsetQ(uint8_t baseQ, uint8_t rule);

#endif  // CROSSPOINT_CLOCK_DST
