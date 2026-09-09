#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <FsrsSched.h>
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "flashcards/DeckFile.h"
#include "flashcards/SessionQueue.h"
#include "flashcards/StateStore.h"
#include "util/ButtonNavigator.h"

/**
 * The study session (FLASHCARD_SPEC.md §5): deck picker -> mode select -> review
 * loop -> done screen, as one activity with a `Screen` enum and a `switch`
 * render, the shape ClockSyncActivity uses.
 *
 * What the screens own:
 *
 *   Picker  the `.deck` leaves in the (normalized) deck folder, listed BY
 *           FILENAME. Nothing is opened to draw this list -- DeckFile::open()
 *           validates the whole index and holds it resident (800 KB at the
 *           40000-card cap), so opening every deck in the folder to read a
 *           39-byte title would cost one full validation and one large
 *           allocation per row. The deck's real title and its due/new/learning
 *           counts appear on the mode screen, which is the screen that needs
 *           them and is reached by opening exactly one deck.
 *   Busy    the two blocking steps (open deck + state file, build a session)
 *           get their own state so the "Loading" frame is on the panel before
 *           the SD work starts -- ClockSyncActivity's SYNCING pattern.
 *   Mode    Due / New only / Cram all, with the counts. Without a usable clock
 *           only Cram is offered (§5): cram writes no scheduling state and so
 *           needs no day number.
 *   Review  front, then front + rule + back after the reveal, paged; the four
 *           grade buttons carry their predicted intervals. A long press --
 *           either front button, or a held finger anywhere on the screen --
 *           opens the card menu (undo / suspend), which is the ONLY route to
 *           those two on a board with no front back/confirm GPIO (§5 pin a).
 *   Done    what the session studied, and when the next intraday card is due.
 *
 * Threading: every field render() reads is written under a RenderLock, and no
 * lock is ever held across the SD work (docs/activity-manager.md).
 *
 * Sleep: this screen does NOT override preventAutoSleep(). The reader -- the
 * other "user is staring at text" activity -- does not either, and there is
 * nothing here that a sleep would corrupt: the state file is brought up to date
 * after every single answer, so the worst a sleep can cost is the unanswered
 * card on screen. The session is rebuilt from disk on the way back in.
 */
class FlashcardStudyActivity final : public Activity {
 public:
  FlashcardStudyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { Picker, Busy, Mode, Review, Done, Failed };
  /** What the Busy screen is about to do once its frame is on the panel. */
  enum class Work : uint8_t { None, OpenDeck, BuildSession };
  /** Which buffer a wrapped line points into, plus the one line that is a rule. */
  enum class LineKind : uint8_t { Front, Back, Rule };
  /** What the card menu offers; the rows it shows depend on the card's state. */
  enum class CardAction : uint8_t { Undo, Suspend };

  // Card text is never held whole in a std::string: each side is read straight
  // into one of two heap buffers sized for the format's 4096-byte cap plus a
  // terminator, allocated once per session and reused by every card.
  static constexpr size_t CARD_TEXT_BYTES = flashcards::DECK_MAX_SLICE_BYTES + 1;
  // Wrapped-line ceiling for one card (front + rule + back). 4096 bytes of real
  // text wraps to well under a hundred lines; the cap only bites on a card that
  // is mostly newlines, and it is reported with a continuation marker rather
  // than dropped silently.
  static constexpr uint16_t MAX_CARD_LINES = 256;
  // Lines held back from the front pass so the reveal is never a visual no-op
  // (§5 pin d): the rule plus at least 32 lines of the back always fit, however
  // many newlines the front carries. Both sides flag their own overflow and the
  // page counter shows '+' when either was cut.
  static constexpr uint16_t RESERVED_BACK_LINES = 33;
  static constexpr uint16_t MAX_FRONT_LINES = MAX_CARD_LINES - RESERVED_BACK_LINES;
  static constexpr size_t MEASURE_BYTES = 192;  // longest span measured in one call
  // 64 resurface slots (256 B, and the same again in the undo snapshot). A
  // session that pushes past this counts the drop and reports it on the Done
  // screen rather than losing the card silently (§5 pin e).
  static constexpr uint8_t MAX_PENDING = 64;
  static constexpr uint8_t REQUEUE_GAP = 3;  // "behind >= 3 other cards" (§5)
  static constexpr size_t MAX_DECKS = 64;
  static constexpr unsigned long HOLD_MS = 700;  // undo / suspend long-press
  static constexpr uint8_t MAX_MENU_ACTIONS = 2;

  /** A wrapped line as a span into `frontText` / `backText`; no per-line string. */
  struct CardLine {
    uint16_t start;
    uint16_t length;
    LineKind kind;
  };

  /**
   * A card waiting to resurface inside this session. `showAfter` counts the
   * other cards that must still be shown before it comes back, so an Again goes
   * behind >= REQUEUE_GAP cards and lands at the end of the session when fewer
   * than that remain.
   */
  struct PendingCard {
    flashcards::Ordinal ordinal;
    uint8_t showAfter;
  };

