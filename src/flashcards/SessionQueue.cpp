#include "SessionQueue.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <Logging.h>
#include <Memory.h>

#ifdef CROSSPOINT_FLASHCARDS_C3
#include <HalHeapGauge.h>  // gateMaxAllocHeap() for the session buffers' pre-alloc guard
#endif

namespace flashcards {
namespace {

#ifdef CROSSPOINT_FLASHCARDS_C3
/**
 * Slack a session buffer must leave behind, matching DictZip and
 * RssSyncActivity. The three asks here are small even at the C3's 2000-card
 * cap — 4 KB for a Cram-all ordinal list, 1600 B for the review picker (200
 * picks x 8 B) and 160 B for the learning picker — but "small" is not the same
 * as "available" on a heap a reading session leaves near 50 KB and fragmented,
 * and the point of the gate is that the study screen gets a refusal it can
 * render instead of a session that half-builds.
 *
 * Each of the three is gated for ITS OWN size at the moment it is claimed
 * (FLASHCARD_SPEC.md §7b.3 pin e), never once for the largest: the claims are
 * sequential, so every one of them faces a heap the previous claim has already
 * changed.
 */
constexpr size_t SESSION_HEAP_HEADROOM_BYTES = 1024;

/** True when `bytes` looks claimable right now; logs and returns false when it does not. */
bool sessionHeapAllows(size_t bytes, const char* what) {
  if (gateMaxAllocHeap() >= bytes + SESSION_HEAP_HEADROOM_BYTES) return true;
  LOG_ERR("DECK", "Low heap for the session %s: %u max block (need %u)", what,
          static_cast<unsigned>(gateMaxAllocHeap()), static_cast<unsigned>(bytes + SESSION_HEAP_HEADROOM_BYTES));
  return false;
}
#endif

/** One selected card, ordered by (sortKey, ordinal) — due day for reviews, unix due for learning. */
struct Pick {
  uint32_t sortKey;
  Ordinal ordinal;
};

bool picksBefore(const Pick& a, const Pick& b) {
  if (a.sortKey != b.sortKey) return a.sortKey < b.sortKey;
  return a.ordinal < b.ordinal;
}

/**
 * Keeps the `capacity` smallest picks out of an arbitrarily long stream, as a
 * max-heap so the worst one is always at [0] and can be dropped in O(log n).
 *
 * This is what lets the queue build stay one sequential scan with a fixed
 * memory bound: 200 picks is 1.6 KB, whereas collecting every due card of a
 * 40000-card deck before sorting would be 320 KB and a second pass. The one
 * allocation is a null-checked makeUniqueNoThrow, so a deck that will not fit
 * fails the build rather than aborting the firmware.
 */
class BoundedPicker {
 public:
  /** Allocates room for `capacity` picks. False on OOM; a zero capacity always succeeds. */
  bool reset(uint16_t capacity) {
    limit = capacity;
    count = 0;
    picks.reset();
    if (capacity == 0) return true;
#ifdef CROSSPOINT_FLASHCARDS_C3
    if (!sessionHeapAllows(static_cast<size_t>(capacity) * sizeof(Pick), "picker")) {
      limit = 0;
      return false;
    }
#endif
    picks = makeUniqueNoThrow<Pick[]>(capacity);
    if (!picks) {
      LOG_ERR("DECK", "Session picker alloc failed for %u picks", static_cast<unsigned>(capacity));
      limit = 0;
      return false;
    }
    return true;
  }

  void offer(uint32_t sortKey, Ordinal ordinal) {
    if (limit == 0) return;
    const Pick pick{sortKey, ordinal};
    if (count < limit) {
      picks[count] = pick;
      siftUp(count);
      count++;
      return;
    }
    if (!picksBefore(pick, picks[0])) return;  // no better than the worst we hold
    picks[0] = pick;
    siftDown(0, count);
  }

