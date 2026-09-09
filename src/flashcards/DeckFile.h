#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <FsrsSched.h>
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

/**
 * Reader for a `CPDK` v1 deck file (FLASHCARD_SPEC.md §2, byte layout in
 * `deck-server/DECK_SERVER_SPEC.md` §3).
 *
 * Two jobs, in this order:
 *
 * 1. **Refuse a deck this build cannot study.** The sync activity already
 *    refused the shallow header grounds before renaming the download into
 *    place (bad magic, wrong version, unknown flags bit, impossible card count,
 *    file shorter than its own index). This layer re-checks all of that — the
 *    file on the card may predate the sync screen, or have been copied over by
 *    USB — and then goes deeper: every index slice must land inside the text
 *    blob and be within the 4096-byte cap, keys must be unique, and a
 *    deck-supplied FSRS parameter block must pass `fsrs::validateParams()`.
 *    Failure is a typed `DeckError`, not a bool: the study screen shows why,
 *    and the file stays on disk for the server to correct (§2.1).
 *
 * 2. **Serve cards by ordinal.** The 20-byte card index is held in RAM (see
 *    the memory note on `open()`); card text is never held — `loadSide()`
 *    seeks and reads the one slice asked for, straight into the caller's
 *    buffer. `back_len == 0` is legal and reads back as an empty string.
 *
 * Nothing in CPDK is 8-aligned, so every multi-byte field is `memcpy`'d into a
 * local rather than read through a cast pointer (DECK_SERVER_SPEC.md §3.6.1).
 *
 * ---------------------------------------------------------------------------
 * **CROSSPOINT_FLASHCARDS_C3 — the small-memory variant** (FLASHCARD_SPEC.md
 * §7b.3). The original X4 is an ESP32-C3: ~380 KB RAM, no PSRAM, a free heap
 * that a reading session leaves near 50 KB and fragmented. The resident index
 * above is not affordable there — 800 KB at the Pro's cap, and even a 2000-card
 * deck would want a single contiguous 40 KB block. So under the flag:
 *
 *   - the deck cap drops to `DECK_MAX_CARDS` = 2000 (a deck above it is refused
 *     with DeckError::BadCardCount, the same typed refusal the Pro gives a
 *     40001-card deck — see the note on that enumerator);
 *   - there is NO resident index. `keyAt()` and `loadSide()` seek and read the
 *     20-byte entry they need, through a small aligned window cache that makes
 *     an ordinal-order walk (state creation, merge) one read per 32 cards
 *     instead of one per card. The whole reader allocates NOTHING on the heap;
 *   - `open()` still validates every index entry, by STREAMING the index in
 *     chunks that stay under 4 KB. No whole-index buffer exists at any point;
 *   - **the duplicate-key check is SKIPPED.** This is a deliberate trust
 *     boundary, documented here because it is the one behavioural divergence
 *     between the two builds. The check costs a sorted copy of every key
 *     (16 KB at the C3 cap, in one contiguous block) to catch a fault that
 *     TWO upstream stages already refuse: `convert_deck.py` derives keys from
 *     (guid, ord) pairs that Anki itself keys uniquely and asserts uniqueness
 *     before writing, and `read_deck.py` — the verifier the deck server runs
 *     over every file it publishes — refuses a deck with a repeated key. What
 *     remains if a duplicate somehow reaches the device is bounded and not
 *     silent: CPST records are addressed by ORDINAL, not by key, so two cards
 *     sharing a key still get their own record; the per-record key echo
 *     (StateStore::readRecord) still checks each ordinal against the deck's own
 *     key for that ordinal, so a duplicate cannot make one card read another's
 *     state; and a re-download merge would carry the same old record onto both
 *     ordinals, i.e. one card's history is copied rather than lost. That is the
 *     accepted worst case. The Pro keeps the check, so a deck that would hit it
 *     is still caught on any device with the RAM to look.
 */
