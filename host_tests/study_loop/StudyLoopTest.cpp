// Host probe + regression suite for the FlashcardStudyActivity REVIEW LOOP:
// the reveal/page/grade tap machine and the in-session requeue ("pending")
// queue. Built by build.sh with plain g++ against the real lib/FsrsSched.
// Never compiled into the firmware.
//
// WHY A MIRROR AND NOT THE ACTIVITY ITSELF. src/activities/flashcards/
// FlashcardStudyActivity.cpp reaches UITheme, GfxRenderer, GUI, OptionPopup,
// StateStore, DeckFile, RenderLock and the ActivityManager singleton; none of
// those has a host stand-in and standing them all up would test the stubs. The
// review loop's DECISION logic, though, is a closed system over a handful of
// ints, so ReviewLoop below is a VERBATIM transcription of it -- the same
// branches in the same order, the same constants, the same field names -- with
// only the render calls and the SD/store calls replaced. Every function here
// carries the source line range it mirrors; a change to one must be mirrored in
// the other, and the "mirror check" group at the bottom pins the constants that
// are shared with the firmware headers so a drift in those at least fails here.
//
// The input layer is modelled from src/MappedInputManager.cpp:
//   * wasScreenTapped()     -> the RELEASE edge of a contact (InputManager
//                              latches touchReleasedEvent for one frame);
//   * wasScreenTouchDown()  -> true WHILE a stationary contact is down and has
//                              been held >= TOUCH_DOWN_SELECT_DELAY_MS (90 ms);
//   * colTouch()            -> Down first, then Tap, each gated on the same
//                              rectangle hit test.
// A finger therefore produces >= 1 "down" frames followed by exactly one
// "release" frame, which is what tapContact() below emits.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "FsrsSched.h"

using fsrs::CardPhase;
using fsrs::CardState;
using fsrs::Grade;
using fsrs::Now;

// --- tiny harness ----------------------------------------------------------

static int g_fail = 0;
static const char* g_grp = "";
#define GROUP(n) (g_grp = (n))
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      printf("  FAIL [%s:%d] %s\n", g_grp, __LINE__, #c); \
      g_fail++;                                           \
    }                                                     \
  } while (0)
