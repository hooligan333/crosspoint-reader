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
 */
namespace flashcards {

/** Card ordinals are u16 throughout: DECK_MAX_CARDS fits with room to spare. */
using Ordinal = uint16_t;

constexpr uint32_t DECK_MAX_CARDS = 40000;
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
  BadCardCount,     // 0, or above the 40000 cap
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

  /** The card key at `ordinal`, or 0 when the ordinal is out of range. */
  uint64_t keyAt(Ordinal ordinal) const;

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

  HalFile file;
  std::unique_ptr<uint8_t[]> index;  // cardCountValue * 20 bytes, raw as on disk
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