namespace flashcards {

/** Card ordinals are u16 throughout: DECK_MAX_CARDS fits with room to spare. */
using Ordinal = uint16_t;

#ifdef CROSSPOINT_FLASHCARDS_C3
/**
 * 2000 cards on the C3 (FLASHCARD_SPEC.md §7b.3). The binding numbers are the
 * ones this cap sizes: the state file's ordinal buffer for a Cram-all session
 * (2 B/card = 4 KB) and the merge's (key, ordinal) pairs (10 B/card = 20 KB, of
 * which the largest single block is the 16 KB key array). At 40000 those become
 * 80 KB and 400 KB, neither of which this device can hand out.
 */
constexpr uint32_t DECK_MAX_CARDS = 2000;
#else
constexpr uint32_t DECK_MAX_CARDS = 40000;
#endif
constexpr uint16_t DECK_MAX_SLICE_BYTES = 4096;
constexpr uint8_t DECK_TITLE_MAX_BYTES = 39;

/** Why a deck would not open. Ok is the only value that leaves it usable. */
enum class DeckError : uint8_t {
  Ok,
  OpenFailed,       // the file will not open at all
  ShortFile,        // shorter than its header, or than its own index end
  BadMagic,         // not "CPDK"
  BadVersion,       // format_version != 1
  UnknownFlags,     // a reserved flags bit is set — the version escape hatch
  BadCardCount,     // 0, or above DECK_MAX_CARDS (40000; 2000 on the C3 variant)
  ReadFailed,       // a read came up short mid-file
  BadParams,        // deck-supplied FSRS weights fail validateParams()
  SliceOutOfRange,  // an index entry points outside the text blob, or is > 4096 B
  DuplicateKey,     // two cards share a key: state could not be kept per card
  OutOfMemory,      // the card index would not fit
};

/** Log-friendly name for a DeckError. User-facing text is the study screen's job. */
const char* deckErrorName(DeckError error);

enum class CardSide : uint8_t { Front, Back };

class DeckFile {
 public:
  DeckFile() = default;
  DeckFile(const DeckFile&) = delete;
  DeckFile& operator=(const DeckFile&) = delete;

  /**
   * Opens and fully validates `path`. On anything but DeckError::Ok the object
   * is left closed and every accessor below reads as an empty deck.
   *
   * Memory: the card index is held as its raw 20 bytes per card — 800 KB at the
   * 40000-card cap, and 1.12 MB peak while the duplicate-key check holds its
   * sorted scratch copy of the keys. Both go through makeUniqueNoThrow (null
   * checked, DeckError::OutOfMemory) and both land in PSRAM on this S3-only
   * feature's build, where allocations of 4 KB and up do (CONFIG_SPIRAM_MALLOC_
   * ALWAYSINTERNAL=4096). Keeping the index resident is what makes keyAt() free
   * for the queue build and the re-download merge, which both walk every card.
   *
   * That resident index is also the term this feature's whole-feature peak is
   * built on, because a StateStore merge runs with the deck open: the composed
   * ~2.0 MB figure is documented once, on StateStore::open().
   *
   * **Under CROSSPOINT_FLASHCARDS_C3 this function allocates nothing at all.**
   * The index is validated by streaming it in 2560-byte chunks off the stack
   * and is then left on the card; the duplicate-key pass, the only other
   * allocation here, is skipped (trust boundary in the file comment above).
   * Peak: the 2560-byte chunk, in a cold leaf frame. There is therefore no
   * DeckError::OutOfMemory path in the C3 build of open().
   */
  DeckError open(const std::string& path);
  void close();
  bool isOpen() const { return cardCountValue != 0; }

  uint32_t cardCount() const { return cardCountValue; }
  uint64_t contentHash() const { return contentHashValue; }
  /** NUL-terminated copy of the header title (read via title_len, never assumed terminated). */
  const char* title() const { return titleText; }

  /** Scheduler parameters for this deck: the deck's own validated w[] if it carries a block, else the defaults. */
  const fsrs::Params& params() const { return schedulerParams; }
  bool hasDeckParams() const { return deckSuppliedParams; }

  /**
   * The card key at `ordinal`, or 0 when the ordinal is out of range.
   *
   * Under CROSSPOINT_FLASHCARDS_C3 this reads the card, so it is no longer
   * free: it costs one 640-byte index read per 32 consecutive ordinals (the
   * window cache), or one per call for a random walk. Every caller in this
   * layer walks ordinals in order, so the sequential figure is the one that
   * applies — 63 reads over a 2000-card deck.
   */
  uint64_t keyAt(Ordinal ordinal) const;