#define CHECKEQ(a, b)                                                                          \
  do {                                                                                         \
    long long A = (long long)(a), B = (long long)(b);                                          \
    if (A != B) {                                                                              \
      printf("  FAIL [%s:%d] %s == %s (got %lld want %lld)\n", g_grp, __LINE__, #a, #b, A, B); \
      g_fail++;                                                                                \
    }                                                                                          \
  } while (0)
#define CHECKSTR(a, b)                                                                                            \
  do {                                                                                                            \
    std::string A = (a), B = (b);                                                                                 \
    if (A != B) {                                                                                                 \
      printf("  FAIL [%s:%d] got \"%s\"\n                 want \"%s\"\n", g_grp, __LINE__, A.c_str(), B.c_str()); \
      g_fail++;                                                                                                   \
    }                                                                                                             \
  } while (0)

// --- the layout the X4 Pro actually renders with ---------------------------
//
// BaseTheme's default metrics (src/components/themes/BaseTheme.h) on the X4
// Pro's 480x800 logical portrait frame. Only the numbers bodyArea() and
// gradeRowTop() consume are here.
namespace layout {
constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 800;
constexpr int TOP_PADDING = 5;
constexpr int HEADER_HEIGHT = 45;
constexpr int VERTICAL_SPACING = 10;
constexpr int CONTENT_SIDE_PADDING = 20;
constexpr int MENU_ROW_HEIGHT = 45;
constexpr int BUTTON_HINTS_HEIGHT = 40;
constexpr int UI10_LINE_HEIGHT = 18;  // renderer.getLineHeight(UI_10_FONT_ID)
constexpr int CARD_LINE_HEIGHT = 22;  // renderer.getLineHeight(NOTOSERIF_16_FONT_ID)
}  // namespace layout

// --- the deck / store the loop talks to ------------------------------------

typedef uint16_t Ordinal;

/**
 * One CPDK v2 card image, in the only terms this mirror deals in.
 *
 * The firmware places an image at a BYTE offset into a card side and emits it
 * when the wrap crosses that offset; here a side already IS a line count, so a
 * placement is "after this many wrapped lines of that side" -- the same
 * insertion point, expressed one layer up. `heightPx` is the converter's
 * pre-sized height, which is what decides how many line slots the block takes.
 * No pixels are drawn on the host; only the layout arithmetic is mirrored.
 */
struct FakeImage {
  uint16_t afterLines = 0;
  uint16_t heightPx = 0;
};

struct FakeCard {
  uint16_t frontLines = 1;  // wrapped line count of the front side
  uint16_t backLines = 1;   // wrapped line count of the back side
  std::vector<FakeImage> frontImages;
  std::vector<FakeImage> backImages;
};

/** Stands in for StateStore: the record map plus the two daily counters. */
struct FakeStore {
  std::map<Ordinal, CardState> records;
  uint16_t newToday = 0;
  uint16_t reviewsToday = 0;
  int commits = 0;  // every commitAnswer(), i.e. every persisted answer

  CardState read(const Ordinal ordinal) const {
    const auto it = records.find(ordinal);
    return it == records.end() ? CardState{} : it->second;
  }
  void commit(const Ordinal ordinal, const CardState& after, const int counted) {
    records[ordinal] = after;
    if (counted == 1) newToday++;
    if (counted == 2) reviewsToday++;
    commits++;
  }
};

// --- one input frame -------------------------------------------------------

struct Frame {
  bool touchDown = false;     // stationary contact held >= 90 ms this frame
  bool touchRelease = false;  // the tap's release edge
  int x = 0;
  int y = 0;
};

// ===========================================================================
//  ReviewLoop -- verbatim mirror of the review half of FlashcardStudyActivity
// ===========================================================================

class ReviewLoop {
 public:
  // FlashcardStudyActivity.h:83-95
  static constexpr uint8_t MAX_PENDING = 64;
  static constexpr uint8_t REQUEUE_GAP = 3;
  static constexpr uint16_t MAX_CARD_LINES = 256;
  static constexpr uint16_t RESERVED_BACK_LINES = 33;
  static constexpr uint16_t MAX_FRONT_LINES = MAX_CARD_LINES - RESERVED_BACK_LINES;
  // SessionQueue.h:64
  static constexpr uint32_t LEARN_AHEAD_SECS = 1200;

  // FlashcardStudyActivity.h:124-128 -- field order kept identical to the
  // firmware's so the two structs cannot drift into different padding.
  struct PendingCard {
#ifdef STUDY_LOOP_FIXED
    uint32_t dueUnix;  // FIX: when the card's own step timer expires
#endif
    Ordinal ordinal;
    uint8_t showAfter;
  };

  // FlashcardStudyActivity.h:138-151
  struct UndoState {
    bool valid = false;
    Ordinal ordinal = 0;
    CardState before{};
    uint16_t newToday = 0;
    uint16_t reviewsToday = 0;
    uint32_t queueIndex = 0;
    uint8_t pendingCount = 0;
    uint16_t studiedNew = 0;
    uint16_t studiedReview = 0;
    uint16_t studiedLearning = 0;
    uint16_t droppedRequeues = 0;
    PendingCard pending[MAX_PENDING] = {};
  };

  enum class Mode : uint8_t { Due, NewOnly, CramAll };

  // --- wiring -------------------------------------------------------------
  std::map<Ordinal, FakeCard> deck;
  FakeStore store;
  std::vector<Ordinal> queue;  // session->cards
  Mode mode = Mode::Due;
  bool clockAvailable = true;
  Now now{};
  fsrs::Params params = fsrs::defaultParams();

  // --- state (FlashcardStudyActivity.h:252-274) ---------------------------
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  uint16_t lineCount = 0;
  uint16_t frontLineCount = 0;  // stands in for the Front-kind span count
  int ruleLine = -1;            // index of the LineKind::Rule slot, -1 when there is none
  bool frontOverflowed = false;
  bool backOverflowed = false;
  // Observation only: where each image block starts, and how many line slots it
  // took. The firmware keeps the same two numbers in the CardLine it emits.
  std::vector<int> imageStarts;
  std::vector<int> imageSpans;
  uint32_t queueIndex = 0;
  PendingCard pending[MAX_PENDING] = {};
  uint8_t pendingCount = 0;
  Ordinal currentOrdinal = 0;
  CardState currentState{};
  bool haveCard = false;
  bool revealed = false;
  uint16_t studiedNew = 0;
  uint16_t studiedReview = 0;
  uint16_t studiedLearning = 0;
  uint16_t droppedRequeues = 0;
  UndoState undo;
  bool finished = false;

  // --- observation --------------------------------------------------------
  std::vector<Ordinal> shown;    // one entry per card put on screen
  std::vector<std::string> log;  // human-readable trace of what the user sees

  void note() {
    char buf[96];
    snprintf(buf, sizeof(buf), "card %u %s page %d/%d", static_cast<unsigned>(currentOrdinal),
             revealed ? "answer" : "question", currentPage + 1, totalPages);
    log.push_back(buf);
  }

  /** What the user can read on the panel right now. */
  std::string visible() const {
    if (!haveCard) return "(blank)";
    const int first = currentPage * linesPerPage;
    const int last = first + linesPerPage < static_cast<int>(lineCount) ? first + linesPerPage : lineCount;
    bool front = false, rule = false, back = false;
    for (int i = first; i < last; i++) {
      if (i < frontLineCount)
        front = true;
      else if (revealed && i == frontLineCount)
        rule = true;
      else if (revealed)
        back = true;
    }
    std::string out;
    if (front) out += "front";
    if (rule) out += out.empty() ? "rule" : "+rule";
    if (back) out += out.empty() ? "back" : "+back";
    if (out.empty()) out = "empty";
    if (revealed) out += "+grades";
    return out;
  }

  // --- layout (FlashcardStudyActivity.cpp, bodyArea) -----------------------
  //
  // Two forms, as in the firmware. bodyHeight() is what is DRAWN right now;
  // bodyHeight(true) is the PAGING geometry, and the wrap uses that one in both
  // states so a front occupies the same pages either side of the reveal. The
  // question screen simply leaves the grade band's rows unused.
  int bodyHeight(const bool withGradeBand) const {
    const int top = layout::TOP_PADDING + layout::HEADER_HEIGHT + layout::VERTICAL_SPACING + layout::UI10_LINE_HEIGHT;
    const int gradeBand = withGradeBand ? layout::MENU_ROW_HEIGHT + layout::VERTICAL_SPACING : 0;
    const int bottom = layout::BUTTON_HINTS_HEIGHT + layout::VERTICAL_SPACING + gradeBand;
    return layout::SCREEN_H - top - bottom;
  }
  int bodyHeight() const { return bodyHeight(revealed); }
  int bodyTop() const {
    return layout::TOP_PADDING + layout::HEADER_HEIGHT + layout::VERTICAL_SPACING + layout::UI10_LINE_HEIGHT;
  }
  static int gradeRowTop() {
    return layout::SCREEN_H - layout::BUTTON_HINTS_HEIGHT - layout::VERTICAL_SPACING - layout::MENU_ROW_HEIGHT;
  }

  // --- appendLine (FlashcardStudyActivity.cpp, appendLine) -----------------
  bool appendLine(const uint16_t lineLimit, bool& overflowed) {
    if (lineCount >= lineLimit) {
      overflowed = true;
      return false;
    }
    lineCount++;
    return true;
  }

  // --- appendImage (FlashcardStudyActivity.cpp, appendImage) ---------------
  //
  // ceil(height / lineHeight) line slots, capped at one page, and PADDED
  // FORWARD so that a block never straddles a page break. The padding is what
  // lets pages stay a plain [page * linesPerPage, +linesPerPage) slice of one
  // flat line array, which is what every tap target and the page counter are
  // built on. Padding and fillers come out of the same budget the text does.
  void appendImage(const uint16_t heightPx, const uint16_t lineLimit, bool& overflowed) {
    const int perPage = linesPerPage < 1 ? 1 : linesPerPage;
    int span = (heightPx + layout::CARD_LINE_HEIGHT - 1) / layout::CARD_LINE_HEIGHT;
    if (span < 1) span = 1;
    if (span > perPage) span = perPage;

    const int used = lineCount % perPage;
    if (used != 0 && used + span > perPage) {
      for (int pad = used; pad < perPage; pad++) {
        if (!appendLine(lineLimit, overflowed)) return;
      }
    }
    imageStarts.push_back(lineCount);
    imageSpans.push_back(span);
    if (!appendLine(lineLimit, overflowed)) return;  // the block's own line
    for (int filler = 1; filler < span; filler++) {
      if (!appendLine(lineLimit, overflowed)) return;
    }
  }

  // --- appendWrapped (FlashcardStudyActivity.cpp, appendWrapped) -----------
  void appendSide(const uint16_t sideLines, const std::vector<FakeImage>& images, const uint16_t lineLimit,
                  bool& overflowed) {
    size_t emitted = 0;
    const auto emitImagesUpTo = [&](const uint16_t reached) {
      while (emitted < images.size() && images[emitted].afterLines <= reached && !overflowed) {
        appendImage(images[emitted].heightPx, lineLimit, overflowed);
        emitted++;
      }
    };
    emitImagesUpTo(0);
    for (uint16_t i = 0; i < sideLines && !overflowed; i++) {
      if (!appendLine(lineLimit, overflowed)) break;
      emitImagesUpTo(static_cast<uint16_t>(i + 1));
    }
    emitImagesUpTo(sideLines);
  }

  // --- wrapForDisplay (FlashcardStudyActivity.cpp:634-661) -----------------
  void wrapForDisplay() {
    lineCount = 0;
    frontLineCount = 0;
    ruleLine = -1;
    currentPage = 0;
    totalPages = 1;
    frontOverflowed = false;
    backOverflowed = false;
    imageStarts.clear();
    imageSpans.clear();

    // linesPerPage is settled BEFORE the wrap: an image block is measured in
    // whole pages, so the wrap has to know the page height it is laying out
    // against. And it is settled from the REVEALED body in both states, so the
    // front's pagination survives the reveal.
    const int lineHeight = layout::CARD_LINE_HEIGHT;
    linesPerPage = bodyHeight(true) / lineHeight;
    if (linesPerPage < 1) linesPerPage = 1;

    const FakeCard& card = deck[currentOrdinal];
    appendSide(card.frontLines, card.frontImages, MAX_FRONT_LINES, frontOverflowed);
    frontLineCount = lineCount;
    ruleLine = -1;
    if (revealed) {
      // The rule only exists if there was a line slot left for it, exactly as in
      // the firmware -- which is why setRevealed() looks for it rather than
      // assuming it sits at frontLineCount.
      if (lineCount < MAX_CARD_LINES) {
        ruleLine = lineCount;
        lineCount++;
      }
      appendSide(card.backLines, card.backImages, MAX_CARD_LINES, backOverflowed);
    }
    totalPages = (lineCount + linesPerPage - 1) / linesPerPage;
    if (totalPages < 1) totalPages = 1;
  }

  // --- loadCurrentCard (FlashcardStudyActivity.cpp:465-512) ----------------
  bool loadCurrentCard() {
    haveCard = false;
    currentState = store.read(currentOrdinal);
    haveCard = true;
    revealed = false;
    currentPage = 0;
    wrapForDisplay();
    shown.push_back(currentOrdinal);
    note();
    return true;
  }

  // --- setRevealed (FlashcardStudyActivity.cpp, setRevealed) ---------------
  //
  // wrapForDisplay() lands on page 0, which is right for the question and wrong
  // for the answer: on a front that fills more than one page, page 0 is still
  // the question and the reveal would look like nothing happened. The answer
  // opens on the page carrying the RULE -- where the answer starts -- and the
  // front stays behind it, one page back, exactly where it was.
  void setRevealed(const bool s) {
    revealed = s;
    wrapForDisplay();
    if (s && ruleLine >= 0) currentPage = ruleLine / linesPerPage;
    note();
  }

  // --- turnPage (FlashcardStudyActivity.cpp:1159-1167) ---------------------
  void turnPage(const int delta) {
    const int target = currentPage + delta;
    if (target < 0 || target >= totalPages) return;
    currentPage = target;
    note();
  }

  // --- decayPending (FlashcardStudyActivity.cpp:325-329) -------------------
  void decayPending() {
    for (uint8_t i = 0; i < pendingCount; i++) {
      if (pending[i].showAfter > 0) pending[i].showAfter--;
    }
  }

  // --- requeueCurrent (FlashcardStudyActivity.cpp:331-345) -----------------
  void requeueCurrent(const uint32_t dueUnix) {
    (void)dueUnix;
    if (pendingCount >= MAX_PENDING) {
      if (droppedRequeues != UINT16_MAX) droppedRequeues++;
      return;
    }
#ifdef STUDY_LOOP_FIXED
    pending[pendingCount].dueUnix = dueUnix;
#endif
    pending[pendingCount].ordinal = currentOrdinal;
    pending[pendingCount].showAfter = REQUEUE_GAP;
    pendingCount++;
  }

  void dropFromPending(const Ordinal ordinal) {
    for (uint8_t i = 0; i < pendingCount; i++) {
      if (pending[i].ordinal != ordinal) continue;
      for (uint8_t j = i + 1; j < pendingCount; j++) pending[j - 1] = pending[j];
      pendingCount--;
      return;
    }
  }

  void removePendingAt(const uint8_t i) {
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < pendingCount; j++) pending[j - 1] = pending[j];
    pendingCount--;
  }

  // --- advanceToNextCard (FlashcardStudyActivity.cpp:371-461) --------------
  void advanceToNextCard() {
    Ordinal next = 0;
    bool found = false;

#ifdef STUDY_LOOP_FIXED
    // FIX 2: a resurfaced card is ready only when its OWN step timer has
    // expired as well as the >= REQUEUE_GAP card gap. Cram has no clock and no
    // schedule, so there the gap alone still governs.
    const bool timed = mode != Mode::CramAll && clockAvailable;
    for (uint8_t i = 0; i < pendingCount && !found; i++) {
      if (pending[i].showAfter != 0) continue;
      if (timed && pending[i].dueUnix > now.unixSecs) continue;
      next = pending[i].ordinal;
      removePendingAt(i);
      found = true;
    }
#else
    for (uint8_t i = 0; i < pendingCount && !found; i++) {
      if (pending[i].showAfter != 0) continue;
      next = pending[i].ordinal;
      removePendingAt(i);
      found = true;
    }
#endif
    if (!found && queueIndex < queue.size()) {
      next = queue[queueIndex++];
      found = true;
    }
    if (!found && pendingCount > 0) {
#ifdef STUDY_LOOP_FIXED
      // FIX 2 (drain half): with nothing else left this is Anki's learn-ahead
      // pull. §5 pin (g) fixes the order -- the card that has WAITED THE MOST
      // CARDS (the lowest showAfter), earliest due only breaking ties -- so a
      // card just answered Again can never be handed straight back, which
      // ordering on due alone would do every time (its 1-minute step is the
      // earliest due in the list).
      uint8_t pick = 0;
      for (uint8_t j = 1; j < pendingCount; j++) {
        if (pending[j].showAfter != pending[pick].showAfter) {
          if (pending[j].showAfter < pending[pick].showAfter) pick = j;
        } else if (pending[j].dueUnix < pending[pick].dueUnix) {
          pick = j;
        }
      }
      next = pending[pick].ordinal;
      removePendingAt(pick);
#else
      next = pending[0].ordinal;
      removePendingAt(0);
#endif
      found = true;
    }
    // (branch 4, drain-time learn-ahead from the state file, is a StateStore
    // scan; the probe's fake store has no unshown intraday cards, so it is
    // modelled as "nothing found".)

    if (!found) {
      finished = true;
      haveCard = false;
      return;
    }
    decayPending();
    currentOrdinal = next;
    loadCurrentCard();
  }

  // --- answerCurrent (FlashcardStudyActivity.cpp:676-768) ------------------
  void answerCurrent(const Grade grade) {
    if (!haveCard) return;

    if (mode == Mode::CramAll) {
      undo.valid = false;  // there is no answer on disk to undo
      // No due to carry: cram's resurfaces are gap-only, and the firmware
      // passes a literal 0 here.
      if (grade == Grade::Again) requeueCurrent(0);
      advanceToNextCard();
      return;
    }

    const CardState before = currentState;
    const CardState after = fsrs::gradeCard(params, before, grade, now, currentOrdinal);

    int counted = 0;  // 0 = Nothing, 1 = NewCard, 2 = Review
    switch (before.state) {
      case CardPhase::New:
        counted = 1;
        break;
      case CardPhase::Review:
        counted = 2;
        break;
      case CardPhase::Learning:
      case CardPhase::Relearning:
        counted = 0;
        break;
    }
    // Snapshotted BEFORE the commit and before the requeue, so it holds the
    // pending list exactly as this answer found it -- pending entries included,
    // dueUnix and all.
    UndoState snapshot;
    snapshot.valid = true;
    snapshot.ordinal = currentOrdinal;
    snapshot.before = before;
    snapshot.newToday = store.newToday;
    snapshot.reviewsToday = store.reviewsToday;
    snapshot.queueIndex = queueIndex;
    snapshot.pendingCount = pendingCount;
    snapshot.studiedNew = studiedNew;
    snapshot.studiedReview = studiedReview;
    snapshot.studiedLearning = studiedLearning;
    snapshot.droppedRequeues = droppedRequeues;
    memcpy(snapshot.pending, pending, sizeof(pending));

    store.commit(currentOrdinal, after, counted);
    undo = snapshot;
    if (counted == 1)
      studiedNew++;
    else if (counted == 2)
      studiedReview++;
    else
      studiedLearning++;

    const bool intraday = after.state == CardPhase::Learning || after.state == CardPhase::Relearning;
    if (intraday && after.due <= now.unixSecs + LEARN_AHEAD_SECS) requeueCurrent(after.due);

    advanceToNextCard();
  }

  // --- undoLastAnswer (FlashcardStudyActivity.cpp:770-798) -----------------
  void undoLastAnswer() {
    if (!undo.valid || mode == Mode::CramAll) return;

    store.records[undo.ordinal] = undo.before;
    store.newToday = undo.newToday;
    store.reviewsToday = undo.reviewsToday;

    queueIndex = undo.queueIndex;
    pendingCount = undo.pendingCount;
    memcpy(pending, undo.pending, sizeof(pending));
    studiedNew = undo.studiedNew;
    studiedReview = undo.studiedReview;
    studiedLearning = undo.studiedLearning;
    droppedRequeues = undo.droppedRequeues;
    currentOrdinal = undo.ordinal;
    undo.valid = false;  // one deep
    finished = false;    // stands in for screen = Screen::Review
    // Re-fronted: the card is shown as if it had never been answered.
    loadCurrentCard();
  }

  // --- loopReview, touch paths only (FlashcardStudyActivity.cpp:1040-1157) -
  void loopReview(const Frame& f) {
    if (finished) return;

    if (revealed) {
      const int rowTop = gradeRowTop();
      const int columnWidth = layout::SCREEN_W / fsrs::GRADE_COUNT;
      // colTouch(col, 0, columnWidth, GRADE_COUNT, rowTop, yEnd)
#ifdef STUDY_LOOP_FIXED
      // FIX 1a: the columns run to the bottom of the panel rather than stopping
      // at menuRowHeight, so each grade target is a hint-band taller over a
      // strip a touch board leaves blank anyway.
      const int yEnd = layout::SCREEN_H;
#else
      const int yEnd = rowTop + layout::MENU_ROW_HEIGHT;
#endif
      const auto inBand = [&](const int x, const int y) {
        return y >= rowTop && y < yEnd && x >= 0 && x / columnWidth < fsrs::GRADE_COUNT;
      };
      if (f.touchDown && inBand(f.x, f.y)) return;  // RowTouch::Down -> ignored
      if (f.touchRelease && inBand(f.x, f.y)) {
        answerCurrent(static_cast<Grade>(f.x / columnWidth));
        return;
      }
#ifdef STUDY_LOOP_FIXED
      // FIX 1b: the answer side pages too. Checked AFTER the grade band, so a
      // tap on a grade button is never a page turn, and restricted to the card
      // body so the band's own outer columns (Again / Easy) stay grades.
      if (f.touchRelease && totalPages > 1 && f.y >= bodyTop() && f.y < bodyTop() + bodyHeight()) {
        if (f.x < layout::SCREEN_W / 4) {
          turnPage(-1);
          return;
        }
        if (f.x > layout::SCREEN_W * 3 / 4) {
          turnPage(1);
          return;
        }
      }
#endif
      return;
    }

    if (!f.touchRelease) return;
    if (totalPages > 1 && f.x < layout::SCREEN_W / 4) {
      turnPage(-1);
      return;
    }
    if (totalPages > 1 && f.x > layout::SCREEN_W * 3 / 4) {
      turnPage(1);
      return;
    }
    setRevealed(true);
  }

  // --- driving ------------------------------------------------------------
  void start() {
    queueIndex = 0;
    pendingCount = 0;
    studiedNew = studiedReview = studiedLearning = 0;
    droppedRequeues = 0;
    undo.valid = false;
    haveCard = false;
    finished = false;
    advanceToNextCard();
  }

  /** One real finger: >= 1 held frames, then the release frame. */
  void tapContact(const int x, const int y) {
    loopReview(Frame{true, false, x, y});
    loopReview(Frame{false, true, x, y});
  }

  int gradeColumnX(const Grade g) const {
    const int columnWidth = layout::SCREEN_W / fsrs::GRADE_COUNT;
    return static_cast<int>(g) * columnWidth + columnWidth / 2;
  }
  void tapGrade(const Grade g) { tapContact(gradeColumnX(g), gradeRowTop() + layout::MENU_ROW_HEIGHT / 2); }
  /** The strip below the grade boxes -- the button-hint band's height, drawn
   *  blank on a touch board (drawButtonHints() early-returns on hasTouch()). */
  void tapBelowGradeBoxes(const Grade g) {
    tapContact(gradeColumnX(g), layout::SCREEN_H - layout::BUTTON_HINTS_HEIGHT / 2);
  }
  void tapCentre() { tapContact(layout::SCREEN_W / 2, bodyTop() + 40); }
  void tapRightQuarter() { tapContact(layout::SCREEN_W - 10, bodyTop() + 40); }
  void tapLeftQuarter() { tapContact(10, bodyTop() + 40); }
};

