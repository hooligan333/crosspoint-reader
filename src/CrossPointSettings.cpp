#include "CrossPointSettings.h"

#include <BlePageTurnerJson.h>
#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#include "I18nKeys.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SettingsList.h"
#include "fontIds.h"
#include "util/ParagraphIndentMigration.h"
#include "util/PluginLocations.h"
#include "util/Timezones.h"

namespace {

// Stack buffer for "<key>_obf" key construction — avoids a std::string
// allocation per obfuscated setting on every save and load.
constexpr size_t OBF_KEY_BUF = 64;

// Null-terminated copy into a fixed-size settings field.
void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

// Fork r3-r4 images' auto-DST rule (their "clockDstRule" key), identified by
// a fragment of the POSIX TZ rule the named zones in Timezones.cpp carry.
// Index = r4's CLOCK_DST_RULE byte: 1 = US, 2 = EU, 3 = AU (0 = off). The
// fragments are mutually exclusive across the table, so a zone has at most one.
constexpr const char* LEGACY_DST_RULES[] = {nullptr, "M3.2.0,M11.1.0", ",M3.5.0", ",M10.1.0,M4.1.0"};
constexpr uint8_t LEGACY_DST_RULE_COUNT = sizeof(LEGACY_DST_RULES) / sizeof(LEGACY_DST_RULES[0]);

uint8_t legacyDstRuleOf(const char* posixTz) {
  for (uint8_t rule = 1; rule < LEGACY_DST_RULE_COUNT; rule++) {
    if (strstr(posixTz, LEGACY_DST_RULES[rule]) != nullptr) return rule;
  }
  return 0;
}

}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  const CrossPointSettings& s = *this;

  auto visit = [&](const SettingInfo& info) {
    if (!info.key) return;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) return;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        doc[obfKey] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  };
  forEachBaseSetting(visit);

  // touchReaderControls in the legacy single-mode numbering (0 = off, 1 = tap,
  // 2 = swipe, 3 = inverted tap, 4 = the fork's Swipe + Tap), which fork r2-r4
  // images read as the whole touch mode and rewrite without the gesture keys:
  // a bare 1 would come back from one of them as Tap, both directions. So a
  // gesture pair with a legacy equivalent is written as that mode, and the
  // legacy split in fromJson() restores the same pair from it. Mixed or
  // disabled pairs have none and keep 1. Safe for this image and upstream: the
  // toggle load folds anything >= 2 back to its On default, and the legacy
  // split only runs when the gesture keys are absent, which they never are
  // in a file written here.
  if (touchReaderControls != TOUCH_READER_OFF && pageTurnGesture == previousPageGesture) {
    switch (pageTurnGesture) {
      case TAP_ONLY:
        doc["touchReaderControls"] = uint8_t{1};
        break;
      case SWIPE_ONLY:
        doc["touchReaderControls"] = uint8_t{2};
        break;
      case INVERTED_TAP:
        doc["touchReaderControls"] = uint8_t{3};
        break;
      case TAP_AND_SWIPE:
        doc["touchReaderControls"] = uint8_t{4};
        break;
      default:
        break;
    }
  }

  // Keys the fork's r3-r4 images read, so a trip through one keeps the zone.
  // Each firmware's save drops the keys it does not know, and r4 knows only
  // clockUtcOffsetQ + clockDstRule (not clockTimezone/clockDst): written from
  // the chosen zone, r4 shows the right DST-aware time, and back here the
  // clockDstRule migration in fromJson() re-finds the same zone -- every
  // (standard offset, rule) pair in the table has a single first match. The
  // migration is gated on clockTimezone being absent/255, so a file this
  // image wrote itself (real zone) never re-migrates. With DST not on Auto, or
  // a zone r4 has no rule for, the rule is 0 and the offset is the fixed one
  // the clock actually applies (standard, +1 h when DST is forced on), which
  // r4 then shows as-is; the zone name itself cannot survive that trip.
  // With no zone chosen (255) the legacy offset IS the setting (activeIndex()
  // maps it to a fixed entry), so it is written back unchanged with rule 0 --
  // folding a forced-DST hour into it would compound on every save.
  if (clockTimezone < timezones::count()) {
    const TimezoneInfo& tz = timezones::table()[clockTimezone];
    const int offsetQ = tz.stdOffsetQ + (clockDst == CLOCK_DST_ON ? 4 : 0);
    doc["clockUtcOffsetQ"] = static_cast<uint8_t>(std::clamp(offsetQ + 48, 0, 104));
    doc["clockDstRule"] = clockDst == CLOCK_DST_AUTO ? legacyDstRuleOf(tz.posixTz) : uint8_t{0};
  } else {
    doc["clockDstRule"] = uint8_t{0};
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = frontButtonBack;
  doc["frontButtonConfirm"] = frontButtonConfirm;
  doc["frontButtonLeft"] = frontButtonLeft;
  doc["frontButtonRight"] = frontButtonRight;
  // Font family and size — both use dynamic getter/setters in SettingsList (the
  // option lists depend on the SD font registry), so the generic loop skips them.
  doc["fontFamily"] = STORED_NOTOSANS;
  doc["fontSize"] = fontPointSize;
  // SD card font family name — not in SettingsList, save manually
  if (sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = sdFontFamilyName;
  }
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, save manually
  if (dictionaryName[0] != '\0') {
    doc["dictionaryName"] = dictionaryName;
  }
  // The displayed option list varies by board, so this setting uses a dynamic
  // display-index mapping and is skipped by the generic persistence loop.
  doc["longPressMenuFunction"] = longPressMenuFunction;
  doc["shortPwrBtn"] = shortPwrBtn;
#ifdef CROSSPOINT_HOME_TAP_GO_BACK
  // The value GoBack has in THIS build, so a later fork build whose GoBack moved
  // again (upstream appending actions) can remap it on load. See fromJson().
  // Upgrades only: an older build clamps an out-of-range GoBack to the field
  // default in its generic loop before it would look at the stamp.
  doc["homeGoBackAction"] = static_cast<uint8_t>(HomeButtonAction::GoBack);
#endif
#ifdef CROSSPOINT_DICT_SECONDARY
  if (secondaryDictionaryName[0] != '\0') {
    doc["secondaryDictionaryName"] = secondaryDictionaryName;
  }
#endif

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (language < getLanguageCount()) ? LANGUAGE_CODES[language] : "EN";

  // A uint16_t mask, so it does not fit the uint8_t generic loop. Omitted while
  // unconfigured, so the default keeps following the UI language.
  if (keyboardLayouts != 0) {
    doc["keyboardLayouts"] = keyboardLayouts;
  }

  bleturner::writeJson(ble, doc);
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  CrossPointSettings& s = *this;
  bool needsResave = false;

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  auto visit = [&](const SettingInfo& info) {
    if (!info.key) return;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) return;

    if (info.stringOffset) {
      // destPtr starts out holding the struct-initializer default; it stays that
      // way unless the document actually carries a value for this key.
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        needsResave = true;
        return;
      }

      bool loaded = false;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        bool ok = false;
        bool tooLong = false;
        const std::string decoded =
            obfuscation::deobfuscateFromBase64(doc[obfKey] | "", info.stringMaxLen - 1, &ok, &tooLong);
        if (tooLong) {
          LOG_ERR("CPS", "Oversized obfuscated value for key '%s'", info.key);
          needsResave = true;
        }
        if (ok && !decoded.empty()) {
          copyToField(destPtr, decoded.c_str(), info.stringMaxLen);
          loaded = true;
        }
      }
      if (!loaded) {
        // Read as const char*, never `| std::string(...)`: ArduinoJson's
        // std::string converter drags a per-TU copy of the serializer into
        // flash. See the note in PersistableStore.h.
        const char* raw = doc[info.key].is<const char*>() ? doc[info.key].as<const char*>() : nullptr;
        if (raw) {
          // Obfuscated field recovered from a legacy plaintext value -> resave.
          if (info.obfuscated && strcmp(raw, destPtr) != 0) needsResave = true;
          copyToField(destPtr, raw, info.stringMaxLen);
        }
      }
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumLabels().size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  };
  forEachBaseSetting(visit);

  // Plugin System did not exist in older settings files. Preserve the behavior
  // of users who already have SD plugins installed, while keeping the new
  // default disabled on clean installs.
  if (doc["pluginsEnabled"].isNull()) {
    pluginsEnabled = PluginLocations::anyPluginInstalledOnDisk() ? 1 : 0;
    needsResave = true;
  }

  const auto indentSpaces = doc["paragraphIndentSpaces"];
  const bool hasSavedWidth = indentSpaces.is<int>();
  const int savedWidth = hasSavedWidth ? indentSpaces.as<int>() : 0;
  paragraphIndentSpaces = migrateParagraphIndentSpaces(hasSavedWidth, savedWidth, extraParagraphSpacing != 0);
  if (!hasSavedWidth || savedWidth < 0 || savedWidth > 5) needsResave = true;

  // Older files stored one combined touch mode under "touchReaderControls":
  // 0=off, 1=tap, 2=swipe, 3=inverted tap. Split it into the master toggle
  // plus the per-direction gesture pair (the generic loop above already folded
  // out-of-range toggle values back to the On default).
  if (doc["pageTurnGesture"].isNull() && doc["previousPageGesture"].isNull() &&
      doc["touchReaderControls"].is<uint8_t>()) {
    const uint8_t mode = doc["touchReaderControls"].as<uint8_t>();
    // 4 = the fork's Swipe + Tap mode (r2-r4 images; upstream never wrote 4),
    // which upstream's Tap & Swipe gesture now provides.
    if (mode >= 1 && mode <= 4) {
      touchReaderControls = TOUCH_READER_ON;
      pageTurnGesture = mode == 1 ? TAP_ONLY : mode == 2 ? SWIPE_ONLY : mode == 3 ? INVERTED_TAP : TAP_AND_SWIPE;
      previousPageGesture = pageTurnGesture;
      needsResave = true;
    }
  }

  // Fork r3-r4 images (CROSSPOINT_CLOCK_DST) stored an auto-DST rule next to
  // the legacy offset, and while a rule was set that offset was STANDARD time.
  // Upstream's own legacy path maps the offset to a fixed "UTC±HH:MM" entry,
  // which for these files is an hour out all summer. With the rule known, the
  // named zone is unambiguous: same standard offset, same transition rule.
  // Rule bytes: 1 = US, 2 = EU, 3 = AU (0 = off takes upstream's path).
  // 255 is "never chosen" (see Timezones.cpp), and a round-trip through an
  // upstream image writes it out explicitly, so treat it the same as absent.
  // toJson() writes clockDstRule too (see there), always next to a real zone
  // or a rule of 0, so this image's own files never take this path.
  if ((doc["clockTimezone"] | 255) == 255 && doc["clockDstRule"].is<uint8_t>() && clockUtcOffsetQ <= 104) {
    const uint8_t rule = doc["clockDstRule"].as<uint8_t>();
    if (rule >= 1 && rule < LEGACY_DST_RULE_COUNT) {
      const int legacyQ = static_cast<int>(clockUtcOffsetQ) - 48;
      const TimezoneInfo* table = timezones::table();
      for (size_t i = 0; i < timezones::count(); i++) {
        if (table[i].stdOffsetQ == legacyQ && strstr(table[i].posixTz, LEGACY_DST_RULES[rule]) != nullptr) {
          clockTimezone = static_cast<uint8_t>(i);
          clockDst = CLOCK_DST_AUTO;
          needsResave = true;
          break;
        }
      }
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)SLEEP_10_MIN, SLEEP_TIMEOUT_COUNT, (uint8_t)SLEEP_10_MIN);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }
  // "lineSpacing" saves predate the 1.0 option: Tight/Normal/Wide/Extra Wide (0..3) become 1.1..1.45.
  if (doc["lineHeight"].isNull() && doc["lineSpacing"].is<uint8_t>()) {
    const int legacyValue = doc["lineSpacing"].as<uint8_t>();
    lineSpacing = legacyValue + 1 < LINE_COMPRESSION_COUNT ? static_cast<uint8_t>(legacyValue + 1) : LINE_SPACING_120;
    needsResave = true;
  }

  // Home-key migration from this fork's pre-#3516 catalog. The old firmware
  // stored its own HOME_BUTTON_ACTION numbering under two of these three key
  // names; #3516 reuses the names with a different numbering, and the old
  // values land inside the new valid range, so the generic loop's clamp
  // cannot catch the reinterpretation (tap "Go Back" 6 became Sync, long
  // press "Reader Menu" 3 became Refresh). The retired
  // "homeButtonDoubleClickAction" key is the unambiguous marker: only the
  // old firmware ever wrote it.
  if (!doc["homeButtonDoubleClickAction"].isNull()) {
    // Old catalog: 0 Off, 1 Frontlight, 2 Go Home, 3 Reader Menu, 4 Sleep,
    // 5 Screenshot, 6 Go Back.
    static constexpr uint8_t LEGACY_HOME_TO_3516[] = {
        static_cast<uint8_t>(HomeButtonAction::Ignore),            // 0 Off
        static_cast<uint8_t>(HomeButtonAction::ToggleFrontlight),  // 1 Frontlight
        static_cast<uint8_t>(HomeButtonAction::Home),              // 2 Go Home
        static_cast<uint8_t>(HomeButtonAction::ReaderMenu),        // 3 Reader Menu
        static_cast<uint8_t>(HomeButtonAction::Ignore),            // 4 Sleep      (no #3516 equivalent)
        static_cast<uint8_t>(HomeButtonAction::Ignore),            // 5 Screenshot (no #3516 equivalent)
#ifdef CROSSPOINT_HOME_TAP_GO_BACK
        static_cast<uint8_t>(HomeButtonAction::GoBack),            // 6 Go Back
#else
        static_cast<uint8_t>(HomeButtonAction::Home),              // 6 Go Back -> nearest without the flag
#endif
    };
    constexpr uint8_t LEGACY_HOME_COUNT = sizeof(LEGACY_HOME_TO_3516) / sizeof(LEGACY_HOME_TO_3516[0]);
    const auto fold = [&](const char* legacyKey, uint8_t CrossPointSettings::* field) {
      const uint8_t old = doc[legacyKey] | (uint8_t)0xFF;
      if (old < LEGACY_HOME_COUNT) s.*field = LEGACY_HOME_TO_3516[old];
    };
    fold("homeButtonTapAction", &CrossPointSettings::homeButtonTapAction);
    fold("homeButtonDoubleClickAction", &CrossPointSettings::homeButtonDoubleTapAction);
    fold("homeButtonLongPressAction", &CrossPointSettings::homeButtonLongPressAction);
    needsResave = true;  // rewrite drops the retired key, so this fold runs once
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  frontButtonBack = clamp(doc["frontButtonBack"] | (uint8_t)FRONT_HW_BACK, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_BACK);
  frontButtonConfirm =
      clamp(doc["frontButtonConfirm"] | (uint8_t)FRONT_HW_CONFIRM, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_CONFIRM);
  frontButtonLeft = clamp(doc["frontButtonLeft"] | (uint8_t)FRONT_HW_LEFT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_LEFT);
  frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)FRONT_HW_RIGHT, FRONT_BUTTON_HARDWARE_COUNT, FRONT_HW_RIGHT);
  validateFrontButtonMapping(s);

  // Reader font size — an actual point size since 1.5. Files written by 1.4 and
  // earlier hold the old SMALL/MEDIUM/LARGE/EXTRA_LARGE slot in 0..3; no font is
  // renderable at those sizes, so the range is unambiguous and folds to the
  // point sizes those slots used to mean. Drop this once 1.4 upgrades are done.
  uint8_t storedFontSize = doc["fontSize"] | DEFAULT_FONT_POINT_SIZE;
  if (storedFontSize <= LEGACY_FONT_SIZE_MAX) {
    storedFontSize = 12 + storedFontSize * 2;  // 0,1,2,3 -> 12,14,16,18
    needsResave = true;
  }
  fontPointSize = storedFontSize;

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | STORED_NOTOSANS;
  fontFamily = NOTOSANS;
  // These controls use board-specific display-index mappings and are skipped by
  // the generic settings loop. Persisted action IDs remain stable across boards.
  longPressMenuFunction = clamp(doc["longPressMenuFunction"] | (uint8_t)LP_MENU_DISABLED,
                                (uint8_t)LONG_PRESS_MENU_FUNCTION_COUNT, (uint8_t)LP_MENU_DISABLED);
  shortPwrBtn = clamp(doc["shortPwrBtn"] | (uint8_t)IGNORE, (uint8_t)SHORT_PWRBTN_COUNT, (uint8_t)IGNORE);
