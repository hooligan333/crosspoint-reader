#include "StateStore.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "DeckPaths.h"
#include "KeySort.h"

namespace flashcards {
namespace {

// CPST v1 header field offsets (FLASHCARD_SPEC.md §3).
constexpr size_t OFF_VERSION = 4;
constexpr size_t OFF_CARD_COUNT = 8;
constexpr size_t OFF_CONTENT_HASH = 12;
constexpr size_t OFF_COUNTERS_DAY = 20;
constexpr size_t OFF_NEW_TODAY = 24;
constexpr size_t OFF_REV_TODAY = 26;
constexpr size_t OFF_LAST_SEEN_DAY = 28;

constexpr size_t RECORD_KEY_BYTES = 8;
constexpr size_t RECORD_PAYLOAD_BYTES = 20;
constexpr size_t PAYLOAD_OFF_STATE = 14;  // the state byte inside the payload

// Records per read/write for the streaming paths (create, merge, scan). 128
// records is 3584 bytes = exactly seven 512-byte sectors, and because record 0
// starts at CPST_RECORDS_OFFSET (itself sector-aligned) every chunk boundary
// lands on a sector boundary — the SD driver never has to split one.
//
// This is a chunk SIZE, unrelated to the spec's torn-sector risk note: that
// note counts the ~18 records a single 512-byte sector spans, which is the blast
// radius of one bad write and has nothing to do with how much is read at a time.
//
// Cost: 3584 bytes of stack in a leaf function, on cold paths only (session
// start, merge, first study). Sized deliberately over the 504 bytes one sector
// would give, because a 40000-card scan is 2223 reads at 128/chunk against
// 15790 at 18/chunk.
constexpr size_t RECORDS_PER_CHUNK = 128;
constexpr size_t CHUNK_BYTES = RECORDS_PER_CHUNK * CPST_RECORD_BYTES;
static_assert(CHUNK_BYTES % 512 == 0, "record chunks must stay sector-aligned");
static_assert(CPST_RECORDS_OFFSET % 512 == 0, "record 0 must start on a sector boundary");

/** Absolute file offset of `ordinal`'s record. */
size_t recordOffset(Ordinal ordinal) { return CPST_RECORDS_OFFSET + static_cast<size_t>(ordinal) * CPST_RECORD_BYTES; }

// The record payload IS an fsrs::CardState: the library pins the field offsets
// with its own static_asserts, and these two say the file format agrees.
static_assert(sizeof(fsrs::CardState) == RECORD_PAYLOAD_BYTES, "CPST record payload is a CardState");
static_assert(RECORD_KEY_BYTES + RECORD_PAYLOAD_BYTES == CPST_RECORD_BYTES, "CPST record is key + payload");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "CPST fields are little-endian; this host is not");

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

void writeU16(uint8_t* bytes, size_t offset, uint16_t value) { memcpy(bytes + offset, &value, sizeof(value)); }
void writeU32(uint8_t* bytes, size_t offset, uint32_t value) { memcpy(bytes + offset, &value, sizeof(value)); }
void writeU64(uint8_t* bytes, size_t offset, uint64_t value) { memcpy(bytes + offset, &value, sizeof(value)); }

/**
 * An unknown state byte reads as New — a corrupt record restarts its card
 * rather than scheduling it from garbage (FLASHCARD_SPEC.md §4). The scheduler
 * hardens the same way internally; this keeps the queue builder's switches
 * honest and stops the bad byte from being written back out.
 */
void normalizeState(uint8_t* payload) {
  if (payload[PAYLOAD_OFF_STATE] > static_cast<uint8_t>(fsrs::CardPhase::Relearning)) {
    payload[PAYLOAD_OFF_STATE] = static_cast<uint8_t>(fsrs::CardPhase::New);
  }
}

/** Card count a file of `fileSize` bytes can actually hold, capped at the deck limit. */
uint32_t recordsThatFit(size_t fileSize) {
  if (fileSize < CPST_RECORDS_OFFSET) return 0;
  const size_t fits = (fileSize - CPST_RECORDS_OFFSET) / CPST_RECORD_BYTES;
  return fits > DECK_MAX_CARDS ? DECK_MAX_CARDS : static_cast<uint32_t>(fits);
}

}  // namespace