// ===========================================================================
//  scenarios
// ===========================================================================

static void seedNewDeck(ReviewLoop& r, const int cards, const uint16_t frontLines, const uint16_t backLines) {
  for (int i = 1; i <= cards; i++) {
    r.deck[static_cast<Ordinal>(i)] = FakeCard{frontLines, backLines};
    r.queue.push_back(static_cast<Ordinal>(i));
  }
  // A wall-clock that is comfortably inside a day so `due` arithmetic is plain.
  r.now.unixSecs = 1757000000u;
  r.now.dayNumber = 20340;
  r.params.enableFuzz = false;
}

static std::string joinShown(const std::vector<Ordinal>& v, const size_t max) {
  std::string out;
  for (size_t i = 0; i < v.size() && i < max; i++) {
    char b[8];
    snprintf(b, sizeof(b), "%u", static_cast<unsigned>(v[i]));
    if (i) out += " ";
    out += b;
  }
  return out;
}

// --- 1. one tap on a grade button = exactly one answer ---------------------

static void groupOneTapOneGrade() {
  GROUP("grade/one tap one answer");

  // A card that PAGINATES: 8 front lines + rule + 30 back lines = 39 wrapped
  // lines against 27 lines per answer page, so totalPages == 2.
  ReviewLoop r;
  seedNewDeck(r, 6, 8, 30);
  r.start();
  CHECKEQ(r.totalPages, 1);  // the question side alone fits

  r.tapCentre();
  CHECK(r.revealed);
  CHECKEQ(r.totalPages, 2);
  CHECKSTR(r.visible(), "front+rule+back+grades");

  const int commitsBefore = r.store.commits;
  const size_t shownBefore = r.shown.size();
  r.tapGrade(Grade::Good);
  CHECKEQ(r.store.commits - commitsBefore, 1);  // exactly one persisted answer
  CHECKEQ(r.shown.size() - shownBefore, 1u);    // exactly one advance
  CHECK(!r.revealed);                           // the next card is a question again
  CHECKEQ(r.currentPage, 0);
}