#ifdef CROSSPOINT_PWR_TOGGLE_LIGHT
  // r7b and earlier stored Toggle Light as 6, which upstream has since given to
  // CREATE_CLIPPING. Touch boards never offer CREATE_CLIPPING as a short press,
  // so there a stored 6 can only be the fork's legacy value: move it to 10.
  if (BoardConfig::hasTouch() && shortPwrBtn == LEGACY_TOGGLE_LIGHT) {
    shortPwrBtn = TOGGLE_LIGHT;
    needsResave = true;
  }
#endif
#ifdef CROSSPOINT_HOME_TAP_GO_BACK
  // Home-key gestures bound to "Go Back": r7b and earlier persisted GoBack as 11,
  // which upstream has since given to CreateClipping (GoBack is now 12). Fork
  // builds stamp the GoBack value they write ("homeGoBackAction", toJson()), so a
  // file without the stamp was written by an older fork image -- or by a build
  // without the flag, which never persists GoBack at all -- and its 11 is GoBack.
  // Runs on the values the generic loop just read, BEFORE the pre-#3516 long-press
  // migration below, which may legitimately produce CreateClipping itself.
  {
    const uint8_t storedGoBack = doc["homeGoBackAction"] | HOME_GO_BACK_LEGACY_VALUE;
    const auto goBack = static_cast<uint8_t>(HomeButtonAction::GoBack);
    if (storedGoBack != goBack) {
      for (uint8_t* action : {&homeButtonTapAction, &homeButtonDoubleTapAction, &homeButtonLongPressAction}) {
        if (*action == storedGoBack) *action = goBack;
      }
    }
    if (doc["homeGoBackAction"].isNull() || storedGoBack != goBack) needsResave = true;
  }
