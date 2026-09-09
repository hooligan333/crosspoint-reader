#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <FsrsSched.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "DeckFile.h"

/**
 * Scheduling state for one deck: the `CPST` v1 file at
 * `<destFolder>/.state/<leaf>.deck.state` (FLASHCARD_SPEC.md §3).
 *
 * The file is fixed-size records aligned to the deck's ordinals, and it is
 * updated **in place**: one 28-byte seek+write per answered card and one
 * 64-byte header write for the daily counters, never a whole-file rewrite. The
 * accepted risk is written into the spec — a torn sector loses ~18 cards'
 * scheduling state (a 512-byte sector spans 18.3 records); there is no journal
 * in v1. The 448 bytes of reserved padding between the header and record 0 buy
 * the one mitigation that is free: the header is rewritten after every single
 * answer, and without the padding it would share its sector with records 0-15,
 * concentrating that exposure ~200x on the first sixteen cards.
 *
 * `open()` does all the reconciliation the study screen would otherwise have to
 * know about:
 *   - an orphaned `.tmp` (the file is missing but a merge temp is beside it) →
 *     rename it in first: a power cut inside the merge's remove→rename window
 *     leaves the COMPLETE replacement on the card, and the whole point of the
 *     merge is not to throw a deck's history away. A torn `.tmp` simply fails
 *     the header checks below and falls through to a fresh file, exactly as a
 *     torn `.state` does;
 *   - no file yet → create the `.state` directory and initialise every record
 *     as New from the deck's own keys;
 *   - `deck_content_hash` moved (the deck was re-downloaded, §1 staleness) →
 *     run the merge: old records keyed by card key are carried onto the new
 *     ordinals, cards the new deck added start New, cards it dropped are gone,
 *     and the result replaces the file via temp+rename;
 *   - a new local day → zero the daily counters, with the clock-jump guard
 *     below.
 *
 * **Clock-jump guard.** `last_seen_day` is a high-water mark. The effective day
 * is `max(today, last_seen_day)`, so a clock that jumps backwards leaves the
 * counters exactly where they were: it cannot hand out a second 20-new /
 * 200-review allowance for a day already spent. A jump forwards does roll over,
 * which is indistinguishable from a real new day and matches Anki.
 */
namespace flashcards {

/** The header proper. The bytes after it up to CPST_RECORDS_OFFSET are reserved = 0. */
constexpr size_t CPST_HEADER_BYTES = 64;
/**
 * Where record 0 starts (FLASHCARD_SPEC.md §3). NOT the header size: the gap is
 * deliberate padding so the every-answer header write never lands in the same
 * 512-byte sector as a card's scheduling state.
 */
constexpr size_t CPST_RECORDS_OFFSET = 512;
constexpr size_t CPST_RECORD_BYTES = 28;
constexpr uint16_t CPST_VERSION = 1;
/**
 * The FORMAT's record cap, which is not the same thing as a build's deck cap.
 * The CPST file the C3 finds on the card may have been written by a Pro — an SD
 * moved between devices is the ordinary case — and it can then hold up to the
 * Pro's 40000 records. The reading build's own deck cap bounds the records it
 * WRITES; this bounds the ones it is willing to READ on the old side of a merge
 * (FLASHCARD_SPEC.md §7b.3 pin d). One ordinal per record still fits a u16.
 */
constexpr uint32_t CPST_MAX_RECORDS = 40000;

static_assert(CPST_RECORDS_OFFSET >= CPST_HEADER_BYTES, "records must start after the header");

/** Why the state file could not be brought up to date. */
enum class StateError : uint8_t {
  Ok,
  OpenFailed,    // the existing file will not open for read/write
  CreateFailed,  // the .state directory or the new file could not be created
  ReadFailed,
  WriteFailed,
  OutOfMemory,  // the merge buffers would not fit
};

const char* stateErrorName(StateError error);

/** Outcome of reading one record. */
enum class RecordStatus : uint8_t {
  Ok,
  IoError,
  /**
   * The record's key was not the deck's key for that ordinal, so the record was
   * HEALED: rewritten in place as a fresh New card with the correct key, and
   * `stateOut` holds that fresh state. Not an error — the caller may schedule
   * the card exactly as it would any New one. See readRecord().
   */
  KeyMismatch,
};

/** Which daily allowance an answer consumed. The study screen decides; the store just counts. */
enum class Counted : uint8_t { Nothing, NewCard, Review };

class StateStore {
 public:
  StateStore() = default;
  StateStore(const StateStore&) = delete;
  StateStore& operator=(const StateStore&) = delete;