  /** Heapsort in place: after this the picks are ascending, best first. */
  void finish() {
    for (size_t end = count; end > 1; end--) {
      const Pick top = picks[0];
      picks[0] = picks[end - 1];
      picks[end - 1] = top;
      siftDown(0, end - 1);
    }
  }

  size_t size() const { return count; }
  const Pick& operator[](size_t i) const { return picks[i]; }

 private:
  void siftUp(size_t node) {
    while (node > 0) {
      const size_t parent = (node - 1) / 2;
      if (!picksBefore(picks[parent], picks[node])) return;
      const Pick swap = picks[parent];
      picks[parent] = picks[node];
      picks[node] = swap;
      node = parent;
    }
  }

  void siftDown(size_t root, size_t size) {
    while (true) {
      const size_t left = 2 * root + 1;
      if (left >= size) return;
      size_t largest = left;
      const size_t right = left + 1;
      if (right < size && picksBefore(picks[left], picks[right])) largest = right;
      if (!picksBefore(picks[root], picks[largest])) return;
      const Pick swap = picks[root];
      picks[root] = picks[largest];
      picks[largest] = swap;
      root = largest;
    }
  }

  std::unique_ptr<Pick[]> picks;
  size_t limit = 0;  // capacity, i.e. how many picks are kept
  size_t count = 0;
};

struct Builder {
  SessionMode mode = SessionMode::Due;
  fsrs::Now now{};
  uint16_t newLimit = 0;
  BoundedPicker reviews;
  BoundedPicker learning;
  // The new-card queue is capped by policy at DAILY_NEW_LIMIT, so it is a plain
  // 40-byte array rather than a fourth allocation.
  Ordinal newCards[DAILY_NEW_LIMIT] = {};
  uint16_t newCount = 0;
  OrdinalBuffer* cram = nullptr;
  SessionSummary summary;
  uint16_t phaseTotals[4] = {0, 0, 0, 0};
};

bool visitRecord(Ordinal ordinal, const fsrs::CardState& state, void* ctx) {
  Builder& builder = *static_cast<Builder*>(ctx);

  if ((state.flags & fsrs::FLAG_SUSPENDED) != 0) {
    builder.summary.suspended++;
    return true;
  }
  // The cram buffer was reserved at store.recordCount() before the scan, and
  // the scan visits each ordinal once, so this cannot fill up. If it somehow
  // did, stopping the scan yields a short session — never a write past the end.
  if (builder.mode == SessionMode::CramAll && !builder.cram->push(ordinal)) return false;
  // StateStore normalizes an unknown state byte to New before it gets here; the
  // bound is belt and braces on an array index, not a second opinion.
  const uint8_t phase = static_cast<uint8_t>(state.state);
  if (phase < 4) builder.phaseTotals[phase]++;

  // The statistics and the three queues are collected in every mode: the mode
  // only decides which of them the assembly below actually uses, and the picker
  // screen wants the numbers either way.
  switch (state.state) {
    case fsrs::CardPhase::New:
      builder.summary.newAvailable++;
      if (builder.newCount < builder.newLimit) builder.newCards[builder.newCount++] = ordinal;
      break;
    case fsrs::CardPhase::Review:
      // due is a local day number in this state (spec §3).
      if (state.due <= builder.now.dayNumber) {
        builder.summary.dueAvailable++;
        builder.reviews.offer(state.due, ordinal);
      }
      break;
    case fsrs::CardPhase::Learning:
    case fsrs::CardPhase::Relearning:
      // due is unix seconds in these states.
      if (state.due <= builder.now.unixSecs + LEARN_AHEAD_SECS) builder.learning.offer(state.due, ordinal);
      if (state.due > builder.now.unixSecs &&
          (builder.summary.nextDueUnix == 0 || state.due < builder.summary.nextDueUnix)) {
        builder.summary.nextDueUnix = state.due;
      }
      break;
  }
  return true;
}

/** Shared context for the two single-purpose scans below. */
struct DueScan {
  uint32_t after = 0;     // strictly-after bound (nextIntradayDue)
  uint32_t notAfter = 0;  // inclusive upper bound (nextLearnAheadOrdinal)
  uint32_t earliest = 0;  // best due found so far, 0 = none
  Ordinal ordinal = 0;    // its ordinal
  bool found = false;
};

/** True for a card whose `due` is intraday unix seconds and which is studiable. */
bool isIntraday(const fsrs::CardState& state) {
  if ((state.flags & fsrs::FLAG_SUSPENDED) != 0) return false;
  switch (state.state) {
    case fsrs::CardPhase::New:
    case fsrs::CardPhase::Review:
      return false;
    case fsrs::CardPhase::Learning:
    case fsrs::CardPhase::Relearning:
      return true;
  }
  return false;
}

bool visitForNextDue(Ordinal ordinal, const fsrs::CardState& state, void* ctx) {
  (void)ordinal;
  DueScan& scan = *static_cast<DueScan*>(ctx);
  if (!isIntraday(state)) return true;
  if (state.due <= scan.after) return true;
  if (!scan.found || state.due < scan.earliest) {
    scan.earliest = state.due;
    scan.found = true;
  }
  return true;
}

bool visitForLearnAhead(Ordinal ordinal, const fsrs::CardState& state, void* ctx) {
  DueScan& scan = *static_cast<DueScan*>(ctx);
  if (!isIntraday(state)) return true;
  if (state.due > scan.notAfter) return true;
  // Ties go to the lower ordinal, which the ordinal-order scan gives for free.
  if (!scan.found || state.due < scan.earliest) {
    scan.earliest = state.due;
    scan.ordinal = ordinal;
    scan.found = true;
  }
  return true;
}

uint16_t remaining(uint16_t limit, uint16_t used) { return used >= limit ? 0 : static_cast<uint16_t>(limit - used); }

}  // namespace

bool OrdinalBuffer::reserve(uint32_t capacity) {
  countValue = 0;
  if (capacity <= capacityValue) return true;  // an existing block that is big enough is kept
#ifdef CROSSPOINT_FLASHCARDS_C3
  if (!sessionHeapAllows(static_cast<size_t>(capacity) * sizeof(Ordinal), "ordinal buffer")) {
    capacityValue = 0;
    ordinals.reset();
    return false;
  }
#endif
  ordinals = makeUniqueNoThrow<Ordinal[]>(capacity);
  if (!ordinals) {
    capacityValue = 0;
    LOG_ERR("DECK", "Session buffer alloc failed: %u ordinals", static_cast<unsigned>(capacity));
    return false;
  }
  capacityValue = capacity;
  return true;
}

bool OrdinalBuffer::push(Ordinal ordinal) {
  if (countValue >= capacityValue) return false;
  ordinals[countValue++] = ordinal;
  return true;
}

bool OrdinalBuffer::sameContents(const OrdinalBuffer& other) const {
  if (countValue != other.countValue) return false;
  for (uint32_t i = 0; i < countValue; i++) {
    if (ordinals[i] != other.ordinals[i]) return false;
  }
  return true;
}

bool buildSession(StateStore& store, SessionMode mode, fsrs::Now now, Session& out) {
  out.cards.clear();
  out.summary = SessionSummary();

  Builder builder;
  builder.mode = mode;
  builder.now = now;
  builder.newLimit = remaining(DAILY_NEW_LIMIT, store.newToday());
  const uint16_t reviewLimit = remaining(DAILY_REVIEW_LIMIT, store.reviewsToday());
  if (!builder.reviews.reset(reviewLimit)) return false;
  if (!builder.learning.reset(SESSION_LEARNING_LIMIT)) return false;

  // Every buffer this session can need is claimed up front, before a single
  // record is read: the scan below must not be able to run out of room halfway.
  uint32_t capacity = 0;
  switch (mode) {
    case SessionMode::CramAll:
      // 80 KB at the Pro's 40000-card cap; 4 KB at the C3's 2000 (§7b.3).
      capacity = store.recordCount();
      break;
    case SessionMode::NewOnly:
      capacity = builder.newLimit;
      break;
    case SessionMode::Due:
      capacity = static_cast<uint32_t>(reviewLimit) + builder.newLimit + SESSION_LEARNING_LIMIT;
      break;
  }
  if (!out.cards.reserve(capacity)) return false;
  if (mode == SessionMode::CramAll) builder.cram = &out.cards;

  if (!store.scanRecords(&visitRecord, &builder)) {
    LOG_ERR("DECK", "State scan failed; no session built");
    return false;
  }

  builder.reviews.finish();
  builder.learning.finish();
  out.summary = builder.summary;

  switch (mode) {
    case SessionMode::CramAll:
      // out.cards was filled in deck order by the visitor.
      out.summary.newCards = builder.phaseTotals[static_cast<uint8_t>(fsrs::CardPhase::New)];
      out.summary.reviews = builder.phaseTotals[static_cast<uint8_t>(fsrs::CardPhase::Review)];
      out.summary.learning =
          static_cast<uint16_t>(builder.phaseTotals[static_cast<uint8_t>(fsrs::CardPhase::Learning)] +
                                builder.phaseTotals[static_cast<uint8_t>(fsrs::CardPhase::Relearning)]);
      break;

    case SessionMode::NewOnly:
      for (uint16_t i = 0; i < builder.newCount; i++) out.cards.push(builder.newCards[i]);
      out.summary.newCards = builder.newCount;
      break;

    case SessionMode::Due: {
      // Learning cards whose due has actually arrived; the rest of the picker
      // holds cards inside the 20-minute learn-ahead window.
      size_t dueNow = 0;
      while (dueNow < builder.learning.size() && builder.learning[dueNow].sortKey <= now.unixSecs) dueNow++;
      const bool nothingElse = builder.reviews.size() == 0 && builder.newCount == 0 && dueNow == 0;
      const size_t takeLearning = nothingElse ? builder.learning.size() : dueNow;

      const size_t reviewCount = builder.reviews.size();
      const size_t newCount = builder.newCount;
      for (size_t i = 0; i < takeLearning; i++) out.cards.push(builder.learning[i].ordinal);

      // Even Bresenham spread of the new cards through the reviews. The test is
      // "has the new-card share of the slots consumed so far caught up?", which
      // fires at the END of each block — 4 reviews and 2 new cards come out
      // R R N R R N, opening on a review and closing on a new card.
      const size_t total = reviewCount + newCount;
      size_t reviewsTaken = 0;
      size_t newTaken = 0;
      for (size_t slot = 0; slot < total; slot++) {
        const bool takeNew =
            newTaken < newCount && (reviewsTaken >= reviewCount || (newTaken + 1) * total <= (slot + 1) * newCount);
        if (takeNew) {
          out.cards.push(builder.newCards[newTaken++]);
        } else {
          out.cards.push(builder.reviews[reviewsTaken++].ordinal);
        }
      }
      out.summary.reviews = static_cast<uint16_t>(reviewCount);
      out.summary.newCards = static_cast<uint16_t>(newCount);
      out.summary.learning = static_cast<uint16_t>(takeLearning);
      break;
    }
  }
  return true;
}

bool nextLearnAheadOrdinal(StateStore& store, fsrs::Now now, Ordinal& ordinalOut, uint32_t& dueUnixOut) {
  DueScan scan;
  scan.notAfter = now.unixSecs + LEARN_AHEAD_SECS;
  if (!store.scanRecords(&visitForLearnAhead, &scan)) return false;
  if (!scan.found) return false;
  ordinalOut = scan.ordinal;
  dueUnixOut = scan.earliest;
  return true;
}

bool nextIntradayDue(StateStore& store, uint32_t afterUnix, uint32_t& dueUnixOut) {
  DueScan scan;
  scan.after = afterUnix;
  if (!store.scanRecords(&visitForNextDue, &scan)) return false;
  if (!scan.found) return false;
  dueUnixOut = scan.earliest;
  return true;
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