#endif
  if (BoardConfig::hasHomeKey() && doc["homeButtonLongPressAction"].isNull() &&
      !doc["longPressMenuFunction"].isNull()) {
    static constexpr HomeButtonAction LEGACY[] = {HomeButtonAction::Sync,       HomeButtonAction::Ignore,
                                                  HomeButtonAction::Bookmark,   HomeButtonAction::Dictionary,
                                                  HomeButtonAction::ReaderMenu, HomeButtonAction::CreateClipping};
    if (s.longPressMenuFunction < sizeof(LEGACY) / sizeof(LEGACY[0])) {
      s.homeButtonLongPressAction = static_cast<uint8_t>(LEGACY[s.longPressMenuFunction]);
      needsResave = true;
    }
  }

  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(sdFontFamilyName, sfn, sizeof(sdFontFamilyName) - 1);
  sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
  if (sdFontFamilyName[0] == '\0') {
    switch (storedFontFamily) {
      case LEGACY_NOTOSERIF:
        copyToField(sdFontFamilyName, "NotoSerif", sizeof(sdFontFamilyName));
        break;
      case LEGACY_OPENDYSLEXIC:
        copyToField(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName));
        break;
    }
  }
  if (storedFontFamily != STORED_NOTOSANS) needsResave = true;
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, load manually
  copyToField(dictionaryName, doc["dictionaryName"] | "", sizeof(dictionaryName));
