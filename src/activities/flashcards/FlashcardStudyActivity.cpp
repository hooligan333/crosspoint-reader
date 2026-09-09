#include "FlashcardStudyActivity.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "flashcards/DeckPaths.h"
#include "flashcards/StudyClock.h"
#include "fontIds.h"
#include "util/DestFolder.h"

namespace {

// Card text font (FLASHCARD_SPEC.md §5). The UI font is used for everything
// that frames it: header, counts, grade labels, hints.
constexpr int CARD_FONT_ID = NOTOSERIF_16_FONT_ID;

// Grade columns, in Anki order. Index == (uint8_t)fsrs::Grade, and it is also
// the on-screen left-to-right order and the touch column order, so the label
// under a physical button and the box above it are always the same grade.
constexpr StrId GRADE_LABELS[fsrs::GRADE_COUNT] = {StrId::STR_DECK_GRADE_AGAIN, StrId::STR_DECK_GRADE_HARD,
                                                   StrId::STR_DECK_GRADE_GOOD, StrId::STR_DECK_GRADE_EASY};

constexpr uint32_t SECONDS_PER_MINUTE = 60;
constexpr uint32_t SECONDS_PER_HOUR = 3600;
constexpr uint32_t DAYS_PER_MONTH = 30;
constexpr uint32_t DAYS_PER_YEAR = 365;

/**
 * Case-insensitive insertion sort of the deck leaves, so the picker reads the
 * way a file browser does.
 *
 * Insertion rather than std::sort: the list is capped at MAX_DECKS and the
 * introsort instantiation for vector<string> costs several hundred bytes of
 * IROM on a budget measured in bytes (platformio.ini, flashcards block). At 64
 * items the quadratic term is invisible next to the directory scan that
 * produced them.
 */
void sortLeaves(std::vector<std::string>& leaves) {
  for (size_t i = 1; i < leaves.size(); i++) {
    std::string held = std::move(leaves[i]);
    size_t j = i;
    while (j > 0 && strcasecmp(leaves[j - 1].c_str(), held.c_str()) > 0) {
      leaves[j] = std::move(leaves[j - 1]);
      j--;
    }
    leaves[j] = std::move(held);
  }
}

/** The picker label: the leaf without its `.deck` extension. */
std::string deckLabel(const std::string& leaf) {
  const size_t extLength = strlen(flashcards::DECK_EXTENSION);
  if (leaf.size() > extLength) return leaf.substr(0, leaf.size() - extLength);
  return leaf;
}

/**
 * "HH:MM" (or "H:MM AM") for a UTC unix time, in the user's local offset and
 * clock format. halClock::formatTime only ever formats *now*, and this needs an
 * arbitrary future due time, so the wall-clock split is done here.
 */
void formatLocalTime(const uint32_t unixSecs, char* out, const size_t outBytes) {
  const int64_t local = static_cast<int64_t>(unixSecs) + flashcards::localUtcOffsetSecs();
  // Floored modulo: a negative local time (a UTC-12 device just after the
  // epoch) must still land inside the day, not on a negative hour.
  const int64_t secondsOfDay = ((local % 86400) + 86400) % 86400;
  const unsigned hour = static_cast<unsigned>(secondsOfDay / SECONDS_PER_HOUR);
  const unsigned minute = static_cast<unsigned>((secondsOfDay % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE);
  if (SETTINGS.clockFormat == 1) {
    const unsigned hour12 = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(out, outBytes, "%u:%02u %s", hour12, minute, hour < 12 ? "AM" : "PM");
    return;
  }
  snprintf(out, outBytes, "%02u:%02u", hour, minute);
}

}  // namespace

FlashcardStudyActivity::FlashcardStudyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FlashcardStudy", renderer, mappedInput) {}

// --- Lifecycle ---

void FlashcardStudyActivity::onEnter() {
  Activity::onEnter();
  // Normalized here rather than trusted: the web settings API and a hand-edited
  // settings.json write flashcardDestFolder verbatim, and DeckPaths requires an
  // already-normalized folder from every caller.
  destFolder = normalizeDestFolder(SETTINGS.flashcardDestFolder, DECK_DEFAULT_FOLDER);
  clockAvailable = flashcards::clockIsAvailable();
  scanDeckFolder();
  screen = Screen::Picker;
  requestUpdate();
}

void FlashcardStudyActivity::onExit() {
  // The popup outlives nothing: its callback captures `this`, so it must not be
  // left armed over a teardown.
  cardMenu.dismiss();
  // State store first, then the deck it borrows: StateStore holds a DeckFile*
  // and reads the deck's keys on every record it touches.
  closeDeck();
  releaseSessionBuffers();
  Activity::onExit();
}

void FlashcardStudyActivity::closeDeck() {
  store.close();
  deck.close();
  session.reset();
  haveCard = false;
}

// --- Picker ---

void FlashcardStudyActivity::scanDeckFolder() {
  deckLeaves.clear();
  deckLeaves.reserve(MAX_DECKS);
  deckSelection = 0;

  auto dir = Storage.open(destFolder.c_str());
  if (!dir || !dir.isDirectory()) return;  // no folder yet: the empty-state hint covers it
  dir.rewindDirectory();

  char name[96];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;  // skips .state, which lives in here
    entry.getName(name, sizeof(name));
    // Dot files are macOS AppleDouble forks and our own hidden state; neither
    // is a deck, and offering one would fail validation on selection.
    if (name[0] == '.') continue;
    if (!FsHelpers::checkFileExtension(std::string_view{name}, flashcards::DECK_EXTENSION)) continue;
    if (deckLeaves.size() >= MAX_DECKS) break;  // capped listing; the rest stay unlisted
    deckLeaves.emplace_back(name);
  }
  sortLeaves(deckLeaves);
}

int FlashcardStudyActivity::deckRowsPerPage() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = GUI.getMenuRowHeight(renderer);
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const int usable = renderer.getScreenHeight() - top - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return std::max(1, usable / std::max(1, rowHeight + metrics.menuSpacing));
}