const char* stateErrorName(StateError error) {
  switch (error) {
    case StateError::Ok:
      return "ok";
    case StateError::OpenFailed:
      return "open failed";
    case StateError::CreateFailed:
      return "create failed";
    case StateError::ReadFailed:
      return "read failed";
    case StateError::WriteFailed:
      return "write failed";
    case StateError::OutOfMemory:
      return "out of memory";
  }
  return "unknown";
}

void StateStore::close() {
  file = HalFile();
  deck = nullptr;
  recordCountValue = 0;
  newTodayValue = 0;
  reviewsTodayValue = 0;
  countersDayValue = 0;
  lastSeenDayValue = 0;
  mismatchSeen = false;
}

StateError StateStore::open(const DeckFile& deckRef, const std::string& destFolder, const std::string& deckLeaf,
                            uint16_t today) {
  close();
  if (!deckRef.isOpen()) return StateError::OpenFailed;
  deck = &deckRef;

  const std::string path = statePathFor(destFolder, deckLeaf);
  adoptOrphanedTemp(path);

  // What is already on the card decides which of the three paths runs. A file
  // that will not open, is too short for its own header, or carries a header
  // this build does not recognise is discarded rather than refused: losing a
  // deck's scheduling history is recoverable, refusing to study it is not.
  bool adopt = false;
  bool merge = false;
  uint32_t oldCount = 0;
  {
    HalFile existing;
    if (Storage.openFileForRead("DECK", path, existing)) {
      const size_t fileSize = existing.fileSize();
      uint8_t header[CPST_HEADER_BYTES];
      if (fileSize >= CPST_HEADER_BYTES && existing.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
          memcmp(header, "CPST", 4) == 0 && readU16(header, OFF_VERSION) == CPST_VERSION) {
        const uint32_t claimed = readU32(header, OFF_CARD_COUNT);
        const uint32_t held = recordsThatFit(fileSize);
        oldCount = claimed < held ? claimed : held;  // a truncated file holds what it holds
        const uint64_t hash = readU64(header, OFF_CONTENT_HASH);

        // Counters carry across a merge: a re-download is not a new day and
        // must not hand back a spent allowance.
        countersDayValue = static_cast<uint16_t>(readU32(header, OFF_COUNTERS_DAY));
        newTodayValue = readU16(header, OFF_NEW_TODAY);
        reviewsTodayValue = readU16(header, OFF_REV_TODAY);
        lastSeenDayValue = static_cast<uint16_t>(readU32(header, OFF_LAST_SEEN_DAY));

        const bool sameDeck = hash == deck->contentHash() && claimed == deck->cardCount() && held >= deck->cardCount();
        adopt = sameDeck;
        merge = !sameDeck;
      } else {
        LOG_ERR("DECK", "State header unusable, rebuilding: %s", path.c_str());
      }
    }
  }

  StateError result = StateError::Ok;
  if (adopt) {
    file = Storage.open(path.c_str(), O_RDWR);
    if (!file) {
      LOG_ERR("DECK", "State file will not open for update: %s", path.c_str());
      result = StateError::OpenFailed;
    }
  } else if (merge) {
    LOG_INF("DECK", "Deck content changed, merging state: %s (%u -> %u cards)", path.c_str(),
            static_cast<unsigned>(oldCount), static_cast<unsigned>(deck->cardCount()));
    result = mergeFromExisting(path, oldCount);
  } else {
    // No usable file: a first study session, or a header this build rejected.
    // Fresh counters, since there is nothing to carry over.
    countersDayValue = today;
    newTodayValue = 0;
    reviewsTodayValue = 0;
    lastSeenDayValue = today;
    result = createFresh(path, today);
  }
  if (result != StateError::Ok) {
    close();
    return result;
  }

  recordCountValue = deck->cardCount();
  if (!applyDayRollover(today)) {
    close();
    return StateError::WriteFailed;
  }
  return StateError::Ok;
}

