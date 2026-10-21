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
// press, so there a stored 6 can only be the fork's legacy value. Pinned to the
// CrossPointSettings enum by static_asserts in CrossPointSettings.cpp.
inline constexpr uint8_t R7B_TOGGLE_LIGHT = 6;
inline constexpr uint8_t TOGGLE_LIGHT = 10;

// Returns true when it rewrote the value (the caller resaves).
inline bool remapLegacyToggleLight(const bool hasTouch, uint8_t& shortPwrBtn) {
  if (!hasTouch || shortPwrBtn != R7B_TOGGLE_LIGHT) return false;
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

}  // namespace settings_migration
