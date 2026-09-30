#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#ifdef CROSSPOINT_SDFONT_ADVANCE_LIMIT
#include <HalHeapGauge.h>  // free-heap gate for the cache release below
#endif
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/dictionaryIcons.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/DictPlainPaging.h"
#include "util/HtmlToPlainText.h"

namespace fui = freeink::ui;

namespace {

// Header text rows start this far below the theme's top padding.
constexpr int HEADER_TEXT_OFFSET = 10;

// Pencil button: a finger-sized square centered on the 24 px glyph, which sits
// this far below the page-counter row, both above the body's first line.
constexpr int EDIT_HIT_SIZE = 44;
constexpr int EDIT_ICON_GAP = 4;

// Keyboard cap for an edited word: well above any headword, well below the
// dictionary's 256-byte scan buffer.
constexpr size_t MAX_EDIT_WORD_BYTES = 128;

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

#ifdef CROSSPOINT_SDFONT_ADVANCE_LIMIT
// Cache-release floor, enlarged-advance-table builds only. Dropping the SD font
// caches on exit also drops the persistent advance table, and at the raised cap
// that table holds a whole CJK section's metadata -- so the page underneath pays
// a full advance/kern/ligature rebuild for every word looked up. Only worth that
// when the heap actually needs the space; matches the 40 KB floor SdCardFont
// uses for its own retention bets (MINI_RETAIN_MIN_FREE_HEAP).
constexpr size_t CACHE_RETAIN_MIN_FREE_HEAP = 40 * 1024;
#endif

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition);
    wrapText();
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  Activity::onExit();
#ifdef CROSSPOINT_SDFONT_ADVANCE_LIMIT
  if (gateFreeHeap() >= CACHE_RETAIN_MIN_FREE_HEAP) return;
#endif
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
}

DictionaryDefinitionActivity::ContentFrame DictionaryDefinitionActivity::contentFrame() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  return {isLandscapeCw ? hintGutterWidth : 0, isInverted ? metrics.buttonHintsHeight : 0,
          renderer.getScreenWidth() - hintGutterWidth};
}

// Right-aligned with the page counter, on the row below the header text. Computed the same
// way whether or not the counter is drawn (single-page definitions hide it).
DictionaryDefinitionActivity::EditBox DictionaryDefinitionActivity::editBox() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const ContentFrame frame = contentFrame();
  const int headerY = frame.y + metrics.topPadding + HEADER_TEXT_OFFSET;
  const int iconX = frame.x + frame.width - SIDE_PADDING - icon_dict_edit_24.w;
  // Below the taller of the counter and the (bold UI_12) headword, so a long
  // headword running to the right edge cannot touch the glyph.
  const int rowHeight = std::max(renderer.getLineHeight(UI_10_FONT_ID), renderer.getLineHeight(UI_12_FONT_ID));
  const int iconY = headerY + rowHeight + EDIT_ICON_GAP;
  const int inset = (EDIT_HIT_SIZE - icon_dict_edit_24.w) / 2;
  return {iconX - inset, iconY - inset, EDIT_HIT_SIZE};
}

// The definition stays alive under the keyboard so Cancel returns to it
// unchanged; only a confirmed, non-empty edit finishes it.
void DictionaryDefinitionActivity::openEditor() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_DICT_EDIT_WORD), headword,
                                                           MAX_EDIT_WORD_BYTES);
  if (!keyboard) {
    LOG_ERR("DICT", "OOM: edit-word keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    const auto* edited = std::get_if<KeyboardResult>(&result.data);
    if (result.isCancelled || !edited || edited->text.empty()) return;
    setResult(KeyboardResult{edited->text});
    finish();
  });
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int topArea = (isInverted ? metrics.buttonHintsHeight : 0) + metrics.topPadding + metrics.headerHeight;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;
  return {renderer.getScreenWidth() - hintGutterWidth - 2 * SIDE_PADDING,
          renderer.getScreenHeight() - topArea - bottomArea};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}

