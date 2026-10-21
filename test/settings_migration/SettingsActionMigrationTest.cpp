// Fork (r8): the persisted-enum migrations CrossPointSettings::fromJson() runs
// on a loaded settings.json -- short-power Toggle Light 6 -> 10 on touch boards
// and Home-key Go Back 11 -> 12 keyed on the "homeGoBackAction" stamp.
//
// load() below mirrors exactly what fromJson() does with these keys: the stamp
// is "present or not" plus its value (HOME_GO_BACK_LEGACY_VALUE when absent),
// both migrations run on the values the generic loop read, and either one
// asks for a resave. save() mirrors toJson(): the values as they are plus the
// stamp, which a flagged build writes on every save.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

#include "util/HomeButtonInput.h"
#include "util/SettingsActionMigration.h"

namespace {

using A = HomeButtonAction;
constexpr auto u8 = [](A action) { return static_cast<uint8_t>(action); };

// CrossPointSettings::SHORT_PWRBTN values this test needs (CrossPointSettings.h
// is not host-buildable; the two migration values are pinned there by
// static_assert against settings_migration::).
constexpr uint8_t IGNORE = 0;
constexpr uint8_t CREATE_CLIPPING = 6;

struct File {
  std::optional<uint8_t> homeGoBackAction;  // the stamp; nullopt = key absent
  uint8_t tap, doubleTap, longPress;
  uint8_t shortPwrBtn;
};

struct Loaded {
  uint8_t tap, doubleTap, longPress;
  uint8_t shortPwrBtn;
  bool needsResave = false;
};

Loaded load(const File& file, const bool hasTouch) {
  Loaded s{file.tap, file.doubleTap, file.longPress, file.shortPwrBtn};
  if (settings_migration::remapLegacyToggleLight(hasTouch, s.shortPwrBtn)) s.needsResave = true;
  const uint8_t storedGoBack = file.homeGoBackAction.value_or(HOME_GO_BACK_LEGACY_VALUE);
  uint8_t* const actions[] = {&s.tap, &s.doubleTap, &s.longPress};
  if (settings_migration::remapHomeGoBack(file.homeGoBackAction.has_value(), storedGoBack, u8(A::GoBack), actions)) {
    s.needsResave = true;
  }
  return s;
}

File save(const Loaded& s) { return {u8(A::GoBack), s.tap, s.doubleTap, s.longPress, s.shortPwrBtn}; }

}  // namespace

TEST(SettingsActionMigration, ValuesThisTestAssumes) {
  // The r8 renumbering both migrations exist for.
  EXPECT_EQ(HOME_GO_BACK_LEGACY_VALUE, 11);
  EXPECT_EQ(u8(A::CreateClipping), 11);
  EXPECT_EQ(u8(A::GoBack), 12);
  EXPECT_EQ(settings_migration::R7B_TOGGLE_LIGHT, CREATE_CLIPPING);
  EXPECT_EQ(settings_migration::TOGGLE_LIGHT, 10);
}

TEST(SettingsActionMigration, FreshInstallDefaultsAreLeftAlone) {
  // CrossPointSettings.h field defaults under the flags: tap Go Back, double
  // tap frontlight, long press reader menu, short power Ignore. Nothing there
  // equals the legacy 11 or 6, so a first save only adds the stamp.
  const File fresh{std::nullopt, u8(A::GoBack), u8(A::ToggleFrontlight), u8(A::ReaderMenu), IGNORE};
  for (const bool touch : {true, false}) {
    const Loaded s = load(fresh, touch);
    EXPECT_EQ(s.tap, u8(A::GoBack));
    EXPECT_EQ(s.doubleTap, u8(A::ToggleFrontlight));
    EXPECT_EQ(s.longPress, u8(A::ReaderMenu));
    EXPECT_EQ(s.shortPwrBtn, IGNORE);
    EXPECT_TRUE(s.needsResave) << "unstamped: resave once to write the stamp";
    const Loaded again = load(save(s), touch);
    EXPECT_FALSE(again.needsResave);
  }
}

TEST(SettingsActionMigration, R7bCarryOverMovesBothValues) {
  // r7b: Go Back = 11 on tap and long press, Toggle Light = 6, no stamp.
  const File r7b{std::nullopt, 11, u8(A::ToggleFrontlight), 11, 6};
  const Loaded s = load(r7b, /*hasTouch=*/true);
  EXPECT_EQ(s.tap, u8(A::GoBack));
  EXPECT_EQ(s.longPress, u8(A::GoBack));
  EXPECT_EQ(s.doubleTap, u8(A::ToggleFrontlight)) << "unrelated actions keep their value";
  EXPECT_EQ(s.shortPwrBtn, settings_migration::TOGGLE_LIGHT);
  EXPECT_TRUE(s.needsResave);
}

