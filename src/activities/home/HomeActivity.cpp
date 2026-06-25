#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FreeInkUI.h>
#include <FreeInkUIGfxRenderer.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/FreeInkThemeIconRegistry.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(const std::vector<int>& coverHeights) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      bool hasMissingThumb = false;
      for (const int coverHeight : coverHeights) {
        std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        if (!Storage.exists(coverPath.c_str())) {
          hasMissingThumb = true;
          break;
        }
      }

      if (hasMissingThumb) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = true;
          for (const int coverHeight : coverHeights) {
            std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
            if (!Storage.exists(coverPath.c_str())) {
              success = epub.generateThumbBmp(coverHeight) && success;
            }
          }
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = true;
            for (const int coverHeight : coverHeights) {
              std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
              if (!Storage.exists(coverPath.c_str())) {
                success = xtc.generateThumbBmp(coverHeight) && success;
              }
            }
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  LOG_DBG("HOME", "Loaded %d/%d recent book(s) for home theme", static_cast<int>(recentBooks.size()),
          metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);
  coverSelectorIndex = recentBooks.empty() ? 0 : std::min(selectorIndex, static_cast<int>(recentBooks.size()) - 1);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = makeUniqueNoThrow<uint8_t[]>(needed);
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                   coverBufferSize)) {
    coverBuffer.reset();
    coverBufferSize = 0;
    return false;
  }
  coverBufferSelectorIndex = coverSelectorIndex;
  coverBufferStripSelected = selectorIndex < static_cast<int>(recentBooks.size());
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                     coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  coverBuffer.reset();
  coverBufferSize = 0;
  coverBufferStored = false;
  coverBufferSelectorIndex = -1;
  coverBufferStripSelected = false;
}

void HomeActivity::loop() {
  if (UITheme::getInstance().hasFreeInkHomeLayout()) {
    buttonNavigator.onNext([this] {
      if (!recentBooks.empty()) {
        coverSelectorIndex = ButtonNavigator::nextIndex(coverSelectorIndex, recentBooks.size());
        selectorIndex = coverSelectorIndex;
        requestUpdate();
      }
    });

    buttonNavigator.onPrevious([this] {
      if (!recentBooks.empty()) {
        coverSelectorIndex = ButtonNavigator::previousIndex(coverSelectorIndex, recentBooks.size());
        selectorIndex = coverSelectorIndex;
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      homeTabIndex = ButtonNavigator::previousIndex(homeTabIndex, 4);
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      homeTabIndex = ButtonNavigator::nextIndex(homeTabIndex, 4);
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      switch (homeTabIndex) {
        case 0:
          if (!recentBooks.empty()) onSelectBook(recentBooks[coverSelectorIndex].path);
          break;
        case 1:
          onRecentsOpen();
          break;
        case 2:
          if (hasOpdsServers) {
            onOpdsBrowserOpen();
          } else {
            onFileTransferOpen();
          }
          break;
        case 3:
          onSettingsOpen();
          break;
        default:
          break;
      }
    }
    return;
  }

  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      coverSelectorIndex = selectorIndex;
    }
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    if (selectorIndex < static_cast<int>(recentBooks.size())) {
      coverSelectorIndex = selectorIndex;
    }
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
      switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
        case HomeMenuItem::FILE_BROWSER:
          onFileBrowserOpen();
          break;
        case HomeMenuItem::RECENTS:
          onRecentsOpen();
          break;
        case HomeMenuItem::OPDS_BROWSER:
          onOpdsBrowserOpen();
          break;
        case HomeMenuItem::FILE_TRANSFER:
          onFileTransferOpen();
          break;
        case HomeMenuItem::SETTINGS_MENU:
          onSettingsOpen();
          break;
        default:
          break;
      }
    }
  }
}

