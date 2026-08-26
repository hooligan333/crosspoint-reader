#pragma once

#include <cstdint>

// Pure, header-only state machine for arbitrating a capacitive Home-key double
// click. Feeds on one main-loop frame at a time while a first tap is pending.
struct HomeTapTracker {
  bool armed = false;
  unsigned long armedAt = 0;
  // Opaque token identifying the screen the window was armed against (the
  // ActivityManager's activity generation). Survives disarm() so the caller can
  // still compare it on the frame the window expires, which is the frame the
  // deferred single-tap action is finally dispatched. Defaulted so callers that
  // do not care — the unit tests — can keep arming with a timestamp alone.
  uint32_t armedGeneration = 0;

  void arm(unsigned long now, uint32_t generation = 0) {
    armed = true;
    armedAt = now;
    armedGeneration = generation;
  }

  void disarm() { armed = false; }

  enum class Step { None, DoubleClick, WindowExpired };

  // Feed one main-loop frame while a tap is pending. Returns what to do.
  // Expiry is checked BEFORE honoring a second tap: if a loop stall delays this
  // call past windowMs, a tap that arrives "late" must not be misread as the
  // second click of an expired pair — it starts a fresh window instead.
  Step update(bool secondTapSeen, unsigned long now, unsigned long windowMs) {
    if (!armed) return Step::None;
    if (now - armedAt >= windowMs) {
      disarm();
      return Step::WindowExpired;
    }
    if (secondTapSeen) {
      disarm();
      return Step::DoubleClick;
    }
    return Step::None;
  }
};