static void groupGradeFromAnyPage() {
  GROUP("grade/one tap from any page");

  ReviewLoop r;
  seedNewDeck(r, 6, 8, 30);
  r.start();
  r.tapCentre();
  CHECKEQ(r.totalPages, 2);

  // Reach page 2 of the ANSWER. Before the fix this is impossible: the revealed
  // branch of loopReview has no page handling at all, so the back of a card
  // that spans two pages cannot be read.
  r.tapRightQuarter();
#ifdef STUDY_LOOP_FIXED
  CHECKEQ(r.currentPage, 1);
  CHECKSTR(r.visible(), "back+grades");
#else
  CHECKEQ(r.currentPage, 0);  // REPRODUCED: the second page is unreachable
#endif

  const int commitsBefore = r.store.commits;
  r.tapGrade(Grade::Good);
  CHECKEQ(r.store.commits - commitsBefore, 1);  // grading still works from there
  CHECK(!r.revealed);
}

static void groupGradeBandReachesPanelBottom() {
  GROUP("grade/the grade columns reach the panel bottom");

  // What this pins is the TARGET, not a label. On a touch board every theme's
  // drawButtonHints() early-returns on gpio.hasTouch(), so the strip under the
  // grade boxes is drawn EMPTY -- the fix runs the four grade columns down over
  // it so each target is a band taller, and a finger that lands just low of the
  // "Good" box still answers Good instead of doing nothing.
  // (render()'s grade labels: FlashcardStudyActivity.cpp:1467, and they only
  // ever appear on the button-only boards, where colTouch() cannot fire.)
  ReviewLoop r;
  seedNewDeck(r, 4, 3, 4);
  r.start();
  r.tapCentre();
  const int commits = r.store.commits;
  r.tapBelowGradeBoxes(Grade::Good);
#ifdef STUDY_LOOP_FIXED
  CHECKEQ(r.store.commits - commits, 1);
  CHECK(!r.revealed);
  CHECKEQ(static_cast<int>(r.store.records[1].state), static_cast<int>(CardPhase::Learning));
#else
  // REPRODUCED: the columns stop one menuRowHeight down, so the strip below the
  // boxes is outside every target and the tap does nothing at all.
  CHECKEQ(r.store.commits - commits, 0);
  CHECK(r.revealed);
#endif
}

