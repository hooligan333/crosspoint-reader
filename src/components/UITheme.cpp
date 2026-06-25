#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFontRegistry.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"
#include "fontIds.h"

UITheme UITheme::instance;

namespace {
bool parseThemeFontFile(const char* filename, const std::string& family, uint8_t& pointSize) {
  if (filename == nullptr || family.empty()) return false;
  const size_t familyLen = family.size();

  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t len = strlen(filename);
  if (len <= familyLen + 1 + kExtLen || strcmp(filename + len - kExtLen, kExt) != 0) return false;
  if (strncmp(filename, family.c_str(), familyLen) != 0 || filename[familyLen] != '_') return false;

  const char* sizeStart = filename + familyLen + 1;
  const char* sizeEnd = filename + len - kExtLen;
  int value = 0;
  for (const char* p = sizeStart; p < sizeEnd; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    value = value * 10 + (*p - '0');
    if (value > 255) return false;
  }
  if (value <= 0) return false;
  pointSize = static_cast<uint8_t>(value);
  return true;
}

void scanThemeFontDir(const char* dirPath, const std::string& familyName, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t pointSize = 0;
    if (!parseThemeFontFile(nameBuffer, familyName, pointSize)) continue;
    bool duplicate = false;
    for (const auto& existing : family.files) {
      if (existing.pointSize == pointSize) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + nameBuffer;
    info.pointSize = pointSize;
    info.style = 0;
    family.files.push_back(std::move(info));
  }
}
}  // namespace

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::refreshRegistry() { themeRegistry.discover(); }

void UITheme::releaseSdThemeAssetMemory() {
  // Keep active SD theme backing storage intact; currentTheme may hold pointers
  // into it. This only releases discovered theme metadata that can be rebuilt.
  themeRegistry.clear();
}

void UITheme::releaseSdThemeForNetwork(GfxRenderer& renderer) {
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  // Free the SD theme's loaded glyph data and revert to the configured built-in
  // theme. setTheme() also resets the font ids and clears the registry, so the
  // current theme no longer points into freed SD assets. WiFi/lwip init needs a
  // large contiguous block; reclaiming the theme fonts gives it the best chance.
  // Reversible: callers restore via reload() + prepareSdAssets() on return.
  themeFontManager.unloadAll(renderer);
  setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme));

  const size_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  LOG_INF("UI", "Released SD theme for network: free %u->%u, largest block %u->%u", (unsigned)freeBefore,
          (unsigned)freeAfter, (unsigned)largestBefore, (unsigned)largestAfter);
}

void UITheme::prepareSdAssets(GfxRenderer& renderer) {
  themeFontManager.unloadAll(renderer);
  if (currentSdThemePath.empty() || currentSdUiFontFamily.empty()) return;

  SdCardFontFamilyInfo family;
  family.name = currentSdUiFontFamily;

  char dirPath[220];
  snprintf(dirPath, sizeof(dirPath), "%s/fonts", currentSdThemePath.c_str());
  scanThemeFontDir(dirPath, currentSdUiFontFamily, family);
  snprintf(dirPath, sizeof(dirPath), "%s/fonts/%s", currentSdThemePath.c_str(), currentSdUiFontFamily.c_str());
  scanThemeFontDir(dirPath, currentSdUiFontFamily, family);

  if (family.files.empty()) {
    LOG_ERR("UI", "Theme UI font family has no files: %s", currentSdUiFontFamily.c_str());
    return;
  }
  std::sort(family.files.begin(), family.files.end(),
            [](const auto& a, const auto& b) { return a.pointSize < b.pointSize; });

  if (!themeFontManager.loadAllSizes(family, renderer)) {
    LOG_ERR("UI", "Failed to load theme UI font family: %s", currentSdUiFontFamily.c_str());
    return;
  }

  applyThemeFontOverrides();
  buildCurrentSdTheme();
}

std::vector<int> UITheme::getHomeCoverThumbHeights() const {
  std::vector<int> heights;
  heights.reserve(1 + currentSdHomeRecents.slots.size());
  auto addHeight = [&heights](int height) {
    if (height > 0 && std::find(heights.begin(), heights.end(), height) == heights.end()) {
      heights.push_back(height);
    }
  };

  addHeight(currentMetrics->homeCoverHeight);
  if (currentSdHomeRecents.type == ThemeHomeRecentsType::CoverStrip) {
    for (const auto& slot : currentSdHomeRecents.slots) {
      addHeight(slot.height);
    }
  }
  if (currentSdHomeLayout.enabled) {
    std::function<void(const ThemeHomeElementSpec&)> collectLayoutHeight = [&](const ThemeHomeElementSpec& item) {
      if (item.type == ThemeHomeElementType::BookCard || item.type == ThemeHomeElementType::CoverGrid) {
        addHeight(item.coverHeight > 0 ? item.coverHeight : item.height);
      }
      for (const auto& child : item.children) {
        collectLayoutHeight(child);
      }
    };
    for (const auto& item : currentSdHomeLayout.elements) {
      collectLayoutHeight(item);
    }
  }
  return heights;
}