// Greedy word-wrap of `definition` into byte spans, then height-based
// pagination. '\n' breaks lines; a blank line survives (runs collapsed to one)
// as a half-height paragraph gap. NULs from multi-type StarDict entries were
// normalized to newlines in onEnter; '\r' is dropped by treating it as a
// space at a token edge.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x01 /* REGULAR */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    const auto len = static_cast<uint16_t>(lineEnd - lineStart);
    // One blank line per gap; none before the first text line.
    if (len != 0 || (!lines.empty() && lines.back().len != 0)) lines.push_back({lineStart, len});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  dict_plain_paging::paginate(lines, lineHeight, body.height, pageStarts);
  totalPages = static_cast<int>(pageStarts.size());
  currentPage = 0;
}

void DictionaryDefinitionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (editable && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openEditor();
    return;
  }

  // Same swipe mapping and per-direction gesture settings as the reader's
  // detectTouchPageTurn (LTR): right-to-left = next, left-to-right = previous.
  // Checked after Back, so a left-edge swipe still exits.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left && ReaderUtils::gestureAllowsSwipe(SETTINGS.pageTurnGesture)) {
    nextPage();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right && ReaderUtils::gestureAllowsSwipe(SETTINGS.previousPageGesture)) {
    previousPage();
    return;
  }

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. The pencil button is tested first so it never turns.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    if (editable) {
      const EditBox box = editBox();
      if (tx >= box.x && tx < box.x + box.size && ty >= box.y && ty < box.y + box.size) {
        openEditor();
        return;
      }
    }
    if (tx < renderer.getScreenWidth() / 3) {
      previousPage();
    } else {
      nextPage();
    }
    return;
  }

  buttonNavigator.onNext([this] { nextPage(); });
  buttonNavigator.onPrevious([this] { previousPage(); });
}

void DictionaryDefinitionActivity::nextPage() {
  if (currentPage + 1 < totalPages) {
    currentPage++;
    requestUpdate();
  }
}

void DictionaryDefinitionActivity::previousPage() {
  if (currentPage > 0) {
    currentPage--;
    requestUpdate();
  }
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  if (pageStarts.empty()) return;
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const size_t firstLine = pageStarts[currentPage];
  const size_t lastLine =
      static_cast<size_t>(currentPage) + 1 < pageStarts.size() ? pageStarts[currentPage + 1] : lines.size();
  int y = startY;
  for (size_t i = firstLine; i < lastLine; i++) {
    const int advance = dict_plain_paging::lineAdvance(lines, i, firstLine, lineHeight);
    if (lines[i].len != 0) {
      const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
      memcpy(buf, definition.c_str() + lines[i].start, len);
      buf[len] = '\0';
      renderer.drawText(fontId, x, y, buf);
    }
    y += advance;
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const ContentFrame frame = contentFrame();
  const int contentX = frame.x;
  const int contentWidth = frame.width;
  const int contentY = frame.y;

  // Header: matched headword left, page counter right, pencil below it.
  const int headerY = contentY + metrics.topPadding + HEADER_TEXT_OFFSET;
  renderer.drawText(UI_12_FONT_ID, contentX + SIDE_PADDING, headerY, headword.c_str(), true, EpdFontFamily::BOLD);
  if (totalPages > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - SIDE_PADDING - counterWidth, headerY, counter);
  }
  // Touch only: button boards reach the editor through the Confirm hint.
  if (editable && mappedInput.hasTouch()) {
    const EditBox box = editBox();
    const auto size = static_cast<int16_t>(box.size);
    fui::GfxRendererTarget target(renderer);
    target.bitmap(fui::Rect{static_cast<int16_t>(box.x), static_cast<int16_t>(box.y), size, size},
                  fui::bitmapFromIcon(icon_dict_edit_24), fui::BitmapMode::Center);
  }

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const int bodyStartY = contentY + metrics.topPadding + metrics.headerHeight;
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), editable ? tr(STR_DICT_EDIT_WORD) : "",
                                            (currentPage > 0 ? "<" : ""), (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