static void groupPageTapNeverGrades() {
  GROUP("grade/page taps never grade");

  ReviewLoop r;
  seedNewDeck(r, 6, 8, 30);
  r.start();
  r.tapCentre();
  const int commits = r.store.commits;
  r.tapRightQuarter();
  r.tapLeftQuarter();
  r.tapRightQuarter();
  CHECKEQ(r.store.commits, commits);  // paging must never commit an answer
  CHECK(r.revealed);
}

// --- 2. a Good on a new card must not come back after N cards -------------

static void groupPendingHonoursDue() {
  GROUP("pending/Good on a new card waits for its step timer");

  ReviewLoop r;
  seedNewDeck(r, 20, 3, 4);  // short cards: one page, so taps are unambiguous
  r.start();

  // Answer Good on everything the loop offers, 10 cards deep, with the wall
  // clock standing still (a fast reviewer: 10 answers well inside 10 minutes).
  for (int i = 0; i < 10 && !r.finished; i++) {
    r.tapCentre();
    r.tapGrade(Grade::Good);
  }

#ifdef STUDY_LOOP_FIXED
  // Every card's 10-minute step is still in the future, so the session walks
  // straight down the new queue.
  CHECKSTR(joinShown(r.shown, 11), "1 2 3 4 5 6 7 8 9 10 11");
#endif
#ifndef STUDY_LOOP_FIXED
  // REPRODUCED: card 1 is back on screen as the fifth card, three cards after a
  // Good that scheduled it 10 minutes out.
  CHECKSTR(joinShown(r.shown, 11), "1 2 3 4 1 2 3 4 5 6 7");
#endif
}

static void groupPendingReturnsWhenDue() {
  GROUP("pending/a learning card does return once its timer expires");

  ReviewLoop r;
  seedNewDeck(r, 6, 3, 4);
  r.start();
  for (int i = 0; i < 6 && !r.finished; i++) {
    r.tapCentre();
    r.tapGrade(Grade::Good);
  }
  CHECK(!r.finished);
#ifdef STUDY_LOOP_FIXED
  // Six new cards, each on a 10-minute step and none of them due. The queue is
  // walked straight through; only once it is EMPTY does the drain-time
  // learn-ahead pull the earliest-due card (card 1) forward -- which is what
  // Anki does when nothing else is left, and is what keeps the session from
  // ending on a card that is minutes away.
  CHECKSTR(joinShown(r.shown, 7), "1 2 3 4 5 6 1");
  CHECKEQ(r.pendingCount, 5);
#else
  CHECKSTR(joinShown(r.shown, 7), "1 2 3 4 1 2 3");
#endif

  // Eleven minutes on, the rest are genuinely due; the session drains them and
  // every card graduates rather than looping.
  r.now.unixSecs += 660;
  int guard = 0;
  while (!r.finished && guard++ < 200) {
    r.tapCentre();
    r.tapGrade(Grade::Good);
  }
  CHECK(r.finished);
  for (Ordinal o = 1; o <= 6; o++)
    CHECKEQ(static_cast<int>(r.store.records[o].state), static_cast<int>(CardPhase::Review));
}

static void groupAgainStillRequeuesBehindThree() {
  GROUP("pending/Again still comes back behind >= 3 cards");

  ReviewLoop r;
  seedNewDeck(r, 8, 3, 4);
  r.start();
  // Again on card 1 puts it on the 1-minute step; the >= 3 gap is what the spec
  // asks for, and one minute of real reviewing passes in the meantime.
  r.tapCentre();
  r.tapGrade(Grade::Again);
  for (int i = 0; i < 3 && !r.finished; i++) {
    r.now.unixSecs += 30;  // 30 s per card: card 1's 60 s step expires during it
    r.tapCentre();
    r.tapGrade(Grade::Good);
  }
  // 1 2 3 4 then 1 again -- behind exactly three other cards.
  CHECKSTR(joinShown(r.shown, 5), "1 2 3 4 1");
}

static void groupAgainNeverRepeatsBackToBack() {
  GROUP("pending/the drain never hands a card straight back to itself");

  // §5 pin (g): "a card answered Again is never the immediate next card while
  // another parked card is available". The 8-card scenario above never gets
  // near this -- it always has more built queue to serve -- so the drain branch
  // (advanceToNextCard's third) needs a case of its own: two cards, both
  // parked, and the built queue exhausted, which leaves the drain as the ONLY
  // thing choosing.
  ReviewLoop r;
  seedNewDeck(r, 2, 3, 4);
  r.start();
  r.tapCentre();
  r.tapGrade(Grade::Good);  // card 1 parked, 10-minute step, showAfter decays to 2
  r.tapCentre();
  r.tapGrade(Grade::Again);  // card 2 parked, 1-minute step, showAfter 3; queue empty

  // Card 2's due is by far the earliest of the two, so a drain ordering on due
  // ALONE would put the card just answered Again straight back on screen. Card
  // 1 has waited a card longer, and the pin gives it the slot.
  CHECKSTR(joinShown(r.shown, 3), "1 2 1");
}