namespace {
using FIFrame = freeink::ui::Frame<32>;

freeink::ui::Rect fiRect(const freeink::ui::Rect& safe, const ThemeHomeElementSpec& item) {
  const int16_t x = static_cast<int16_t>(safe.x + item.x);
  const int16_t y = static_cast<int16_t>(safe.y + item.y);
  const int16_t w = static_cast<int16_t>(item.width > 0 ? item.width : safe.right() - x);
  const int16_t h = static_cast<int16_t>(item.height > 0 ? item.height : safe.bottom() - y);
  return freeink::ui::Rect{x, y, w, h};
}

freeink::ui::FontId fontTokenFor(int fontId) {
  if (fontId == SMALL_FONT_ID) return freeink::ui::GfxRendererTarget::FONT_SMALL;
  if (fontId == UI_12_FONT_ID) return freeink::ui::GfxRendererTarget::FONT_TITLE;
  return freeink::ui::GfxRendererTarget::FONT_BODY;
}

freeink::ui::TextStyle textStyleFor(const ThemeHomeElementSpec& item) {
  freeink::ui::TextStyle style;
  style.font = fontTokenFor(item.fontId);
  style.bold = item.bold;
  style.maxLines = item.type == ThemeHomeElementType::BookCard ? 2 : 1;
  return style;
}

freeink::ui::StyleSet cardStyles(uint8_t radius = 4) {
  auto styles = freeink::ui::defaultListRowStyles();
  styles.normal.background = freeink::ui::Paint::solid(freeink::ui::Color::White);
  styles.normal.border = freeink::ui::Paint::solid(freeink::ui::Color::Black);
  styles.normal.borderWidth = 1;
  styles.normal.radius = radius;
  styles.selected.background = freeink::ui::Paint::solid(freeink::ui::Color::Black);
  styles.selected.foreground = freeink::ui::Paint::solid(freeink::ui::Color::White);
  styles.selected.border = freeink::ui::Paint::solid(freeink::ui::Color::Black);
  styles.selected.borderWidth = 1;
  styles.selected.radius = radius;
  return styles;
}

void drawFolderTabs(const GfxRenderer& renderer, freeink::ui::Rect rect, const ThemeHomeElementSpec& item) {
  const std::vector<std::string> fallbackIcons = {"book-open", "library", "wifi", "sliders-horizontal"};
  const auto& icons = item.icons.empty() ? fallbackIcons : item.icons;
  if (icons.empty()) return;

  const int count = std::min<int>(icons.size(), 6);
  const int gap = std::max(0, item.gap);
  const int tabW = (rect.width - gap * (count - 1)) / count;
  const int slant = std::max(6, rect.height / 3);
  const int selected = std::max(0, std::min(item.selectedIndex, count - 1));

  for (int i = 0; i < count; ++i) {
    const int x = rect.x + i * (tabW + gap);
    const int w = i == count - 1 ? rect.right() - x : tabW;
    const bool active = i == selected;
    const int pointsX[4] = {x, x + w - slant, x + w, x + slant};
    const int pointsY[4] = {rect.y, rect.y, rect.bottom(), rect.bottom()};

    renderer.fillPolygon(pointsX, pointsY, 4, active ? false : true);
    renderer.drawLine(pointsX[0], pointsY[0], pointsX[1], pointsY[1], 1, true);
    renderer.drawLine(pointsX[1], pointsY[1], pointsX[2], pointsY[2], 1, true);
    renderer.drawLine(pointsX[2], pointsY[2], pointsX[3], pointsY[3], 1, true);
    renderer.drawLine(pointsX[3], pointsY[3], pointsX[0], pointsY[0], 1, true);

    const int iconSize = std::min(24, std::max(16, rect.height - 8));
    const int iconX = x + (w - iconSize) / 2;
    const int iconY = rect.y + (rect.height - iconSize) / 2;
    if (!active) renderer.fillRect(iconX - 3, iconY - 3, iconSize + 6, iconSize + 6, false);
    const freeink::Icon* icon = findFreeInkThemeIcon(icons[i].c_str(), iconSize);
    if (icon != nullptr) renderer.drawFreeInkIcon(*icon, iconX, iconY, iconSize, iconSize);
  }
}
}  // namespace

