#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "SdCardFontManager.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/SdCardThemeRegistry.h"
#include "fontIds.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  const ThemeFreeInkComponentList& getFreeInkComponents() const { return currentSdFreeInkComponents; }
  const ThemeFreeInkIconMap& getFreeInkIcons() const { return currentSdFreeInkIcons; }
  const ThemeHomeLayoutSpec& getHomeLayout() const { return currentSdHomeLayout; }
  bool hasFreeInkHomeLayout() const { return currentSdHomeLayout.enabled; }
  int getSmallFontId() const { return currentSmallFontId; }
  int getMediumFontId() const { return currentMediumFontId; }
  int getLargeFontId() const { return currentLargeFontId; }
  std::vector<int> getHomeCoverThumbHeights() const;
  SdCardThemeRegistry& registry() { return themeRegistry; }
  void refreshRegistry();
  void releaseSdThemeAssetMemory();
  void prepareSdAssets(GfxRenderer& renderer);
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  const ThemeMetrics* currentMetrics;
  ThemeMetrics currentSdMetrics;
  ThemeHomeRecentsSpec currentSdHomeRecents;
  ThemeButtonMenuSpec currentSdButtonMenu;
  ThemeListSpec currentSdList;
  ThemeButtonHintsSpec currentSdButtonHints;
  ThemeTabBarSpec currentSdTabBar;
  ThemeHeaderSpec currentSdHeader;
  ThemeHomeLayoutSpec currentSdHomeLayout;
  std::string currentSdThemePath;
  std::string currentSdUiFontFamily;
  bool currentSdInheritsClassic = false;
  int currentSmallFontId = SMALL_FONT_ID;
  int currentMediumFontId = UI_10_FONT_ID;
  int currentLargeFontId = UI_12_FONT_ID;
  ThemeIconMap currentSdIcons;
  ThemeNamedIconMap currentSdNamedIcons;
  ThemeFreeInkComponentList currentSdFreeInkComponents;
  ThemeFreeInkIconMap currentSdFreeInkIcons;
  ThemeNamedFreeInkIconMap currentSdNamedFreeInkIcons;
  std::unique_ptr<BaseTheme> currentTheme;
  SdCardThemeRegistry themeRegistry;
  SdCardFontManager themeFontManager;

  void buildCurrentSdTheme();
  void applyThemeFontOverrides();
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
