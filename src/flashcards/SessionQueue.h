#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <FsrsSched.h>

#include <cstdint>
#include <memory>

#include "DeckFile.h"
#include "StateStore.h"

/**
 * Turns a deck's state file into the ordinals a study session will show, in the
 * order it will show them (FLASHCARD_SPEC.md §5).
 *
 * One sequential pass over the records, selecting into three bounded queues:
 *
 *   - **due reviews** — `state == Review` and `due <= today`, at most
 *     `200 - rev_today` of them, the most overdue first (due day, then ordinal);
 *   - **new cards** — deck order, at most `20 - new_today`;
 *   - **intraday learning** — `state ∈ {Learning, Relearning}` whose unix due
 *     has arrived. Cards due within the next 20 minutes are pulled forward ONLY
 *     when nothing else is left to study (Anki's learn-ahead), so a session
 *     never sits waiting on a 10-minute step it could have filled with reviews.
 *     That decision is made again at DRAIN time by nextLearnAheadOrdinal()
 *     below — a session that empties out mid-way must not need a full rebuild
 *     to notice a card that has since come inside the window.
 *
 * Suspended cards are skipped everywhere. Learning cards go at the front (they
 * are the time-critical ones); reviews and new cards are then interleaved by an
 * even Bresenham spread — with R reviews and N new cards, slot `i` takes a new
 * card when `(taken + 1) * (R + N) <= (i + 1) * N`, which puts one new card
 * every `(R+N)/N` slots and is a pure function of the two counts, so a session
 * rebuilt from the same state comes out identical. Note the shape that comes
 * out: each new card lands at the END of its block of reviews, so a session
 * OPENS on a review and CLOSES on a new card (4 reviews + 2 new gives
 * R R N R R N). That is the intended spread, not an off-by-one.
 *
 * **Nothing here allocates through a container that can abort.** The queue is
 * bounded by construction in the scheduled modes — 200 reviews + 20 new + 200
 * learning, so 420 u16 ordinals, 840 bytes — but `CramAll` is unbounded: every
 * non-suspended ordinal in deck order, 80 KB at the 40000-card cap. A vector
 * reserve of that size on a device with `-fno-exceptions` is a panic, not a
 * failure, so every buffer on this path is a null-checked makeUniqueNoThrow and
 * buildSession() returns false when one does not fit. Per §5, CramAll must also
 * be treated as read-only for scheduling by the caller — nothing here writes
 * state.
 *
 * **Under CROSSPOINT_FLASHCARDS_C3** (FLASHCARD_SPEC.md §7b.3) the deck cap is
 * 2000, so that same CramAll buffer is 4 KB rather than 80 KB and the scheduled
 * modes are unchanged at 840 bytes; nothing here needed a different shape, only
 * a smaller `recordCount()`. What the flag DOES add is a largest-free-block
 * gate in front of each of those allocations — 4 KB is not much, but it is one
 * contiguous block on a heap a reading session leaves near 50 KB and
 * fragmented, and the gate turns "the reader panicked" into "the study screen
 * said it was out of memory". The refusal surfaces exactly as it already did:
 * buildSession() returns false.
 */
namespace flashcards {

/** Fixed v1 policy (FLASHCARD_SPEC.md §0.4): no per-deck tunables. */
constexpr uint16_t DAILY_NEW_LIMIT = 20;
constexpr uint16_t DAILY_REVIEW_LIMIT = 200;
constexpr uint32_t LEARN_AHEAD_SECS = 1200;
/**
 * Learning cards a single session will carry. Not in the spec — the spec caps
 * reviews and new cards but leaves the learning queue open, and this layer
 * needs a bound to keep the session buffer a fixed size. Mirroring the review
 * cap is the least surprising choice; anything past it comes up in the next
 * session, which is where it would have landed anyway.
 */
constexpr uint16_t SESSION_LEARNING_LIMIT = 200;

enum class SessionMode : uint8_t {
  Due,      // the default: due reviews + new cards + intraday learning
  NewOnly,  // new cards only, still inside the daily new allowance
  CramAll,  // every non-suspended card, deck order, scheduling untouched
};

/**
 * A fixed-capacity list of ordinals on the heap: `std::vector` with the one
 * behaviour this firmware cannot have removed.
 *
 * `reserve()` is the only thing that allocates, it goes through
 * makeUniqueNoThrow, and it returns false rather than aborting when the block
 * does not fit. `push()` never grows — a full buffer refuses the ordinal — so
 * no operation after reserve() can allocate, fail unexpectedly, or move the
 * elements out from under a caller holding a pointer.
 */
class OrdinalBuffer {
 public:
  OrdinalBuffer() = default;
  OrdinalBuffer(const OrdinalBuffer&) = delete;
  OrdinalBuffer& operator=(const OrdinalBuffer&) = delete;