void StateStore::adoptOrphanedTemp(const std::string& path) {
  // The merge below writes `path`.tmp, removes `path`, then renames. A power
  // cut between those last two steps leaves the COMPLETE merged state sitting
  // in the temp with nothing pointing at it, and the old code would have run
  // createFresh over the top of it — losing a deck's entire history while its
  // replacement was on the card. A rename that simply returned false leaves the
  // same orphan. So: if the state file is gone and a temp is there, the temp IS
  // the state file (FLASHCARD_SPEC.md §3).
  //
  // Adoption is unconditional and unvalidated on purpose. A TORN temp is not a
  // special case: it fails the magic/version/size checks in open() exactly as a
  // torn .state would, and falls through to a fresh file. Checking it here
  // would only duplicate those checks.
  const std::string tmpPath = path + ".tmp";
  if (Storage.exists(path.c_str()) || !Storage.exists(tmpPath.c_str())) return;
  if (Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_INF("DECK", "Adopted an orphaned merge temp: %s", path.c_str());
  } else {
    LOG_ERR("DECK", "Could not adopt orphaned merge temp: %s", tmpPath.c_str());
  }
}

StateError StateStore::createFresh(const std::string& path, uint16_t today) {
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string dir = path.substr(0, slash);
    // R3 owns this mkdir: the sync activity only ever deletes from .state (§3).
    if (!dir.empty() && !Storage.ensureDirectoryExists(dir.c_str())) {
      LOG_ERR("DECK", "Could not create state directory: %s", dir.c_str());
      return StateError::CreateFailed;
    }
  }

  if (!Storage.openFileForWrite("DECK", path, file)) {
    LOG_ERR("DECK", "Could not create state file: %s", path.c_str());
    return StateError::CreateFailed;
  }
  recordCountValue = deck->cardCount();
  countersDayValue = today;
  lastSeenDayValue = today;
  if (!writeHeaderAndPadding()) return StateError::WriteFailed;

  // Every card starts New: a zeroed payload IS the New state (state byte 0,
  // no reviews, no flags), so only the key varies per record.
  uint8_t chunk[CHUNK_BYTES];
  memset(chunk, 0, sizeof(chunk));
  const uint32_t cards = deck->cardCount();
  for (uint32_t first = 0; first < cards; first += RECORDS_PER_CHUNK) {
    uint32_t count = cards - first;
    if (count > RECORDS_PER_CHUNK) count = RECORDS_PER_CHUNK;
    for (uint32_t i = 0; i < count; i++) {
      writeU64(chunk, i * CPST_RECORD_BYTES, deck->keyAt(static_cast<Ordinal>(first + i)));
      memset(chunk + i * CPST_RECORD_BYTES + RECORD_KEY_BYTES, 0, RECORD_PAYLOAD_BYTES);
    }
    const size_t bytes = count * CPST_RECORD_BYTES;
    if (file.write(chunk, bytes) != bytes) {
      LOG_ERR("DECK", "Short write initialising state: %s", path.c_str());
      return StateError::WriteFailed;
    }
  }
  file.flush();
  LOG_INF("DECK", "Created state for %u cards: %s", static_cast<unsigned>(cards), path.c_str());
  return StateError::Ok;
}

