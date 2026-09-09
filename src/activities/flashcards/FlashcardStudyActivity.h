#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <FsrsSched.h>
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "CardImage.h"
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
  /**
   * Which buffer a wrapped line points into, plus the three line slots that
   * point into no buffer at all: the reveal rule, a card image, and the blank
   * filler an image occupies below its own first line.
   */
  enum class LineKind : uint8_t { Front, Back, Rule, Image, Blank };
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
  // 64 resurface slots (512 B, and the same again in the undo snapshot). A
  // session that pushes past this counts the drop and reports it on the Done
  // screen rather than losing the card silently (§5 pin e).
  static constexpr uint8_t MAX_PENDING = 64;
  static constexpr uint8_t REQUEUE_GAP = 3;  // "behind >= 3 other cards" (§5)
  static constexpr size_t MAX_DECKS = 64;
  static constexpr unsigned long HOLD_MS = 700;  // undo / suspend long-press
  static constexpr uint8_t MAX_MENU_ACTIONS = 2;

  // A card side may carry at most four images (DECK_SERVER_SPEC.md §3.7), so a
  // card carries at most eight. They live in one fixed member array — 8 x 16 B,
  // read once per card load, never a heap block.
  static constexpr uint8_t MAX_CARD_IMAGES = flashcards::DECK_MAX_IMAGES_PER_SIDE * 2;

  /**
   * A wrapped line as a span into `frontText` / `backText`; no per-line string.
   *
   * On a LineKind::Image line the two fields mean something else, and they are
   * the only place they do: `start` is the slot in `cardImages`, and `length` is
   * how many line slots the image block occupies (its own, plus that many minus
   * one LineKind::Blank fillers behind it). Overloading them rather than growing
   * the struct keeps a 256-line card at 1.5 KB — the array is allocated once per
   * session and is one of the three buffers that make a session possible at all.
   */
  struct CardLine {
    uint16_t start;
    uint16_t length;
    LineKind kind;
  };

  /**
   * A card waiting to resurface inside this session.
   *
   * TWO gates, and a card has to clear both. `showAfter` counts the other cards
   * that must still be shown before it comes back, so an Again goes behind
   * >= REQUEUE_GAP cards; `dueUnix` is the intraday due the answer actually
   * scheduled, so a card on the 10-minute learning step does not come back
   * three cards later just because three cards went past. The card gap alone
   * was the bug behind "the first card keeps coming back": Anki brings a
   * learning card back when its STEP TIMER expires, and only pulls it forward
   * early through learn-ahead once the session has nothing else to show --
   * which is exactly what advanceToNextCard()'s third branch is.
   *
   * `dueUnix` is meaningless in cram (nothing is scheduled and there may be no
   * clock at all), and the gate is skipped there: the card gap alone governs,
   * as it always did.
   */
  struct PendingCard {
    uint32_t dueUnix;
    flashcards::Ordinal ordinal;
    uint8_t showAfter;
  };

  /**
   * One-deep undo. It restores the whole answer, not just the record: the
   * pre-answer CardState, the two daily counters, where the queue had got to,
   * the resurface list and the session tallies. Snapshotting the pending list
   * wholesale (512 bytes) is what makes the restore exact -- the answer may have
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
  /** Parks the current card for later; `dueUnix` is its scheduled intraday due (0 in cram). */
  void requeueCurrent(uint32_t dueUnix);
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
  /**
   * The card body as it is DRAWN right now: the grade band is subtracted only
   * once the answer is showing, because only then is anything drawn there.
   */
  BodyArea bodyArea() const { return bodyArea(revealed); }
  /**
   * The card body with the grade band explicitly present or absent.
   *
   * `bodyArea(true)` is the PAGING geometry, and the wrap uses it in both
   * states. Sizing pages by the visible body instead would give the question
   * side taller pages than the answer side, so revealing would re-flow the front
   * under the reader: the same front would occupy different pages, and a reader
   * who had paged to the middle of a long question would be thrown somewhere
   * else by the reveal. Laying both states out against the shorter (revealed)
   * body costs the question screen a few rows of unused space at the bottom —
   * where the grade band is about to appear anyway — and buys a front that pages
   * identically either side of the reveal.
   */
  BodyArea bodyArea(bool withGradeBand) const;
  int gradeRowTop() const;
  int measureSpan(const char* text, size_t length) const;
  /**
   * Wraps one side into at most `lineLimit` total lines; sets `overflowed` if it
   * did not fit.
   *
   * `images` / `imageCount` are that side's placements, in text order, and
   * `imageSlot` is where the first of them sits in `cardImages`. They are
   * emitted as the wrap passes their `textOffset`, so an image counts against
   * exactly the same line budget the text does.
   */
  void appendWrapped(const char* text, uint16_t length, LineKind kind, int maxWidth, uint16_t lineLimit,
                     bool& overflowed, const flashcards::DeckImagePlacement* images, uint8_t imageCount,
                     uint8_t imageSlot);
  /** Appends one line slot if the budget allows; sets `overflowed` when it does not. */
  bool appendLine(const CardLine& line, uint16_t lineLimit, bool& overflowed);
  /**
   * Appends the image block for `slot`: page padding if it would straddle a page
   * break, then its own line plus its blank fillers.
   */
  void appendImage(uint8_t slot, uint16_t lineLimit, bool& overflowed);
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
  /**
   * Not const, unlike its three siblings: a card image is decoded WHEN THE PAGE
   * CARRYING IT PAINTS (FLASHCARD_SPEC.md §2.2), which means reading its encoded
   * bytes off the card and running the decoder from inside the render. Nothing
   * here allocates — the buffer and the decoder were claimed at session start —
   * but both are mutated, and saying so is better than a mutable member.
   */
  void renderReview();
  /** Draws one image into its reserved box, or a placeholder if anything refuses. */
  void drawCardImage(const CardLine& line, const BodyArea& body, int y, int lineHeight);
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
  // The current card's images: front placements first, then back. Filled by
  // loadCurrentCard() while `haveCard` is false, so the render task never reads
  // it half-written, and read again only by the wrap and the draw.
  flashcards::DeckImagePlacement cardImages[MAX_CARD_IMAGES] = {};
  uint8_t frontImageCount = 0;
  uint8_t backImageCount = 0;
  // One encoded-JPEG buffer for the whole session, sized to the deck's largest
  // image (DeckFile::maxImageBytes()) and claimed only for a deck that has any.
  // A deck without images pays nothing, and the render path never allocates.
  std::unique_ptr<uint8_t[]> imageBytes;
  uint32_t imageBytesCapacity = 0;
  flashcards::CardImageDecoder imageDecoder;
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
