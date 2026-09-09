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
constexpr uint16_t CPDK_FORMAT_VERSION_IMAGES = 2;
constexpr uint16_t CPDK_FLAG_FSRS_PARAMS = 0x0001;
constexpr uint16_t CPDK_FLAG_HAS_IMAGES = 0x0002;

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

// v2 image-table and placement-table field offsets (DECK_SERVER_SPEC.md §3.7).
constexpr size_t IMAGE_OFF_BLOB_OFF = 0;
constexpr size_t IMAGE_OFF_BYTE_LEN = 4;
constexpr size_t IMAGE_OFF_WIDTH = 8;
constexpr size_t IMAGE_OFF_HEIGHT = 10;
constexpr size_t IMAGE_OFF_RESERVED = 12;
constexpr size_t PLACEMENT_OFF_ORDINAL = 0;
constexpr size_t PLACEMENT_OFF_SIDE = 2;
constexpr size_t PLACEMENT_OFF_RESERVED = 3;
constexpr size_t PLACEMENT_OFF_TEXT_OFFSET = 4;
constexpr size_t PLACEMENT_OFF_IMAGE_INDEX = 8;

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

// The v2 tables are validated in stack chunks on BOTH variants — neither is
// ever held resident (DeckFile.h's v2 note), so the C3's 4 KB streaming rule is
// simply the rule for both. 128 entries is 2048 B of image table or 1536 B of
// placements, in a leaf frame that lives only for the length of open().
constexpr uint32_t V2_VALIDATE_CHUNK_ENTRIES = 128;
static_assert(V2_VALIDATE_CHUNK_ENTRIES * 16 <= 4096, "the v2 table validator must stream in 4 KB or less");

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

/**
 * Walks a JPEG's marker segments to its start-of-frame and checks it against
 * what DECK_SERVER_SPEC.md §3.7's encoding contract promises: SOF0 (baseline),
 * one component (8-bit grayscale), and exactly the dimensions the image table
 * claims.
 *
 * Hand-rolled rather than asked of the decoder, and deliberately the same walk
 * `read_deck.py`'s `jpeg_frame()` does, because the decoder is the thing being
 * protected: JPEGDEC decodes a progressive frame at a DC-only eighth scale
 * (lib/Epub/.../JpegToFramebufferConverter.cpp) and would silently draw a
 * quarter-sized smear where the pager has reserved a full-sized box, and a
 * three-component frame is a colour image this deck should never contain. A
 * mismatch is one refused picture, not a refused deck.
 *
 * The `0xFF` fill-byte run, the standalone markers and the segment-length bounds
 * below are all the format's, not defensive padding: a truncated blob is the
 * realistic corruption here and every step has to stay inside `bytes`.
 */