void HomeActivity::drawRecentCoverInRect(int bookIndex, Rect rect, int thumbHeight) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(recentBooks.size()) ||
      recentBooks[bookIndex].coverBmpPath.empty()) {
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
    renderer.fillRect(rect.x, rect.y + rect.height / 3, rect.width, 2 * rect.height / 3, true);
    return;
  }

  const std::string coverPath =
      UITheme::getCoverThumbPath(recentBooks[bookIndex].coverBmpPath, thumbHeight > 0 ? thumbHeight : rect.height);
  HalFile file;
  if (!Storage.openFileForRead("HOME", coverPath.c_str(), file)) {
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
    return;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() == BmpReaderError::Ok) {
    renderer.drawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height, 0.0f, 0.0f);
  } else {
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  }
  file.close();
}

bool HomeActivity::renderFreeInkHomeLayout(const ThemeHomeLayoutSpec& layout) {
  if (!layout.enabled) return false;

  auto& theme = UITheme::getInstance();
  freeink::ui::GfxRendererTarget target(renderer);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, theme.getSmallFontId());
  target.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, theme.getMediumFontId());
  target.setFont(freeink::ui::GfxRendererTarget::FONT_TITLE, theme.getLargeFontId());

  auto device = target.deviceContext();
  device.safeArea = freeink::ui::Insets{GfxRenderer::VIEWABLE_MARGIN_TOP, GfxRenderer::VIEWABLE_MARGIN_RIGHT,
                                        GfxRenderer::VIEWABLE_MARGIN_BOTTOM, GfxRenderer::VIEWABLE_MARGIN_LEFT};
  device.minTouchSize = 32;
  freeink::ui::InputSnapshot input;
  freeink::ui::InteractionBuffer<32> interactions;
  FIFrame frame(target, device, input, interactions);
  const freeink::ui::Rect safe = frame.safeRect();

  std::vector<std::string> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  if (hasOpdsServers) menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));

  for (const auto& item : layout.elements) {
    const auto rect = fiRect(safe, item);
    switch (item.type) {
      case ThemeHomeElementType::Box:
        if (item.fill) target.fill(rect, freeink::ui::Paint::dither(freeink::ui::Color::LightGray), item.radius);
        if (item.outline) {
          target.stroke(rect, freeink::ui::Paint::solid(freeink::ui::Color::Black), std::max(1, item.lineWidth),
                        item.radius);
        }
        break;
      case ThemeHomeElementType::Divider:
        target.line({rect.x, rect.y}, {static_cast<int16_t>(rect.x + rect.width), rect.y},
                    static_cast<uint8_t>(std::max(1, item.lineWidth)),
                    freeink::ui::Paint::solid(freeink::ui::Color::Black));
        break;
      case ThemeHomeElementType::Label: {
        auto style = textStyleFor(item);
        target.text(rect, item.text.c_str(), style);
        break;
      }
      case ThemeHomeElementType::TabBar: {
        if (!item.icons.empty()) {
          ThemeHomeElementSpec tabItem = item;
          tabItem.selectedIndex = homeTabIndex;
          drawFolderTabs(renderer, rect, tabItem);
          break;
        }
        const std::vector<std::string> fallback = {"reading", "library", "network", "settings"};
        const auto& labels = item.labels.empty() ? fallback : item.labels;
        std::vector<freeink::ui::TabItem> tabs;
        tabs.reserve(labels.size());
        for (size_t i = 0; i < labels.size() && i < 8; ++i) {
          tabs.push_back(
              {labels[i].c_str(), static_cast<int16_t>(i), static_cast<int>(i) == std::max(0, item.selectedIndex)});
        }
        freeink::ui::TabBarProps props;
        props.tabs = tabs.data();
        props.count = static_cast<uint8_t>(tabs.size());
        props.text = textStyleFor(item);
        props.tabStyles = cardStyles(static_cast<uint8_t>(std::max(0, item.radius)));
        props.divider = item.outline;
        props.tabInset = freeink::ui::Insets{2, 2, 2, 2};
        freeink::ui::tabBar(frame, rect, props);
        break;
      }
      case ThemeHomeElementType::BookCard: {
        const int bookIndex =
            recentBooks.empty() ? -1 : std::min(coverSelectorIndex, static_cast<int>(recentBooks.size()) - 1);
        freeink::ui::BookCardProps props;
        props.title = bookIndex >= 0 ? recentBooks[bookIndex].title.c_str() : tr(STR_NO_OPEN_BOOK);
        props.author = bookIndex >= 0 ? recentBooks[bookIndex].author.c_str() : tr(STR_START_READING);
        props.titleText = textStyleFor(item);
        props.titleText.font = freeink::ui::GfxRendererTarget::FONT_TITLE;
        props.titleText.bold = true;
        props.authorText = textStyleFor(item);
        props.authorText.font = freeink::ui::GfxRendererTarget::FONT_BODY;
        props.metaText = props.authorText;
        props.styles = cardStyles(static_cast<uint8_t>(std::max(0, item.radius)));
        props.coverSize = {
            static_cast<int16_t>(item.coverWidth > 0 ? item.coverWidth : 70),
            static_cast<int16_t>(item.coverHeight > 0 ? item.coverHeight : rect.height - item.padding * 2)};
        props.padding = {static_cast<int16_t>(item.padding), static_cast<int16_t>(item.padding),
                         static_cast<int16_t>(item.padding), static_cast<int16_t>(item.padding)};
        props.progress = 0;
        props.progressMax = 100;
        freeink::ui::bookCard(frame, rect, props);
        const int coverH = props.coverSize.height;
        const int coverW = props.coverSize.width;
        const Rect coverRect{rect.x + item.padding, rect.y + (rect.height - coverH) / 2, coverW, coverH};
        drawRecentCoverInRect(bookIndex, coverRect, coverH);
        break;
      }
      case ThemeHomeElementType::CoverGrid: {
        const int count = std::min<int>(item.count, recentBooks.size());
        std::vector<freeink::ui::CoverGridItem> covers;
        covers.reserve(count);
        for (int i = 0; i < count; ++i) {
          covers.push_back({recentBooks[i].title.c_str(),
                            {},
                            {},
                            selectorIndex == i ? freeink::ui::StateSelected : freeink::ui::StateNormal,
                            static_cast<int16_t>(i),
                            true});
        }
        freeink::ui::CoverGridProps props;
        props.items = covers.data();
        props.count = static_cast<uint16_t>(covers.size());
        props.selectedIndex = selectorIndex < count ? selectorIndex : -1;
        props.titleText = textStyleFor(item);
        props.titleText.font = freeink::ui::GfxRendererTarget::FONT_SMALL;
        props.cellStyles = cardStyles(static_cast<uint8_t>(std::max(0, item.radius)));
        props.columns = static_cast<uint8_t>(std::max(1, item.columns));
        props.coverSize = {static_cast<int16_t>(item.coverWidth > 0 ? item.coverWidth : 78),
                           static_cast<int16_t>(item.coverHeight > 0 ? item.coverHeight : 110)};
        props.rowHeight = static_cast<int16_t>(item.height > 0 ? item.height : props.coverSize.height + 22);
        props.gap = static_cast<int16_t>(std::max(0, item.gap));
        props.labelHeight = item.showTitle ? 20 : 0;
        freeink::ui::coverGrid(frame, rect, props);
        const int cellW = (rect.width - (props.columns - 1) * props.gap) / props.columns;
        for (int i = 0; i < count; ++i) {
          const int col = i % props.columns;
          const int row = i / props.columns;
          const Rect coverRect{rect.x + col * (cellW + props.gap) + (cellW - props.coverSize.width) / 2,
                               rect.y + row * (props.rowHeight + props.gap), props.coverSize.width,
                               props.coverSize.height};
          drawRecentCoverInRect(i, coverRect, props.coverSize.height);
        }
        break;
      }
      case ThemeHomeElementType::MenuGrid: {
        const int cols = std::max(1, item.columns);
        const int gap = std::max(0, item.gap);
        const int cellW = (rect.width - (cols - 1) * gap) / cols;
        const int rows = (static_cast<int>(menuItems.size()) + cols - 1) / cols;
        const int cellH = rows > 0 ? (rect.height - (rows - 1) * gap) / rows : rect.height;
        for (int i = 0; i < static_cast<int>(menuItems.size()); ++i) {
          const int col = i % cols;
          const int row = i / cols;
          freeink::ui::ButtonProps props;
          props.label = menuItems[i].c_str();
          props.text = textStyleFor(item);
          props.styles = cardStyles(static_cast<uint8_t>(std::max(0, item.radius)));
          props.state = selectorIndex - static_cast<int>(recentBooks.size()) == i ? freeink::ui::StateSelected
                                                                                  : freeink::ui::StateNormal;
          freeink::ui::button(
              frame,
              {static_cast<int16_t>(rect.x + col * (cellW + gap)), static_cast<int16_t>(rect.y + row * (cellH + gap)),
               static_cast<int16_t>(cellW), static_cast<int16_t>(cellH)},
              props);
        }
        break;
      }
      case ThemeHomeElementType::MetricCards: {
        const std::vector<std::string> labels =
            item.labels.empty() ? std::vector<std::string>{"last read", "total read", "completed"} : item.labels;
        const int count = std::min<int>(std::max(1, item.count), labels.size());
        const int gap = std::max(0, item.gap);
        const int cardW = (rect.width - (count - 1) * gap) / count;
        for (int i = 0; i < count; ++i) {
          freeink::ui::MetricCardProps props;
          props.label = labels[i].c_str();
          props.value = i == 0 ? "0" : (i == 1 ? "19" : "3");
          props.unit = i == 0 ? "min" : (i == 1 ? "h" : "books");
          props.labelText = textStyleFor(item);
          props.valueText = props.labelText;
          props.valueText.font = freeink::ui::GfxRendererTarget::FONT_TITLE;
          props.valueText.bold = true;
          props.captionText = props.labelText;
          props.styles = cardStyles(static_cast<uint8_t>(std::max(0, item.radius)));
          freeink::ui::metricCard(
              frame,
              {static_cast<int16_t>(rect.x + i * (cardW + gap)), rect.y, static_cast<int16_t>(cardW), rect.height},
              props);
        }
        break;
      }
    }
  }

  renderer.displayBuffer();
  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(UITheme::getInstance().getHomeCoverThumbHeights());
  }
  return true;
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasFreeInkHomeLayout()) {
    renderer.clearScreen();
    if (renderFreeInkHomeLayout(UITheme::getInstance().getHomeLayout())) return;
  }
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  constexpr int coverCacheBleed = 12;
  const bool hasCoverArea = metrics.homeCoverTileHeight > 0 && metrics.homeCoverHeight > 0;

  renderer.clearScreen();

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. Include a small bleed
  // because cover-strip themes can draw selection outlines just outside the
  // nominal cover tile.
  coverRectX = 0;
  coverRectY = hasCoverArea ? std::max(0, metrics.homeTopPadding - coverCacheBleed) : 0;
  coverRectW = pageWidth;
  coverRectH = hasCoverArea
                   ? std::min(pageHeight - coverRectY,
                              metrics.homeCoverTileHeight + (metrics.homeTopPadding - coverRectY) + coverCacheBleed)
                   : 0;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && metrics.homeShowContinueReadingHeader && !recentBooks.empty()
                     ? recentBooks[std::min(coverSelectorIndex, static_cast<int>(recentBooks.size()) - 1)].title.c_str()
                     : nullptr);

  const bool selectorSensitiveCoverCache = GUI.homeCoverCacheDependsOnSelector();
  const bool coverStripSelected = selectorIndex < static_cast<int>(recentBooks.size());
  bool bufferRestored = hasCoverArea && coverBufferStored &&
                        (!selectorSensitiveCoverCache || (coverBufferSelectorIndex == coverSelectorIndex &&
                                                          coverBufferStripSelected == coverStripSelected)) &&
                        restoreCoverBuffer();

  if (hasCoverArea) {
    GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                            recentBooks, coverSelectorIndex, coverRendered, coverBufferStored, bufferRestored,
                            std::bind(&HomeActivity::storeCoverBuffer, this), coverStripSelected);
  } else {
    coverRendered = false;
    coverBufferStored = false;
    bufferRestored = false;
  }

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (metrics.homeCoverHeight > 0 && !recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(UITheme::getInstance().getHomeCoverThumbHeights());
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