#ifdef CROSSPOINT_DICT_SECONDARY
  copyToField(secondaryDictionaryName, doc["secondaryDictionaryName"] | "", sizeof(secondaryDictionaryName));
#endif

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  // Absent means unconfigured, which is the default.
  if (doc["keyboardLayouts"].is<uint16_t>()) {
    keyboardLayouts = doc["keyboardLayouts"].as<uint16_t>();
  }

  if (!bleturner::readJson(ble, doc)) {
    needsResave = true;
  }

  if (needsResave) {
    LOG_DBG("CPS", "Resaving settings to update format");
    requestResave();
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

CrossPointSettings::StatusBarSpec CrossPointSettings::statusBarSpec() const {
  StatusBarSpec spec;
  spec.showChapterPageCount = statusBarChapterPageCount != 0;
  spec.showBookProgressPercent = statusBarBookProgressPercentage != 0;
  spec.titleMode = statusBarTitle;
  spec.showBattery = statusBarBattery != 0;
  spec.showBatteryPercent = hideBatteryPercentage == HIDE_NEVER;
  spec.clockMode = statusBarClock;
  spec.clock12h = clockFormat == 1;
  spec.progressBarMode = statusBarProgressBar;
  spec.progressBarHeightPx =
      statusBarProgressBar != HIDE_PROGRESS ? static_cast<uint8_t>((statusBarProgressBarThickness + 1) * 2) : 0;
  spec.xtcMode = xtcStatusBarMode;
  return spec;
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                      const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.characterSpacing = getCharacterSpacing();
  spec.wordSpacingPercent = wordSpacing;
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphIndentSpaces = paragraphIndentSpaces;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  // Same layout as Display, so switching between them keeps the section cache.
  spec.imageRendering = imageRendering == IMAGES_DISPLAY_BW ? static_cast<uint8_t>(IMAGES_DISPLAY) : imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  return spec;
}

float CrossPointSettings::getReaderLineCompression() const {
  return LINE_SPACING_MULTIPLIERS[lineSpacing < LINE_COMPRESSION_COUNT ? lineSpacing : LINE_SPACING_120];
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
    case REFRESH_NEVER:
      // Effectively disables the periodic full refresh; the page counter
      // counts down from here and never reaches the threshold in practice.
      return std::numeric_limits<int>::max();
  }
}

void CrossPointSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  fontPointSize =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  saveToFile();
}

int CrossPointSettings::getReaderFontId() const {
  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  // A built-in family only exists at BUILTIN_READER_POINT_SIZES, so a size
  // carried over from an SD family may not be one of them. ensureLoaded()
  // normally persists the snap; snap again here (without allocating — this runs
  // in the page render loop) so rendering is correct even before it has run.
  const uint8_t pt =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  switch (pt) {
    case 12:
      return NOTOSANS_12_FONT_ID;
    case 16:
      return NOTOSANS_16_FONT_ID;
    case 18:
      return NOTOSANS_18_FONT_ID;
    case 14:
    default:
      return NOTOSANS_14_FONT_ID;
  }
}