void FlashcardStudyActivity::openSelectedDeck() {
  if (deckSelection < 0 || deckSelection >= static_cast<int>(deckLeaves.size())) {
    fail(StrId::STR_DECK_OPEN_FAILED);
    return;
  }
  const std::string& leaf = deckLeaves[deckSelection];
  closeDeck();

  const flashcards::DeckError deckError = deck.open(flashcards::deckPathFor(destFolder, leaf));
  if (deckError != flashcards::DeckError::Ok) {
    LOG_ERR("DECK", "Deck open failed (%s): %s", flashcards::deckErrorName(deckError), leaf.c_str());
#ifdef CROSSPOINT_FLASHCARDS_C3
    // FLASHCARD_SPEC.md §7b.3 pin b. On this build the cap is 2000 cards, and a
    // deck above it is an ordinary thing to meet — one built for the Pro,
    // copied across by USB, past the sync screen that would have refused it. It
    // is not a broken file and must not be reported as one: "too large for this
    // device" tells the reader to fetch a smaller deck, where "could not be
    // opened" sends them looking for a corruption that is not there.
    // BadCardCount also covers a 0-card deck, which the deck server and the
    // sync screen both refuse upstream and which nobody can act on either way.
    fail(deckError == flashcards::DeckError::BadCardCount ? StrId::STR_DECK_TOO_LARGE : StrId::STR_DECK_OPEN_FAILED);
#else
    fail(StrId::STR_DECK_OPEN_FAILED);
#endif
    return;
  }

  // Without a clock there is no honest day number, so the counters are rolled
  // against the day the file already carries (StateStore takes the high-water
  // day) and only Cram -- which writes nothing -- is offered below.
  clockAvailable = flashcards::clockIsAvailable();
  if (!clockAvailable || !flashcards::studyNow(now)) {
    clockAvailable = false;
    now = fsrs::Now{};
  }
  const flashcards::StateError stateError = store.open(deck, destFolder, leaf, now.dayNumber);
  if (stateError != flashcards::StateError::Ok) {
    LOG_ERR("DECK", "State open failed (%s): %s", flashcards::stateErrorName(stateError), leaf.c_str());
    closeDeck();
    fail(StrId::STR_DECK_STATE_FAILED);
    return;
  }

  session = makeUniqueNoThrow<flashcards::Session>();
  if (!session) {
    LOG_ERR("DECK", "OOM: session (%u bytes)", static_cast<unsigned>(sizeof(flashcards::Session)));
    closeDeck();
    fail(StrId::STR_DECK_STATE_FAILED);
    return;
  }
  // A Due build is what the mode screen's counts come from, and selecting Due
  // then reuses this queue rather than scanning the file a second time.
  if (!flashcards::buildSession(store, flashcards::SessionMode::Due, now, *session)) {
    closeDeck();
    fail(StrId::STR_DECK_STATE_FAILED);
    return;
  }

  {
    RenderLock lock(*this);
    mode = clockAvailable ? flashcards::SessionMode::Due : flashcards::SessionMode::CramAll;
    modeSelection = 0;
    screen = Screen::Mode;
  }
  requestUpdate();
}

void FlashcardStudyActivity::goToPicker() {
  cardMenu.dismiss();
  // Screen FIRST, under the lock, and only then the teardown: the buffers and
  // the session about to be freed are exactly what a Review or Mode render
  // reads, and a Back that lands while one is in flight would otherwise free
  // them under it. Taking the lock waits that frame out, and the Picker frames
  // that can follow read neither (renderPicker() is deckLeaves only, and the
  // header stops reading the deck on this screen -- see render()).
  {
    RenderLock lock(*this);
    screen = Screen::Picker;
    haveCard = false;
  }
  closeDeck();
  releaseSessionBuffers();
  requestUpdate();
}

void FlashcardStudyActivity::fail(const StrId message) {
  cardMenu.dismiss();
  {
    RenderLock lock(*this);
    failure = message;
    screen = Screen::Failed;
    haveCard = false;
  }
  requestUpdate();
}

// --- Session buffers ---

bool FlashcardStudyActivity::allocateSessionBuffers() {
  // Allocated once per session and reused by every card: the format caps a side
  // at 4096 bytes, which is far too big for the task stack, and re-allocating
  // per card would churn the heap for the whole session.
  if (!frontText) frontText = makeUniqueNoThrow<char[]>(CARD_TEXT_BYTES);
  if (!backText) backText = makeUniqueNoThrow<char[]>(CARD_TEXT_BYTES);
  if (!lines) lines = makeUniqueNoThrow<CardLine[]>(MAX_CARD_LINES);
  if (frontText && backText && lines) return true;
  LOG_ERR("DECK", "OOM: card buffers (2x%u B text + %u B lines)", static_cast<unsigned>(CARD_TEXT_BYTES),
          static_cast<unsigned>(MAX_CARD_LINES * sizeof(CardLine)));
  releaseSessionBuffers();
  return false;
}

void FlashcardStudyActivity::releaseSessionBuffers() {
  frontText.reset();
  backText.reset();
  lines.reset();
  lineCount = 0;
}

// --- Mode -> review ---

bool FlashcardStudyActivity::buildSelectedSession() {
  if (!session) return false;
  // Due was already built for the mode screen's counts; only the other two
  // modes need the file scanned again.
  if (mode != flashcards::SessionMode::Due && !flashcards::buildSession(store, mode, now, *session)) return false;
  return allocateSessionBuffers();
}

void FlashcardStudyActivity::startReview() {
  queueIndex = 0;
  pendingCount = 0;
  studiedNew = studiedReview = studiedLearning = 0;
  droppedRequeues = 0;
  nextDueUnix = 0;
  undo.valid = false;
  haveCard = false;
  advanceToNextCard();
}

void FlashcardStudyActivity::finishSession() {
  cardMenu.dismiss();
  // Recomputed from disk, not from the summary the session was built with: by
  // now every card answered in this session has moved its due.
  nextDueUnix = 0;
  if (clockAvailable && mode != flashcards::SessionMode::CramAll) {
    uint32_t due = 0;
    if (flashcards::nextIntradayDue(store, now.unixSecs, due)) nextDueUnix = due;
  }
  {
    RenderLock lock(*this);
    haveCard = false;
    screen = Screen::Done;
  }
  requestUpdate();
}

bool FlashcardStudyActivity::refreshNow() {
  if (mode == flashcards::SessionMode::CramAll) return true;  // cram never reads the clock
  return flashcards::studyNow(now);
}

// --- Queue ---

void FlashcardStudyActivity::decayPending() {
  for (uint8_t i = 0; i < pendingCount; i++) {
    if (pending[i].showAfter > 0) pending[i].showAfter--;
  }
}

void FlashcardStudyActivity::requeueCurrent(const uint32_t dueUnix) {
  // A full list drops the card rather than growing: its new due is minutes
  // away, so the drain-time learn-ahead pull picks it up again once the list
  // has room, and in cram nothing was persisted to lose. The drop is counted
  // and shown on the Done screen (§5 pin e) -- 64 cards deep it is a corner
  // case, but never a silent one.
  if (pendingCount >= MAX_PENDING) {
    if (droppedRequeues != UINT16_MAX) droppedRequeues++;
    return;
  }
  pending[pendingCount].dueUnix = dueUnix;
  pending[pendingCount].ordinal = currentOrdinal;
  pending[pendingCount].showAfter = REQUEUE_GAP;
  pendingCount++;
}

void FlashcardStudyActivity::dropFromPending(const flashcards::Ordinal ordinal) {
  for (uint8_t i = 0; i < pendingCount; i++) {
    if (pending[i].ordinal != ordinal) continue;
    for (uint8_t j = i + 1; j < pendingCount; j++) pending[j - 1] = pending[j];
    pendingCount--;
    return;
  }
}

/**
 * Picks the next card, in the order §5 asks for: a resurfaced card that has
 * waited its turn AND come due, then the built queue, then whatever is still
 * waiting (the "at end if fewer remain" half of the requeue rule, which is also
 * this session's learn-ahead pull), and only when all of that is empty does it
 * go back to the state file for a learn-ahead card.
 *
 * `now` is refreshed by the callers that can reach a due comparison --
 * answerCurrent() and suspendCurrent() both read the clock before handing over
 * -- so the comparisons below run against a time no older than the action that
 * got us here, and the hot path needs no clock read of its own. startReview()
 * is the one caller that does not refresh, and harmlessly so: it enters with an
 * EMPTY pending list, so there is no parked due to compare, and by the time
 * there is one an answer has been through refreshNow().
 */
