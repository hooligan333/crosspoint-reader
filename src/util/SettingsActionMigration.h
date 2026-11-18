#pragma once

#include <cstdint>

// Fork (r8): the two persisted-enum migrations CrossPointSettings::fromJson()
// runs on a loaded settings.json, as pure functions so a host test can pin them
// (test/settings_migration). fromJson() decides WHEN each runs (flags, order
// against the other legacy folds) and reads the inputs out of the document;
// these decide WHAT changes.
namespace settings_migration {

// Short power button "Toggle Light" (CROSSPOINT_PWR_TOGGLE_LIGHT). r7b and
// earlier stored it as 6; upstream has since given 6 to CREATE_CLIPPING and the
// fork value moved to 10. Touch boards never offer CREATE_CLIPPING as a short
// press, so there a stored 6 can only be the fork's legacy value. r8-r10 stored
// it as 10; upstream #4435 has since given 10 to PWR_FRONTLIGHT, a double-click-
// only action no board offers for the short press, so a stored short-press 10
// is the fork's on every board, and the value moved to 11. Pinned to the
// CrossPointSettings enum by static_asserts in CrossPointSettings.cpp.
inline constexpr uint8_t R7B_TOGGLE_LIGHT = 6;
inline constexpr uint8_t R10_TOGGLE_LIGHT = 10;
inline constexpr uint8_t TOGGLE_LIGHT = 11;

// Returns true when it rewrote the value (the caller resaves).
inline bool remapLegacyToggleLight(const bool hasTouch, uint8_t& shortPwrBtn) {
  if (!(hasTouch && shortPwrBtn == R7B_TOGGLE_LIGHT) && shortPwrBtn != R10_TOGGLE_LIGHT) return false;
  shortPwrBtn = TOGGLE_LIGHT;
  return true;
}

// Home-key "Go Back" (CROSSPOINT_HOME_TAP_GO_BACK). Fork builds stamp the value
// GoBack has in the build that wrote the file ("homeGoBackAction"); a file
// without the stamp was written by r7b or earlier -- or by a build without the
// flag, which never persists GoBack at all -- with GoBack = 11, which upstream
// has since given to CreateClipping. `storedGoBack` is the stamp, or that legacy
// value when unstamped; every gesture holding it becomes this build's GoBack.
// Returns true when the file needs a resave (unstamped, or stamped by a build
// whose GoBack differs), so the stamp is written and the remap runs once.
inline bool remapHomeGoBack(const bool stamped, const uint8_t storedGoBack, const uint8_t goBack,
                            uint8_t* const (&actions)[3]) {
  if (storedGoBack != goBack) {
    for (uint8_t* action : actions) {
      if (*action == storedGoBack) *action = goBack;
    }
  }
  return !stamped || storedGoBack != goBack;
}

// Double-click power button (CROSSPOINT_PWR_TOGGLE_LIGHT builds). Upstream
// #4435 replaced the r10-and-earlier "doubleClickPwrLight" toggle (on = a power
// double click toggles the frontlight; absent = on) with "doublePwrBtn", and
// reads the old key only when the new one is absent. A flagged build also
// writes the old key, derived from doublePwrBtn, so an r10 image rolled back
// onto this file keeps the user's choice instead of falling back to its
// default (on). r10 has no other double-click action, so anything but the
// frontlight is written as off: r10 then adds no double-click wait to single
// clicks. Never read back while doublePwrBtn is present. PWR_FRONTLIGHT is
// pinned to the CrossPointSettings enum by static_assert in CrossPointSettings.cpp.
inline constexpr uint8_t DOUBLE_PWR_FRONTLIGHT = 10;

inline constexpr uint8_t legacyDoubleClickPwrLight(const uint8_t doublePwrBtn) {
  return doublePwrBtn == DOUBLE_PWR_FRONTLIGHT ? 1 : 0;
}

}  // namespace settings_migration