bool jpegIsBaselineGrayOfSize(const uint8_t* data, size_t bytes, uint16_t width, uint16_t height) {
  if (data == nullptr || bytes < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 1 < bytes) {
    if (data[i] != 0xFF) return false;
    uint8_t marker = data[i + 1];
    i += 2;
    while (marker == 0xFF && i < bytes) marker = data[i++];  // fill bytes
    // TEM and the standalone RSTn markers carry no segment; EOI before any
    // frame means there is nothing to check.
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
      if (marker == 0xD9) return false;
      continue;
    }
    if (i + 2 > bytes) return false;
    const size_t segmentLength = (static_cast<size_t>(data[i]) << 8) | data[i + 1];
    if (segmentLength < 2 || i + segmentLength > bytes) return false;
    // 0xC0..0xCF are the frame markers except DHT (C4), JPG (C8) and DAC (CC).
    const bool isFrame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (isFrame) {
      if (marker != 0xC0) return false;  // progressive / lossless / arithmetic
      if (segmentLength < 8) return false;
      const uint16_t frameHeight = static_cast<uint16_t>((static_cast<uint16_t>(data[i + 3]) << 8) | data[i + 4]);
      const uint16_t frameWidth = static_cast<uint16_t>((static_cast<uint16_t>(data[i + 5]) << 8) | data[i + 6]);
      return data[i + 7] == 1 && frameWidth == width && frameHeight == height;
    }
    if (marker == 0xDA) return false;  // start of scan before any frame
    i += segmentLength;
  }
  return false;
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
    case DeckError::BadImageTable:
      return "bad image table";
    case DeckError::BadPlacement:
      return "bad image placement";
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
  imageCountValue = 0;
  placementCountValue = 0;
  imageTableOffset = 0;
  placementTableOffset = 0;
  imageBlobStart = 0;
  maxImageBytesValue = 0;
  placementWindowFirst = PLACEMENT_WINDOW_EMPTY;
  placementWindowCount = 0;
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
  const bool isImageVersion = formatVersion == CPDK_FORMAT_VERSION_IMAGES;
  if (formatVersion != CPDK_FORMAT_VERSION && !isImageVersion) {
    close();
    return DeckError::BadVersion;
  }
  // Unknown flags bits are the format's version escape hatch: refuse rather
  // than guess what the block after the header means (§2.1). Which bits are
  // known is a property of the VERSION: has_images on a v1 file is an unknown
  // bit, because v1 is exactly the version that does not know what it means.
  const uint16_t knownFlags =
      static_cast<uint16_t>(CPDK_FLAG_FSRS_PARAMS | (isImageVersion ? CPDK_FLAG_HAS_IMAGES : 0));
  if ((flags & static_cast<uint16_t>(~knownFlags)) != 0) {
    close();
    return DeckError::UnknownFlags;
  }
  // A v2 file always carries images: the converter emits v1 when an --images
  // run embedded nothing, so that an image-free deck never becomes a file a v1
  // device refuses (DECK_SERVER_SPEC.md §3.7). A v2 header without the bit is
  // therefore not "v2 with no pictures", it is a file nobody wrote.
  if (isImageVersion && (flags & CPDK_FLAG_HAS_IMAGES) == 0) {
    LOG_ERR("DECK", "v2 header without has_images: %s", path.c_str());
    close();
    return DeckError::BadImageTable;
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

  size_t indexStart = CPDK_HEADER_BYTES + (hasParamsBlock ? CPDK_PARAMS_BYTES : 0);
  // v2 puts the image table and the placement table HERE, between the optional
  // FSRS block and the card index, so both have to be read and validated before
  // the index's own offset is even known (DECK_SERVER_SPEC.md §3.7). Nothing is
  // held: the tables are streamed and then left on the card.
  uint64_t imageBlobLen = 0;
  if (isImageVersion) {
    const DeckError sectionsError = readImageSections(fileSize, cards, indexStart, imageBlobLen);
    if (sectionsError != DeckError::Ok) {
      close();
      return sectionsError;
    }
  }

  const size_t indexBytes = static_cast<size_t>(cards) * CPDK_INDEX_ENTRY_BYTES;
  const size_t blobStartValue = indexStart + indexBytes;  // cards is capped, so this cannot overflow
  if (fileSize < blobStartValue) {
    close();
    return DeckError::ShortFile;
  }

  // The image blob runs to EOF exactly as v1's text blob does, so the text
  // blob's length is what is left between the card index and where the image
  // blob starts — and on a v1 file, where imageBlobLen is 0, that IS EOF and
  // this arithmetic is the v1 arithmetic unchanged. `imageBlobLen` is the blob's
  // EXACT length rather than an upper bound on it, because validateImageTable()
  // proved the table tiles the blob densely (§2.2 I1 pins); the boundary check
  // further down leans on that being exact.
  if (imageBlobLen > fileSize) {
    close();
    return DeckError::ShortFile;
  }
  const size_t imageBlobStartValue = fileSize - static_cast<size_t>(imageBlobLen);
  if (imageBlobStartValue < blobStartValue) {
    LOG_ERR("DECK", "Image blob (%u B) does not fit after the index end %u in %u B: %s",
            static_cast<unsigned>(imageBlobLen), static_cast<unsigned>(blobStartValue), static_cast<unsigned>(fileSize),
            path.c_str());
    close();
    return DeckError::ShortFile;
  }
  const size_t textBlobBytes = imageBlobStartValue - blobStartValue;
  imageBlobStart = static_cast<uint32_t>(imageBlobStartValue);

  // The largest end any card index entry claims, accumulated by the slice
  // validation below. On a v2 file it is checked against the text blob's
  // derived length; see the note at that check.
  uint32_t maxTextEnd = 0;

#ifdef CROSSPOINT_FLASHCARDS_C3
  // No resident index and no allocation: the index stays on the card and is
  // walked here once, in chunks, purely to validate it. Everything keyAt() and
  // loadSide() need afterwards is re-read on demand (FLASHCARD_SPEC.md §7b.3).
  cardCountValue = cards;
  indexFileOffset = static_cast<uint32_t>(indexStart);
  blobStart = static_cast<uint32_t>(blobStartValue);
  blobBytes = static_cast<uint32_t>(textBlobBytes);
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
        if (frontOff + frontLen > maxTextEnd) maxTextEnd = frontOff + frontLen;
        if (backOff + backLen > maxTextEnd) maxTextEnd = backOff + backLen;
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
  // Seek rather than read on: the v2 table validation above walks the file, so
  // the cursor is only still sitting at the index on a v1 deck.
  if (!file.seek(indexStart)) {
    close();
    return DeckError::ReadFailed;
  }
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
  blobBytes = static_cast<uint32_t>(textBlobBytes);

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
    if (frontOff + frontLen > maxTextEnd) maxTextEnd = frontOff + frontLen;
    if (backOff + backLen > maxTextEnd) maxTextEnd = backOff + backLen;
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

  // The two blob boundaries have to AGREE, and only a v2 file has two of them.
  //
  // The card index says where the text blob ends (`maxTextEnd`, the largest end
  // any slice claims — the converter writes the slices back to back, so the last
  // one ends at the blob's end). The image table says where the image blob
  // starts, by way of the EOF anchor: `file_size - blob_length`, with
  // `blob_length` now exact because validateImageTable() proved the blob tiles.
  // A well-formed v2 file therefore satisfies
  //   file_size == index_end + text_blob + image_blob
  // exactly, and one stray byte appended to the download breaks it. Without this
  // the stray byte is INVISIBLE: it slides the whole EOF-anchored image blob by
  // one, every image reads a byte off, every sniff fails, and the card renders
  // as a full set of placeholder boxes with no error anywhere (§2.2 I1 pins,
  // "closes the EOF-anchored blob's one-stray-byte fragility structurally").
  //
  // v1 is deliberately left alone: its single blob is anchored at the END OF THE
  // INDEX, not at EOF, so trailing slack there cannot move a single byte of card
  // text and refusing it would only reject files this reader has always read.
  if (isImageVersion && maxTextEnd != blobBytes) {
    LOG_ERR("DECK", "Text blob is %u B but the index fills %u of them; the image blob is misplaced: %s",
            static_cast<unsigned>(blobBytes), static_cast<unsigned>(maxTextEnd), path.c_str());
    close();
    return DeckError::BadImageTable;
  }

  // Placements last: they are checked against the card index (a text_offset has
  // to be inside the side it names), so this is the first point at which the
  // index is available to ask, on either variant.
  if (placementCountValue != 0) {
    const DeckError placementError = validatePlacements();
    if (placementError != DeckError::Ok) {
      close();
      return placementError;
    }
  }

  LOG_INF("DECK", "Opened %s: %u cards, hash 0x%08lx%08lx%s%s", path.c_str(), static_cast<unsigned>(cards),
          static_cast<unsigned long>(contentHashValue >> 32),
          static_cast<unsigned long>(contentHashValue & 0xFFFFFFFFu), deckSuppliedParams ? ", deck params" : "",
          imageCountValue != 0 ? ", v2 images" : "");
  return DeckError::Ok;
}