void FlashcardStudyActivity::advanceToNextCard() {
  flashcards::Ordinal next = 0;
  bool found = false;
  // Cram schedules nothing and may be running without a clock at all, so its
  // resurfaces are governed by the card gap alone (§5 pin b).
  const bool timedRequeue = mode != flashcards::SessionMode::CramAll && clockAvailable;

  for (uint8_t i = 0; i < pendingCount && !found; i++) {
    if (pending[i].showAfter != 0) continue;
    // A learning step is a TIMER, not a card count: a card whose 10-minute step
    // has not expired stays parked even once three cards have gone past. It
    // still comes back inside this session -- through the drain branch below,
    // which is where Anki's learn-ahead lives.
    if (timedRequeue && pending[i].dueUnix > now.unixSecs) continue;
    next = pending[i].ordinal;
    for (uint8_t j = i + 1; j < pendingCount; j++) pending[j - 1] = pending[j];
    pendingCount--;
    found = true;
  }
  if (!found && session && queueIndex < session->cards.size()) {
    next = session->cards[queueIndex++];
    found = true;
  }
  if (!found && pendingCount > 0) {
    // Nothing else left to show, so the wait ends here rather than the session
    // does: every entry was parked with a due inside the learn-ahead window, so
    // pulling one forward is the same thing nextLearnAheadOrdinal() does for
    // the cards on disk.
    //
    // The ORDER is §5 pin (g), verbatim: "the one that has WAITED THE MOST
    // CARDS, earliest-due among equals -- a card answered Again is never the
    // immediate next card while another parked card is available". `showAfter`
    // counts the cards an entry is STILL waiting for, so the LOWEST showAfter
    // has waited longest and due only breaks ties. Ordering on due alone would
    // hand the drain straight back to the card just answered Again -- its
    // 1-minute step is the earliest due there is -- which is the back-to-back
    // repeat this change exists to stop. With showAfter and dues equal (cram,
    // or a no-clock session, where every due is 0) this is insertion order,
    // the longest-waiting card, as before.
    uint8_t pick = 0;
    for (uint8_t j = 1; j < pendingCount; j++) {
      if (pending[j].showAfter != pending[pick].showAfter) {
        if (pending[j].showAfter < pending[pick].showAfter) pick = j;
      } else if (pending[j].dueUnix < pending[pick].dueUnix) {
        pick = j;
      }
    }
    next = pending[pick].ordinal;
    for (uint8_t j = static_cast<uint8_t>(pick + 1); j < pendingCount; j++) pending[j - 1] = pending[j];
    pendingCount--;
    found = true;
  }
  if (!found && mode == flashcards::SessionMode::Due && clockAvailable && refreshNow()) {
    // Drain-time learn-ahead (§5 pin c: DUE sessions only -- a New-only session
    // that pulled a learning card forward would stop being new-only). Consulted
    // with the resurface list empty, so the ordinal it returns cannot be one
    // this session is already holding -- the caller obligation
    // nextLearnAheadOrdinal() documents. One sequential scan, and only when the
    // session would otherwise be over.
    //
    // refreshNow() here is also where a session that straddles 04:00 picks the
    // new day up for SCHEDULING, while the session's counters stay on the day it
    // opened with (§5 pin f, documented behavior: StateStore rolled them once at
    // open and this screen never re-rolls them mid-session).
    flashcards::Ordinal ahead = 0;
    // Out parameter only: the due time is what nextLearnAheadOrdinal() selects
    // on, and this screen shows the card rather than its clock time.
    uint32_t aheadDueUnix = 0;
    if (flashcards::nextLearnAheadOrdinal(store, now, ahead, aheadDueUnix)) {
      next = ahead;
      found = true;
    }
  }

  if (!found) {
    finishSession();
    return;
  }

  decayPending();
  currentOrdinal = next;
  if (!loadCurrentCard()) {
    fail(StrId::STR_DECK_OPEN_FAILED);
    return;
  }
  {
    RenderLock lock(*this);
    screen = Screen::Review;
  }
  requestUpdate();
}

// --- Card text ---

bool FlashcardStudyActivity::loadCurrentCard() {
  if (!frontText || !backText || !lines) return false;

  // The two text buffers are about to be overwritten, and the line spans on
  // screen point straight into them: drop haveCard under the lock first so the
  // render task skips the body until the new card is wrapped (renderReview()
  // returns early on !haveCard). Taking the lock also waits out the frame that
  // may be mid-draw right now.
  {
    RenderLock lock(*this);
    haveCard = false;
  }

  // SD work next, with no lock held.
  frontLength = 0;
  backLength = 0;
  if (!deck.loadSide(currentOrdinal, flashcards::CardSide::Front, frontText.get(), CARD_TEXT_BYTES, frontLength)) {
    return false;
  }
  if (!deck.loadSide(currentOrdinal, flashcards::CardSide::Back, backText.get(), CARD_TEXT_BYTES, backLength)) {
    return false;
  }
  fsrs::CardState state{};
  if (store.readRecord(currentOrdinal, state) == flashcards::RecordStatus::IoError) return false;

  // No preview in cram: cram PERSISTS NO ANSWERS (§5 pin b -- StateStore's own
  // open / rollover / heal writes are exempt), so it applies none of these
  // intervals and showing them would promise something the mode does not do.
  // Without a clock there is no `now` to compute them from either.
  //
  // The intervals shown are computed against the `now` this card was LOADED at;
  // answerCurrent() re-reads the clock and grades against that one. The two can
  // differ by however long the card was on screen, which moves an intraday
  // preview by at most that much -- the label is a forecast, not a promise.
  const bool wantPreview = clockAvailable && mode != flashcards::SessionMode::CramAll;
  const fsrs::Preview nextPreview = wantPreview ? fsrs::previewIntervals(deck.params(), state, now) : fsrs::Preview{};

  {
    RenderLock lock(*this);
    currentState = state;
    preview = nextPreview;
    haveCard = true;
    revealed = false;
    currentPage = 0;
    wrapForDisplay();
  }
  return true;
}

const char* FlashcardStudyActivity::bufferFor(const LineKind kind) const {
  switch (kind) {
    case LineKind::Front:
      return frontText.get();
    case LineKind::Back:
      return backText.get();
    case LineKind::Rule:
      return nullptr;
  }
  return nullptr;
}

int FlashcardStudyActivity::measureSpan(const char* text, size_t length) const {
  char buffer[MEASURE_BYTES];
  length = std::min(length, MEASURE_BYTES - 1);
  memcpy(buffer, text, length);
  buffer[length] = '\0';
  return renderer.getTextAdvanceX(CARD_FONT_ID, buffer, EpdFontFamily::REGULAR);
}

/**
 * Greedy word-wrap of one card side into line spans, the same shape
 * DictionaryDefinitionActivity uses for definitions.
 *
 * It is here rather than GfxRenderer::wrappedText because that helper cannot
 * serve this screen: it treats '\n' as an ordinary character (CPDK uses it as
 * the card's own line break and, doubled, its paragraph break, §2), it
 * ELLIPSISES anything past `maxLines` and any word wider than the column, and
 * it builds a std::vector<std::string> per call. Card text may not be silently
 * truncated, so the spans below page instead, and they point into the two card
 * buffers rather than copying.
 */