StateError StateStore::mergeFromExisting(const std::string& path, uint32_t oldCount) {
  // The old state, sorted by card key, so the new deck's index can be walked
  // once against it (FLASHCARD_SPEC.md §3). Three allocations rather than one
  // array of structs: a {u64, u16, u8[20]} would pad to 32 bytes per card.
  std::unique_ptr<uint64_t[]> oldKeys;
  std::unique_ptr<uint16_t[]> oldSlots;
  std::unique_ptr<uint8_t[]> oldPayloads;
  uint8_t* payloadBase = nullptr;
  if (oldCount > 0) {
    oldKeys = makeUniqueNoThrow<uint64_t[]>(oldCount);
    oldSlots = makeUniqueNoThrow<uint16_t[]>(oldCount);
    oldPayloads = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(oldCount) * RECORD_PAYLOAD_BYTES);
    if (!oldKeys || !oldSlots || !oldPayloads) {
      LOG_ERR("DECK", "Merge buffers alloc failed for %u records", static_cast<unsigned>(oldCount));
      return StateError::OutOfMemory;
    }

    payloadBase = oldPayloads.get();
    HalFile old;
    if (!Storage.openFileForRead("DECK", path, old)) return StateError::OpenFailed;
    if (!old.seek(CPST_RECORDS_OFFSET)) return StateError::ReadFailed;

    uint8_t chunk[CHUNK_BYTES];
    for (uint32_t first = 0; first < oldCount; first += RECORDS_PER_CHUNK) {
      uint32_t count = oldCount - first;
      if (count > RECORDS_PER_CHUNK) count = RECORDS_PER_CHUNK;
      const size_t bytes = count * CPST_RECORD_BYTES;
      if (old.read(chunk, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("DECK", "Short read merging state: %s", path.c_str());
        return StateError::ReadFailed;
      }
      for (uint32_t i = 0; i < count; i++) {
        const uint8_t* record = chunk + i * CPST_RECORD_BYTES;
        const uint32_t slot = first + i;
        oldKeys[slot] = readU64(record, 0);
        oldSlots[slot] = static_cast<uint16_t>(slot);
        uint8_t* payload = payloadBase + static_cast<size_t>(slot) * RECORD_PAYLOAD_BYTES;
        memcpy(payload, record + RECORD_KEY_BYTES, RECORD_PAYLOAD_BYTES);
        normalizeState(payload);
      }
    }
    sortKeys(oldKeys.get(), oldSlots.get(), oldCount);
  }

  // The merged file is built beside the old one and renamed over it. An
  // interrupted merge costs the temp file and nothing else; a crash inside the
  // remove→rename window at the end costs nothing either, because the next
  // open() adopts the orphaned temp (adoptOrphanedTemp).
  const std::string tmpPath = path + ".tmp";
  recordCountValue = deck->cardCount();
  bool tempComplete = false;
  ScopedCleanup dropTemp{[&] {
    if (tempComplete) return;
    file = HalFile();  // the temp must be closed before it can be removed
    Storage.remove(tmpPath.c_str());
  }};
  {
    if (!Storage.openFileForWrite("DECK", tmpPath, file)) {
      LOG_ERR("DECK", "Could not open merge temp: %s", tmpPath.c_str());
      return StateError::CreateFailed;
    }
    if (!writeHeaderAndPadding()) return StateError::WriteFailed;

    uint8_t chunk[CHUNK_BYTES];
    const uint32_t cards = deck->cardCount();
    uint32_t carried = 0;
    for (uint32_t first = 0; first < cards; first += RECORDS_PER_CHUNK) {
      uint32_t count = cards - first;
      if (count > RECORDS_PER_CHUNK) count = RECORDS_PER_CHUNK;
      for (uint32_t i = 0; i < count; i++) {
        const uint64_t key = deck->keyAt(static_cast<Ordinal>(first + i));
        uint8_t* record = chunk + i * CPST_RECORD_BYTES;
        writeU64(record, 0, key);
        const uint32_t found = oldCount > 0 ? findKey(oldKeys.get(), oldCount, key) : KEY_NOT_FOUND;
        if (found == KEY_NOT_FOUND) {
          // A card the new deck added: starts New, like any unseen card.
          memset(record + RECORD_KEY_BYTES, 0, RECORD_PAYLOAD_BYTES);
        } else {
          memcpy(record + RECORD_KEY_BYTES, payloadBase + static_cast<size_t>(oldSlots[found]) * RECORD_PAYLOAD_BYTES,
                 RECORD_PAYLOAD_BYTES);
          carried++;
        }
      }
      const size_t bytes = count * CPST_RECORD_BYTES;
      if (file.write(chunk, bytes) != bytes) {
        LOG_ERR("DECK", "Short write merging state: %s", tmpPath.c_str());
        return StateError::WriteFailed;
      }
    }
    file.flush();
    LOG_INF("DECK", "Merged state: %u of %u cards kept their schedule", static_cast<unsigned>(carried),
            static_cast<unsigned>(cards));
    // The temp file must be closed before the rename: SdFat will not rename a
    // path that still has an open handle.
    file = HalFile();
    tempComplete = true;
  }

  oldKeys.reset();
  oldSlots.reset();
  oldPayloads.reset();

  Storage.remove(path.c_str());
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    // The temp is deliberately NOT removed: it is now the only copy of the
    // merged state, and the next open() adopts it.
    LOG_ERR("DECK", "Could not rename merged state into place, leaving the temp: %s", tmpPath.c_str());
    return StateError::WriteFailed;
  }
  file = Storage.open(path.c_str(), O_RDWR);
  if (!file) return StateError::OpenFailed;
  return StateError::Ok;
}

