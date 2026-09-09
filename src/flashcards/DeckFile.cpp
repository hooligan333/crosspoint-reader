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
  index.reset();
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

  LOG_INF("DECK", "Opened %s: %u cards, hash 0x%08lx%08lx%s", path.c_str(), static_cast<unsigned>(cards),
          static_cast<unsigned long>(contentHashValue >> 32),
          static_cast<unsigned long>(contentHashValue & 0xFFFFFFFFu), deckSuppliedParams ? ", deck params" : "");
  return DeckError::Ok;
}

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