void FlashcardStudyActivity::appendWrapped(const char* text, const uint16_t length, const LineKind kind,
                                           const int maxWidth, const uint16_t lineLimit, bool& overflowed) {
  const int spaceWidth = renderer.getSpaceWidth(CARD_FONT_ID, EpdFontFamily::REGULAR);

  uint16_t lineStart = 0;
  uint16_t lineEnd = 0;
  int lineWidth = 0;

  const auto flushLine = [&](const uint16_t nextStart) {
    if (lineCount >= lineLimit) {
      overflowed = true;
      return;
    }
    lines[lineCount++] = CardLine{lineStart, static_cast<uint16_t>(lineEnd - lineStart), kind};
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  // Space and newline are the only separators this has to know about: DeckFile
  // rewrites every C0 byte except '\n' to a space as it reads a side, so a tab
  // can never reach here (DeckFile.cpp, FLASHCARD_SPEC.md §2.1).
  uint16_t i = 0;
  while (i < length && !overflowed) {
    const char c = text[i];
    if (c == '\n') {
      flushLine(static_cast<uint16_t>(i + 1));
      i++;
      continue;
    }
    if (c == ' ') {
      i++;
      continue;
    }

    const uint16_t tokenStart = i;
    while (i < length && text[i] != ' ' && text[i] != '\n' && static_cast<size_t>(i - tokenStart) < MEASURE_BYTES - 1) {
      i++;
    }
    // Back off a cut that landed mid-UTF-8 so neither measure nor draw ever
    // sees a partial sequence.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint16_t tokenLength = static_cast<uint16_t>(i - tokenStart);
    const int tokenWidth = measureSpan(text + tokenStart, tokenLength);

    // The byte bound on the "fits" test below keeps every emitted span drawable
    // in one measure/draw buffer, so the render path never has to truncate a
    // line it was handed. A lone token is already bounded by the scan above.
    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = static_cast<uint16_t>(tokenStart + tokenLength);
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               static_cast<size_t>(tokenStart + tokenLength - lineStart) <= MEASURE_BYTES - 1) {
      lineEnd = static_cast<uint16_t>(tokenStart + tokenLength);
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = static_cast<uint16_t>(tokenStart + tokenLength);
      lineWidth = tokenWidth;
    }

    // A token wider than the column is alone on its line by now; split it at
    // the widest fitting codepoint boundary instead of ellipsising it away.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1 && !overflowed) {
      const uint16_t span = static_cast<uint16_t>(lineEnd - lineStart);
      uint16_t lastFit = 0;
      for (uint16_t f = 1; f <= span; f++) {
        if (f != span && (text[lineStart + f] & 0xC0) == 0x80) continue;
        if (measureSpan(text + lineStart, f) > maxWidth) break;
        lastFit = f;
      }
      if (lastFit == 0) {
        // One glyph too wide for the column still has to make progress; take
        // its whole UTF-8 sequence rather than emitting a broken fragment.
        lastFit = 1;
        while (lastFit < span && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint16_t rest = static_cast<uint16_t>(lineStart + lastFit);
      lineEnd = rest;
      flushLine(rest);
      lineEnd = static_cast<uint16_t>(rest + (span - lastFit));
      lineWidth = measureSpan(text + lineStart, static_cast<uint16_t>(lineEnd - lineStart));
    }
  }
  if (lineEnd > lineStart) flushLine(length);
}

void FlashcardStudyActivity::wrapForDisplay() {
  lineCount = 0;
  frontOverflowed = false;
  backOverflowed = false;
  currentPage = 0;
  totalPages = 1;
  if (!lines) return;

  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return;

  // The front is capped short of the buffer (§5 pin d) whether or not the answer
  // is showing, so that revealing a front made of 300 newlines still has room
  // for the rule and the first 32 lines of the back -- and so that the front
  // pages identically either side of the reveal.
  appendWrapped(frontText.get(), frontLength, LineKind::Front, body.width, MAX_FRONT_LINES, frontOverflowed);
  if (revealed) {
    // The front stays on the answer screen: recall is judged against the
    // question, and a rule marks where the answer starts. Both sides page as
    // one flow, so neither can be cut off.
    if (lineCount < MAX_CARD_LINES) lines[lineCount++] = CardLine{0, 0, LineKind::Rule};
    appendWrapped(backText.get(), backLength, LineKind::Back, body.width, MAX_CARD_LINES, backOverflowed);
  }

  const int lineHeight = renderer.getLineHeight(CARD_FONT_ID);
  linesPerPage = std::max(1, body.height / std::max(1, lineHeight));
  totalPages = std::max(1, (lineCount + linesPerPage - 1) / linesPerPage);
}

// --- Answering ---

void FlashcardStudyActivity::setRevealed(const bool shown) {
  {
    RenderLock lock(*this);
    revealed = shown;
    // The grade row appears with the answer, so the body shrinks: re-wrap
    // against the new column height rather than paging stale lines.
    wrapForDisplay();
  }
  requestUpdate();
}

void FlashcardStudyActivity::answerCurrent(const fsrs::Grade grade) {
  if (!haveCard) return;

  // Cram PERSISTS NO ANSWERS (§5 pin b): no gradeCard, no record write, no
  // counter -- the grade only decides whether the card comes back this session.
  // The store's own housekeeping writes (file open, day rollover, healing a
  // short or corrupt record) are exempt and still happen underneath.
  if (mode == flashcards::SessionMode::CramAll) {
    {
      RenderLock lock(*this);
      undo.valid = false;  // there is no answer on disk to undo
    }
    // No due to carry: cram's resurfaces are gap-only (see advanceToNextCard).
    if (grade == fsrs::Grade::Again) requeueCurrent(0);
    advanceToNextCard();
    return;
  }

  if (!refreshNow()) {
    // The clock disappeared mid-session; nothing may be persisted without an
    // honest day number, so end the session rather than write a wrong due.
    LOG_ERR("DECK", "Clock lost mid-session; ending");
    finishSession();
    return;
  }

  const fsrs::CardState before = currentState;
  const uint64_t key = deck.keyAt(currentOrdinal);
  const fsrs::CardState after = fsrs::gradeCard(deck.params(), before, grade, now, key);

  // Which allowance the answer spends is decided by the PRE-answer phase, which
  // is Anki's rule and the one StateStore's Counted expects: a New card being
  // seen for the first time spends a new-card slot, a card that was in Review
  // spends a review slot, and a learning/relearning re-step spends neither --
  // otherwise a card answered Again four times would eat four of the day's 200
  // reviews for one card.
  flashcards::Counted counted = flashcards::Counted::Nothing;
  switch (before.state) {
    case fsrs::CardPhase::New:
      counted = flashcards::Counted::NewCard;
      break;
    case fsrs::CardPhase::Review:
      counted = flashcards::Counted::Review;
      break;
    case fsrs::CardPhase::Learning:
    case fsrs::CardPhase::Relearning:
      counted = flashcards::Counted::Nothing;
      break;
  }

  UndoState snapshot;
  snapshot.valid = true;
  snapshot.ordinal = currentOrdinal;
  snapshot.before = before;
  snapshot.newToday = store.newToday();
  snapshot.reviewsToday = store.reviewsToday();
  snapshot.queueIndex = queueIndex;
  snapshot.pendingCount = pendingCount;
  snapshot.studiedNew = studiedNew;
  snapshot.studiedReview = studiedReview;
  snapshot.studiedLearning = studiedLearning;
  snapshot.droppedRequeues = droppedRequeues;
  memcpy(snapshot.pending, pending, sizeof(pending));

  if (!store.commitAnswer(currentOrdinal, after, counted)) {
    LOG_ERR("DECK", "Answer write failed for ordinal %u", static_cast<unsigned>(currentOrdinal));
    fail(StrId::STR_DECK_STATE_FAILED);
    return;
  }

  {
    RenderLock lock(*this);
    undo = snapshot;
    switch (counted) {
      case flashcards::Counted::NewCard:
        studiedNew++;
        break;
      case flashcards::Counted::Review:
        studiedReview++;
        break;
      case flashcards::Counted::Nothing:
        studiedLearning++;
        break;
    }
  }

  // A card that is still intraday and comes due inside the learn-ahead window
  // belongs to this session, not the next one.
  const bool intraday = after.state == fsrs::CardPhase::Learning || after.state == fsrs::CardPhase::Relearning;
  if (intraday && after.due <= now.unixSecs + flashcards::LEARN_AHEAD_SECS) requeueCurrent(after.due);

  advanceToNextCard();
}

void FlashcardStudyActivity::undoLastAnswer() {
  if (!undo.valid || mode == flashcards::SessionMode::CramAll) return;

  if (!store.writeRecord(undo.ordinal, undo.before) || !store.writeCounters(undo.newToday, undo.reviewsToday)) {
    LOG_ERR("DECK", "Undo write failed for ordinal %u", static_cast<unsigned>(undo.ordinal));
    fail(StrId::STR_DECK_STATE_FAILED);
    return;
  }

  {
    RenderLock lock(*this);
    queueIndex = undo.queueIndex;
    pendingCount = undo.pendingCount;
    memcpy(pending, undo.pending, sizeof(pending));
    studiedNew = undo.studiedNew;
    studiedReview = undo.studiedReview;
    studiedLearning = undo.studiedLearning;
    droppedRequeues = undo.droppedRequeues;
    currentOrdinal = undo.ordinal;
    undo.valid = false;  // one deep
    screen = Screen::Review;
  }
  // Re-fronted: the card is shown as if it had never been answered.
  if (!loadCurrentCard()) {
    fail(StrId::STR_DECK_OPEN_FAILED);
    return;
  }
  requestUpdate();
}

void FlashcardStudyActivity::suspendCurrent() {
  // Cram persists no answers and FLAG_SUSPENDED is exactly that kind of write,
  // so suspend is deliberately inert there (§5 pin b) -- and the menu does not
  // offer the row in the first place.
  if (!haveCard || mode == flashcards::SessionMode::CramAll) return;

  fsrs::CardState suspended = currentState;
  suspended.flags |= fsrs::FLAG_SUSPENDED;
  // Not counted: suspending is not an answer and spends no daily allowance.
  if (!store.writeRecord(currentOrdinal, suspended)) {
    LOG_ERR("DECK", "Suspend write failed for ordinal %u", static_cast<unsigned>(currentOrdinal));
    fail(StrId::STR_DECK_STATE_FAILED);
    return;
  }
  {
    RenderLock lock(*this);
    undo.valid = false;  // the undo record would restore a different answer
  }
  dropFromPending(currentOrdinal);
  // advanceToNextCard() compares every parked due against `now`, so refresh it
  // here the way answerCurrent() does rather than advancing on the clock of
  // whatever came before. Unlike an answer this cannot end the session on a
  // failure: nothing scheduling-bearing was written, and a stale `now` can only
  // hold a card parked slightly longer.
  (void)refreshNow();
  advanceToNextCard();
}

// --- Card menu ---

/**
 * The touch route to undo and suspend (§5 pin a).
 *
 * The X4 Pro has PIN_UNASSIGNED for both front buttons, so the Back/Confirm
 * holds below can never fire there and this menu is the only way in. It is
 * opened by a screen long press, which the SDK classifier fires WHILE the finger
 * is still down and which consumes the rest of that contact -- so the lift that
 * follows cannot also reveal the card, turn a page or hit a grade column. Long
 * press therefore wins over every tap on this screen by construction, not by
 * ordering luck.
 *
 * Rows are built per opening from what the card can actually do; with neither
 * available (an unrevealed first card, or any card in cram) the hold is inert
 * rather than opening an empty dialog.
 */
void FlashcardStudyActivity::openCardMenu() {
  StrId rows[MAX_MENU_ACTIONS];
  menuActionCount = 0;

  if (undo.valid && mode != flashcards::SessionMode::CramAll) {
    rows[menuActionCount] = StrId::STR_DECK_UNDO;
    menuActions[menuActionCount++] = CardAction::Undo;
  }
  if (haveCard && revealed && mode != flashcards::SessionMode::CramAll) {
    rows[menuActionCount] = StrId::STR_DECK_SUSPEND;
    menuActions[menuActionCount++] = CardAction::Suspend;
  }
  if (menuActionCount == 0) return;

  menuChoicePending = false;
  cardMenu.show(StrId::STR_DECK_CARD_MENU, rows, menuActionCount, 0, [this](const int index) {
    // Recorded, not run: see the note on `cardMenu` in the header.
    if (index < 0 || index >= menuActionCount) return;
    menuChoice = menuActions[index];
    menuChoicePending = true;
  });
  requestUpdate();
}

void FlashcardStudyActivity::applyMenuChoice() {
  if (!menuChoicePending) return;
  menuChoicePending = false;
  switch (menuChoice) {
    case CardAction::Undo:
      undoLastAnswer();
      return;
    case CardAction::Suspend:
      suspendCurrent();
      return;
  }
}

// --- Input ---

void FlashcardStudyActivity::loop() {
  switch (screen) {
    case Screen::Busy: {
      // First tick paints "Loading", then the blocking SD work runs with no
      // lock held (ClockSyncActivity's SYNCING shape).
      requestUpdateAndWait();
      const Work work = pendingWork;
      pendingWork = Work::None;
      switch (work) {
        case Work::OpenDeck:
          openSelectedDeck();
          break;
        case Work::BuildSession:
          if (!buildSelectedSession()) {
            closeDeck();
            fail(StrId::STR_DECK_STATE_FAILED);
            break;
          }
          startReview();
          break;
        case Work::None:
          goToPicker();
          break;
      }
      return;
    }
    case Screen::Picker:
      loopPicker();
      return;
    case Screen::Mode:
      loopMode();
      return;
    case Screen::Review:
      loopReview();
      return;
    case Screen::Done:
      loopDone();
      return;
    case Screen::Failed:
      loopDone();  // same dismissal: anything goes back to the picker
      return;
  }
}

void FlashcardStudyActivity::loopPicker() {
  const int count = static_cast<int>(deckLeaves.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
  if (count == 0) return;

  buttonNavigator.onNext([this, count] {
    deckSelection = ButtonNavigator::nextIndex(deckSelection, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    deckSelection = ButtonNavigator::previousIndex(deckSelection, count);
    requestUpdate();
  });

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = GUI.getMenuRowHeight(renderer);
  const int perPage = deckRowsPerPage();
  const int windowTop = (deckSelection / perPage) * perPage;
  // The hit grid starts at the rect the rows are drawn into, exactly as
  // HomeActivity's does: BaseTheme insets its first row by verticalSpacing but
  // RoundedRaff and Lyra start at rect.y, so adding that inset here would push
  // the bands off the rows on two of the three themes.
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int row = -1;
  const auto touch = mappedInput.rowTouch(row, top, rowHeight + metrics.menuSpacing,
                                          std::min(perPage, count - windowTop), 0, INT32_MAX, rowHeight);
  if (touch != MappedInputManager::RowTouch::None) {
    deckSelection = windowTop + row;
    if (touch == MappedInputManager::RowTouch::Down) {
      requestUpdate();
      return;
    }
    pendingWork = Work::OpenDeck;
    {
      RenderLock lock(*this);
      screen = Screen::Busy;
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    pendingWork = Work::OpenDeck;
    {
      RenderLock lock(*this);
      screen = Screen::Busy;
    }
    requestUpdate();
  }
}

void FlashcardStudyActivity::loopMode() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    goToPicker();
    return;
  }
  // Without a clock the mode list is Cram alone (§5), so there is nothing to
  // navigate and selection is fixed.
  const int count = clockAvailable ? 3 : 1;

  buttonNavigator.onNext([this, count] {
    modeSelection = ButtonNavigator::nextIndex(modeSelection, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    modeSelection = ButtonNavigator::previousIndex(modeSelection, count);
    requestUpdate();
  });

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = GUI.getMenuRowHeight(renderer);
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3 +
                  renderer.getLineHeight(UI_10_FONT_ID) * 2;
  int row = -1;
  // Same rect the mode rows are drawn into (see the note in loopPicker()).
  const auto touch = mappedInput.rowTouch(row, top, rowHeight + metrics.menuSpacing, count, 0, INT32_MAX, rowHeight);
  const bool activated =
      touch == MappedInputManager::RowTouch::Tap || mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (touch == MappedInputManager::RowTouch::Down) {
    modeSelection = row;
    requestUpdate();
    return;
  }
  if (!activated) return;
  if (touch == MappedInputManager::RowTouch::Tap) modeSelection = row;

  if (!clockAvailable) {
    mode = flashcards::SessionMode::CramAll;
  } else {
    switch (modeSelection) {
      case 1:
        mode = flashcards::SessionMode::NewOnly;
        break;
      case 2:
        mode = flashcards::SessionMode::CramAll;
        break;
      default:
        mode = flashcards::SessionMode::Due;
        break;
    }
  }
  pendingWork = Work::BuildSession;
  {
    RenderLock lock(*this);
    screen = Screen::Busy;
  }
  requestUpdate();
}

void FlashcardStudyActivity::loopReview() {
  // The menu owns the frame while it is up: handleInput() answers true for every
  // input it sees, so nothing below can reveal, page or grade underneath it. The
  // choice it recorded runs here, after its callback has returned.
  if (cardMenu.handleInput(mappedInput, [this] { requestUpdate(); })) {
    applyMenuChoice();
    return;
  }

  // Long presses are read before the short actions on the same buttons:
  // wasLongPressed() consumes the release that would otherwise also fire, and
  // ActivityManager swallows that frame globally. The button holds are the
  // shortcut; the screen hold below is the route that exists on every board.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, HOLD_MS)) {
    undoLastAnswer();
    return;
  }
  if (revealed && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, HOLD_MS)) {
    suspendCurrent();
    return;
  }
  // Before every touch read below: consuming the long press suppresses the rest
  // of the contact, so the finger that opened the menu cannot also tap through
  // to the card under it.
  int holdX = 0;
  int holdY = 0;
  if (mappedInput.wasScreenLongPress(holdX, holdY)) {
    openCardMenu();
    return;
  }

  if (revealed) {
    // The left-edge back SWIPE goes back to the question rather than grading:
    // wasReleased(Back) answers true for that gesture too, and a touch user who
    // revealed by accident needs a way out that is not an answer.
    if (mappedInput.wasBackGesture()) {
      setRevealed(false);
      return;
    }
    // Every front button is a grade on the answer side, labelled with the grade
    // it applies (see render()). Leaving is by Back on the question side, by the
    // back swipe above, by the Home gesture, or by answering.
    const MappedInputManager::Button gradeButtons[fsrs::GRADE_COUNT] = {
        MappedInputManager::Button::Back, MappedInputManager::Button::Confirm, MappedInputManager::Button::NavPrevious,
        MappedInputManager::Button::NavNext};
    for (uint8_t g = 0; g < fsrs::GRADE_COUNT; g++) {
      if (mappedInput.wasReleased(gradeButtons[g])) {
        answerCurrent(static_cast<fsrs::Grade>(g));
        return;
      }
    }

    // The grade columns run from the boxes to the bottom of the panel instead
    // of stopping one menuRowHeight down. That is about the TARGET, not about
    // labels: every theme's drawButtonHints() early-returns on gpio.hasTouch(),
    // so on a touch board (the X4 Pro) the hint band under the boxes is left
    // BLANK -- there is nothing down there to compete with, and running the
    // four columns over it buys a taller target on the one screen where a
    // mis-tap costs an answer. A button-only board does get the four grade
    // names printed there, but colTouch() never fires without touch, so this
    // read is simply dead on those boards rather than a second control.
    const int rowTop = gradeRowTop();
    const int columnWidth = renderer.getScreenWidth() / fsrs::GRADE_COUNT;
    int column = -1;
    if (mappedInput.colTouch(column, 0, columnWidth, fsrs::GRADE_COUNT, rowTop, renderer.getScreenHeight()) ==
        MappedInputManager::RowTouch::Tap) {
      answerCurrent(static_cast<fsrs::Grade>(column));
      return;
    }

    // Paged answers turn in the card body's outer quarters, the same gesture
    // the question side uses. Read AFTER the grade columns and bounded to the
    // body, so the band's own outer columns (Again / Easy) stay grades and a
    // tap that means "answer" can never mean "page": on a card whose front,
    // rule and back span more than one page the back is otherwise unreachable
    // -- there is no other page gesture here, and the front buttons are all
    // four spoken for by the grades.
    int tapX = 0;
    int tapY = 0;
    if (totalPages > 1 && mappedInput.wasScreenTapped(tapX, tapY)) {
      const BodyArea body = bodyArea();
      if (tapY >= body.y && tapY < body.y + body.height) {
        if (tapX < renderer.getScreenWidth() / 4) {
          turnPage(-1);
        } else if (tapX > renderer.getScreenWidth() * 3 / 4) {
          turnPage(1);
        }
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishSession();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setRevealed(true);
    return;
  }
  buttonNavigator.onNext([this] { turnPage(1); });
  buttonNavigator.onPrevious([this] { turnPage(-1); });

  int tapX = 0;
  int tapY = 0;
  if (!mappedInput.wasScreenTapped(tapX, tapY)) return;
  // On a paged question the outer quarters turn the page; everything else
  // reveals, which is the one gesture a card front needs.
  if (totalPages > 1 && tapX < renderer.getScreenWidth() / 4) {
    turnPage(-1);
    return;
  }
  if (totalPages > 1 && tapX > renderer.getScreenWidth() * 3 / 4) {
    turnPage(1);
    return;
  }
  setRevealed(true);
}

void FlashcardStudyActivity::turnPage(const int delta) {
  const int target = currentPage + delta;
  if (target < 0 || target >= totalPages) return;
  {
    RenderLock lock(*this);
    currentPage = target;
  }
  requestUpdate();
}

void FlashcardStudyActivity::loopDone() {
  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tapX, tapY)) {
    goToPicker();
  }
}

// --- Layout ---

FlashcardStudyActivity::BodyArea FlashcardStudyActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // One line is always reserved between the header and the card for the page /
  // overflow indicator, drawn right-aligned in that strip. Always, not only when
  // there is something to show: the strip is what keeps the indicator out of the
  // header band (where a centred theme title sits) without making the wrap
  // geometry depend on the wrap's own result.
  const int top =
      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + renderer.getLineHeight(UI_10_FONT_ID);
  const int gradeBand = revealed ? metrics.menuRowHeight + metrics.verticalSpacing : 0;
  const int bottom = metrics.buttonHintsHeight + metrics.verticalSpacing + gradeBand;
  return BodyArea{metrics.contentSidePadding, top, renderer.getScreenWidth() - 2 * metrics.contentSidePadding,
                  renderer.getScreenHeight() - top - bottom};
}