void UITheme::reload() {
  if (SETTINGS.sdThemeName[0] != '\0') {
    const SdCardThemeInfo* themeInfo = themeRegistry.findTheme(SETTINGS.sdThemeName);
    if (themeInfo == nullptr) {
      refreshRegistry();
      themeInfo = themeRegistry.findTheme(SETTINGS.sdThemeName);
    }
    if (themeInfo == nullptr) {
      LOG_ERR("UI", "SD theme not found: %s (falling back to built-in theme)", SETTINGS.sdThemeName);
      themeRegistry.clear();
      SETTINGS.sdThemeName[0] = '\0';
      SETTINGS.saveToFile();
      setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme));
      return;
    }

    LOG_DBG("UI", "Using SD theme: %s recentsType=%d count=%d slots=%d", themeInfo->id.c_str(),
            static_cast<int>(themeInfo->homeRecents.type), themeInfo->metrics.homeRecentBooksCount,
            static_cast<int>(themeInfo->homeRecents.slots.size()));
    currentSdMetrics = themeInfo->metrics;
    currentSdHomeRecents = themeInfo->homeRecents;
    currentSdButtonMenu = themeInfo->buttonMenu;
    currentSdList = themeInfo->list;
    currentSdButtonHints = themeInfo->buttonHints;
    currentSdTabBar = themeInfo->tabBar;
    currentSdHeader = themeInfo->header;
    currentSdHomeLayout = themeInfo->homeLayout;
    currentSdSettingsScreen = themeInfo->settingsScreen;
    currentSdReaderMenuScreen = themeInfo->readerMenuScreen;
    currentSdSettingsMetrics = currentSdSettingsScreen.metrics;
    currentSdReaderMenuMetrics = currentSdReaderMenuScreen.metrics;
    currentSdThemePath = themeInfo->path;
    currentSdUiFontFamily = themeInfo->uiFontFamily;
    currentSdIcons = themeInfo->icons;
    currentSdNamedIcons = themeInfo->namedIcons;
    currentSdFreeInkIcons = themeInfo->freeInkIcons;
    currentSdNamedFreeInkIcons = themeInfo->namedFreeInkIcons;
    currentSdInheritsClassic = themeInfo->inherits == "classic";
    themeRegistry.clear();
    buildCurrentSdTheme();
    return;
  }

  setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme));
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  std::unique_ptr<BaseTheme> nextTheme;
  const ThemeMetrics* nextMetrics = &LyraMetrics::values;

  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      nextTheme = std::make_unique<BaseTheme>();
      nextMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      nextTheme = std::make_unique<LyraTheme>();
      nextMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      nextTheme = std::make_unique<RoundedRaffTheme>();
      nextMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      nextTheme = std::make_unique<Lyra3CoversTheme>();
      nextMetrics = &Lyra3CoversMetrics::values;
      break;
    default:
      LOG_DBG("UI", "Using Lyra theme");
      nextTheme = std::make_unique<LyraTheme>();
      nextMetrics = &LyraMetrics::values;
      break;
  }

  currentTheme = std::move(nextTheme);
  currentMetrics = nextMetrics;
  currentSdMetrics = ThemeMetrics{};
  currentSdHomeRecents = ThemeHomeRecentsSpec{};
  currentSdButtonMenu = ThemeButtonMenuSpec{};
  currentSdList = ThemeListSpec{};
  currentSdButtonHints = ThemeButtonHintsSpec{};
  currentSdTabBar = ThemeTabBarSpec{};
  currentSdHeader = ThemeHeaderSpec{};
  currentSdHomeLayout = ThemeHomeLayoutSpec{};
  currentSdSettingsScreen = ThemeScreenChromeSpec{};
  currentSdReaderMenuScreen = ThemeScreenChromeSpec{};
  currentSdSettingsMetrics = ThemeMetrics{};
  currentSdReaderMenuMetrics = ThemeMetrics{};
  currentSdThemePath.clear();
  currentSdUiFontFamily.clear();
  currentSdInheritsClassic = false;
  currentSmallFontId = SMALL_FONT_ID;
  currentMediumFontId = UI_10_FONT_ID;
  currentLargeFontId = UI_12_FONT_ID;
  currentSdIcons.clear();
  currentSdNamedIcons.clear();
  currentSdFreeInkIcons.clear();
  currentSdNamedFreeInkIcons.clear();
  currentSettingsTheme.reset();
  currentReaderMenuTheme.reset();
  themeRegistry.clear();
}