  /**
   * The card key at `ordinal`, with an error channel: false when the ordinal is
   * out of range OR — under CROSSPOINT_FLASHCARDS_C3 — the index read failed.
   * `keyOut` is untouched on failure.
   *
   * This is the accessor the state layer must use (FLASHCARD_SPEC.md §7b.3 pin
   * a). Without a resident index a key read IS disk I/O, and keyAt()'s "0 on
   * failure" is indistinguishable from a real key of 0: a state record checked
   * against a failed read looks TORN, and the §3 self-heal then rewrites it as
   * a fresh New card. One bad sector under the index would therefore wipe the
   * schedule of every card in the deck. A failed read is an I/O ERROR — it
   * aborts the operation and heals nothing.
   *
   * On the Pro build the index is resident, so this cannot fail for an in-range
   * ordinal; it stays a header inline that no firmware call site uses (the state
   * layer's guarded alternates take the keyAt() path there verbatim), so it adds
   * nothing to that build and exists for the host suite to assert the contract
   * in both configurations.
   */
#ifdef CROSSPOINT_FLASHCARDS_C3
  bool keyAtChecked(Ordinal ordinal, uint64_t& keyOut) const;
#else
  bool keyAtChecked(Ordinal ordinal, uint64_t& keyOut) const {
    if (ordinal >= cardCountValue) return false;
    keyOut = keyAt(ordinal);
    return true;
  }
#endif

  /**
   * Reads one side of a card into `buffer` and NUL-terminates it.
   *
   * `bufferBytes` must leave room for that terminator; DECK_MAX_SLICE_BYTES + 1
   * always suffices. A zero-length side succeeds with `lengthOut = 0` and an
   * empty string. Returns false only on a bad ordinal, a buffer that is too
   * small, or a failed read — never on empty text.
   *
   * The bytes are SANITIZED on the way out: any C0 control byte other than
   * '\n' is replaced with a space (FLASHCARD_SPEC.md §2.1). One byte of bad
   * text never costs the reader a whole deck, and lengths are unchanged.
   */
  bool loadSide(Ordinal ordinal, CardSide side, char* buffer, size_t bufferBytes, uint16_t& lengthOut);

 private:
  /** Index entry fields for `ordinal`, memcpy'd out of the resident index. */
  bool sliceAt(Ordinal ordinal, CardSide side, uint32_t& offsetOut, uint16_t& lengthOut) const;

#ifdef CROSSPOINT_FLASHCARDS_C3
  /** Bytes in one CPDK index entry — the header's copy of DeckFile.cpp's constant. */
  static constexpr uint32_t C3_INDEX_ENTRY_BYTES = 20;
  /**
   * Index entries the on-demand window holds: 32 entries = 640 bytes, resident
   * for the deck's lifetime and the ONLY thing this reader keeps in RAM.
   *
   * A window rather than a single entry because every caller here walks the
   * deck in ordinal order — createFresh and the merge ask for keyAt(0),
   * keyAt(1), … — so an aligned block turns one read per card into one read per
   * 32. It stays a plain member array: 640 bytes is well inside the "allocated
   * once for the object's lifetime" case and needs no heap at all, which is the
   * whole point on this device. A single-entry cache would be simpler and would
   * still be correct; it would just cost 2000 reads where this costs 63.
   */
  static constexpr uint32_t C3_INDEX_WINDOW_ENTRIES = 32;
  /** windowFirst when the window holds nothing. */
  static constexpr uint32_t C3_WINDOW_EMPTY = 0xFFFFFFFFu;

  /**
   * Points `entryOut` at `ordinal`'s 20 index bytes, refilling the window from
   * the card when it is not already there. False on a bad ordinal or a failed
   * read. The pointer is valid only until the next call, and is bounded by
   * `windowCount` rather than by the window's capacity, so no arithmetic here
   * can hand back a pointer past the bytes actually read.
   */
  bool entryFor(Ordinal ordinal, const uint8_t*& entryOut) const;

  // Mutable because keyAt()/sliceAt() are const observers that nonetheless have
  // to touch the card: without a resident index, reading a key IS file I/O.
  mutable HalFile file;
  mutable uint8_t window[C3_INDEX_WINDOW_ENTRIES * C3_INDEX_ENTRY_BYTES] = {};
  mutable uint32_t windowFirst = C3_WINDOW_EMPTY;  // ordinal of window[0], aligned to the window size
  mutable uint32_t windowCount = 0;                // entries actually loaded (short at the deck's end)
  uint32_t indexFileOffset = 0;                    // absolute file offset of index entry 0
#else
  HalFile file;
  std::unique_ptr<uint8_t[]> index;  // cardCountValue * 20 bytes, raw as on disk
#endif
  uint32_t cardCountValue = 0;
  uint64_t contentHashValue = 0;
  uint32_t blobStart = 0;  // absolute file offset of the text blob
  uint32_t blobBytes = 0;
  bool deckSuppliedParams = false;
  fsrs::Params schedulerParams{};
  char titleText[DECK_TITLE_MAX_BYTES + 1] = {};
};

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