int FlashcardStudyActivity::gradeRowTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - metrics.menuRowHeight;
}

void FlashcardStudyActivity::formatInterval(const uint8_t grade, char* out, const size_t outBytes) const {
  out[0] = '\0';
  if (!clockAvailable || mode == flashcards::SessionMode::CramAll || grade >= fsrs::GRADE_COUNT) return;

  const uint32_t value = preview.next[grade];
  if (preview.intraday[grade]) {
    if (value < SECONDS_PER_MINUTE) {
      snprintf(out, outBytes, "%s", tr(STR_DECK_INTERVAL_LT_MIN));
      return;
    }
    if (value < SECONDS_PER_HOUR) {
      snprintf(out, outBytes, tr(STR_DECK_INTERVAL_MIN_FORMAT), static_cast<int>(value / SECONDS_PER_MINUTE));
      return;
    }
    snprintf(out, outBytes, tr(STR_DECK_INTERVAL_HOUR_FORMAT), static_cast<int>(value / SECONDS_PER_HOUR));
    return;
  }
  if (value < DAYS_PER_MONTH) {
    snprintf(out, outBytes, tr(STR_DECK_INTERVAL_DAY_FORMAT), static_cast<int>(value));
    return;
  }
  if (value < DAYS_PER_YEAR) {
    snprintf(out, outBytes, tr(STR_DECK_INTERVAL_MONTH_FORMAT), static_cast<int>(value / DAYS_PER_MONTH));
    return;
  }
  snprintf(out, outBytes, tr(STR_DECK_INTERVAL_YEAR_FORMAT), static_cast<int>(value / DAYS_PER_YEAR));
}