void UITheme::buildCurrentSdTheme() {
  currentSettingsTheme.reset();
  currentReaderMenuTheme.reset();
  if (currentSdInheritsClassic) {
    currentTheme = std::make_unique<BaseTheme>();
    currentMetrics = &currentSdMetrics;
    return;
  }
  const ThemeHomeRecentsSpec* homeRecents =
      currentSdHomeRecents.type != ThemeHomeRecentsType::Default ? &currentSdHomeRecents : nullptr;
  const ThemeButtonMenuSpec* buttonMenu = currentSdButtonMenu.enabled ? &currentSdButtonMenu : nullptr;
  const ThemeListSpec* list = currentSdList.enabled ? &currentSdList : nullptr;
  const ThemeButtonHintsSpec* buttonHints = currentSdButtonHints.enabled ? &currentSdButtonHints : nullptr;
  const ThemeTabBarSpec* tabBar = currentSdTabBar.enabled ? &currentSdTabBar : nullptr;
  const ThemeHeaderSpec* header = currentSdHeader.enabled ? &currentSdHeader : nullptr;
  currentTheme = std::make_unique<LyraTheme>(&currentSdMetrics, homeRecents, buttonMenu, list, buttonHints, tabBar,
                                             header, currentSdThemePath.c_str(), &currentSdIcons,
                                             &currentSdFreeInkIcons, &currentSdNamedIcons, &currentSdNamedFreeInkIcons);
  currentMetrics = &currentSdMetrics;
  currentSettingsTheme = buildScreenTheme(currentSdSettingsScreen);
  currentReaderMenuTheme = buildScreenTheme(currentSdReaderMenuScreen);
}

std::unique_ptr<BaseTheme> UITheme::buildScreenTheme(const ThemeScreenChromeSpec& screen) {
  if (!screen.enabled || currentSdInheritsClassic) return nullptr;
  const ThemeListSpec* list = screen.list.enabled ? &screen.list : (currentSdList.enabled ? &currentSdList : nullptr);
  const ThemeButtonHintsSpec* buttonHints = screen.buttonHints.enabled
                                                ? &screen.buttonHints
                                                : (currentSdButtonHints.enabled ? &currentSdButtonHints : nullptr);
  const ThemeTabBarSpec* tabBar =
      screen.tabBar.enabled ? &screen.tabBar : (currentSdTabBar.enabled ? &currentSdTabBar : nullptr);
  const ThemeHeaderSpec* header =
      screen.header.enabled ? &screen.header : (currentSdHeader.enabled ? &currentSdHeader : nullptr);
  return std::make_unique<LyraTheme>(&screen.metrics, nullptr, nullptr, list, buttonHints, tabBar, header,
                                     currentSdThemePath.c_str(), &currentSdIcons, &currentSdFreeInkIcons,
                                     &currentSdNamedIcons, &currentSdNamedFreeInkIcons);
}

void UITheme::applyThemeFontOverrides() {
  const int smallId = themeFontManager.getFontIdForPointSize(currentSdUiFontFamily, 8);
  const int mediumId = themeFontManager.getFontIdForPointSize(currentSdUiFontFamily, 10);
  const int largeId = themeFontManager.getFontIdForPointSize(currentSdUiFontFamily, 12);
  currentSmallFontId = smallId != 0 ? smallId : SMALL_FONT_ID;
  currentMediumFontId = mediumId != 0 ? mediumId : UI_10_FONT_ID;
  currentLargeFontId = largeId != 0 ? largeId : UI_12_FONT_ID;
  auto remap = [smallId, mediumId, largeId](int& fontId) {
    if (fontId == SMALL_FONT_ID && smallId != 0) fontId = smallId;
    if (fontId == UI_10_FONT_ID && mediumId != 0) fontId = mediumId;
    if (fontId == UI_12_FONT_ID && largeId != 0) fontId = largeId;
  };

  remap(currentSdButtonMenu.fontId);
  remap(currentSdList.fontId);
  remap(currentSdList.subtitleFontId);
  remap(currentSdList.valueFontId);
  remap(currentSdButtonHints.fontId);
  remap(currentSdTabBar.fontId);
  remap(currentSdHeader.fontId);
  auto remapScreen = [&](ThemeScreenChromeSpec& screen) {
    remap(screen.list.fontId);
    remap(screen.list.subtitleFontId);
    remap(screen.list.valueFontId);
    remap(screen.buttonHints.fontId);
    remap(screen.tabBar.fontId);
    remap(screen.header.fontId);
  };
  remapScreen(currentSdSettingsScreen);
  remapScreen(currentSdReaderMenuScreen);
  for (auto& slot : currentSdHomeRecents.slots) {
    remap(slot.title.fontId);
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += currentMetrics->buttonHintsHeight;
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += currentMetrics->buttonHintsHeight;
        safeArea.height -= currentMetrics->buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= currentMetrics->buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar =
      SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
      SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE || SETTINGS.statusBarBattery ||
      SETTINGS.statusBarClock != CrossPointSettings::STATUS_BAR_CLOCK_MODE::STATUS_BAR_CLOCK_HIDE;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}