// --- CPDK v2 image sections --------------------------------------------------

DeckError DeckFile::readImageSections(const size_t fileSize, const uint32_t cards, size_t& offsetInOut,
                                      uint64_t& imageBlobLenOut) {
  size_t offset = offsetInOut;

  // The cap on BOTH v2 tables is four images per SIDE over the two sides of
  // every card (FLASHCARD_SPEC.md §2.2, I1 pins). It is written out here once,
  // in the same shape the sync screen's pre-rename gate writes it
  // (FlashcardSyncActivity.cpp), rather than kept as two constants that can
  // drift apart: a 4x image cap next to an 8x placement cap is a bound the
  // converter cannot satisfy, which is exactly the drift that happened.
  const uint32_t sectionCap = 2u * DECK_MAX_IMAGES_PER_SIDE * cards;

  // Both counts are read before either is used to size anything, and neither is
  // ever multiplied out to test whether its table fits: a count off the card is
  // a raw u32, so the section bounds are computed DIVISION-style against the
  // bytes actually left in the file (`count <= remaining / entry_size`), which
  // cannot overflow whatever the count says (§2.2 I1 pins).
  if (!file.seek(offset)) return DeckError::ReadFailed;
  uint8_t counts[4];
  if (fileSize < offset + sizeof(counts)) return DeckError::ShortFile;
  if (file.read(counts, sizeof(counts)) != static_cast<int>(sizeof(counts))) return DeckError::ReadFailed;
  const uint32_t images = readU32(counts, 0);
  offset += sizeof(counts);

  // `image_count >= 1` is the has_images gate restated: a v2 file with an empty
  // table is the file the converter refuses to write (§3.7).
  if (images == 0 || images > sectionCap) {
    LOG_ERR("DECK", "image_count %u out of range for %u cards", static_cast<unsigned>(images),
            static_cast<unsigned>(cards));
    return DeckError::BadImageTable;
  }
  if (images > (fileSize - offset) / CPDK_IMAGE_ENTRY_BYTES) return DeckError::ShortFile;
  imageTableOffset = static_cast<uint32_t>(offset);
  imageCountValue = images;
  offset += static_cast<size_t>(images) * CPDK_IMAGE_ENTRY_BYTES;  // proved to fit by the division above

  if (fileSize < offset + sizeof(counts)) return DeckError::ShortFile;
  if (!file.seek(offset)) return DeckError::ReadFailed;
  if (file.read(counts, sizeof(counts)) != static_cast<int>(sizeof(counts))) return DeckError::ReadFailed;
  const uint32_t placements = readU32(counts, 0);
  offset += sizeof(counts);

  // §3.7 caps placements per (ordinal, side) at 4 rather than capping the table
  // as a whole; four per side over two sides of every card IS that cap, restated
  // as the bound this has to know before it walks the table. The per-side count
  // is still checked entry by entry in validatePlacements().
  if (placements > sectionCap) {
    LOG_ERR("DECK", "placement_count %u out of range for %u cards", static_cast<unsigned>(placements),
            static_cast<unsigned>(cards));
    return DeckError::BadPlacement;
  }
  if (placements > (fileSize - offset) / CPDK_PLACEMENT_ENTRY_BYTES) return DeckError::ShortFile;
  placementTableOffset = static_cast<uint32_t>(offset);
  placementCountValue = placements;
  offset += static_cast<size_t>(placements) * CPDK_PLACEMENT_ENTRY_BYTES;

  const DeckError tableError = validateImageTable(fileSize, imageBlobLenOut);
  if (tableError != DeckError::Ok) return tableError;

  offsetInOut = offset;
  return DeckError::Ok;
}