  /**
   * Opens, creates or merges the state file for `deck`, then rolls the daily
   * counters to `today`. `deck` must stay open and alive for the lifetime of
   * this store: its keys are what every record is checked against.
   *
   * **Memory — the authoritative composed figure for this feature.** Creation
   * and the merge stream through a 3584-byte record chunk on the stack. The
   * merge additionally holds the OLD state, sorted by key: keys (8 B/card),
   * their ordinals (2 B/card) and their 20-byte payloads, i.e. 1.20 MB at the
   * 40000-card cap, all through makeUniqueNoThrow and all released before
   * open() returns. That merge runs with `deck` OPEN, so its resident 20-byte
   * card index (800 KB at the cap, DeckFile::open()) is live at the same time:
   *
   *     COMPOSED PEAK ~= 800 KB (deck index) + 1.20 MB (merge buffers) ~= 2.0 MB
   *
   * which is why this feature is S3/PSRAM-only (FLASHCARD_SPEC.md §0.5) and why
   * every one of those buffers is a null-checked makeUniqueNoThrow rather than
   * a vector. DeckFile's own 1.12 MB open-time peak (index + duplicate-key
   * scratch) does NOT overlap this one: the scratch is freed before open()
   * returns, long before a store is opened against the deck.
   *
   * **Under CROSSPOINT_FLASHCARDS_C3 the composed peak is ~24 KB** (§7b.3).
   * DeckFile holds nothing, so the only heap here is the merge's (key, ordinal)
   * pairs — 10 B/record, 20 KB for 2000 records, largest single block the key
   * array. The 20-byte payloads are NOT held: each matched card's old record
   * comes out of an 896-byte read-ahead window over the still-open old file
   * (§7b.3 pin c), which sits on the merge's frame beside the 3584-byte output
   * chunk. Each array is gated on gateMaxAllocHeap() for its OWN size,
   * immediately before it is claimed (pin e, the pattern DictZip and
   * RssSyncActivity use), and fails as StateError::OutOfMemory, never as an
   * abort. Note that the OLD side is bounded by CPST_MAX_RECORDS and not by
   * this build's deck cap (pin d): a state file written by a Pro is merged
   * whole when its pair buffers fit, and refused with the file untouched when
   * they do not. 40000 old records would want 400 KB and are refused here;
   * what the C3 actually meets is a few thousand. Creation, adoption and the
   * orphan-adopt path allocate nothing at all in either build: they stream
   * through the same 3584-byte stack chunk.
   */
  StateError open(const DeckFile& deck, const std::string& destFolder, const std::string& deckLeaf, uint16_t today);
  void close();
  bool isOpen() const { return recordCountValue != 0; }

  uint32_t recordCount() const { return recordCountValue; }
  uint16_t newToday() const { return newTodayValue; }
  uint16_t reviewsToday() const { return reviewsTodayValue; }
  /** The day the counters belong to — the high-water day, not necessarily the clock's today. */
  uint16_t countersDay() const { return countersDayValue; }
  /** Sticky: a record's key did not match the deck's, and was healed. Diagnostics only. */
  bool keyMismatchSeen() const { return mismatchSeen; }

  /**
   * Reads the record at `ordinal`. `stateOut` is zeroed (i.e. a New card) before
   * anything else happens, so it is never left holding the caller's stale value
   * on any return path.
   *
   * A record whose key is not the deck's key for that ordinal is a torn write.
   * It is HEALED rather than reported: the ordinal is rewritten in place as a
   * fresh New card carrying the deck's key, `stateOut` holds that fresh state,
   * and the return is RecordStatus::KeyMismatch — a diagnostic, not a failure.
   * The card re-enters the queues as new. Skipping it instead would hide it
   * from every session mode forever, which is strictly worse than restarting
   * one card (FLASHCARD_SPEC.md §3). IoError is returned only when the heal
   * itself could not be written, or the read failed.
   */
  RecordStatus readRecord(Ordinal ordinal, fsrs::CardState& stateOut);
  /** In-place 28-byte write, flushed. The key is rewritten from the deck, repairing a torn one. */
  bool writeRecord(Ordinal ordinal, const fsrs::CardState& state);
  /** In-place header write. Used by the undo path to put the counters back. */
  bool writeCounters(uint16_t newCards, uint16_t reviews);
  /**
   * Record + counters, the pair the spec requires after every answer, and ONE
   * flush: the record write skips its own sync when the header write that
   * follows will do it. The RAM counters are only bumped once that header is on
   * the card, and are rolled back if it is not, so RAM and disk never disagree.
   */
  bool commitAnswer(Ordinal ordinal, const fsrs::CardState& state, Counted counted);

  /**
   * Visits every record in ordinal order in one sequential pass. Returning
   * false from `visit` stops the scan early (and is not an error).
   *
   * Records whose key does not match the deck are HEALED and then visited, the
   * same contract readRecord() has: the ordinal is rewritten as a fresh New
   * card with the deck's key and visited with that state, so a torn key costs
   * one card its history instead of hiding it from every session mode.
   * `keyMismatchSeen()` reports that it happened.
   */
  using RecordVisitor = bool (*)(Ordinal ordinal, const fsrs::CardState& state, void* ctx);
  bool scanRecords(RecordVisitor visit, void* ctx);

 private:
  StateError createFresh(const std::string& path, uint16_t today);
  StateError mergeFromExisting(const std::string& path, uint32_t oldCount);
  /** Renames an orphaned complete `.tmp` into place when `path` itself is gone. */
  static void adoptOrphanedTemp(const std::string& path);
  bool writeHeader();
  /** Header + the 448 reserved bytes that push record 0 out to CPST_RECORDS_OFFSET. */
  bool writeHeaderAndPadding();
  /** The in-place record write; `flushNow` is false when a header write will sync anyway. */
  bool writeRecordAt(Ordinal ordinal, const fsrs::CardState& state, bool flushNow);
  /** Rewrites `ordinal` as a fresh New record with the deck's key. */
  bool healRecord(Ordinal ordinal, fsrs::CardState& stateOut);
  bool applyDayRollover(uint16_t today);

  const DeckFile* deck = nullptr;
  HalFile file;
  uint32_t recordCountValue = 0;
  uint16_t newTodayValue = 0;
  uint16_t reviewsTodayValue = 0;
  uint16_t countersDayValue = 0;
  uint16_t lastSeenDayValue = 0;
  bool mismatchSeen = false;
};

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
