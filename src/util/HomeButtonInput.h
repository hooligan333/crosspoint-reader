#pragma once

#include <cstdint>

// Persisted indices: append actions without reordering existing values.
enum class HomeButtonAction : uint8_t {
  Home,
  Ignore,
  NextPage,
  Refresh,
  Footnotes,
  Confirm,
  Sync,
  Bookmark,
  Dictionary,
  ReaderMenu,
  ToggleFrontlight,
#ifdef CROSSPOINT_HOME_TAP_GO_BACK
  // Fork action: climb ONE activity level rather than jumping all the way Home.
  // Appended last and behind the flag, so the persisted indices above are
  // untouched and a flags-off build is byte-identical upstream -- a settings.json
  // written by a flagged build with a gesture bound to GoBack clamps back to the
  // field default on load, because CrossPointSettings::fromJson() bounds ENUM
  // rows by home_button::ACTION_LABELS' size. Same pattern as
  // CrossPointSettings::SHORT_PWRBTN::TOGGLE_LIGHT.
  GoBack,
#endif
  Count
};

class HomeButtonInput {
 public:
  static constexpr uint32_t DOUBLE_TAP_MS = 350;

  HomeButtonAction update(uint32_t now, bool tapped, bool held, bool swiped, bool pressed, HomeButtonAction tap,
                          HomeButtonAction doubleTap, HomeButtonAction longPress) {
    if (swiped) {
      reset();
      return HomeButtonAction::Ignore;
    }
    if (held) {
      reset();
      return longPress;
    }
    if (pending && !secondContact && now - tappedAt > DOUBLE_TAP_MS) {
      pending = tapped;
      tappedAt = now;
      return tap;
    }
    if (pending && pressed) secondContact = true;
    if (!tapped) return HomeButtonAction::Ignore;
    if (doubleTap == HomeButtonAction::Ignore) {
      reset();
      return tap;
    }
    if (pending) {
      reset();
      return doubleTap;
    }
    pending = true;
    tappedAt = now;
    return HomeButtonAction::Ignore;
  }

  void reset() {
    pending = false;
    secondContact = false;
  }

 private:
  uint32_t tappedAt = 0;
  bool pending = false;
  bool secondContact = false;
};