/**
 * Streams the image table, checking every entry AND the one structural property
 * that makes an EOF-anchored blob safe to address: the blob is DENSE.
 *
 * The image blob has no length field of its own — it runs to EOF, mirroring
 * v1's text blob — so its start is derived as `file_size - blob_length` and
 * every `blob_off` is read relative to that. Nothing in an entry can therefore
 * detect that the file grew by a byte: the whole blob simply shifts, every
 * image reads one byte off, and a deck of pictures silently becomes a deck of
 * placeholder boxes. §2.2's I1 pins close that structurally by making the blob
 * DENSE — table order is blob order, the first entry sits at offset 0, and each
 * entry begins exactly where the one before it ended — which is checked here,
 * from the table alone. `blobLen` then stops being "the largest end anyone
 * claims" and becomes the blob's exact length, and open() can hold the whole
 * file's length to the sum of its own parts rather than to an inequality.
 */
DeckError DeckFile::validateImageTable(const size_t fileSize, uint64_t& imageBlobLenOut) {
  uint8_t chunk[V2_VALIDATE_CHUNK_ENTRIES * CPDK_IMAGE_ENTRY_BYTES];
  uint64_t blobLen = 0;  // == where the previous entry ended, i.e. where this one must start
  uint32_t largest = 0;

  if (!file.seek(imageTableOffset)) return DeckError::ReadFailed;
  for (uint32_t first = 0; first < imageCountValue; first += V2_VALIDATE_CHUNK_ENTRIES) {
    uint32_t count = imageCountValue - first;
    if (count > V2_VALIDATE_CHUNK_ENTRIES) count = V2_VALIDATE_CHUNK_ENTRIES;
    const size_t bytes = static_cast<size_t>(count) * CPDK_IMAGE_ENTRY_BYTES;
    if (file.read(chunk, bytes) != static_cast<int>(bytes)) return DeckError::ReadFailed;
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t* const entry = chunk + static_cast<size_t>(i) * CPDK_IMAGE_ENTRY_BYTES;
      const uint32_t blobOff = readU32(entry, IMAGE_OFF_BLOB_OFF);
      const uint32_t byteLen = readU32(entry, IMAGE_OFF_BYTE_LEN);
      const uint16_t width = readU16(entry, IMAGE_OFF_WIDTH);
      const uint16_t height = readU16(entry, IMAGE_OFF_HEIGHT);
      const uint32_t reserved = readU32(entry, IMAGE_OFF_RESERVED);
      const bool ok = reserved == 0 && byteLen != 0 && byteLen <= DECK_MAX_IMAGE_BYTES && width != 0 &&
                      width <= DECK_MAX_IMAGE_EDGE && height != 0 && height <= DECK_MAX_IMAGE_EDGE;
      if (!ok) {
        LOG_ERR("DECK", "Image %u invalid: %ux%u, %u B, reserved %u", static_cast<unsigned>(first + i),
                static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(byteLen),
                static_cast<unsigned>(reserved));
        return DeckError::BadImageTable;
      }
      // The blob tiles: entry 0 starts at 0, and every entry after it starts
      // where its predecessor ended. A gap, an overlap, a non-zero first offset
      // and a table written in a different order from the blob all land here as
      // one comparison, because `blobLen` IS the previous entry's end.
      if (blobOff != blobLen) {
        LOG_ERR("DECK", "Image %u starts at %u, not at %u: the image blob is not densely tiled",
                static_cast<unsigned>(first + i), static_cast<unsigned>(blobOff), static_cast<unsigned>(blobLen));
        return DeckError::BadImageTable;
      }
      // 64-bit so the sum of two u32s cannot wrap back inside the file; whether
      // the file is long enough for the blob this adds up to is open()'s check.
      const uint64_t end = static_cast<uint64_t>(blobOff) + byteLen;
      if (end > fileSize) {
        LOG_ERR("DECK", "Image %u runs to %u, past the %u B file", static_cast<unsigned>(first + i),
                static_cast<unsigned>(end), static_cast<unsigned>(fileSize));
        return DeckError::BadImageTable;
      }
      blobLen = end;
      if (byteLen > largest) largest = byteLen;
    }
  }

  imageBlobLenOut = blobLen;
  maxImageBytesValue = largest;
  return DeckError::Ok;
}