static void groupUndoRestoresPending() {
  GROUP("undo/the widened pending entry survives snapshot + restore");

  // The undo snapshot memcpy()s the whole pending array, so widening
  // PendingCard with dueUnix widened what undo has to carry. Grade, undo, and
  // compare the array field by field against what it held going in.
  ReviewLoop r;
  seedNewDeck(r, 4, 3, 4);
  r.start();
  r.tapCentre();
  r.tapGrade(Grade::Good);  // card 1 parked; card 2 on screen
  r.tapCentre();

  // What the answer about to be given will find, and must be handed back.
  const uint8_t pendingBefore = r.pendingCount;
  ReviewLoop::PendingCard expected[ReviewLoop::MAX_PENDING];
  memcpy(expected, r.pending, sizeof(expected));
  const uint16_t newTodayBefore = r.store.newToday;
  CHECKEQ(pendingBefore, 1);
  CHECKEQ(expected[0].ordinal, 1);
  CHECKEQ(expected[0].showAfter, ReviewLoop::REQUEUE_GAP - 1);  // one card has gone past
#ifdef STUDY_LOOP_FIXED
  CHECKEQ(expected[0].dueUnix, r.now.unixSecs + 600);  // card 1's 10-minute step
#endif

  r.tapGrade(Grade::Good);  // card 2 answered: parks itself and decays card 1
  CHECKEQ(r.pendingCount, 2);
  CHECK(r.undo.valid);

  r.undoLastAnswer();
  CHECKEQ(r.pendingCount, pendingBefore);
  for (uint8_t i = 0; i < pendingBefore; i++) {
    CHECKEQ(r.pending[i].ordinal, expected[i].ordinal);
    CHECKEQ(r.pending[i].showAfter, expected[i].showAfter);
#ifdef STUDY_LOOP_FIXED
    CHECKEQ(r.pending[i].dueUnix, expected[i].dueUnix);
#endif
  }
  // And the rest of the answer with it: the record, the counter, the card.
  CHECKEQ(static_cast<int>(r.store.records[2].state), static_cast<int>(CardPhase::New));
  CHECKEQ(r.store.newToday, newTodayBefore);
  CHECKEQ(r.currentOrdinal, 2);
  CHECK(!r.revealed);  // re-fronted
  CHECK(!r.undo.valid);
}

static void groupSessionStillEnds() {
  GROUP("pending/session end is still detected");

  ReviewLoop r;
  seedNewDeck(r, 2, 3, 4);
  r.start();
  int guard = 0;
  while (!r.finished && guard++ < 200) {
    r.tapCentre();
    r.tapGrade(Grade::Good);
  }
  CHECK(r.finished);
  CHECK(guard < 200);
  // Two new cards, each answered twice (new -> 10 min step -> graduated).
  CHECKEQ(r.store.commits, 4);
  CHECKEQ(r.store.newToday, 2);
  CHECKEQ(r.studiedNew, 2);
  CHECKEQ(r.studiedLearning, 2);
}

static void groupCramUnchanged() {
  GROUP("pending/cram semantics unchanged");

  ReviewLoop r;
  seedNewDeck(r, 6, 3, 4);
  r.mode = ReviewLoop::Mode::CramAll;
  r.clockAvailable = false;
  r.start();
  // Again on card 1 requeues it behind three cards and persists nothing.
  r.tapCentre();
  r.tapGrade(Grade::Again);
  for (int i = 0; i < 3 && !r.finished; i++) {
    r.tapCentre();
    r.tapGrade(Grade::Good);
  }
  CHECKSTR(joinShown(r.shown, 5), "1 2 3 4 1");
  CHECKEQ(r.store.commits, 0);
}

// --- 2b. CPDK v2 card images: the layout math only ------------------------

/** A one-card deck whose front carries `images`, loaded and wrapped. */
static ReviewLoop imageCard(const uint16_t frontLines, const std::vector<FakeImage>& images,
                            const uint16_t backLines = 1) {
  ReviewLoop r;
  FakeCard card;
  card.frontLines = frontLines;
  card.backLines = backLines;
  card.frontImages = images;
  r.deck[0] = card;
  r.queue = {0};
  r.store.records[0] = CardState{};
  r.start();
  return r;
}

/** Every image block lies entirely inside one page -- the invariant paging rests on. */
static bool noImageStraddlesAPage(const ReviewLoop& r) {
  for (size_t i = 0; i < r.imageStarts.size(); i++) {
    const int page = r.imageStarts[i] / r.linesPerPage;
    const int lastLine = r.imageStarts[i] + r.imageSpans[i] - 1;
    if (lastLine / r.linesPerPage != page) return false;
  }
  return true;
}

static void groupImageOccupiesWholeLines() {
  GROUP("images/line count");
  // 22 px per line. A 220 px image is exactly ten lines; 221 px is eleven,
  // because a block is never cut short of its own height.
  ReviewLoop r = imageCard(0, {FakeImage{0, 220}}, 0);
  CHECKEQ(r.imageSpans.size(), 1u);
  CHECKEQ(r.imageSpans[0], 10);
  CHECKEQ(r.lineCount, 10);
  CHECKEQ(r.imageStarts[0], 0);

  ReviewLoop odd = imageCard(0, {FakeImage{0, 221}}, 0);
  CHECKEQ(odd.imageSpans[0], 11);

  // A one-pixel image still occupies a whole line: the pager has no half slots.
  ReviewLoop tiny = imageCard(0, {FakeImage{0, 1}}, 0);
  CHECKEQ(tiny.imageSpans[0], 1);
  CHECKEQ(tiny.lineCount, 1);

  // An image-only card (no text at all) is a real card with a real page, not a
  // blank screen -- §3.7 explicitly allows a side of length 0 with a placement.
  CHECKEQ(tiny.totalPages, 1);
  CHECK(tiny.haveCard);
}

static void groupImageTallerThanThePageIsCapped() {
  GROUP("images/page-height cap");
  ReviewLoop r = imageCard(0, {FakeImage{0, 2000}}, 0);
  // Capped at one page, so it can never make a page that no page can hold.
  CHECKEQ(r.imageSpans[0], r.linesPerPage);
  CHECKEQ(r.lineCount, r.linesPerPage);
  CHECKEQ(r.totalPages, 1);
  CHECK(noImageStraddlesAPage(r));
}

static void groupImageNeverStraddlesAPageBreak() {
  GROUP("images/page padding");
  ReviewLoop probe = imageCard(0, {}, 0);
  const int perPage = probe.linesPerPage;
  CHECK(perPage > 6);

  // Placed after enough text that the block cannot fit in what is left of the
  // page: the page is padded out and the image opens the next one.
  const uint16_t before = static_cast<uint16_t>(perPage - 2);
  ReviewLoop r = imageCard(static_cast<uint16_t>(perPage + 4), {FakeImage{before, 110}}, 0);
  CHECKEQ(r.imageSpans[0], 5);
  CHECKEQ(r.imageStarts[0], perPage);      // top of page 2, not two lines short of it
  CHECKEQ(r.imageStarts[0] % perPage, 0);  // ... which is what "page aligned" means here
  CHECK(noImageStraddlesAPage(r));

  // One that DOES fit in the remainder is not pushed forward: the padding is a
  // fix for straddling, not a rule that every image starts a page.
  ReviewLoop fits = imageCard(static_cast<uint16_t>(perPage + 4), {FakeImage{2, 44}}, 0);
  CHECKEQ(fits.imageStarts[0], 2);
  CHECK(noImageStraddlesAPage(fits));

  // Four images on one side, the format's cap, at assorted heights and offsets:
  // none of them may straddle, whatever the padding did to the ones before.
  ReviewLoop four = imageCard(static_cast<uint16_t>(perPage * 2),
                              {FakeImage{1, 200}, FakeImage{4, 90}, FakeImage{4, 300}, FakeImage{20, 60}}, 0);
  CHECKEQ(four.imageSpans.size(), 4u);
  CHECK(noImageStraddlesAPage(four));
  // A tie (two placements at one offset) emits two blocks, in table order.
  CHECK(four.imageStarts[2] > four.imageStarts[1]);
}