// --- Rendering ---

void FlashcardStudyActivity::renderPicker() const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (deckLeaves.empty()) {
    drawTwoLineMessage(tr(STR_DECK_NONE_FOUND), tr(STR_DECK_SYNC_HINT));
    return;
  }

  const int perPage = deckRowsPerPage();
  const int count = static_cast<int>(deckLeaves.size());
  const int windowTop = (deckSelection / perPage) * perPage;
  drawRows(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, std::min(perPage, count - windowTop),
           deckSelection - windowTop, windowTop);
}

/**
 * The one themed-list call site both list screens go through.
 *
 * `windowOffset >= 0` draws deck leaves from that offset; -1 draws the mode
 * rows. Sharing it is not only tidiness: every distinct lambda handed to
 * drawButtonMenu instantiates its own std::function invoker, and this screen's
 * IROM budget is measured in bytes (platformio.ini, flashcards block). Rows
 * carry no icon, so the icon callback is a null std::function rather than a
 * second invoker (the themes that draw icons null-check it).
 *
 * The deck list is handed to the theme one PAGE at a time, so RoundedRaff's
 * scrollbar -- which windows the rows it is given a second time -- describes the
 * page rather than the whole list. Accepted: the alternative is either paging
 * this screen the way that one theme does it or teaching every theme the
 * caller's window, and the picker's page counter is the deck count itself.
 */