DeckError DeckFile::validatePlacements() {
  uint8_t chunk[V2_VALIDATE_CHUNK_ENTRIES * CPDK_PLACEMENT_ENTRY_BYTES];
  // The sort is by (ordinal, side, text_offset) and is NON-decreasing, so the
  // run length below only has to count while all three parts of the key hold
  // still — a repeat of the whole key is a legal tie, and two images at one
  // offset are exactly what a tie means.
  bool havePrevious = false;
  uint16_t previousOrdinal = 0;
  uint8_t previousSide = 0;
  uint32_t previousTextOffset = 0;
  uint32_t sideRun = 0;

  if (!file.seek(placementTableOffset)) return DeckError::ReadFailed;
  for (uint32_t first = 0; first < placementCountValue; first += V2_VALIDATE_CHUNK_ENTRIES) {
    uint32_t count = placementCountValue - first;
    if (count > V2_VALIDATE_CHUNK_ENTRIES) count = V2_VALIDATE_CHUNK_ENTRIES;
    const size_t bytes = static_cast<size_t>(count) * CPDK_PLACEMENT_ENTRY_BYTES;
    // Re-seek every chunk: sliceAt() below is file I/O on the C3 variant, so
    // the cursor this loop left behind is not the cursor it comes back to.
    if (!file.seek(placementTableOffset + static_cast<size_t>(first) * CPDK_PLACEMENT_ENTRY_BYTES)) {
      return DeckError::ReadFailed;
    }
    if (file.read(chunk, bytes) != static_cast<int>(bytes)) return DeckError::ReadFailed;
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t* const entry = chunk + static_cast<size_t>(i) * CPDK_PLACEMENT_ENTRY_BYTES;
      const uint16_t ordinal = readU16(entry, PLACEMENT_OFF_ORDINAL);
      const uint8_t side = entry[PLACEMENT_OFF_SIDE];
      const uint8_t reserved = entry[PLACEMENT_OFF_RESERVED];
      const uint32_t textOffset = readU32(entry, PLACEMENT_OFF_TEXT_OFFSET);
      const uint32_t imageIndex = readU32(entry, PLACEMENT_OFF_IMAGE_INDEX);

      if (reserved != 0 || side > 1 || ordinal >= cardCountValue || imageIndex >= imageCountValue) {
        LOG_ERR("DECK", "Placement %u invalid: card %u side %u image %u reserved %u", static_cast<unsigned>(first + i),
                static_cast<unsigned>(ordinal), static_cast<unsigned>(side), static_cast<unsigned>(imageIndex),
                static_cast<unsigned>(reserved));
        return DeckError::BadPlacement;
      }

      if (havePrevious) {
        const bool ordered = ordinal > previousOrdinal ||
                             (ordinal == previousOrdinal &&
                              (side > previousSide || (side == previousSide && textOffset >= previousTextOffset)));
        if (!ordered) {
          LOG_ERR("DECK", "Placement %u out of order: (%u,%u,%u) after (%u,%u,%u)", static_cast<unsigned>(first + i),
                  static_cast<unsigned>(ordinal), static_cast<unsigned>(side), static_cast<unsigned>(textOffset),
                  static_cast<unsigned>(previousOrdinal), static_cast<unsigned>(previousSide),
                  static_cast<unsigned>(previousTextOffset));
          return DeckError::BadPlacement;
        }
      }
      const bool sameSide = havePrevious && ordinal == previousOrdinal && side == previousSide;
      sideRun = sameSide ? sideRun + 1 : 1;
      if (sideRun > DECK_MAX_IMAGES_PER_SIDE) {
        LOG_ERR("DECK", "Card %u side %u carries more than %u images", static_cast<unsigned>(ordinal),
                static_cast<unsigned>(side), static_cast<unsigned>(DECK_MAX_IMAGES_PER_SIDE));
        return DeckError::BadPlacement;
      }

      // The offset has to be inside the side it names — `== length` is the
      // legal "below all the text" placement, and a side of length 0 with a
      // placement at 0 is an image-only card side (§3.7).
      uint32_t sliceOffset = 0;
      uint16_t sliceLength = 0;
      const CardSide cardSide = side == 0 ? CardSide::Front : CardSide::Back;
      if (!sliceAt(static_cast<Ordinal>(ordinal), cardSide, sliceOffset, sliceLength)) return DeckError::ReadFailed;
      if (textOffset > sliceLength) {
        LOG_ERR("DECK", "Placement %u at %u is past the %u B side of card %u", static_cast<unsigned>(first + i),
                static_cast<unsigned>(textOffset), static_cast<unsigned>(sliceLength), static_cast<unsigned>(ordinal));
        return DeckError::BadPlacement;
      }

      havePrevious = true;
      previousOrdinal = ordinal;
      previousSide = side;
      previousTextOffset = textOffset;
    }
  }
  return DeckError::Ok;
}