static void groupImagesPageWithTheAnswer() {
  GROUP("images/reveal repaging");
  ReviewLoop probe = imageCard(0, {}, 0);
  const int perPage = probe.linesPerPage;

  ReviewLoop r;
  FakeCard card;
  card.frontLines = 3;
  card.backLines = 3;
  card.frontImages = {FakeImage{1, 300}};
  card.backImages = {FakeImage{1, 300}};
  r.deck[0] = card;
  r.queue = {0};
  r.store.records[0] = CardState{};
  r.start();
  const int questionLines = r.lineCount;
  CHECK(noImageStraddlesAPage(r));
  CHECKEQ(r.imageSpans.size(), 1u);

  // Revealing appends the answer, so the whole card -- images included -- is
  // laid out again. The page HEIGHT does not change: both states are paged
  // against the revealed body, which is what keeps the front where it was.
  r.tapCentre();
  CHECK(r.revealed);
  CHECKEQ(r.linesPerPage, perPage);
  CHECKEQ(r.imageSpans.size(), 2u);
  CHECK(r.lineCount > questionLines);
  CHECK(noImageStraddlesAPage(r));
  // Both sides are reachable by paging, and the answer is never a no-op.
  CHECK(r.totalPages >= 1);
  bool sawBack = false;
  for (int page = 0; page < r.totalPages; page++) {
    r.currentPage = page;
    if (r.visible().find("back") != std::string::npos) sawBack = true;
  }
  CHECK(sawBack);
}

static void groupImagesShareTheLineBudget() {
  GROUP("images/line budget");
  // Images are drawn from the same budget the text is, so a front stuffed with
  // them overflows exactly as a front stuffed with newlines does -- and the
  // reserved-back rule still holds: the rule and the back still get their lines.
  ReviewLoop r;
  FakeCard card;
  card.frontLines = ReviewLoop::MAX_FRONT_LINES;
  card.backLines = 40;
  card.frontImages = {FakeImage{1, 400}, FakeImage{2, 400}, FakeImage{3, 400}, FakeImage{4, 400}};
  r.deck[0] = card;
  r.queue = {0};
  r.store.records[0] = CardState{};
  r.start();
  CHECK(r.frontOverflowed);
  CHECKEQ(r.lineCount, ReviewLoop::MAX_FRONT_LINES);

  r.tapCentre();
  // The front is still capped at MAX_FRONT_LINES, so the rule plus the back's
  // first RESERVED_BACK_LINES - 1 lines always fit -- the reveal shows
  // something whatever the front did.
  CHECK(r.lineCount > ReviewLoop::MAX_FRONT_LINES);
  CHECK(r.lineCount <= ReviewLoop::MAX_CARD_LINES);
  CHECK(r.frontOverflowed);  // ... and the overflow marker still says so

  // A back that overflows through images alone raises the BACK flag, so images
  // are accounted to the side they belong to and not to whichever side happened
  // to be wrapping. The '+' the page counter draws is `frontOverflowed ||
  // backOverflowed` -- ONE marker for both sides, not one per side. That is a
  // deliberate, accepted deviation from §5 pin d's "per-side overflow
  // indicators": the counter strip is one right-aligned line of UI_10 text
  // beside the page numbers, and two markers there would cost more room than
  // the distinction is worth on a 480 px panel. The per-side FLAGS are still
  // tracked separately, which is what this check pins, so restoring a per-side
  // marker is a render change and nothing more.
  ReviewLoop back;
  FakeCard heavy;
  heavy.frontLines = 1;
  heavy.backLines = ReviewLoop::MAX_CARD_LINES;
  heavy.backImages = {FakeImage{1, 400}};
  back.deck[0] = heavy;
  back.queue = {0};
  back.store.records[0] = CardState{};
  back.start();
  CHECK(!back.frontOverflowed);
  back.tapCentre();
  CHECK(back.backOverflowed);
  CHECKEQ(back.lineCount, ReviewLoop::MAX_CARD_LINES);
}

static void groupImagesLeaveTheLoopAlone() {
  GROUP("images/loop untouched");
  // The whole point of padding rather than splitting: a card with images grades
  // and pages exactly like one without. Four images, a paginated answer, one
  // tap on Good -- one answer, one commit, and the session moves on.
  ReviewLoop r;
  for (Ordinal i = 0; i < 3; i++) {
    FakeCard card;
    card.frontLines = 6;
    card.backLines = 40;
    card.frontImages = {FakeImage{1, 150}};
    card.backImages = {FakeImage{2, 150}, FakeImage{2, 150}};
    r.deck[i] = card;
    r.store.records[i] = CardState{};
    r.queue.push_back(i);
  }
  r.start();
  r.tapCentre();
  CHECK(r.totalPages > 1);
  const int pagesBefore = r.totalPages;
  r.tapRightQuarter();
#ifdef STUDY_LOOP_FIXED
  // Answer-side page taps only exist after the fix; the shipped build's lack of
  // them is what groupGradeFromAnyPage() reproduces, and images do not change
  // that either way.
  CHECKEQ(r.currentPage, 1);
#endif
  CHECKEQ(r.totalPages, pagesBefore);  // paging an image page changes no layout
  CHECK(noImageStraddlesAPage(r));
  r.tapGrade(Grade::Good);
  CHECKEQ(r.store.commits, 1);
  CHECKEQ(r.shown.size(), 2u);
  CHECKEQ(r.currentPage, 0);  // the next card starts at its first page
}

// --- 2c. what the reveal lands on, and what it must not move --------------

static void groupRevealOpensOnTheAnswer() {
  GROUP("reveal/the answer opens on the page carrying the rule");

  ReviewLoop probe = imageCard(0, {}, 0);
  const int perPage = probe.linesPerPage;

  // A front of 40 wrapped lines fills more than one page on its own, so the
  // rule and the first line of the back land on page 2. Landing the reveal on
  // page 0 would put the QUESTION back on screen with a grade row under it --
  // the reveal would read as a no-op, which is the whole complaint.
  ReviewLoop text;
  seedNewDeck(text, 3, 40, 6);
  text.start();
  CHECKEQ(text.currentPage, 0);
  CHECK(text.frontLineCount > static_cast<uint16_t>(perPage));
  text.tapCentre();
  CHECK(text.revealed);
  CHECKEQ(text.currentPage, text.ruleLine / perPage);
  CHECKEQ(text.currentPage, 1);
  CHECK(text.visible().find("rule") != std::string::npos);
  CHECK(text.visible().find("back") != std::string::npos);

  // Un-revealing goes back to page 0 of the question: a reader who revealed by
  // accident gets the card as it was, not the card scrolled to its last page.
  text.setRevealed(false);
  CHECK(!text.revealed);
  CHECKEQ(text.currentPage, 0);
  CHECKSTR(text.visible(), "front");

  // The same thing with pictures rather than words: two page-tall images push
  // the rule further down, and the reveal follows it.
  ReviewLoop pictures;
  FakeCard card;
  card.frontLines = 2;
  card.backLines = 4;
  card.frontImages = {FakeImage{1, 2000}, FakeImage{2, 2000}};  // each capped at one page
  pictures.deck[0] = card;
  pictures.queue = {0};
  pictures.store.records[0] = CardState{};
  pictures.start();
  CHECKEQ(pictures.imageSpans.size(), 2u);
  CHECKEQ(pictures.imageSpans[0], perPage);
  CHECKEQ(pictures.imageSpans[1], perPage);
  pictures.tapCentre();
  CHECK(pictures.currentPage > 0);
  CHECKEQ(pictures.currentPage, pictures.ruleLine / perPage);
  CHECK(pictures.visible().find("rule") != std::string::npos);
  CHECK(noImageStraddlesAPage(pictures));
}

