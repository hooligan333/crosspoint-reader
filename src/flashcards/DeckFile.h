#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <FsrsSched.h>
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

/**
 * Reader for a `CPDK` v1 or v2 deck file (FLASHCARD_SPEC.md §2/§2.2, byte layout
 * in `deck-server/DECK_SERVER_SPEC.md` §3/§3.7).
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
 *
 * ---------------------------------------------------------------------------
 * **CPDK v2 — card images** (FLASHCARD_SPEC.md §2.2, layout in
 * DECK_SERVER_SPEC.md §3.7). A v2 file inserts an image table and a placement
 * table between the optional FSRS block and the card index, and appends an
 * image blob that runs to EOF. Both variants of this reader accept v1 AND v2 —
 * there is no build flag for it, and a v1 file is byte-for-byte the file it
 * always was.
 *
 * **Neither table is ever held resident, on either variant.** The Pro's
 * resident card index is affordable because it is 20 bytes per card; the image
 * tables are not bounded that way. At the Pro's 40000-card cap the format
 * permits 8x40000 image entries (5.12 MB) and 8x40000 placements (3.84 MB) —
 * 8.96 MB on top of the 800 KB index and the ~2.0 MB whole-feature peak
 * documented on StateStore::open(), which is more than this feature's PSRAM
 * budget can promise. So both tables are STREAM-VALIDATED at open (in chunks
 * that stay under 4 KB, the C3 rule) and are then left on the card, exactly as
 * the C3 variant treats the card index:
 *
 *   - `imagesForSide()` binary-searches the placement table on disk — it is
 *     sorted by (ordinal, side, text_offset), which is the key it searches —
 *     through a 16-entry (192-byte) window that also serves the forward walk of
 *     that card's <= 4 placements, then reads one 16-byte image-table entry per
 *     placement. Around 20 short reads per card side, against a card load that
 *     already pays a text read and an e-ink repaint;
 *   - that window is the ONLY thing v2 adds to this object's footprint, and it
 *     is a plain member array. `open()` allocates nothing new on either
 *     variant, and a v1 deck touches none of this code at all:
 *     `imagesForSide()` returns 0 without a single read when image_count is 0.
 *
 * What is NOT checked at open is each image's JPEG frame header (read_deck.py's
 * §3.7 check 4). Sniffing it costs one seek per image — minutes of SD seeks at
 * the format's cap — so it moves to `loadImage()`, which refuses a
 * non-baseline, non-grayscale or wrong-sized image at the moment it is asked
 * for the bytes. The study screen draws a placeholder box for that one image
 * and the deck still opens, which is the right outcome on a device that cannot
 * afford to read the whole blob to find out. `read_deck.py` remains the gate
 * that catches it before the file is ever published.
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

// --- CPDK v2 card-image caps (DECK_SERVER_SPEC.md §3.7) ---------------------
/** Encoded JPEG bytes per image. The one figure the study screen's buffer is sized by. */
constexpr uint32_t DECK_MAX_IMAGE_BYTES = 64 * 1024;
/** Longest side the converter emits. The device never scales; it draws as delivered. */
constexpr uint16_t DECK_MAX_IMAGE_EDGE = 440;
/**
 * Placements per (ordinal, side). The 5th image on a side keeps its `[image]` marker.
 *
 * This is the ONE number both v2 table caps are derived from, and they are both
 * spelled `2 * DECK_MAX_IMAGES_PER_SIDE * card_count` at the point of use rather
 * than given constants of their own (FLASHCARD_SPEC.md §2.2, I1 pins): a card
 * has two sides, so four per side is eight per card for BOTH `image_count` and
 * `placement_count`. An earlier `DECK_MAX_IMAGES_PER_CARD = 4` made the image
 * cap 4x while the per-side rule already permitted 4 front + 4 back — a bound
 * the converter could not satisfy, and one that only drifted because it was
 * written down twice.
 */
constexpr uint8_t DECK_MAX_IMAGES_PER_SIDE = 4;

/** Why a deck would not open. Ok is the only value that leaves it usable. */
enum class DeckError : uint8_t {
  Ok,
  OpenFailed,       // the file will not open at all
  ShortFile,        // shorter than its header, or than its own index end
  BadMagic,         // not "CPDK"
  BadVersion,       // format_version is neither 1 nor 2
  UnknownFlags,     // a reserved flags bit is set — the version escape hatch
  BadCardCount,     // 0, or above DECK_MAX_CARDS (40000; 2000 on the C3 variant)
  ReadFailed,       // a read came up short mid-file
  BadParams,        // deck-supplied FSRS weights fail validateParams()
  SliceOutOfRange,  // an index entry points outside the text blob, or is > 4096 B
  DuplicateKey,     // two cards share a key: state could not be kept per card
  OutOfMemory,      // the card index would not fit
  // v2 only (DECK_SERVER_SPEC.md §3.7): image_count out of range, an entry with
  // a bad byte_len / dimensions / reserved field, a v2 header that does not
  // set has_images (a deck with no images is written as v1, so that a v1 device
  // can still read it — a v2 file therefore always carries at least one image),
  // or an image blob whose entries do not tile it densely from offset 0.
  BadImageTable,
  // v2 only: the placement table is out of order, or an entry names an ordinal,
  // side, image or text offset that does not exist.
  BadPlacement,
};

/** Log-friendly name for a DeckError. User-facing text is the study screen's job. */
const char* deckErrorName(DeckError error);

enum class CardSide : uint8_t { Front, Back };