bool StateStore::writeHeader() {
  uint8_t header[CPST_HEADER_BYTES];
  memset(header, 0, sizeof(header));
  memcpy(header, "CPST", 4);
  writeU16(header, OFF_VERSION, CPST_VERSION);
  writeU32(header, OFF_CARD_COUNT, recordCountValue);
  writeU64(header, OFF_CONTENT_HASH, deck != nullptr ? deck->contentHash() : 0);
  writeU32(header, OFF_COUNTERS_DAY, countersDayValue);
  writeU16(header, OFF_NEW_TODAY, newTodayValue);
  writeU16(header, OFF_REV_TODAY, reviewsTodayValue);
  writeU32(header, OFF_LAST_SEEN_DAY, lastSeenDayValue);

  if (!file.seek(0)) return false;
  if (file.write(header, sizeof(header)) != sizeof(header)) return false;
  file.flush();
  return true;
}

bool StateStore::writeHeaderAndPadding() {
  if (!writeHeader()) return false;
  // The reserved gap is written out explicitly rather than seek()ed over: a
  // seek past end-of-file is not a portable way to grow one, and the padding
  // must read back as zeros for a reader that ever gives those bytes meaning.
  // 64 bytes at a time, because this runs under createFresh/mergeFromExisting,
  // which already hold a 3584-byte record chunk on the same stack.
  constexpr size_t PAD_BYTES = CPST_RECORDS_OFFSET - CPST_HEADER_BYTES;
  uint8_t zeros[64] = {};
  static_assert(PAD_BYTES % sizeof(zeros) == 0, "the reserved gap must be a whole number of writes");
  if (!file.seek(CPST_HEADER_BYTES)) return false;
  for (size_t done = 0; done < PAD_BYTES; done += sizeof(zeros)) {
    if (file.write(zeros, sizeof(zeros)) != sizeof(zeros)) return false;
  }
  return true;
}

bool StateStore::applyDayRollover(uint16_t today) {
  // last_seen_day is a high-water mark, so the effective day never moves
  // backwards and a backwards clock jump cannot re-issue a spent allowance.
  const uint16_t effective = today > lastSeenDayValue ? today : lastSeenDayValue;
  bool dirty = false;
  if (countersDayValue != effective) {
    // Only a genuine move forward resets: a counters_day ahead of the
    // high-water day is a corrupt header, and is clamped down with the
    // counters left spent.
    if (effective > countersDayValue) {
      newTodayValue = 0;
      reviewsTodayValue = 0;
    }
    countersDayValue = effective;
    dirty = true;
  }
  if (lastSeenDayValue != effective) {
    lastSeenDayValue = effective;
    dirty = true;
  }
  return dirty ? writeHeader() : true;
}

bool StateStore::healRecord(Ordinal ordinal, fsrs::CardState& stateOut) {
  // A torn key means the record cannot be trusted to belong to this ordinal's
  // card, but SKIPPING it would hide that card from every session mode for as
  // long as the deck's content hash stays put — i.e. forever, since open() only
  // merges when the hash moves. One card restarting as new is the strictly
  // smaller loss, so the record is repaired on the spot (FLASHCARD_SPEC.md §3).
  mismatchSeen = true;
  stateOut = fsrs::CardState{};
  LOG_ERR("DECK", "Torn state record at ordinal %u; restarting that card as new", static_cast<unsigned>(ordinal));
  return writeRecordAt(ordinal, stateOut, true);
}

RecordStatus StateStore::readRecord(Ordinal ordinal, fsrs::CardState& stateOut) {
  // Zeroed before anything can fail: no return path below leaves the caller
  // looking at whatever it passed in.
  stateOut = fsrs::CardState{};
  if (!isOpen() || ordinal >= recordCountValue) return RecordStatus::IoError;

  uint8_t record[CPST_RECORD_BYTES];
  if (!file.seek(recordOffset(ordinal))) return RecordStatus::IoError;
  if (file.read(record, sizeof(record)) != static_cast<int>(sizeof(record))) return RecordStatus::IoError;

  if (readU64(record, 0) != deck->keyAt(ordinal)) {
    return healRecord(ordinal, stateOut) ? RecordStatus::KeyMismatch : RecordStatus::IoError;
  }
  normalizeState(record + RECORD_KEY_BYTES);
  memcpy(&stateOut, record + RECORD_KEY_BYTES, RECORD_PAYLOAD_BYTES);
  return RecordStatus::Ok;
}