static void groupFrontPagesIdenticallyAcrossTheReveal() {
  GROUP("reveal/the front keeps its pagination");

  // The reveal must not re-flow the question underneath the reader. Both states
  // are paged against the REVEALED body, so the front's line count, its page
  // height, its image blocks and the page each of them sits on are the same
  // before and after -- an image-bearing front, because images are laid out in
  // whole pages and are the layer that would move first if the page height
  // changed under them.
  ReviewLoop r;
  FakeCard card;
  card.frontLines = 30;
  card.backLines = 12;
  card.frontImages = {FakeImage{4, 300}, FakeImage{25, 260}};
  card.backImages = {FakeImage{2, 300}};
  r.deck[0] = card;
  r.queue = {0};
  r.store.records[0] = CardState{};
  r.start();

  const int perPageBefore = r.linesPerPage;
  const uint16_t frontLinesBefore = r.frontLineCount;
  const std::vector<int> startsBefore = r.imageStarts;
  const std::vector<int> spansBefore = r.imageSpans;
  CHECKEQ(startsBefore.size(), 2u);
  CHECK(noImageStraddlesAPage(r));

  r.tapCentre();
  CHECKEQ(r.linesPerPage, perPageBefore);
  CHECKEQ(r.frontLineCount, frontLinesBefore);
  // The front's two blocks are still the first two, in the same slots, on the
  // same pages. (The back adds a third behind them.)
  CHECKEQ(r.imageStarts.size(), 3u);
  CHECKEQ(r.imageStarts[0], startsBefore[0]);
  CHECKEQ(r.imageStarts[1], startsBefore[1]);
  CHECKEQ(r.imageSpans[0], spansBefore[0]);
  CHECKEQ(r.imageSpans[1], spansBefore[1]);
  CHECKEQ(r.imageStarts[0] / r.linesPerPage, startsBefore[0] / perPageBefore);
  CHECKEQ(r.imageStarts[1] / r.linesPerPage, startsBefore[1] / perPageBefore);
  CHECK(noImageStraddlesAPage(r));

  // And back again: un-revealing restores exactly the question that was there.
  r.setRevealed(false);
  CHECKEQ(r.linesPerPage, perPageBefore);
  CHECKEQ(r.lineCount, frontLinesBefore);
  CHECKEQ(r.imageStarts.size(), 2u);
  CHECKEQ(r.imageStarts[0], startsBefore[0]);
  CHECKEQ(r.imageStarts[1], startsBefore[1]);
  CHECKEQ(r.currentPage, 0);
}

// --- 3. constants this mirror shares with the firmware --------------------

static void groupMirror() {
  GROUP("mirror/shared constants");
  CHECKEQ(ReviewLoop::REQUEUE_GAP, 3);
  CHECKEQ(ReviewLoop::MAX_PENDING, 64);
  CHECKEQ(ReviewLoop::LEARN_AHEAD_SECS, 1200);
  CHECKEQ(fsrs::GRADE_COUNT, 4);
  // The grade band and the card body must not overlap, or a grade tap would be
  // ambiguous with the answer-side page taps added by the fix.
  ReviewLoop r;
  r.revealed = true;
  CHECK(r.bodyTop() + r.bodyHeight() <= ReviewLoop::gradeRowTop());
  // A Good on a new card lands on the second learning step, 10 minutes out.
  fsrs::Params p = fsrs::defaultParams();
  p.enableFuzz = false;
  Now n{};
  n.unixSecs = 1757000000u;
  n.dayNumber = 20340;
  const CardState after = fsrs::gradeCard(p, CardState{}, Grade::Good, n, 1);
  CHECKEQ(static_cast<int>(after.state), static_cast<int>(CardPhase::Learning));
  CHECKEQ(after.due - n.unixSecs, 600);
  // An Again is due SOONER than a Good, which is what makes ordering the drain
  // on due alone hand a card straight back to itself -- the premise of
  // groupAgainNeverRepeatsBackToBack().
  const CardState again = fsrs::gradeCard(p, CardState{}, Grade::Again, n, 1);
  CHECK(again.due < after.due);
}

/** `--trace`: the user-visible frame sequence for the reported repro. */
static void traceUserRepro() {
  // A card long enough to paginate on the answer side (8 front + rule + 30 back
  // lines against 27 lines per answer page), in a 20-new-card Due session.
  ReviewLoop r;
  seedNewDeck(r, 20, 8, 30);
  r.start();
  printf("  reveal, then Good, four cards deep (clock still):\n");
  for (int i = 0; i < 5 && !r.finished; i++) {
    r.tapCentre();
    r.tapGrade(Grade::Good);
  }
  for (const std::string& line : r.log) printf("    %s\n", line.c_str());
  printf("  cards shown: %s\n", joinShown(r.shown, 32).c_str());

  ReviewLoop p;
  seedNewDeck(p, 20, 8, 30);
  p.start();
  p.tapCentre();
  printf("  one paginated answer, right-quarter taps: page %d/%d -> ", p.currentPage + 1, p.totalPages);
  p.tapRightQuarter();
  printf("%d/%d (%s)\n", p.currentPage + 1, p.totalPages, p.visible().c_str());
}

int main(int argc, char** argv) {
  const bool trace = argc > 1 && strcmp(argv[1], "--trace") == 0;
#ifdef STUDY_LOOP_FIXED
  printf("== study review loop (FIXED) ==\n");
#else
  printf("== study review loop (as shipped -- reproduces the field reports) ==\n");
#endif
  groupOneTapOneGrade();
  groupGradeFromAnyPage();
  groupGradeBandReachesPanelBottom();
  groupPageTapNeverGrades();
  groupPendingHonoursDue();
  groupPendingReturnsWhenDue();
  groupAgainStillRequeuesBehindThree();
  groupAgainNeverRepeatsBackToBack();
  groupUndoRestoresPending();
  groupSessionStillEnds();
  groupCramUnchanged();
  groupImageOccupiesWholeLines();
  groupImageTallerThanThePageIsCapped();
  groupImageNeverStraddlesAPageBreak();
  groupImagesPageWithTheAnswer();
  groupImagesShareTheLineBudget();
  groupImagesLeaveTheLoopAlone();
  groupRevealOpensOnTheAnswer();
  groupFrontPagesIdenticallyAcrossTheReveal();
  groupMirror();
  if (trace) traceUserRepro();

  if (g_fail) {
    printf("FAILED: %d check(s)\n", g_fail);
    return 1;
  }
  printf("ok\n");
  return 0;
}