void FlashcardStudyActivity::drawRows(const int top, const int count, const int selected,
                                      const int windowOffset) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const StrId modeRows[3] = {StrId::STR_DECK_MODE_DUE, StrId::STR_DECK_MODE_NEW, StrId::STR_DECK_MODE_CRAM};
  // Without a clock only Cram is offered (FLASHCARD_SPEC.md §5), so the single
  // row on that screen is Cram, not Due.
  const int modeBase = clockAvailable ? 0 : 2;
  GUI.drawButtonMenu(
      renderer, Rect{0, top, renderer.getScreenWidth(), renderer.getScreenHeight() - top - metrics.buttonHintsHeight},
      count, selected,
      [this, windowOffset, &modeRows, modeBase](const int index) {
        if (windowOffset < 0) return std::string(I18N.get(modeRows[modeBase + index]));
        return deckLabel(deckLeaves[windowOffset + index]);
      },
      nullptr);
}

void FlashcardStudyActivity::renderMode() const {
  if (!session) return;  // only reachable with a built session; guard, not a path
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // `dueAvailable` / `newAvailable` are deck-wide, but SessionSummary has no
  // deck-wide learning count -- `learning` is what the Due build actually took.
  // That is the honest number this layer can report without a second scan.
  char counts[64];
  const auto& summary = session->summary;
  snprintf(counts, sizeof(counts), tr(STR_DECK_MODE_COUNTS_FORMAT), static_cast<int>(summary.dueAvailable),
           static_cast<int>(summary.newAvailable), static_cast<int>(summary.learning));
  renderer.drawCenteredText(UI_10_FONT_ID, y, counts);
  y += lineHeight;

  if (!clockAvailable) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_DECK_NO_CLOCK));
  } else if (session->cards.empty()) {
    // A Due build that came out empty is "nothing to do", not a failure; the
    // other two modes stay on offer below.
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_DECK_NOTHING_DUE));
    if (summary.nextDueUnix != 0) {
      char when[24];
      char line[64];
      formatLocalTime(summary.nextDueUnix, when, sizeof(when));
      snprintf(line, sizeof(line), tr(STR_DECK_NEXT_DUE_FORMAT), when);
      renderer.drawCenteredText(UI_10_FONT_ID, y + lineHeight, line);
    }
  }

  drawRows(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3 + lineHeight * 2,
           clockAvailable ? 3 : 1, modeSelection, -1);
}