bool StateStore::writeRecordAt(Ordinal ordinal, const fsrs::CardState& state, bool flushNow) {
  if (!isOpen() || ordinal >= recordCountValue) return false;

  uint8_t record[CPST_RECORD_BYTES];
  writeU64(record, 0, deck->keyAt(ordinal));
  memcpy(record + RECORD_KEY_BYTES, &state, RECORD_PAYLOAD_BYTES);
  normalizeState(record + RECORD_KEY_BYTES);

  if (!file.seek(recordOffset(ordinal))) return false;
  if (file.write(record, sizeof(record)) != sizeof(record)) return false;
  if (flushNow) file.flush();
  return true;
}

bool StateStore::writeRecord(Ordinal ordinal, const fsrs::CardState& state) {
  return writeRecordAt(ordinal, state, true);
}

bool StateStore::writeCounters(uint16_t newCards, uint16_t reviews) {
  if (!isOpen()) return false;
  newTodayValue = newCards;
  reviewsTodayValue = reviews;
  return writeHeader();
}

bool StateStore::commitAnswer(Ordinal ordinal, const fsrs::CardState& state, Counted counted) {
  // One sync per answer: the record write skips its flush whenever the header
  // write below is going to sync the file anyway.
  const bool headerFollows = counted != Counted::Nothing;
  if (!writeRecordAt(ordinal, state, !headerFollows)) return false;
  if (!headerFollows) return true;

  // The RAM counters move only once the header carrying them is on the card.
  // Bumping first and writing after would leave the session believing it had
  // spent an allowance the file does not record, and the next open() would hand
  // it back — the counters must never be ahead of the disk.
  const uint16_t previousNew = newTodayValue;
  const uint16_t previousReviews = reviewsTodayValue;
  switch (counted) {
    case Counted::Nothing:
      break;  // unreachable: headerFollows is false there
    case Counted::NewCard:
      if (newTodayValue < UINT16_MAX) newTodayValue++;
      break;
    case Counted::Review:
      if (reviewsTodayValue < UINT16_MAX) reviewsTodayValue++;
      break;
  }
  if (!writeHeader()) {
    newTodayValue = previousNew;
    reviewsTodayValue = previousReviews;
    return false;
  }
  return true;
}

bool StateStore::scanRecords(RecordVisitor visit, void* ctx) {
  if (!isOpen() || visit == nullptr) return false;

  uint8_t chunk[CHUNK_BYTES];
  fsrs::CardState state{};
  for (uint32_t first = 0; first < recordCountValue; first += RECORDS_PER_CHUNK) {
    uint32_t count = recordCountValue - first;
    if (count > RECORDS_PER_CHUNK) count = RECORDS_PER_CHUNK;
    const size_t bytes = count * CPST_RECORD_BYTES;
    // Seek per chunk rather than relying on the stream position: healing a torn
    // record below writes to this same handle and moves it.
    if (!file.seek(recordOffset(static_cast<Ordinal>(first)))) return false;
    if (file.read(chunk, bytes) != static_cast<int>(bytes)) return false;
    for (uint32_t i = 0; i < count; i++) {
      uint8_t* record = chunk + i * CPST_RECORD_BYTES;
      const Ordinal ordinal = static_cast<Ordinal>(first + i);
      if (readU64(record, 0) != deck->keyAt(ordinal)) {
        // Healed and then visited — same contract as readRecord(). Skipping it
        // would take the card out of every session mode permanently.
        if (!healRecord(ordinal, state)) return false;
      } else {
        normalizeState(record + RECORD_KEY_BYTES);
        memcpy(&state, record + RECORD_KEY_BYTES, RECORD_PAYLOAD_BYTES);
      }
      if (!visit(ordinal, state, ctx)) return true;
    }
  }
  return true;
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