  /**
   * One-deep undo. It restores the whole answer, not just the record: the
   * pre-answer CardState, the two daily counters, where the queue had got to,
   * the resurface list and the session tallies. Snapshotting the pending list
   * wholesale (256 bytes) is what makes the restore exact -- the answer may have
   * pushed the card onto it, or pulled a different one off it -- and, with the
   * list full, may have been the answer whose resurface was dropped.
   */
  struct UndoState {
    bool valid = false;
    flashcards::Ordinal ordinal = 0;
    fsrs::CardState before{};
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

  // --- Picker ---
  void scanDeckFolder();
  int deckRowsPerPage() const;
  void openSelectedDeck();

  // --- Session ---
  bool allocateSessionBuffers();
  void releaseSessionBuffers();
  void closeDeck();
  bool buildSelectedSession();
  void startReview();
  void finishSession();
  /** Reads the clock again; false (and the session ends) when it has gone away. */
  bool refreshNow();

  // --- Queue ---
  void advanceToNextCard();
  void requeueCurrent();
  void dropFromPending(flashcards::Ordinal ordinal);
  void decayPending();

  // --- Answering ---
  void setRevealed(bool shown);
  void turnPage(int delta);
  void answerCurrent(fsrs::Grade grade);
  void undoLastAnswer();
  void suspendCurrent();
  bool loadCurrentCard();

  // --- Card menu (undo / suspend) ---
  /** Builds the rows the current card actually supports; no rows = no popup. */
  void openCardMenu();
  /** Runs the row picked this frame, once the popup's callback has returned. */
  void applyMenuChoice();

  // --- Layout / text ---
  struct BodyArea {
    int x;
    int y;
    int width;
    int height;
  };
  BodyArea bodyArea() const;
  int gradeRowTop() const;
  int measureSpan(const char* text, size_t length) const;
  /** Wraps one side into at most `lineLimit` total lines; sets `overflowed` if it did not fit. */
  void appendWrapped(const char* text, uint16_t length, LineKind kind, int maxWidth, uint16_t lineLimit,
                     bool& overflowed);
  void wrapForDisplay();
  const char* bufferFor(LineKind kind) const;
  /** "<1m" / "10m" / "3d" / "2mo", via the i18n unit formats. */
  void formatInterval(uint8_t grade, char* out, size_t outBytes) const;

  // --- Input handlers, one per screen ---
  void loopPicker();
  void loopMode();
  void loopReview();
  void loopDone();

  // --- Render helpers, one per screen ---
  void renderPicker() const;
  void renderMode() const;
  void renderReview() const;
  void renderDone() const;
  void drawTwoLineMessage(const char* headline, const char* detail) const;
  /** Shared themed-list draw: `windowOffset >= 0` = deck rows from there, -1 = mode rows. */
  void drawRows(int top, int count, int selected, int windowOffset) const;

  void goToPicker();
  void fail(StrId message);

  Screen screen = Screen::Picker;
  Work pendingWork = Work::None;
  StrId failure = StrId::STR_ERROR_MSG;

  std::string destFolder;
  std::vector<std::string> deckLeaves;  // filenames, with the .deck extension
  // The drawn window is a pure function of the selection and the row count
  // (page = selection / rowsPerPage), so loop() and render() agree on which
  // rows are on screen without either writing the other's state.
  int deckSelection = 0;

  // Declared before `store`: StateStore holds a DeckFile* for the whole of its
  // life and checks every record against the deck's keys, so the deck must
  // outlive it. Member order gives that for free on destruction.
  flashcards::DeckFile deck;
  flashcards::StateStore store;
  std::unique_ptr<flashcards::Session> session;
  flashcards::SessionMode mode = flashcards::SessionMode::Due;
  int modeSelection = 0;
  bool clockAvailable = false;
  fsrs::Now now{};

  std::unique_ptr<char[]> frontText;
  std::unique_ptr<char[]> backText;
  std::unique_ptr<CardLine[]> lines;
  uint16_t frontLength = 0;
  uint16_t backLength = 0;
  uint16_t lineCount = 0;
  bool frontOverflowed = false;
  bool backOverflowed = false;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;

  uint32_t queueIndex = 0;
  PendingCard pending[MAX_PENDING] = {};
  uint8_t pendingCount = 0;
  flashcards::Ordinal currentOrdinal = 0;
  fsrs::CardState currentState{};
  fsrs::Preview preview{};
  bool haveCard = false;
  bool revealed = false;

  uint16_t studiedNew = 0;
  uint16_t studiedReview = 0;
  uint16_t studiedLearning = 0;
  uint16_t droppedRequeues = 0;
  uint32_t nextDueUnix = 0;

  UndoState undo;
  ButtonNavigator buttonNavigator;

  // The card menu. Its rows are rebuilt per opening, so row index alone does not
  // identify an action: `menuActions` maps the row back to what it does. The
  // popup's callback only records the choice -- running undo/suspend from inside
  // it would re-enter the popup (and destroy the std::function mid-call when the
  // action dismisses it), so applyMenuChoice() runs it once handleInput returns.
  // Everything but the popup itself is loop-task only -- render() reads none of
  // it, so none of it needs the RenderLock.
  OptionPopup cardMenu;
  CardAction menuActions[MAX_MENU_ACTIONS] = {};
  uint8_t menuActionCount = 0;
  CardAction menuChoice = CardAction::Undo;
  bool menuChoicePending = false;
};

#endif  // CROSSPOINT_FLASHCARDS