void FlashcardStudyActivity::renderReview() const {
  // No card resident means the spans below point into buffers that loadCurrentCard()
  // is rewriting right now: draw the frame without a body rather than a torn one.
  if (!haveCard) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const BodyArea body = bodyArea();
  const int lineHeight = renderer.getLineHeight(CARD_FONT_ID);

  // A card that fits one page is centred in the body, which is the look §5
  // asks for; a longer one starts at the top and pages.
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lineCount));
  const int drawn = std::max(0, lastLine - firstLine);
  int y = totalPages == 1 ? body.y + std::max(0, (body.height - drawn * lineHeight) / 2) : body.y;

  char text[MEASURE_BYTES];
  for (int i = firstLine; i < lastLine; i++) {
    const CardLine& line = lines[i];
    if (line.kind == LineKind::Rule) {
      renderer.drawLine(body.x, y + lineHeight / 2, body.x + body.width, y + lineHeight / 2);
      y += lineHeight;
      continue;
    }
    if (line.length != 0) {
      const char* buffer = bufferFor(line.kind);
      // appendWrapped() bounds every span at MEASURE_BYTES - 1; the min() is the
      // belt to that invariant's braces, not a truncation this can reach.
      const size_t length = std::min(static_cast<size_t>(line.length), MEASURE_BYTES - 1);
      memcpy(text, buffer + line.start, length);
      text[length] = '\0';
      renderer.drawCenteredText(CARD_FONT_ID, y, text);
    }
    y += lineHeight;
  }

  // Page position, and the marker for a card so long that EITHER side hit its
  // line cap -- never a silent truncation. It sits in the reserved strip below
  // the header band, not inside it: the themed header centres its title and this
  // would land on top of it.
  const bool overflowed = frontOverflowed || backOverflowed;
  if (totalPages > 1 || overflowed) {
    char counter[24];
    snprintf(counter, sizeof(counter), "%d/%d%s", currentPage + 1, totalPages, overflowed ? "+" : "");
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, renderer.getScreenWidth() - metrics.contentSidePadding - counterWidth,
                      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, counter);
  }

  if (!revealed) return;

  // Four grade boxes, left to right in Anki order, each carrying the interval
  // that grade would buy. They are drawn in the content area (not over the hint
  // band) so the touch columns and the drawn boxes are the same rectangle.
  const int rowTop = gradeRowTop();
  const int columnWidth = renderer.getScreenWidth() / fsrs::GRADE_COUNT;
  const int labelHeight = renderer.getLineHeight(UI_10_FONT_ID);
  char interval[16];
  for (uint8_t g = 0; g < fsrs::GRADE_COUNT; g++) {
    const int x = g * columnWidth;
    renderer.drawRect(x + 2, rowTop, columnWidth - 4, metrics.menuRowHeight);
    const char* label = I18N.get(GRADE_LABELS[g]);
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, x + (columnWidth - labelWidth) / 2, rowTop + 4, label);
    formatInterval(g, interval, sizeof(interval));
    if (interval[0] == '\0') continue;
    const int intervalWidth = renderer.getTextWidth(SMALL_FONT_ID, interval);
    renderer.drawText(SMALL_FONT_ID, x + (columnWidth - intervalWidth) / 2, rowTop + 4 + labelHeight, interval);
  }
}

void FlashcardStudyActivity::renderDone() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = renderer.getScreenHeight() / 2 - lineHeight * 2;

  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_DECK_SESSION_DONE), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;

  constexpr int COUNTER_COUNT = 3;
  const StrId formats[COUNTER_COUNT] = {StrId::STR_DECK_STUDIED_NEW_FORMAT, StrId::STR_DECK_STUDIED_REVIEW_FORMAT,
                                        StrId::STR_DECK_STUDIED_LEARNING_FORMAT};
  const int counts[COUNTER_COUNT] = {studiedNew, studiedReview, studiedLearning};
  char line[48];
  for (int i = 0; i < COUNTER_COUNT; i++) {
    if (counts[i] == 0) continue;
    snprintf(line, sizeof(line), I18N.get(formats[i]), counts[i]);
    renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    y += lineHeight;
  }

  // Cards an Again could not put back because the resurface list was full (§5
  // pin e). They keep whatever the answer wrote; they just did not come round
  // again in this session.
  if (droppedRequeues != 0) {
    snprintf(line, sizeof(line), tr(STR_DECK_DROPPED_FORMAT), static_cast<int>(droppedRequeues));
    renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    y += lineHeight;
  }

  if (nextDueUnix == 0) return;
  char when[24];
  formatLocalTime(nextDueUnix, when, sizeof(when));
  snprintf(line, sizeof(line), tr(STR_DECK_NEXT_DUE_FORMAT), when);
  renderer.drawCenteredText(UI_10_FONT_ID, y + metrics.verticalSpacing, line);
}

/** The centred headline + detail pair the empty and error screens both draw. */
void FlashcardStudyActivity::drawTwoLineMessage(const char* headline, const char* detail) const {
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int midY = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, midY - lineHeight, headline, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, midY + lineHeight, detail);
}

void FlashcardStudyActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  // The header names what the user is looking at: the feature on the picker,
  // the deck's own CPDK title once one is open. The picker deliberately does not
  // consult `deck` at all -- goToPicker() switches to this screen under the lock
  // and closes the deck immediately after, so a Picker frame must not be reading
  // it (see goToPicker()).
  const bool showDeckTitle = screen != Screen::Picker && deck.isOpen() && deck.title()[0] != '\0';
  const char* title = showDeckTitle ? deck.title() : tr(STR_DECK_STUDY);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  MappedInputManager::Labels labels;
  switch (screen) {
    case Screen::Picker:
      renderPicker();
      labels =
          mappedInput.mapLabels(tr(STR_BACK), deckLeaves.empty() ? "" : tr(STR_SELECT),
                                deckLeaves.empty() ? "" : tr(STR_DIR_UP), deckLeaves.empty() ? "" : tr(STR_DIR_DOWN));
      break;
    case Screen::Busy:
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_LOADING));
      labels = mappedInput.mapLabels("", "", "", "");
      break;
    case Screen::Mode:
      renderMode();
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      break;
    case Screen::Review:
      renderReview();
      if (revealed) {
        // The four front buttons ARE the four grades. mapLabels puts each label
        // under the hardware button that carries that logical role, so a
        // remapped or rotated device still labels every button with the grade it
        // will actually apply; the boxes above stay in canonical Anki order for
        // touch.
        labels = mappedInput.mapLabels(tr(STR_DECK_GRADE_AGAIN), tr(STR_DECK_GRADE_HARD), tr(STR_DECK_GRADE_GOOD),
                                       tr(STR_DECK_GRADE_EASY));
      } else {
        labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DECK_SHOW_ANSWER), currentPage > 0 ? "<" : "",
                                       currentPage + 1 < totalPages ? ">" : "");
      }
      break;
    case Screen::Done:
      renderDone();
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
    case Screen::Failed:
      drawTwoLineMessage(tr(STR_ERROR_MSG), I18N.get(failure));
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }

  // The card menu draws over the frame just built (and paints its own hints and
  // buffer flip), so it goes last and short-circuits the rest.
  if (cardMenu.processRender(renderer, mappedInput)) return;

  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_FLASHCARDS