TEST(SettingsActionMigration, ToggleLightRemapIsTouchBoardsOnly) {
  // A non-touch board offers CREATE_CLIPPING (6) as a short press, so 6 stays.
  const File file{u8(A::GoBack), u8(A::Home), u8(A::Home), u8(A::Home), CREATE_CLIPPING};
  const Loaded s = load(file, /*hasTouch=*/false);
  EXPECT_EQ(s.shortPwrBtn, CREATE_CLIPPING);
  EXPECT_FALSE(s.needsResave);
}

TEST(SettingsActionMigration, MigratedFileIsStableOnEveryLaterLoad) {
  const File r7b{std::nullopt, 11, 11, 11, 6};
  const Loaded first = load(r7b, true);
  ASSERT_TRUE(first.needsResave);
  const Loaded second = load(save(first), true);
  const Loaded third = load(save(second), true);
  for (const Loaded* s : {&second, &third}) {
    EXPECT_FALSE(s->needsResave) << "no resave loop";
    EXPECT_EQ(s->tap, u8(A::GoBack));
    EXPECT_EQ(s->doubleTap, u8(A::GoBack));
    EXPECT_EQ(s->longPress, u8(A::GoBack));
    EXPECT_EQ(s->shortPwrBtn, settings_migration::TOGGLE_LIGHT);
  }
}

TEST(SettingsActionMigration, StampedCreateClippingStaysCreateClipping) {
  // An r8 user who deliberately binds Save Clipping (11): the stamp says this
  // file's Go Back is 12, so 11 is CreateClipping and is left alone.
  const File r8{u8(A::GoBack), u8(A::GoBack), u8(A::ToggleFrontlight), u8(A::CreateClipping), IGNORE};
  const Loaded s = load(r8, true);
  EXPECT_EQ(s.longPress, u8(A::CreateClipping));
  EXPECT_EQ(s.tap, u8(A::GoBack));
  EXPECT_FALSE(s.needsResave);
}

TEST(SettingsActionMigration, UpstreamCarryOverReadsUnstampedElevenAsGoBack) {
  // Documented behaviour (r8 review m3): a file from upstream stock -- or from
  // the fork's own env:x4pro, which builds without CROSSPOINT_HOME_TAP_GO_BACK
  // and so never writes the stamp -- that binds Save Clipping (11) is
  // indistinguishable from an r7b Go Back. It loads as Go Back. Accepted: the
  // daily-driver env (combo) always stamps.
  const File upstream{std::nullopt, u8(A::Home), u8(A::ToggleFrontlight), u8(A::CreateClipping), IGNORE};
  const Loaded s = load(upstream, true);
  EXPECT_EQ(s.longPress, u8(A::GoBack));
  EXPECT_EQ(s.tap, u8(A::Home));
  EXPECT_TRUE(s.needsResave);
  // ...and once stamped it stays Go Back (the reinterpretation is not undone).
  EXPECT_EQ(load(save(s), true).longPress, u8(A::GoBack));
}

TEST(SettingsActionMigration, UpstreamShortPowerSixOnTouchBecomesToggleLight) {
  // Documented behaviour: upstream offers CREATE_CLIPPING (6) as a short press
  // only on non-touch boards, so a touch-board file can hold 6 only from r7b --
  // or from a non-touch board's file carried over; that one reads as Toggle Light.
  const File upstream{std::nullopt, u8(A::Home), u8(A::Home), u8(A::Home), CREATE_CLIPPING};
  EXPECT_EQ(load(upstream, true).shortPwrBtn, settings_migration::TOGGLE_LIGHT);
}

TEST(SettingsActionMigration, StampFromABuildWithADifferentGoBackIsRemapped) {
  // A later fork build whose Go Back moved again (upstream appending actions)
  // stamps its own value; loading that file here maps it back to 12.
  const uint8_t laterGoBack = u8(A::GoBack) + 1;
  const File later{laterGoBack, laterGoBack, u8(A::ToggleFrontlight), u8(A::Home), IGNORE};
  const Loaded s = load(later, true);
  EXPECT_EQ(s.tap, u8(A::GoBack));
  EXPECT_EQ(s.longPress, u8(A::Home));
  EXPECT_TRUE(s.needsResave);
  EXPECT_FALSE(load(save(s), true).needsResave);
}