/**
 * One entry of the v2 image table, resolved: `fileOffset` is ABSOLUTE, so the
 * caller never has to know where the image blob starts.
 *
 * `width`/`height` are what the converter encoded and what the file's own JPEG
 * frame must agree with — the device never scales, it draws as delivered
 * (DECK_SERVER_SPEC.md §3.7).
 */
struct DeckImage {
  uint32_t fileOffset;
  uint32_t byteLength;
  uint16_t width;
  uint16_t height;
};

/** Where one image sits in a card side's text: drawn BEFORE the byte at `textOffset`. */
struct DeckImagePlacement {
  uint32_t textOffset;
  DeckImage image;
};

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

  // --- CPDK v2 card images ---------------------------------------------------

  /** True for a v2 deck that carries at least one image; false for every v1 deck. */
  bool hasImages() const { return imageCountValue != 0; }

  /**
   * The largest `byte_len` in this deck's image table, 0 when there are none.
   *
   * Recorded during the open-time validation pass, which reads every entry
   * anyway. It exists so the study screen can size ONE encoded-image buffer at
   * session start and reuse it for every image the session draws, instead of
   * allocating inside the render path or reserving the format's 64 KB cap for a
   * deck whose pictures are 20 KB (FLASHCARD_SPEC.md §2.2).
   */
  uint32_t maxImageBytes() const { return maxImageBytesValue; }

  /**
   * The images placed on one card side, in text order, into `out`.
   *
   * Returns how many were written — 0 to `min(capacity, 4)`; 0 immediately, and
   * with no I/O at all, for a v1 deck. `capacity` should be
   * DECK_MAX_IMAGES_PER_SIDE: the format caps a side at four, and a deck that
   * somehow carried more was refused at open.
   *
   * Costs a binary search of the on-disk placement table (about 14 windowed
   * reads at the format's cap, 5 on a realistic deck) plus one 16-byte
   * image-table read per placement found. A read failure mid-search is reported
   * as "no images here" rather than as a card that will not load: a bad sector
   * under the placement table must cost the pictures, not the study session.
   */
  uint8_t imagesForSide(Ordinal ordinal, CardSide side, DeckImagePlacement* out, uint8_t capacity);

  /**
   * Reads one image's encoded JPEG bytes into `buffer`.
   *
   * `bufferBytes` must be at least `image.byteLength`. False on a short buffer,
   * a failed read, or bytes that do not sniff as the baseline single-component
   * JPEG of exactly `image.width` x `image.height` that §3.7's encoding contract
   * promises — the embedded decoder is baseline-only, and handing it a
   * progressive frame is how a decoder walks off its own buffers. The caller
   * draws a placeholder box; nothing here ever refuses the deck.
   */
  bool loadImage(const DeckImage& image, uint8_t* buffer, size_t bufferBytes);

 private:
  /** Index entry fields for `ordinal`, memcpy'd out of the resident index. */
  bool sliceAt(Ordinal ordinal, CardSide side, uint32_t& offsetOut, uint16_t& lengthOut) const;

  // --- v2 image/placement tables (neither is resident; see the class comment) --
  /** Bytes in one image-table entry and one placement-table entry (§3.7). */
  static constexpr uint32_t CPDK_IMAGE_ENTRY_BYTES = 16;
  static constexpr uint32_t CPDK_PLACEMENT_ENTRY_BYTES = 12;
  /**
   * Placement entries the on-disk binary search reads at a time: 16 entries =
   * 192 bytes, the only RAM v2 costs this object. It is sized to cover a
   * card's whole run in one read (a side holds at most 4 placements, and both
   * sides of one card at most 8) while keeping the search's probe reads short.
   */
  static constexpr uint32_t PLACEMENT_WINDOW_ENTRIES = 16;
  static constexpr uint32_t PLACEMENT_WINDOW_EMPTY = 0xFFFFFFFFu;

  /** Reads image/placement counts and validates both tables. Called only for v2. */
  DeckError readImageSections(size_t fileSize, uint32_t cards, size_t& offsetInOut, uint64_t& imageBlobLenOut);
  /** Streams the image table, validating every entry and finding the blob's length. */
  DeckError validateImageTable(size_t fileSize, uint64_t& imageBlobLenOut);
  /** Streams the placement table, validating order, ranges and the per-side cap. */
  DeckError validatePlacements();
  /**
   * Points `entryOut` at placement `slot`'s 12 bytes, refilling the window when
   * it is not already there. The pointer is valid until the next call and is
   * bounded by what was actually read, never by the window's capacity.
   */
  bool placementAt(uint32_t slot, const uint8_t*& entryOut);
  /** The image-table entry at `imageIndex`, resolved to absolute file offsets. */
  bool imageAt(uint32_t imageIndex, DeckImage& out);

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
  uint32_t blobBytes = 0;  // TEXT blob length: on v2 it stops where the image blob starts

  // v2 image sections. All zero on a v1 deck, which is what makes every image
  // accessor a no-op there without a version test of its own.
  uint32_t imageCountValue = 0;
  uint32_t placementCountValue = 0;
  uint32_t imageTableOffset = 0;      // absolute file offset of image entry 0
  uint32_t placementTableOffset = 0;  // absolute file offset of placement entry 0
  uint32_t imageBlobStart = 0;        // absolute file offset of image-blob byte 0
  uint32_t maxImageBytesValue = 0;
  uint8_t placementWindow[PLACEMENT_WINDOW_ENTRIES * CPDK_PLACEMENT_ENTRY_BYTES] = {};
  uint32_t placementWindowFirst = PLACEMENT_WINDOW_EMPTY;
  uint32_t placementWindowCount = 0;

  bool deckSuppliedParams = false;
  fsrs::Params schedulerParams{};
  char titleText[DECK_TITLE_MAX_BYTES + 1] = {};
};

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