bool DeckFile::placementAt(const uint32_t slot, const uint8_t*& entryOut) {
  entryOut = nullptr;
  if (slot >= placementCountValue) return false;

  const uint32_t first = (slot / PLACEMENT_WINDOW_ENTRIES) * PLACEMENT_WINDOW_ENTRIES;
  if (placementWindowFirst != first || placementWindowCount == 0) {
    uint32_t count = placementCountValue - first;
    if (count > PLACEMENT_WINDOW_ENTRIES) count = PLACEMENT_WINDOW_ENTRIES;
    const size_t bytes = static_cast<size_t>(count) * CPDK_PLACEMENT_ENTRY_BYTES;
    placementWindowFirst = PLACEMENT_WINDOW_EMPTY;
    placementWindowCount = 0;
    if (!file.seek(placementTableOffset + static_cast<size_t>(first) * CPDK_PLACEMENT_ENTRY_BYTES)) return false;
    if (file.read(placementWindow, bytes) != static_cast<int>(bytes)) {
      LOG_ERR("DECK", "Placement read failed at %u", static_cast<unsigned>(first));
      return false;
    }
    placementWindowFirst = first;
    placementWindowCount = count;
  }
  // Bounded by what was actually READ, not by the window's capacity — the last
  // window is short, and this is what makes an overrun structurally impossible.
  const uint32_t offsetInWindow = slot - first;
  if (offsetInWindow >= placementWindowCount) return false;
  entryOut = placementWindow + static_cast<size_t>(offsetInWindow) * CPDK_PLACEMENT_ENTRY_BYTES;
  return true;
}