  /** Makes room for `capacity` ordinals and empties the buffer. False on OOM. */
  bool reserve(uint32_t capacity);
  void clear() { countValue = 0; }
  /** Appends unless the buffer is already at capacity. */
  bool push(Ordinal ordinal);

  uint32_t size() const { return countValue; }
  uint32_t capacity() const { return capacityValue; }
  bool empty() const { return countValue == 0; }
  Ordinal operator[](uint32_t index) const { return ordinals[index]; }
  const Ordinal* begin() const { return ordinals.get(); }
  const Ordinal* end() const { return ordinals.get() + countValue; }
  bool sameContents(const OrdinalBuffer& other) const;

 private:
  std::unique_ptr<Ordinal[]> ordinals;
  uint32_t capacityValue = 0;
  uint32_t countValue = 0;
};

/**
 * What the session is made of, for the picker screen and the done screen.
 *
 * `reviews`/`newCards`/`learning` describe the queue that was built;
 * `dueAvailable`/`newAvailable` describe what the deck holds regardless of the
 * daily caps, which is what a "45 due, 200/day reached" line needs.
 */
struct SessionSummary {
  uint16_t reviews = 0;
  uint16_t newCards = 0;
  uint16_t learning = 0;
  uint16_t dueAvailable = 0;
  uint16_t newAvailable = 0;
  uint16_t suspended = 0;
  /**
   * Earliest intraday due strictly after `now`, or 0 when no card is waiting on
   * the clock. This is a property of the DECK, not of what was left out: a
   * learning card pulled into this session by learn-ahead still has a future
   * due and is still counted here, so a non-zero value does not by itself mean
   * there is anything to come back for.
   */
  uint32_t nextDueUnix = 0;
};

struct Session {
  OrdinalBuffer cards;
  SessionSummary summary;
};

/**
 * Builds `out` from `store`. Returns false when the scan itself failed (an
 * unreadable state file) or when the session buffer would not fit in RAM
 * (CramAll on a huge deck); an empty queue with a filled summary is a
 * successful "nothing due" answer.
 *
 * The learn-ahead cards it includes are the ones inside the window AT BUILD
 * TIME. The study screen is expected to call nextLearnAheadOrdinal() when it
 * drains this queue rather than rebuilding (FLASHCARD_SPEC.md §5).
 */
bool buildSession(StateStore& store, SessionMode mode, fsrs::Now now, Session& out);

/**
 * The learning card the session should pull forward next: the earliest
 * Learning/Relearning ordinal due at or before `now + LEARN_AHEAD_SECS`,
 * suspended cards excluded, ties broken by ordinal.
 *
 * This is the DRAIN-time half of learn-ahead. When the built queue empties, the
 * study screen calls this instead of rebuilding the session: one sequential
 * scan, no allocation, no daily-counter arithmetic. A card that was 25 minutes
 * out when the session started, and is 5 minutes out by the time the queue is
 * empty, is picked up here.
 *
 * Callers must skip an ordinal they have already shown and re-answered — this
 * looks only at what is on disk, and a card answered Again is legitimately due
 * again within the window.
 *
 * Returns false when nothing is inside the window (or the scan failed, which is
 * indistinguishable and equally means "show the done screen").
 */
bool nextLearnAheadOrdinal(StateStore& store, fsrs::Now now, Ordinal& ordinalOut, uint32_t& dueUnixOut);

/**
 * Earliest intraday due strictly after `afterUnix`, for the "next card at
 * HH:MM" line on the done screen. False when no learning card is waiting.
 *
 * A second pass over the records rather than a cached value: by the time the
 * session ends every due has moved, and the pass is one sequential read.
 */
bool nextIntradayDue(StateStore& store, uint32_t afterUnix, uint32_t& dueUnixOut);

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
