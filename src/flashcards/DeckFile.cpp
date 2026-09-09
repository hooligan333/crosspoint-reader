#include "DeckFile.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "KeySort.h"

namespace flashcards {
namespace {

constexpr size_t CPDK_HEADER_BYTES = 64;
constexpr size_t CPDK_PARAMS_BYTES = 84;  // f32 w[21], present iff flags bit 0
constexpr size_t CPDK_INDEX_ENTRY_BYTES = 20;
constexpr uint16_t CPDK_FORMAT_VERSION = 1;
constexpr uint16_t CPDK_FLAG_FSRS_PARAMS = 0x0001;

// Header field offsets (DECK_SERVER_SPEC.md §3.1).
constexpr size_t OFF_FORMAT_VERSION = 4;
constexpr size_t OFF_FLAGS = 6;
constexpr size_t OFF_CARD_COUNT = 8;
constexpr size_t OFF_CONTENT_HASH = 12;
constexpr size_t OFF_TITLE_LEN = 20;
constexpr size_t OFF_TITLE = 21;

// Index entry field offsets, relative to the entry.
constexpr size_t ENTRY_OFF_KEY = 0;
constexpr size_t ENTRY_OFF_FRONT_OFF = 8;
constexpr size_t ENTRY_OFF_FRONT_LEN = 12;
constexpr size_t ENTRY_OFF_BACK_OFF = 14;
constexpr size_t ENTRY_OFF_BACK_LEN = 18;

// Every CPDK field is little-endian on the wire and is memcpy'd straight into a
// local below, so the bytes only land in the right order on a little-endian
// host. True of every ESP32 target and of the host the unit tests run on, but
// worth failing the build over rather than meeting as a garbled hash (§2.1).
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "CPDK fields are little-endian; this host is not");

// The resident index is read in bounded chunks rather than one 800 KB call, so
// a short read is caught at the chunk that failed and the transfer size stays
// something an SD driver is happy with.
constexpr size_t INDEX_READ_CHUNK_BYTES = 16 * 1024;

#ifdef CROSSPOINT_FLASHCARDS_C3
// Index entries validated per read on the C3, where nothing is held resident
// (FLASHCARD_SPEC.md §7b.3 caps this at 4 KB). 128 entries is 2560 bytes in a
// leaf frame that lives only for the length of open(), and validates a
// 2000-card index in 16 reads.
constexpr uint32_t C3_VALIDATE_CHUNK_ENTRIES = 128;
constexpr size_t C3_VALIDATE_CHUNK_BYTES = C3_VALIDATE_CHUNK_ENTRIES * CPDK_INDEX_ENTRY_BYTES;
static_assert(C3_VALIDATE_CHUNK_BYTES <= 4096, "the C3 index validator must stream in 4 KB or less");
#endif

uint16_t readU16(const uint8_t* bytes, size_t offset) {
  uint16_t value = 0;
  memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

uint32_t readU32(const uint8_t* bytes, size_t offset) {
  uint32_t value = 0;
  memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

uint64_t readU64(const uint8_t* bytes, size_t offset) {
  uint64_t value = 0;
  memcpy(&value, bytes + offset, sizeof(value));
  return value;
}

}  // namespace

const char* deckErrorName(DeckError error) {
  switch (error) {
    case DeckError::Ok:
      return "ok";
    case DeckError::OpenFailed:
      return "open failed";
    case DeckError::ShortFile:
      return "truncated";
    case DeckError::BadMagic:
      return "not a CPDK file";
    case DeckError::BadVersion:
      return "unsupported version";
    case DeckError::UnknownFlags:
      return "unknown header flags";
    case DeckError::BadCardCount:
      return "bad card count";
    case DeckError::ReadFailed:
      return "read failed";
    case DeckError::BadParams:
      return "invalid FSRS parameters";
    case DeckError::SliceOutOfRange:
      return "card index out of range";
    case DeckError::DuplicateKey:
      return "duplicate card key";
    case DeckError::OutOfMemory:
      return "out of memory";
  }
  return "unknown";
}

void DeckFile::close() {
  file = HalFile();
#ifdef CROSSPOINT_FLASHCARDS_C3
  windowFirst = C3_WINDOW_EMPTY;
  windowCount = 0;
  indexFileOffset = 0;
#else
  index.reset();
#endif
  cardCountValue = 0;
  contentHashValue = 0;
  blobStart = 0;
  blobBytes = 0;
  deckSuppliedParams = false;
  titleText[0] = '\0';
}

DeckError DeckFile::open(const std::string& path) {
  close();

  if (!Storage.openFileForRead("DECK", path, file)) return DeckError::OpenFailed;

  const size_t fileSize = file.fileSize();
  if (fileSize < CPDK_HEADER_BYTES) {
    close();
    return DeckError::ShortFile;
  }

  uint8_t header[CPDK_HEADER_BYTES];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    close();
    return DeckError::ReadFailed;
  }
  if (memcmp(header, "CPDK", 4) != 0) {
    close();
    return DeckError::BadMagic;
  }

  const uint16_t formatVersion = readU16(header, OFF_FORMAT_VERSION);
  const uint16_t flags = readU16(header, OFF_FLAGS);
  const uint32_t cards = readU32(header, OFF_CARD_COUNT);
  if (formatVersion != CPDK_FORMAT_VERSION) {
    close();
    return DeckError::BadVersion;
  }
  // Unknown flags bits are the format's version escape hatch: refuse rather
  // than guess what the block after the header means (§2.1).
  if ((flags & static_cast<uint16_t>(~CPDK_FLAG_FSRS_PARAMS)) != 0) {
    close();
    return DeckError::UnknownFlags;
  }
  if (cards == 0 || cards > DECK_MAX_CARDS) {
    close();
    return DeckError::BadCardCount;
  }

  contentHashValue = readU64(header, OFF_CONTENT_HASH);

  // The title is never assumed NUL-terminated: it is copied by title_len, which
  // is clamped rather than refused — an over-long length is cosmetic, and the
  // hard gates are format_version and flags (§2.1).
  uint8_t titleLen = header[OFF_TITLE_LEN];
  if (titleLen > DECK_TITLE_MAX_BYTES) titleLen = DECK_TITLE_MAX_BYTES;
  memcpy(titleText, header + OFF_TITLE, titleLen);
  titleText[titleLen] = '\0';

  schedulerParams = fsrs::defaultParams();
  const bool hasParamsBlock = (flags & CPDK_FLAG_FSRS_PARAMS) != 0;
  if (hasParamsBlock) {
    if (fileSize < CPDK_HEADER_BYTES + CPDK_PARAMS_BYTES) {
      close();
      return DeckError::ShortFile;
    }
    uint8_t block[CPDK_PARAMS_BYTES];
    if (file.read(block, sizeof(block)) != static_cast<int>(sizeof(block))) {
      close();
      return DeckError::ReadFailed;
    }
    // f32 at offset 64 + 4k: 4-aligned on disk but not guaranteed in a buffer
    // the compiler only knows as uint8_t[], so memcpy here too.
    memcpy(schedulerParams.w, block, sizeof(schedulerParams.w));
    static_assert(sizeof(schedulerParams.w) == CPDK_PARAMS_BYTES, "CPDK params block is f32 w[21]");
    // Out-of-bounds weights are not merely inaccurate, they are silently
    // degenerate (FsrsSched.h on w[20] == 0), so the deck is refused (§4).
    if (!fsrs::validateParams(schedulerParams)) {
      close();
      return DeckError::BadParams;
    }
    deckSuppliedParams = true;
  }

  const size_t indexStart = CPDK_HEADER_BYTES + (hasParamsBlock ? CPDK_PARAMS_BYTES : 0);
  const size_t indexBytes = static_cast<size_t>(cards) * CPDK_INDEX_ENTRY_BYTES;
  const size_t blobStartValue = indexStart + indexBytes;  // cards is capped, so this cannot overflow
  if (fileSize < blobStartValue) {
    close();
    return DeckError::ShortFile;
  }

#ifdef CROSSPOINT_FLASHCARDS_C3
  // No resident index and no allocation: the index stays on the card and is
  // walked here once, in chunks, purely to validate it. Everything keyAt() and
  // loadSide() need afterwards is re-read on demand (FLASHCARD_SPEC.md §7b.3).
  cardCountValue = cards;
  indexFileOffset = static_cast<uint32_t>(indexStart);
  blobStart = static_cast<uint32_t>(blobStartValue);
  blobBytes = static_cast<uint32_t>(fileSize - blobStartValue);
  windowFirst = C3_WINDOW_EMPTY;
  windowCount = 0;

  // Slice integrity, streamed. Same rule as the resident path below — every
  // front and back must land inside the blob and stay inside the 4096-byte cap,
  // with the subtraction ordered so no sum can wrap — but read a chunk at a
  // time, so a 2000-card index is checked in full without ever holding it.
  {
    uint8_t chunk[C3_VALIDATE_CHUNK_BYTES];
    if (!file.seek(indexFileOffset)) {
      close();
      return DeckError::ReadFailed;
    }
    for (uint32_t first = 0; first < cards; first += C3_VALIDATE_CHUNK_ENTRIES) {
      uint32_t count = cards - first;
      if (count > C3_VALIDATE_CHUNK_ENTRIES) count = C3_VALIDATE_CHUNK_ENTRIES;
      const size_t bytes = static_cast<size_t>(count) * CPDK_INDEX_ENTRY_BYTES;
      if (file.read(chunk, bytes) != static_cast<int>(bytes)) {
        close();
        return DeckError::ReadFailed;
      }
      for (uint32_t i = 0; i < count; i++) {
        const uint8_t* entry = chunk + static_cast<size_t>(i) * CPDK_INDEX_ENTRY_BYTES;
        const uint32_t frontOff = readU32(entry, ENTRY_OFF_FRONT_OFF);
        const uint16_t frontLen = readU16(entry, ENTRY_OFF_FRONT_LEN);
        const uint32_t backOff = readU32(entry, ENTRY_OFF_BACK_OFF);
        const uint16_t backLen = readU16(entry, ENTRY_OFF_BACK_LEN);
        const bool frontOk =
            frontLen <= DECK_MAX_SLICE_BYTES && frontOff <= blobBytes && blobBytes - frontOff >= frontLen;
        const bool backOk = backLen <= DECK_MAX_SLICE_BYTES && backOff <= blobBytes && blobBytes - backOff >= backLen;
        if (!frontOk || !backOk) {
          LOG_ERR("DECK", "Card %u slice out of range (blob %u B): %s", static_cast<unsigned>(first + i),
                  static_cast<unsigned>(blobBytes), path.c_str());
          close();
          return DeckError::SliceOutOfRange;
        }
      }
    }
  }

  // The duplicate-key pass is SKIPPED here — the one behavioural divergence
  // between the two builds, and a deliberate trust boundary. Its cost is a
  // sorted copy of every key in one contiguous block (16 KB at this build's
  // 2000-card cap, on a heap that is routinely ~50 KB and fragmented); what it
  // catches is already refused twice upstream, by convert_deck.py when it
  // derives the keys and by read_deck.py when the server verifies the published
  // file. The residual exposure is bounded and is spelled out in full on the
  // DeckFile class comment: records are addressed by ordinal, the CPST
  // per-record key echo still guards every ordinal at read time, and the worst
  // case is one card's history being COPIED onto a twin rather than any card
  // reading the wrong state. See FLASHCARD_SPEC.md §7b.3.
#else
  // 800 KB at the 40000-card cap; PSRAM on this feature's S3-only builds.
  index = makeUniqueNoThrow<uint8_t[]>(indexBytes);
  if (!index) {
    LOG_ERR("DECK", "Card index alloc failed: %u B for %u cards", static_cast<unsigned>(indexBytes),
            static_cast<unsigned>(cards));
    close();
    return DeckError::OutOfMemory;
  }
  uint8_t* const indexBase = index.get();
  for (size_t done = 0; done < indexBytes;) {
    size_t chunk = indexBytes - done;
    if (chunk > INDEX_READ_CHUNK_BYTES) chunk = INDEX_READ_CHUNK_BYTES;
    if (file.read(indexBase + done, chunk) != static_cast<int>(chunk)) {
      close();
      return DeckError::ReadFailed;
    }
    done += chunk;
  }

  cardCountValue = cards;
  blobStart = static_cast<uint32_t>(blobStartValue);
  blobBytes = static_cast<uint32_t>(fileSize - blobStartValue);

  // Slice integrity: every front and back must land inside the blob and stay
  // inside the 4096-byte cap. A truncated download is the realistic failure
  // mode (DECK_SERVER_SPEC.md §3.6.4); the subtraction below is ordered so no
  // sum can wrap.
  for (uint32_t ordinal = 0; ordinal < cards; ordinal++) {
    const uint8_t* entry = indexBase + static_cast<size_t>(ordinal) * CPDK_INDEX_ENTRY_BYTES;
    const uint32_t frontOff = readU32(entry, ENTRY_OFF_FRONT_OFF);
    const uint16_t frontLen = readU16(entry, ENTRY_OFF_FRONT_LEN);
    const uint32_t backOff = readU32(entry, ENTRY_OFF_BACK_OFF);
    const uint16_t backLen = readU16(entry, ENTRY_OFF_BACK_LEN);
    const bool frontOk = frontLen <= DECK_MAX_SLICE_BYTES && frontOff <= blobBytes && blobBytes - frontOff >= frontLen;
    const bool backOk = backLen <= DECK_MAX_SLICE_BYTES && backOff <= blobBytes && blobBytes - backOff >= backLen;
    if (!frontOk || !backOk) {
      LOG_ERR("DECK", "Card %u slice out of range (blob %u B): %s", static_cast<unsigned>(ordinal),
              static_cast<unsigned>(blobBytes), path.c_str());
      close();
      return DeckError::SliceOutOfRange;
    }
  }

  // Duplicate keys would make two cards share one state record, so the deck is
  // refused (§2.1). Sorted scratch copy rather than the O(n²) pairwise scan:
  // 320 KB at the cap, freed before open() returns.
  auto scratch = makeUniqueNoThrow<uint64_t[]>(cards);
  if (!scratch) {
    LOG_ERR("DECK", "Key scratch alloc failed: %u B", static_cast<unsigned>(cards * sizeof(uint64_t)));
    close();
    return DeckError::OutOfMemory;
  }
  for (uint32_t ordinal = 0; ordinal < cards; ordinal++) {
    scratch[ordinal] = readU64(indexBase + static_cast<size_t>(ordinal) * CPDK_INDEX_ENTRY_BYTES, ENTRY_OFF_KEY);
  }
  sortKeys(scratch.get(), nullptr, cards);
  for (uint32_t i = 1; i < cards; i++) {
    if (scratch[i] == scratch[i - 1]) {
      LOG_ERR("DECK", "Duplicate card key 0x%08lx%08lx: %s", static_cast<unsigned long>(scratch[i] >> 32),
              static_cast<unsigned long>(scratch[i] & 0xFFFFFFFFu), path.c_str());
      close();
      return DeckError::DuplicateKey;
    }
  }
  scratch.reset();
#endif  // CROSSPOINT_FLASHCARDS_C3

  LOG_INF("DECK", "Opened %s: %u cards, hash 0x%08lx%08lx%s", path.c_str(), static_cast<unsigned>(cards),
          static_cast<unsigned long>(contentHashValue >> 32),
          static_cast<unsigned long>(contentHashValue & 0xFFFFFFFFu), deckSuppliedParams ? ", deck params" : "");
  return DeckError::Ok;
}

#ifdef CROSSPOINT_FLASHCARDS_C3

bool DeckFile::entryFor(Ordinal ordinal, const uint8_t*& entryOut) const {
  entryOut = nullptr;
  if (ordinal >= cardCountValue) return false;

  // Windows are aligned to their own size, so a walk in ordinal order refills
  // once every C3_INDEX_WINDOW_ENTRIES cards and a repeat visit to the card the
  // study screen is showing costs nothing at all.
  const uint32_t first = (static_cast<uint32_t>(ordinal) / C3_INDEX_WINDOW_ENTRIES) * C3_INDEX_WINDOW_ENTRIES;
  if (windowFirst != first || windowCount == 0) {
    uint32_t count = cardCountValue - first;
    if (count > C3_INDEX_WINDOW_ENTRIES) count = C3_INDEX_WINDOW_ENTRIES;
    const size_t bytes = static_cast<size_t>(count) * C3_INDEX_ENTRY_BYTES;
    windowFirst = C3_WINDOW_EMPTY;
    windowCount = 0;
    if (!file.seek(indexFileOffset + static_cast<size_t>(first) * C3_INDEX_ENTRY_BYTES)) return false;
    if (file.read(window, bytes) != static_cast<int>(bytes)) {
      LOG_ERR("DECK", "Index read failed at ordinal %u", static_cast<unsigned>(first));
      return false;
    }
    windowFirst = first;
    windowCount = count;
  }
  // The returned pointer is bounded by what was actually READ, not by the
  // window's capacity: windowCount is short at the deck's last window, and this
  // bound is what makes an overrun structurally impossible rather than merely
  // unreachable. Defensive and untestable from outside — `ordinal` is already
  // known to be below cardCountValue and the refill above loads every entry up
  // to it, so nothing drives this return.
  const uint32_t offsetInWindow = static_cast<uint32_t>(ordinal) - first;
  if (offsetInWindow >= windowCount) return false;
  entryOut = window + static_cast<size_t>(offsetInWindow) * C3_INDEX_ENTRY_BYTES;
  return true;
}

bool DeckFile::keyAtChecked(Ordinal ordinal, uint64_t& keyOut) const {
  const uint8_t* entry = nullptr;
  if (!entryFor(ordinal, entry)) return false;
  keyOut = readU64(entry, ENTRY_OFF_KEY);
  return true;
}

uint64_t DeckFile::keyAt(Ordinal ordinal) const {
  // Kept for the callers that have no error path of their own. Anything that
  // COMPARES this against a stored key must use keyAtChecked() instead — a 0
  // here is indistinguishable from a real key of 0 (header note on that call).
  uint64_t key = 0;
  return keyAtChecked(ordinal, key) ? key : 0;
}

bool DeckFile::sliceAt(Ordinal ordinal, CardSide side, uint32_t& offsetOut, uint16_t& lengthOut) const {
  const uint8_t* entry = nullptr;
  if (!entryFor(ordinal, entry)) return false;
  switch (side) {
    case CardSide::Front:
      offsetOut = readU32(entry, ENTRY_OFF_FRONT_OFF);
      lengthOut = readU16(entry, ENTRY_OFF_FRONT_LEN);
      return true;
    case CardSide::Back:
      offsetOut = readU32(entry, ENTRY_OFF_BACK_OFF);
      lengthOut = readU16(entry, ENTRY_OFF_BACK_LEN);
      return true;
  }
  return false;
}

#else

uint64_t DeckFile::keyAt(Ordinal ordinal) const {
  if (ordinal >= cardCountValue) return 0;
  const uint8_t* const entry = index.get();
  return readU64(entry + static_cast<size_t>(ordinal) * CPDK_INDEX_ENTRY_BYTES, ENTRY_OFF_KEY);
}

bool DeckFile::sliceAt(Ordinal ordinal, CardSide side, uint32_t& offsetOut, uint16_t& lengthOut) const {
  if (ordinal >= cardCountValue) return false;
  const uint8_t* const base = index.get();
  const uint8_t* const entry = base + static_cast<size_t>(ordinal) * CPDK_INDEX_ENTRY_BYTES;
  switch (side) {
    case CardSide::Front:
      offsetOut = readU32(entry, ENTRY_OFF_FRONT_OFF);
      lengthOut = readU16(entry, ENTRY_OFF_FRONT_LEN);
      return true;
    case CardSide::Back:
      offsetOut = readU32(entry, ENTRY_OFF_BACK_OFF);
      lengthOut = readU16(entry, ENTRY_OFF_BACK_LEN);
      return true;
  }
  return false;
}

#endif  // CROSSPOINT_FLASHCARDS_C3

bool DeckFile::loadSide(Ordinal ordinal, CardSide side, char* buffer, size_t bufferBytes, uint16_t& lengthOut) {
  lengthOut = 0;
  if (buffer == nullptr || bufferBytes == 0) return false;
  buffer[0] = '\0';

  uint32_t offset = 0;
  uint16_t length = 0;
  if (!sliceAt(ordinal, side, offset, length)) return false;
  if (bufferBytes < static_cast<size_t>(length) + 1) return false;

  // An empty side is legal (DECK_SERVER_SPEC.md §3.6.5) and needs no read at
  // all: the buffer already holds the empty string.
  if (length == 0) return true;

  if (!file.seek(blobStart + offset)) return false;
  if (file.read(buffer, length) != static_cast<int>(length)) {
    buffer[0] = '\0';
    return false;
  }
  // The converter strips C0 control bytes and read_deck.py refuses a file that
  // still carries one, but the device sanitizes anyway (FLASHCARD_SPEC.md §2.1):
  // a deck copied over by USB, or corrupted after its header checked out, must
  // not be able to put a raw control byte through the text renderer. Offending
  // bytes become spaces, one for one, so every offset and length stays valid —
  // a whole deck is never refused over one byte. '\n' is the line break the
  // format defines and is the only C0 that survives.
  for (uint16_t i = 0; i < length; i++) {
    const unsigned char byte = static_cast<unsigned char>(buffer[i]);
    if (byte < 0x20 && byte != '\n') buffer[i] = ' ';
  }
  // The text carries no terminator of its own, so it is terminated in place —
  // in the one spare byte checked for above (DECK_SERVER_SPEC.md §3.6.8).
  buffer[length] = '\0';
  lengthOut = length;
  return true;
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