bool DeckFile::imageAt(const uint32_t imageIndex, DeckImage& out) {
  if (imageIndex >= imageCountValue) return false;
  uint8_t entry[CPDK_IMAGE_ENTRY_BYTES];
  if (!file.seek(imageTableOffset + static_cast<size_t>(imageIndex) * CPDK_IMAGE_ENTRY_BYTES)) return false;
  if (file.read(entry, sizeof(entry)) != static_cast<int>(sizeof(entry))) return false;
  // Every field was range-checked at open, so this only has to resolve the
  // blob-relative offset into the absolute one the caller reads at.
  out.fileOffset = imageBlobStart + readU32(entry, IMAGE_OFF_BLOB_OFF);
  out.byteLength = readU32(entry, IMAGE_OFF_BYTE_LEN);
  out.width = readU16(entry, IMAGE_OFF_WIDTH);
  out.height = readU16(entry, IMAGE_OFF_HEIGHT);
  return true;
}

uint8_t DeckFile::imagesForSide(const Ordinal ordinal, const CardSide side, DeckImagePlacement* const out,
                                const uint8_t capacity) {
  // The v1 fast path, and the reason no caller needs a version test: an
  // image-free deck answers without touching the card.
  if (out == nullptr || capacity == 0 || placementCountValue == 0 || ordinal >= cardCountValue) return 0;
  // A side holds at most four however much room the caller offered.
  const uint8_t limit = capacity < DECK_MAX_IMAGES_PER_SIDE ? capacity : DECK_MAX_IMAGES_PER_SIDE;

  const uint8_t wantSide = side == CardSide::Front ? 0 : 1;
  // Lower bound on (ordinal, side) over a table open() proved sorted. The
  // window makes each probe cost at most one 192-byte read, and leaves the
  // run's own entries loaded for the forward walk below.
  uint32_t low = 0;
  uint32_t high = placementCountValue;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    const uint8_t* entry = nullptr;
    if (!placementAt(mid, entry)) return 0;
    const uint16_t midOrdinal = readU16(entry, PLACEMENT_OFF_ORDINAL);
    const uint8_t midSide = entry[PLACEMENT_OFF_SIDE];
    if (midOrdinal < ordinal || (midOrdinal == ordinal && midSide < wantSide)) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  uint8_t found = 0;
  // cppcheck-suppress knownConditionTrueFalse ; `capacity == 0` returned above,
  // so `limit` is 1..4 here -- cppcheck 2.11 carries the unconstrained
  // parameter's possible 0 past that guard (the same false positive it reports
  // on src/components/OptionPopup.h's row loop).
  for (uint32_t slot = low; slot < placementCountValue && found < limit; slot++) {
    const uint8_t* entry = nullptr;
    if (!placementAt(slot, entry)) return found;
    if (readU16(entry, PLACEMENT_OFF_ORDINAL) != ordinal || entry[PLACEMENT_OFF_SIDE] != wantSide) break;
    const uint32_t textOffset = readU32(entry, PLACEMENT_OFF_TEXT_OFFSET);
    const uint32_t imageIndex = readU32(entry, PLACEMENT_OFF_IMAGE_INDEX);
    DeckImage image{};
    // A failed image-table read costs this picture and the ones behind it, not
    // the card: the caller draws what it got and a placeholder for the rest.
    if (!imageAt(imageIndex, image)) return found;
    out[found].textOffset = textOffset;
    out[found].image = image;
    found++;
  }
  return found;
}

bool DeckFile::loadImage(const DeckImage& image, uint8_t* const buffer, const size_t bufferBytes) {
  if (buffer == nullptr || image.byteLength == 0 || bufferBytes < image.byteLength) return false;
  if (!file.seek(image.fileOffset)) return false;
  if (file.read(buffer, image.byteLength) != static_cast<int>(image.byteLength)) return false;
  // The sniff open() could not afford (one seek per image over the whole blob)
  // happens here instead, where the bytes are already in hand. See DeckFile.h.
  if (!jpegIsBaselineGrayOfSize(buffer, image.byteLength, image.width, image.height)) {
    LOG_ERR("DECK", "Image at %u is not a baseline %ux%u grayscale JPEG", static_cast<unsigned>(image.fileOffset),
            static_cast<unsigned>(image.width), static_cast<unsigned>(image.height));
    return false;
  }
  return true;
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
