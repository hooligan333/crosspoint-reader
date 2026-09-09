// Host tests for the flashcard study logic: src/flashcards/{DeckFile,StateStore,
// SessionQueue,KeySort}. Built by build.sh with g++ -DCROSSPOINT_FLASHCARDS
// against the stub HalStorage in stubs/, which is the only thing between this
// layer and the SD card. Never compiled into the firmware.
//
// Layers, in the order they appear below:
//   * KeySort            -- the sort and search the other two lean on
//   * DeckFile           -- the CPDK validation matrix, on real converter bytes
//                           (crosspoint-fixture.deck) and on synthesised decks
//                           mutated one field at a time
//   * StateStore         -- CPST create / adopt / counter rules / merge
//   * SessionQueue       -- queue composition, ordering, caps, learn-ahead
//
// The fixture is the byte-for-byte output of deck-server/convert_deck.py for the
// synthetic 9-card package (x4pro-program/deck-server/decks/), so a converter
// change that breaks the reader shows up here rather than on the device.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "DeckFile.h"
#include "KeySort.h"
#include "SessionQueue.h"
#include "StateStore.h"
// StudyClock.cpp itself needs an RTC and the settings blob, so it is not linked
// here; only its pure, header-inline offset composition is tested.
#include "StudyClock.h"

using flashcards::CardSide;
using flashcards::DeckError;
using flashcards::DeckFile;
using flashcards::Ordinal;
using flashcards::RecordStatus;
using flashcards::Session;
using flashcards::SessionMode;
using flashcards::StateError;
using flashcards::StateStore;
using fsrs::CardPhase;
using fsrs::CardState;

namespace {

// --- tiny harness ------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void beginGroup(const char* name) { g_group = name; }

void record(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    printf("FAIL [%s] line %d: %s\n", g_group, line, expr);
  }
}

#define EXPECT(cond) record((cond), #cond, __LINE__)

#define EXPECT_EQ_U(actual, expected)                                                                               \
  do {                                                                                                              \
    const unsigned long long a_ = static_cast<unsigned long long>(actual);                                          \
    const unsigned long long e_ = static_cast<unsigned long long>(expected);                                        \
    ++g_checks;                                                                                                     \
    if (a_ != e_) {                                                                                                 \
      ++g_failures;                                                                                                 \
      printf("FAIL [%s] line %d: %s == %s (got %llu, want %llu)\n", g_group, __LINE__, #actual, #expected, a_, e_); \
    }                                                                                                               \
  } while (0)

#define EXPECT_STR(actual, expected)                                                                          \
  do {                                                                                                        \
    ++g_checks;                                                                                               \
    if (strcmp((actual), (expected)) != 0) {                                                                  \
      ++g_failures;                                                                                           \
      printf("FAIL [%s] line %d: %s == \"%s\" (got \"%s\")\n", g_group, __LINE__, #actual, expected, actual); \
    }                                                                                                         \
  } while (0)

// --- deck building -----------------------------------------------------------

struct TestCard {
  uint64_t key;
  std::string front;
  std::string back;
};

uint64_t fnv1a64(const uint8_t* bytes, size_t count) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < count; i++) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

void putU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) { memcpy(bytes.data() + offset, &value, 2); }
void putU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) { memcpy(bytes.data() + offset, &value, 4); }
void putU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) { memcpy(bytes.data() + offset, &value, 8); }

uint32_t getU32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0;
  memcpy(&value, bytes.data() + offset, 4);
  return value;
}
uint64_t getU64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0;
  memcpy(&value, bytes.data() + offset, 8);
  return value;
}

uint64_t contentHashOf(const std::vector<TestCard>& cards) {
  std::vector<uint8_t> keyBytes(cards.size() * 8);
  for (size_t i = 0; i < cards.size(); i++) memcpy(keyBytes.data() + i * 8, &cards[i].key, 8);
  return fnv1a64(keyBytes.data(), keyBytes.size());
}

/** A well-formed CPDK v1 file for `cards`; `w` adds the optional parameter block. */
std::vector<uint8_t> buildDeck(const std::vector<TestCard>& cards, const float* w = nullptr) {
  const bool hasParams = w != nullptr;
  const size_t indexStart = 64 + (hasParams ? 84u : 0u);
  std::vector<uint8_t> out(indexStart + cards.size() * 20, 0);
  memcpy(out.data(), "CPDK", 4);
  putU16(out, 4, 1);
  putU16(out, 6, hasParams ? 1 : 0);
  putU32(out, 8, static_cast<uint32_t>(cards.size()));
  putU64(out, 12, contentHashOf(cards));
  const char* title = "Test Deck";
  out[20] = static_cast<uint8_t>(strlen(title));
  memcpy(out.data() + 21, title, strlen(title));
  if (hasParams) memcpy(out.data() + 64, w, 84);

  std::vector<uint8_t> blob;
  for (size_t i = 0; i < cards.size(); i++) {
    const size_t entry = indexStart + i * 20;
    putU64(out, entry, cards[i].key);
    putU32(out, entry + 8, static_cast<uint32_t>(blob.size()));
    putU16(out, entry + 12, static_cast<uint16_t>(cards[i].front.size()));
    blob.insert(blob.end(), cards[i].front.begin(), cards[i].front.end());
    putU32(out, entry + 14, static_cast<uint32_t>(blob.size()));
    putU16(out, entry + 18, static_cast<uint16_t>(cards[i].back.size()));
    blob.insert(blob.end(), cards[i].back.begin(), cards[i].back.end());
  }
  out.insert(out.end(), blob.begin(), blob.end());
  return out;
}

std::vector<TestCard> makeCards(size_t count) {
  std::vector<TestCard> cards;
  cards.reserve(count);
  for (size_t i = 0; i < count; i++) {
    char text[32];
    snprintf(text, sizeof(text), "front %u", static_cast<unsigned>(i));
    std::string front(text);
    snprintf(text, sizeof(text), "back %u", static_cast<unsigned>(i));
    cards.push_back(TestCard{0x1000000000000000ULL + i * 0x11ULL, front, std::string(text)});
  }
  return cards;
}

// --- filesystem scaffolding --------------------------------------------------

// Record 0 does NOT follow the 64-byte header: 448 bytes of reserved padding sit
// between them so the every-answer header write cannot share a 512-byte sector
// with records 0-15 (FLASHCARD_SPEC.md §3). Every raw-bytes assertion below is
// written against this, and the padding is asserted to be zeros.
constexpr size_t REC0 = flashcards::CPST_RECORDS_OFFSET;
constexpr size_t REC = flashcards::CPST_RECORD_BYTES;
static_assert(REC0 == 512, "records start at offset 512");
static_assert(REC == 28, "CPST records are 28 bytes");

/** Byte offset of `ordinal`'s record in a .state file. */
constexpr size_t recAt(size_t ordinal) { return REC0 + ordinal * REC; }
/** Size of a well-formed .state file holding `count` records. */
constexpr size_t stateBytes(size_t count) { return REC0 + count * REC; }

std::string g_root;
const char* DECK_LEAF = "test.deck";

std::string decksDir() { return g_root + "/Decks"; }
std::string deckPath() { return decksDir() + "/" + DECK_LEAF; }
std::string statePath() { return decksDir() + "/.state/" + DECK_LEAF + ".state"; }

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* file = fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const size_t written = bytes.empty() ? 0 : fwrite(bytes.data(), 1, bytes.size(), file);
  fclose(file);
  return written == bytes.size();
}

bool readBytes(const std::string& path, std::vector<uint8_t>& out) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  out.resize(size > 0 ? static_cast<size_t>(size) : 0);
  const size_t read = out.empty() ? 0 : fread(out.data(), 1, out.size(), file);
  fclose(file);
  return read == out.size();
}

bool fileExists(const std::string& path) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) return false;
  fclose(file);
  return true;
}

bool copyFile(const std::string& from, const std::string& to) {
  std::vector<uint8_t> bytes;
  if (!readBytes(from, bytes)) return false;
  return writeBytes(to, bytes);
}

/** Wipes the deck, its state and any stray temp so each group starts clean. */
void resetCard() {
  remove(statePath().c_str());
  remove((statePath() + ".tmp").c_str());
  remove(deckPath().c_str());
}

void makeDirs() {
  Storage.ensureDirectoryExists(decksDir().c_str());
  Storage.ensureDirectoryExists((decksDir() + "/.state").c_str());
}

/** Installs `bytes` as the deck file and opens it. */
DeckError installDeck(DeckFile& deck, const std::vector<uint8_t>& bytes) {
  deck.close();
  writeBytes(deckPath(), bytes);
  return deck.open(deckPath());
}

CardState newCard() {
  CardState card{};
  card.state = CardPhase::New;
  return card;
}

CardState reviewCard(uint16_t dueDay, uint16_t lastReviewDay = 100) {
  CardState card{};
  card.state = CardPhase::Review;
  card.due = dueDay;
  card.lastReviewDay = lastReviewDay;
  card.stability = 12.5f;
  card.difficulty = 5.25f;
  card.reps = 4;
  return card;
}

CardState learningCard(uint32_t dueUnix) {
  CardState card{};
  card.state = CardPhase::Learning;
  card.due = dueUnix;
  card.step = 1;
  card.stability = 1.5f;
  card.difficulty = 5.0f;
  card.reps = 1;
  return card;
}

// --- KeySort -----------------------------------------------------------------

void testKeySort() {
  beginGroup("keysort");

  uint64_t keys[] = {9, 3, 7, 1, 8, 3};
  uint16_t payload[] = {0, 1, 2, 3, 4, 5};
  flashcards::sortKeys(keys, payload, 6);
  EXPECT_EQ_U(keys[0], 1);
  EXPECT_EQ_U(keys[1], 3);
  EXPECT_EQ_U(keys[2], 3);
  EXPECT_EQ_U(keys[3], 7);
  EXPECT_EQ_U(keys[4], 8);
  EXPECT_EQ_U(keys[5], 9);
  // The payload followed its key, whichever of the two 3s landed where.
  EXPECT_EQ_U(payload[0], 3);
  EXPECT(payload[1] == 1 || payload[1] == 5);
  EXPECT(payload[2] == 1 || payload[2] == 5);
  EXPECT_EQ_U(payload[3], 2);
  EXPECT_EQ_U(payload[4], 4);
  EXPECT_EQ_U(payload[5], 0);

  // Degenerate sizes must not touch memory they do not own.
  uint64_t single[] = {42};
  flashcards::sortKeys(single, nullptr, 1);
  flashcards::sortKeys(single, nullptr, 0);
  EXPECT_EQ_U(single[0], 42);

  EXPECT_EQ_U(flashcards::findKey(keys, 6, 1), 0);
  EXPECT_EQ_U(flashcards::findKey(keys, 6, 9), 5);
  EXPECT_EQ_U(flashcards::findKey(keys, 6, 7), 3);
  EXPECT_EQ_U(flashcards::findKey(keys, 6, 4), flashcards::KEY_NOT_FOUND);
  EXPECT_EQ_U(flashcards::findKey(keys, 0, 1), flashcards::KEY_NOT_FOUND);

  // A larger, adversarial ordering: already sorted, reversed, all equal.
  const uint32_t count = 500;
  std::vector<uint64_t> big(count);
  for (uint32_t i = 0; i < count; i++) big[i] = count - i;
  flashcards::sortKeys(big.data(), nullptr, count);
  bool ascending = true;
  for (uint32_t i = 1; i < count; i++) {
    if (big[i - 1] > big[i]) ascending = false;
  }
  EXPECT(ascending);
  for (uint32_t i = 0; i < count; i++) big[i] = 7;
  flashcards::sortKeys(big.data(), nullptr, count);
  EXPECT_EQ_U(big[0], 7);
  EXPECT_EQ_U(big[count - 1], 7);
}

// --- DeckFile ----------------------------------------------------------------

void testRealFixture() {
  beginGroup("deck/fixture");

  DeckFile deck;
  const DeckError error = deck.open(FIXTURE_PATH);
  EXPECT_EQ_U(static_cast<int>(error), static_cast<int>(DeckError::Ok));
  if (error != DeckError::Ok) return;

  EXPECT_EQ_U(deck.cardCount(), 9);
  EXPECT_EQ_U(deck.contentHash(), 0xb82d53465e95b399ULL);
  EXPECT_STR(deck.title(), "CrossPoint Fixture");
  EXPECT(!deck.hasDeckParams());
  EXPECT_EQ_U(deck.keyAt(0), 0x2aa45a723c165f1eULL);
  EXPECT_EQ_U(deck.keyAt(1), 0x5c8046f31cbd0fcbULL);
  EXPECT_EQ_U(deck.keyAt(9), 0);  // out of range

  char text[flashcards::DECK_MAX_SLICE_BYTES + 1];
  uint16_t length = 0;
  EXPECT(deck.loadSide(0, CardSide::Front, text, sizeof(text), length));
  EXPECT_EQ_U(length, 30);
  EXPECT_STR(text, "What is the capital of France?");
  EXPECT(deck.loadSide(0, CardSide::Back, text, sizeof(text), length));
  EXPECT_EQ_U(length, 5);
  EXPECT_STR(text, "Paris");
  EXPECT(deck.loadSide(2, CardSide::Back, text, sizeof(text), length));
  EXPECT_STR(text, "Au");

  // A buffer that cannot hold the slice plus its terminator is refused rather
  // than filled short.
  char small[6];
  EXPECT(!deck.loadSide(0, CardSide::Front, small, sizeof(small), length));
  EXPECT_EQ_U(length, 0);
  EXPECT(!deck.loadSide(99, CardSide::Front, text, sizeof(text), length));
}

void testDeckValidation() {
  beginGroup("deck/validation");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(4);
  const std::vector<uint8_t> good = buildDeck(cards);

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, good)), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(deck.cardCount(), 4);
  EXPECT_EQ_U(deck.contentHash(), contentHashOf(cards));
  deck.close();

  // Missing file.
  remove(deckPath().c_str());
  EXPECT_EQ_U(static_cast<int>(deck.open(deckPath())), static_cast<int>(DeckError::OpenFailed));

  struct Case {
    const char* name;
    DeckError expected;
    void (*mutate)(std::vector<uint8_t>&);
  };
  static const Case cases[] = {
      {"bad magic", DeckError::BadMagic, [](std::vector<uint8_t>& b) { b[0] = 'X'; }},
      {"version 2", DeckError::BadVersion, [](std::vector<uint8_t>& b) { putU16(b, 4, 2); }},
      {"reserved flag", DeckError::UnknownFlags, [](std::vector<uint8_t>& b) { putU16(b, 6, 0x0002); }},
      {"zero cards", DeckError::BadCardCount, [](std::vector<uint8_t>& b) { putU32(b, 8, 0); }},
      {"over the cap", DeckError::BadCardCount, [](std::vector<uint8_t>& b) { putU32(b, 8, 40001); }},
      {"header only", DeckError::ShortFile, [](std::vector<uint8_t>& b) { b.resize(32); }},
      {"index cut short", DeckError::ShortFile, [](std::vector<uint8_t>& b) { b.resize(64 + 3 * 20); }},
      {"slice past the blob", DeckError::SliceOutOfRange, [](std::vector<uint8_t>& b) { putU32(b, 64 + 8, 100000); }},
      {"slice length past the blob", DeckError::SliceOutOfRange,
       [](std::vector<uint8_t>& b) { putU16(b, 64 + 12, 900); }},
      {"back slice past the blob", DeckError::SliceOutOfRange,
       [](std::vector<uint8_t>& b) { putU32(b, 64 + 14, 100000); }},
      {"slice over 4096 B", DeckError::SliceOutOfRange,
       [](std::vector<uint8_t>& b) {
         b.resize(b.size() + 8000, 'x');  // a blob big enough that only the cap refuses it
         putU16(b, 64 + 12, 5000);
       }},
      {"duplicate key", DeckError::DuplicateKey, [](std::vector<uint8_t>& b) { putU64(b, 64 + 60, getU64(b, 64)); }},
  };

  for (const Case& testCase : cases) {
    std::vector<uint8_t> bytes = good;
    testCase.mutate(bytes);
    const DeckError error = installDeck(deck, bytes);
    if (error != testCase.expected) {
      ++g_failures;
      printf("FAIL [%s] case \"%s\": got %s, want %s\n", g_group, testCase.name, flashcards::deckErrorName(error),
             flashcards::deckErrorName(testCase.expected));
    }
    ++g_checks;
    EXPECT(!deck.isOpen());
    EXPECT_EQ_U(deck.cardCount(), 0);
  }
}

void testDeckParameterBlock() {
  beginGroup("deck/params");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(3);
  fsrs::Params defaults = fsrs::defaultParams();

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards, defaults.w))), static_cast<int>(DeckError::Ok));
  EXPECT(deck.hasDeckParams());
  EXPECT(deck.params().w[0] == defaults.w[0]);
  EXPECT(deck.params().w[20] == defaults.w[20]);
  // Everything outside w[] still comes from the device defaults.
  EXPECT(deck.params().desiredRetention == defaults.desiredRetention);
  EXPECT_EQ_U(deck.cardCount(), 3);
  deck.close();

  // w[20] == 0 makes every interval one day forever; validateParams refuses it.
  float degenerate[21];
  memcpy(degenerate, defaults.w, sizeof(degenerate));
  degenerate[20] = 0.0f;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards, degenerate))),
              static_cast<int>(DeckError::BadParams));

  float outOfRange[21];
  memcpy(outOfRange, defaults.w, sizeof(outOfRange));
  outOfRange[0] = -1.0f;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards, outOfRange))),
              static_cast<int>(DeckError::BadParams));

  // The params block also moves the index and the blob: a deck that claims one
  // and is cut off inside it is short, not misread.
  std::vector<uint8_t> cut = buildDeck(cards, defaults.w);
  cut.resize(100);
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, cut)), static_cast<int>(DeckError::ShortFile));

  // A deck WITH the block still resolves its slices, i.e. blob_start moved.
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards, defaults.w))), static_cast<int>(DeckError::Ok));
  char text[64];
  uint16_t length = 0;
  EXPECT(deck.loadSide(2, CardSide::Front, text, sizeof(text), length));
  EXPECT_STR(text, "front 2");
}

void testEmptyBack() {
  beginGroup("deck/empty back");
  resetCard();
  makeDirs();

  std::vector<TestCard> cards = makeCards(2);
  cards[1].back.clear();
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));

  char text[64];
  uint16_t length = 7;
  EXPECT(deck.loadSide(1, CardSide::Back, text, sizeof(text), length));
  EXPECT_EQ_U(length, 0);
  EXPECT_STR(text, "");
  EXPECT(deck.loadSide(1, CardSide::Front, text, sizeof(text), length));
  EXPECT_STR(text, "front 1");
}

/**
 * R3a SHOULD-7. The converter strips C0 control bytes and read_deck.py refuses
 * a file still carrying one, but a deck copied over by USB has been through
 * neither, so the DEVICE sanitizes on load (FLASHCARD_SPEC.md §2.1). Byte for
 * byte, so offsets and lengths are untouched, and never by refusing the deck.
 */
void testC0Sanitize() {
  beginGroup("deck/c0 sanitize");
  resetCard();
  makeDirs();

  std::vector<TestCard> cards = makeCards(3);
  // The reviewer's slice: 61 01 02 62.
  cards[0].front = std::string(
      "a\x01\x02"
      "b",
      4);
  // Every C0 byte except '\n', plus a DEL (0x7F, not C0 — it stays).
  cards[1].front = std::string(
      "x\ty\rz\n\x1B"
      "w\x7F",
      9);
  cards[2].back = std::string("\x00\x1F", 2);

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));

  char text[64];
  uint16_t length = 0;
  EXPECT(deck.loadSide(0, CardSide::Front, text, sizeof(text), length));
  EXPECT_EQ_U(length, 4);  // the length is the slice's, not the printable count
  EXPECT_STR(text, "a  b");

  EXPECT(deck.loadSide(1, CardSide::Front, text, sizeof(text), length));
  EXPECT_EQ_U(length, 9);
  EXPECT_STR(text, "x y z\n w\x7F");  // tab, CR and ESC become spaces; '\n' and DEL survive

  EXPECT(deck.loadSide(2, CardSide::Back, text, sizeof(text), length));
  EXPECT_EQ_U(length, 2);
  EXPECT_STR(text, "  ");  // an embedded NUL becomes a space, not a short string

  // Clean text is byte-identical, i.e. the sweep is not mangling anything else.
  EXPECT(deck.loadSide(2, CardSide::Front, text, sizeof(text), length));
  EXPECT_STR(text, "front 2");
}

// --- StateStore --------------------------------------------------------------

void testStateCreation() {
  beginGroup("state/create");
  resetCard();
  makeDirs();
  remove(statePath().c_str());

  const std::vector<TestCard> cards = makeCards(5);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), 5);
  EXPECT_EQ_U(store.newToday(), 0);
  EXPECT_EQ_U(store.reviewsToday(), 0);
  EXPECT_EQ_U(store.countersDay(), 900);
  EXPECT(!store.keyMismatchSeen());

  for (Ordinal ordinal = 0; ordinal < 5; ordinal++) {
    CardState state{};
    EXPECT_EQ_U(static_cast<int>(store.readRecord(ordinal, state)), static_cast<int>(RecordStatus::Ok));
    EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
    EXPECT_EQ_U(state.reps, 0);
  }
  store.close();

  std::vector<uint8_t> raw;
  EXPECT(readBytes(statePath(), raw));
  EXPECT_EQ_U(raw.size(), stateBytes(5));
  EXPECT(memcmp(raw.data(), "CPST", 4) == 0);
  EXPECT_EQ_U(getU32(raw, 8), 5);
  EXPECT_EQ_U(getU64(raw, 12), deck.contentHash());
  EXPECT_EQ_U(getU32(raw, 20), 900);  // counters_day
  EXPECT_EQ_U(getU32(raw, 28), 900);  // last_seen_day
  // Everything from the end of the header to record 0 is reserved = 0.
  bool paddingIsZero = true;
  for (size_t i = 32; i < REC0; i++) {
    if (raw[i] != 0) paddingIsZero = false;
  }
  EXPECT(paddingIsZero);
  for (size_t i = 0; i < 5; i++) EXPECT_EQ_U(getU64(raw, recAt(i)), cards[i].key);
}

void testStateAdoptAndCounters() {
  beginGroup("state/counters");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(5);
  DeckFile deck;
  installDeck(deck, buildDeck(cards));

  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
    CardState answered = reviewCard(905, 900);
    EXPECT(store.commitAnswer(2, answered, flashcards::Counted::Review));
    EXPECT(store.commitAnswer(3, newCard(), flashcards::Counted::NewCard));
    EXPECT(store.commitAnswer(4, newCard(), flashcards::Counted::Nothing));
    EXPECT_EQ_U(store.newToday(), 1);
    EXPECT_EQ_U(store.reviewsToday(), 1);
  }

  // Same day: the file is adopted, records and counters survive.
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.newToday(), 1);
    EXPECT_EQ_U(store.reviewsToday(), 1);
    CardState state{};
    EXPECT_EQ_U(static_cast<int>(store.readRecord(2, state)), static_cast<int>(RecordStatus::Ok));
    EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::Review));
    EXPECT_EQ_U(state.due, 905);
    EXPECT_EQ_U(state.lastReviewDay, 900);
    EXPECT(state.stability == 12.5f);
  }

  // A new day zeroes the allowance.
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 901)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.newToday(), 0);
    EXPECT_EQ_U(store.reviewsToday(), 0);
    EXPECT_EQ_U(store.countersDay(), 901);
    EXPECT(store.commitAnswer(0, newCard(), flashcards::Counted::NewCard));
    EXPECT(store.commitAnswer(1, newCard(), flashcards::Counted::NewCard));
    EXPECT_EQ_U(store.newToday(), 2);
  }

  // The clock jumps back a week: the day's allowance must NOT be re-issued.
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 894)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.newToday(), 2);
    EXPECT_EQ_U(store.countersDay(), 901);  // clamped forward to the high-water day
  }
  // ...and coming back to the real day does not reset either: day 901 is spent.
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 901)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.newToday(), 2);
    EXPECT_EQ_U(store.countersDay(), 901);
  }
  // The next genuine day rolls over as usual.
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 902)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.newToday(), 0);
    EXPECT_EQ_U(store.countersDay(), 902);
    // The undo path puts spent counters back by hand.
    EXPECT(store.writeCounters(7, 9));
  }
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 902)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.newToday(), 7);
    EXPECT_EQ_U(store.reviewsToday(), 9);
  }
}

void testStateRebuildsFromRubbish() {
  beginGroup("state/rebuild");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(3);
  DeckFile deck;
  installDeck(deck, buildDeck(cards));

  // A state file whose header is not CPST is discarded, not refused: losing a
  // deck's history is recoverable, refusing to study it is not.
  std::vector<uint8_t> rubbish(200, 0xAB);
  EXPECT(writeBytes(statePath(), rubbish));
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 500)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.recordCount(), 3);
    CardState state{};
    EXPECT_EQ_U(static_cast<int>(store.readRecord(0, state)), static_cast<int>(RecordStatus::Ok));
    EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  }

  // A record whose key is not the deck's is a torn write: it is HEALED in
  // place, not skipped. The full self-heal contract is in testTornKeyHeals().
  {
    std::vector<uint8_t> raw;
    EXPECT(readBytes(statePath(), raw));
    putU64(raw, recAt(1), 0xDEADBEEFDEADBEEFULL);
    EXPECT(writeBytes(statePath(), raw));

    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 500)), static_cast<int>(StateError::Ok));
    CardState state{};
    EXPECT_EQ_U(static_cast<int>(store.readRecord(1, state)), static_cast<int>(RecordStatus::KeyMismatch));
    EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
    EXPECT(store.keyMismatchSeen());
    EXPECT_EQ_U(static_cast<int>(store.readRecord(0, state)), static_cast<int>(RecordStatus::Ok));
    EXPECT_EQ_U(static_cast<int>(store.readRecord(3, state)), static_cast<int>(RecordStatus::IoError));
  }

  // An unknown state byte reads as New rather than as garbage.
  {
    std::vector<uint8_t> raw;
    EXPECT(readBytes(statePath(), raw));
    putU64(raw, recAt(1), cards[1].key);  // the heal above already put it back
    raw[recAt(1) + 8 + 14] = 0x5A;        // state byte
    EXPECT(writeBytes(statePath(), raw));

    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 500)), static_cast<int>(StateError::Ok));
    CardState state{};
    EXPECT_EQ_U(static_cast<int>(store.readRecord(1, state)), static_cast<int>(RecordStatus::Ok));
    EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  }
}

void testMerge() {
  beginGroup("state/merge");
  resetCard();
  makeDirs();

  // Four cards; three of them carry real scheduling state.
  std::vector<TestCard> before = makeCards(4);
  DeckFile deck;
  installDeck(deck, buildDeck(before));
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)), static_cast<int>(StateError::Ok));
    EXPECT(store.writeRecord(0, reviewCard(710, 700)));
    EXPECT(store.writeRecord(1, learningCard(1700000000u)));
    CardState suspended = reviewCard(720, 700);
    suspended.flags = fsrs::FLAG_SUSPENDED;
    EXPECT(store.writeRecord(2, suspended));
    EXPECT(store.writeCounters(6, 11));
  }
  deck.close();

  // The server re-exports: card 1 is gone, two cards are added, and the rest
  // are reordered. Only the keys tie the old state to the new ordinals.
  std::vector<TestCard> after;
  after.push_back(before[2]);
  after.push_back(before[3]);
  after.push_back(TestCard{0xAAAA0001ULL, "new front a", "new back a"});
  after.push_back(before[0]);
  after.push_back(TestCard{0xAAAA0002ULL, "new front b", ""});

  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(after))), static_cast<int>(DeckError::Ok));
  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), 5);
  // Counters carry across a re-download: it is not a new day.
  EXPECT_EQ_U(store.newToday(), 6);
  EXPECT_EQ_U(store.reviewsToday(), 11);

  CardState state{};
  // old ordinal 2 (suspended review) -> new ordinal 0
  EXPECT_EQ_U(static_cast<int>(store.readRecord(0, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::Review));
  EXPECT_EQ_U(state.due, 720);
  EXPECT_EQ_U(state.flags, fsrs::FLAG_SUSPENDED);
  // old ordinal 3 was untouched -> still New
  EXPECT_EQ_U(static_cast<int>(store.readRecord(1, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  // a card the new deck added
  EXPECT_EQ_U(static_cast<int>(store.readRecord(2, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  EXPECT_EQ_U(state.reps, 0);
  // old ordinal 0 (the review) -> new ordinal 3, schedule intact
  EXPECT_EQ_U(static_cast<int>(store.readRecord(3, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::Review));
  EXPECT_EQ_U(state.due, 710);
  EXPECT_EQ_U(state.lastReviewDay, 700);
  EXPECT(state.stability == 12.5f);
  EXPECT(!store.keyMismatchSeen());
  store.close();

  std::vector<uint8_t> raw;
  EXPECT(readBytes(statePath(), raw));
  EXPECT_EQ_U(raw.size(), stateBytes(5));
  EXPECT_EQ_U(getU64(raw, 12), deck.contentHash());  // header hash follows the deck
  EXPECT(!fileExists(statePath() + ".tmp"));         // temp+rename left nothing behind

  // Re-opening now that the hashes agree must NOT merge again.
  {
    StateStore again;
    EXPECT_EQ_U(static_cast<int>(again.open(deck, decksDir(), DECK_LEAF, 700)), static_cast<int>(StateError::Ok));
    CardState kept{};
    EXPECT_EQ_U(static_cast<int>(again.readRecord(3, kept)), static_cast<int>(RecordStatus::Ok));
    EXPECT_EQ_U(kept.due, 710);
  }
}

void testMergeEdgeCases() {
  beginGroup("state/merge edges");
  resetCard();
  makeDirs();

  const std::vector<TestCard> before = makeCards(6);
  DeckFile deck;
  installDeck(deck, buildDeck(before));
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 300)), static_cast<int>(StateError::Ok));
    for (Ordinal ordinal = 0; ordinal < 6; ordinal++)
      EXPECT(store.writeRecord(ordinal, reviewCard(310 + ordinal, 300)));
  }

  // A state file cut in half mid-record: the merge keeps what survived and
  // starts the rest fresh instead of reading past the end.
  {
    std::vector<uint8_t> raw;
    EXPECT(readBytes(statePath(), raw));
    raw.resize(recAt(3) + 10);
    // The header still claims six records; the file holds three and a half.
    raw[recAt(3)] = 0;
    EXPECT(writeBytes(statePath(), raw));
  }
  // Same deck, but the truncation makes the file unusable as-is, so the store
  // must rebuild it by merging what it can read.
  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 300)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), 6);
  CardState state{};
  EXPECT_EQ_U(static_cast<int>(store.readRecord(0, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(state.due, 310);
  EXPECT_EQ_U(static_cast<int>(store.readRecord(2, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(state.due, 312);
  EXPECT_EQ_U(static_cast<int>(store.readRecord(5, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  store.close();

  std::vector<uint8_t> raw;
  EXPECT(readBytes(statePath(), raw));
  EXPECT_EQ_U(raw.size(), stateBytes(6));

  // A corrupt state byte in the OLD file is repaired by the merge, not carried.
  {
    EXPECT(readBytes(statePath(), raw));
    raw[recAt(0) + 8 + 14] = 0x77;
    EXPECT(writeBytes(statePath(), raw));
  }
  const std::vector<TestCard> after = makeCards(7);  // superset: same six keys plus one
  deck.close();
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(after))), static_cast<int>(DeckError::Ok));
  StateStore merged;
  EXPECT_EQ_U(static_cast<int>(merged.open(deck, decksDir(), DECK_LEAF, 300)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(merged.recordCount(), 7);
  EXPECT_EQ_U(static_cast<int>(merged.readRecord(0, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  EXPECT_EQ_U(static_cast<int>(merged.readRecord(6, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  EXPECT_EQ_U(static_cast<int>(merged.readRecord(1, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(state.due, 311);
}

/**
 * R3a MUST-1 regression. The merge ends with remove(state) then rename(tmp).
 * A power cut between those two leaves the COMPLETE merged state in the temp
 * and nothing at the real path; the old open() ran createFresh over the top of
 * it and threw the deck's whole history away. open() must adopt the orphan.
 *
 * The same repro stands in for a rename() that simply returned false — the
 * on-disk state (path missing, complete .tmp present) is identical.
 */
void testMergeCrashWindow() {
  beginGroup("state/crash window");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(4);
  DeckFile deck;
  installDeck(deck, buildDeck(cards));
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 600)), static_cast<int>(StateError::Ok));
    for (Ordinal ordinal = 0; ordinal < 4; ordinal++)
      EXPECT(store.writeRecord(ordinal, reviewCard(static_cast<uint16_t>(610 + ordinal), 600)));
    EXPECT(store.writeCounters(3, 5));
  }

  // Reproduce the window exactly: the complete file IS the temp, and the state
  // path is gone.
  EXPECT(copyFile(statePath(), statePath() + ".tmp"));
  EXPECT_EQ_U(remove(statePath().c_str()), 0);
  EXPECT(!fileExists(statePath()));

  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 600)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.recordCount(), 4);
    // Every record survived, and so did the counters. Nothing was rebuilt.
    EXPECT_EQ_U(store.newToday(), 3);
    EXPECT_EQ_U(store.reviewsToday(), 5);
    for (Ordinal ordinal = 0; ordinal < 4; ordinal++) {
      CardState state{};
      EXPECT_EQ_U(static_cast<int>(store.readRecord(ordinal, state)), static_cast<int>(RecordStatus::Ok));
      EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::Review));
      EXPECT_EQ_U(state.due, 610u + ordinal);
    }
    EXPECT(!store.keyMismatchSeen());
  }
  // The temp is gone: it was renamed in, not copied.
  EXPECT(fileExists(statePath()));
  EXPECT(!fileExists(statePath() + ".tmp"));

  // A TORN temp is not a special case. It is adopted just the same and then
  // fails the header checks, exactly as a torn .state would, so the deck ends
  // up with a fresh file rather than an unstudiable one.
  EXPECT_EQ_U(remove(statePath().c_str()), 0);
  EXPECT(writeBytes(statePath() + ".tmp", std::vector<uint8_t>(200, 0x5C)));
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 600)), static_cast<int>(StateError::Ok));
    EXPECT_EQ_U(store.recordCount(), 4);
    CardState state{};
    EXPECT_EQ_U(static_cast<int>(store.readRecord(0, state)), static_cast<int>(RecordStatus::Ok));
    EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  }
  EXPECT(!fileExists(statePath() + ".tmp"));

  // A temp beside a file that IS there is left strictly alone: only an orphan
  // is adopted, so a merge that crashed BEFORE the remove cannot be mistaken
  // for one that crashed after it.
  EXPECT(writeBytes(statePath() + ".tmp", std::vector<uint8_t>(200, 0x11)));
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 600)), static_cast<int>(StateError::Ok));
  }
  std::vector<uint8_t> leftover;
  EXPECT(readBytes(statePath() + ".tmp", leftover));
  EXPECT_EQ_U(leftover.size(), 200);
  EXPECT_EQ_U(leftover[0], 0x11);
  remove((statePath() + ".tmp").c_str());
}

/**
 * R3a MUST-2 regression. A torn record key used to be skipped by scanRecords
 * and reported by readRecord, which took the card out of EVERY session mode for
 * as long as the deck's content hash stayed put — i.e. forever, since only a
 * hash change triggers a merge. It must self-heal instead: the ordinal is
 * rewritten as a fresh New card with the deck's key, and the card comes back as
 * new.
 */
void testTornKeyHeals() {
  beginGroup("state/torn key heals");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(4);
  DeckFile deck;
  installDeck(deck, buildDeck(cards));
  {
    StateStore store;
    EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 800)), static_cast<int>(StateError::Ok));
    for (Ordinal ordinal = 0; ordinal < 4; ordinal++) EXPECT(store.writeRecord(ordinal, reviewCard(790, 780)));
  }

  // Tear the key of ordinal 2 — half of a 28-byte write that never landed.
  {
    std::vector<uint8_t> raw;
    EXPECT(readBytes(statePath(), raw));
    putU64(raw, recAt(2), 0x0123456789ABCDEFULL);
    EXPECT(writeBytes(statePath(), raw));
  }

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 800)), static_cast<int>(StateError::Ok));

  fsrs::Now now{1700000000u, 800};
  Session session;
  EXPECT(flashcards::buildSession(store, SessionMode::Due, now, session));
  EXPECT(store.keyMismatchSeen());
  // Three reviews survived; the torn card is now the deck's one New card...
  EXPECT_EQ_U(session.summary.dueAvailable, 3);
  EXPECT_EQ_U(session.summary.newAvailable, 1);
  EXPECT_EQ_U(session.summary.newCards, 1);
  // ...and it is IN the queue, which is the whole point.
  EXPECT_EQ_U(session.cards.size(), 4);
  bool healedCardIsQueued = false;
  for (Ordinal ordinal : session.cards) {
    if (ordinal == 2) healedCardIsQueued = true;
  }
  EXPECT(healedCardIsQueued);
  store.close();

  // The file was repaired on disk: the deck's key is back and the payload is a
  // zeroed New record.
  std::vector<uint8_t> raw;
  EXPECT(readBytes(statePath(), raw));
  EXPECT_EQ_U(raw.size(), stateBytes(4));
  EXPECT_EQ_U(getU64(raw, recAt(2)), cards[2].key);
  bool payloadIsZero = true;
  for (size_t i = 8; i < REC; i++) {
    if (raw[recAt(2) + i] != 0) payloadIsZero = false;
  }
  EXPECT(payloadIsZero);
  // Its neighbours were not touched.
  EXPECT_EQ_U(getU64(raw, recAt(1)), cards[1].key);
  EXPECT_EQ_U(getU64(raw, recAt(3)), cards[3].key);

  // A fresh store over the repaired file sees nothing wrong any more, and the
  // healed card reads back as New through readRecord() too.
  StateStore again;
  EXPECT_EQ_U(static_cast<int>(again.open(deck, decksDir(), DECK_LEAF, 800)), static_cast<int>(StateError::Ok));
  CardState state = reviewCard(999, 999);  // deliberately dirty: must be overwritten
  EXPECT_EQ_U(static_cast<int>(again.readRecord(2, state)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  EXPECT_EQ_U(state.due, 0);
  EXPECT(!again.keyMismatchSeen());

  // readRecord zeroes stateOut before every early return, so a rejected
  // ordinal cannot leave the caller reading its own stale card.
  CardState stale = reviewCard(555, 550);
  EXPECT_EQ_U(static_cast<int>(again.readRecord(99, stale)), static_cast<int>(RecordStatus::IoError));
  EXPECT_EQ_U(static_cast<int>(stale.state), static_cast<int>(CardPhase::New));
  EXPECT_EQ_U(stale.due, 0);
  EXPECT_EQ_U(stale.reps, 0);
}

/** R3a NIT-16: a 40000-card deck, the cap, exercising both chunked readers. */
void testMaximumDeck() {
  beginGroup("state/40000 cards");
  resetCard();
  makeDirs();

  const size_t count = 40000;
  const std::vector<TestCard> cards = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(deck.cardCount(), count);
  // The index is read in 16 KB chunks (800 KB here) and the ordinals at both
  // ends of it must still resolve.
  EXPECT_EQ_U(deck.keyAt(0), cards[0].key);
  EXPECT_EQ_U(deck.keyAt(static_cast<Ordinal>(count - 1)), cards[count - 1].key);

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), count);

  std::vector<uint8_t> raw;
  EXPECT(readBytes(statePath(), raw));
  EXPECT_EQ_U(raw.size(), stateBytes(count));
  // Record chunks are 128 records; check one at a chunk boundary, one mid-chunk
  // and the very last, so a short final chunk would show up here.
  EXPECT_EQ_U(getU64(raw, recAt(128)), cards[128].key);
  EXPECT_EQ_U(getU64(raw, recAt(20001)), cards[20001].key);
  EXPECT_EQ_U(getU64(raw, recAt(count - 1)), cards[count - 1].key);

  // A scheduled card near the end proves the whole scan runs, not just the
  // first chunk.
  EXPECT(store.writeRecord(static_cast<Ordinal>(count - 1), reviewCard(990, 980)));

  fsrs::Now now{1700000000u, 1000};
  Session session;
  EXPECT(flashcards::buildSession(store, SessionMode::CramAll, now, session));
  EXPECT_EQ_U(session.cards.size(), count);
  EXPECT_EQ_U(session.cards[0], 0);
  EXPECT_EQ_U(session.cards[static_cast<uint32_t>(count) - 1], count - 1);

  EXPECT(flashcards::buildSession(store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.summary.dueAvailable, 1);
  EXPECT_EQ_U(session.summary.reviews, 1);
  EXPECT_EQ_U(session.summary.newCards, 20);  // the daily allowance, not 39999
  EXPECT_EQ_U(session.summary.newAvailable, count - 1);
  EXPECT_EQ_U(session.cards.size(), 21);
  store.close();
  resetCard();
}

// --- SessionQueue ------------------------------------------------------------

struct QueueFixture {
  DeckFile deck;
  StateStore store;

  bool build(size_t cardCount, uint16_t today) {
    resetCard();
    makeDirs();
    if (installDeck(deck, buildDeck(makeCards(cardCount))) != DeckError::Ok) return false;
    return store.open(deck, decksDir(), DECK_LEAF, today) == StateError::Ok;
  }
};

void testQueueBasics() {
  beginGroup("queue/basics");

  QueueFixture fixture;
  EXPECT(fixture.build(12, 1000));

  // 0..3 due reviews (out of due order), 4 due tomorrow, 5 suspended,
  // 6 learning due now, 7 learning due in 5 minutes, 8..11 new.
  EXPECT(fixture.store.writeRecord(0, reviewCard(999, 990)));
  EXPECT(fixture.store.writeRecord(1, reviewCard(1000, 990)));
  EXPECT(fixture.store.writeRecord(2, reviewCard(995, 990)));
  EXPECT(fixture.store.writeRecord(3, reviewCard(999, 990)));
  EXPECT(fixture.store.writeRecord(4, reviewCard(1001, 990)));
  CardState suspended = reviewCard(990, 980);
  suspended.flags = fsrs::FLAG_SUSPENDED;
  EXPECT(fixture.store.writeRecord(5, suspended));
  const uint32_t nowUnix = 1700000000u;
  EXPECT(fixture.store.writeRecord(6, learningCard(nowUnix - 60)));
  EXPECT(fixture.store.writeRecord(7, learningCard(nowUnix + 300)));

  fsrs::Now now{nowUnix, 1000};
  Session session;
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, session));

  EXPECT_EQ_U(session.summary.dueAvailable, 4);
  EXPECT_EQ_U(session.summary.newAvailable, 4);
  EXPECT_EQ_U(session.summary.suspended, 1);
  EXPECT_EQ_U(session.summary.reviews, 4);
  EXPECT_EQ_U(session.summary.newCards, 4);
  EXPECT_EQ_U(session.summary.learning, 1);  // only the one that is actually due
  EXPECT_EQ_U(session.summary.nextDueUnix, nowUnix + 300);
  EXPECT_EQ_U(session.cards.size(), 9);

  // Learning first, then the reviews in (due day, ordinal) order with the new
  // cards spread evenly through them.
  EXPECT_EQ_U(session.cards[0], 6);
  const std::vector<Ordinal> tail(session.cards.begin() + 1, session.cards.end());
  const Ordinal expected[] = {2, 8, 0, 9, 3, 10, 1, 11};
  for (size_t i = 0; i < tail.size() && i < 8; i++) EXPECT_EQ_U(tail[i], expected[i]);

  // The suspended card and the not-yet-due review are nowhere in the queue.
  for (Ordinal ordinal : session.cards) EXPECT(ordinal != 5 && ordinal != 4 && ordinal != 7);

  uint32_t nextDue = 0;
  EXPECT(flashcards::nextIntradayDue(fixture.store, nowUnix, nextDue));
  EXPECT_EQ_U(nextDue, nowUnix + 300);
  EXPECT(!flashcards::nextIntradayDue(fixture.store, nowUnix + 3600, nextDue));
}

void testQueueInterleaveIsDeterministic() {
  beginGroup("queue/interleave");

  QueueFixture fixture;
  EXPECT(fixture.build(6, 500));
  EXPECT(fixture.store.writeRecord(0, reviewCard(499, 490)));
  EXPECT(fixture.store.writeRecord(1, reviewCard(499, 490)));
  EXPECT(fixture.store.writeRecord(2, reviewCard(499, 490)));
  EXPECT(fixture.store.writeRecord(3, reviewCard(499, 490)));
  // ordinals 4, 5 stay New

  fsrs::Now now{1700000000u, 500};
  Session first;
  Session second;
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, first));
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, second));
  EXPECT_EQ_U(first.cards.size(), 6);
  // Four reviews, two new: the Bresenham spread puts each new card at the END
  // of its block, so the session opens on a review and closes on a new card.
  const Ordinal expected[] = {0, 1, 4, 2, 3, 5};
  for (uint32_t i = 0; i < first.cards.size() && i < 6; i++) EXPECT_EQ_U(first.cards[i], expected[i]);
  EXPECT(first.cards.sameContents(second.cards));
}

void testQueueDailyLimits() {
  beginGroup("queue/limits");

  QueueFixture fixture;
  EXPECT(fixture.build(40, 800));
  for (Ordinal ordinal = 0; ordinal < 30; ordinal++) {
    EXPECT(fixture.store.writeRecord(ordinal, reviewCard(static_cast<uint16_t>(770 + ordinal), 700)));
  }
  // 10 new cards left (ordinals 30..39). Most of both allowances is spent.
  EXPECT(fixture.store.writeCounters(18, 198));

  fsrs::Now now{1700000000u, 800};
  Session session;
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.summary.dueAvailable, 30);
  EXPECT_EQ_U(session.summary.newAvailable, 10);
  EXPECT_EQ_U(session.summary.reviews, 2);   // 200 - 198
  EXPECT_EQ_U(session.summary.newCards, 2);  // 20 - 18
  EXPECT_EQ_U(session.cards.size(), 4);
  // The two reviews taken are the two most overdue, not the first two scanned.
  EXPECT_EQ_U(session.cards[0], 0);
  EXPECT_EQ_U(session.cards[2], 1);
  EXPECT_EQ_U(session.cards[1], 30);
  EXPECT_EQ_U(session.cards[3], 31);

  // Allowance fully spent: nothing is offered, but the counts still report.
  EXPECT(fixture.store.writeCounters(20, 200));
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.cards.size(), 0);
  EXPECT_EQ_U(session.summary.dueAvailable, 30);
  EXPECT_EQ_U(session.summary.newAvailable, 10);
}

void testQueueLearnAhead() {
  beginGroup("queue/learn-ahead");

  const uint32_t nowUnix = 1700000000u;
  QueueFixture fixture;
  EXPECT(fixture.build(4, 600));
  // Everything is a learning card ahead of now; one is inside the 20-minute
  // window, one is well past it.
  EXPECT(fixture.store.writeRecord(0, learningCard(nowUnix + 600)));
  EXPECT(fixture.store.writeRecord(1, learningCard(nowUnix + 4000)));
  EXPECT(fixture.store.writeRecord(2, learningCard(nowUnix + 1100)));
  CardState done = reviewCard(700, 600);  // due next week, not today
  EXPECT(fixture.store.writeRecord(3, done));

  fsrs::Now now{nowUnix, 600};
  Session session;
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, session));
  // Nothing else to do, so the two cards inside the window are pulled forward,
  // earliest first; the one 66 minutes out is not.
  EXPECT_EQ_U(session.cards.size(), 2);
  EXPECT_EQ_U(session.cards[0], 0);
  EXPECT_EQ_U(session.cards[1], 2);
  EXPECT_EQ_U(session.summary.learning, 2);
  EXPECT_EQ_U(session.summary.nextDueUnix, nowUnix + 600);

  // Give the session a new card to chew on and the learn-ahead cards stay put.
  CardState fresh = newCard();
  EXPECT(fixture.store.writeRecord(3, fresh));
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.cards.size(), 1);
  EXPECT_EQ_U(session.cards[0], 3);
  EXPECT_EQ_U(session.summary.learning, 0);
}

void testQueueModes() {
  beginGroup("queue/modes");

  QueueFixture fixture;
  EXPECT(fixture.build(8, 400));
  EXPECT(fixture.store.writeRecord(0, reviewCard(399, 390)));
  EXPECT(fixture.store.writeRecord(1, reviewCard(500, 390)));  // not due
  CardState suspended = newCard();
  suspended.flags = fsrs::FLAG_SUSPENDED;
  EXPECT(fixture.store.writeRecord(2, suspended));
  EXPECT(fixture.store.writeRecord(3, learningCard(1699999000u)));
  // 4..7 stay New.

  fsrs::Now now{1700000000u, 400};
  Session session;

  EXPECT(flashcards::buildSession(fixture.store, SessionMode::NewOnly, now, session));
  EXPECT_EQ_U(session.cards.size(), 4);
  EXPECT_EQ_U(session.cards[0], 4);
  EXPECT_EQ_U(session.cards[3], 7);
  EXPECT_EQ_U(session.summary.newCards, 4);
  EXPECT_EQ_U(session.summary.suspended, 1);

  // NewOnly still respects the daily new allowance.
  EXPECT(fixture.store.writeCounters(18, 0));
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::NewOnly, now, session));
  EXPECT_EQ_U(session.cards.size(), 2);
  EXPECT(fixture.store.writeCounters(0, 0));

  EXPECT(flashcards::buildSession(fixture.store, SessionMode::CramAll, now, session));
  EXPECT_EQ_U(session.cards.size(), 7);  // everything but the suspended card
  for (uint32_t i = 0; i < session.cards.size(); i++) {
    EXPECT(session.cards[i] != 2);
    if (i > 0) EXPECT(session.cards[i] > session.cards[i - 1]);  // deck order
  }
  EXPECT_EQ_U(session.summary.reviews, 2);
  EXPECT_EQ_U(session.summary.learning, 1);
  EXPECT_EQ_U(session.summary.newCards, 4);
  EXPECT_EQ_U(session.summary.suspended, 1);

  // Cram ignores the daily limits entirely.
  EXPECT(fixture.store.writeCounters(20, 200));
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::CramAll, now, session));
  EXPECT_EQ_U(session.cards.size(), 7);
}

/**
 * R3a SHOULD-6. Learn-ahead has to be re-evaluated when the queue DRAINS, not
 * only when it is built: a card 25 minutes out at session start is 5 minutes
 * out by the time the queue empties, and Anki pulls it forward. This is the
 * accessor R3b's drain path calls instead of rebuilding the session.
 */
void testLearnAheadAtDrain() {
  beginGroup("queue/learn-ahead drain");

  const uint32_t nowUnix = 1700000000u;
  QueueFixture fixture;
  EXPECT(fixture.build(6, 900));
  EXPECT(fixture.store.writeRecord(0, reviewCard(899, 890)));          // a due review, not intraday
  EXPECT(fixture.store.writeRecord(1, learningCard(nowUnix + 1500)));  // 25 min out
  EXPECT(fixture.store.writeRecord(2, learningCard(nowUnix + 900)));   // 15 min out
  CardState suspended = learningCard(nowUnix + 60);                    // 1 min out, but suspended
  suspended.flags = fsrs::FLAG_SUSPENDED;
  EXPECT(fixture.store.writeRecord(3, suspended));
  CardState relearn = learningCard(nowUnix + 900);  // ties with ordinal 2
  relearn.state = CardPhase::Relearning;
  EXPECT(fixture.store.writeRecord(4, relearn));
  // ordinal 5 stays New.

  Ordinal ordinal = 0xFFFF;
  uint32_t due = 0;
  fsrs::Now now{nowUnix, 900};
  // Ordinal 1 is outside the 20-minute window; 2 and 4 tie and the lower
  // ordinal wins; the suspended card is invisible.
  EXPECT(flashcards::nextLearnAheadOrdinal(fixture.store, now, ordinal, due));
  EXPECT_EQ_U(ordinal, 2);
  EXPECT_EQ_U(due, nowUnix + 900);

  // Twenty minutes later the 25-minute card is inside the window; the two that
  // were are now simply due.
  fsrs::Now later{nowUnix + 1200, 900};
  EXPECT(flashcards::nextLearnAheadOrdinal(fixture.store, later, ordinal, due));
  EXPECT_EQ_U(ordinal, 2);
  EXPECT_EQ_U(due, nowUnix + 900);

  // With the two nearest answered away, the 25-minute card is what comes next —
  // but only once the clock has moved, which is the whole point of re-asking at
  // drain time rather than reusing the answer the session was built with.
  EXPECT(fixture.store.writeRecord(2, reviewCard(1000, 900)));
  EXPECT(fixture.store.writeRecord(4, reviewCard(1000, 900)));
  EXPECT(!flashcards::nextLearnAheadOrdinal(fixture.store, now, ordinal, due));
  EXPECT(flashcards::nextLearnAheadOrdinal(fixture.store, later, ordinal, due));
  EXPECT_EQ_U(ordinal, 1);
  EXPECT_EQ_U(due, nowUnix + 1500);

  // Nothing intraday left at all: the done screen's answer, not an error.
  EXPECT(fixture.store.writeRecord(1, reviewCard(1000, 900)));
  EXPECT(!flashcards::nextLearnAheadOrdinal(fixture.store, now, ordinal, due));
  // The suspended card stays out even when it is the only intraday record.
  EXPECT(!flashcards::nextLearnAheadOrdinal(fixture.store, later, ordinal, due));
}

void testQueueEmptyDeck() {
  beginGroup("queue/empty");

  QueueFixture fixture;
  EXPECT(fixture.build(3, 200));
  CardState suspended = newCard();
  suspended.flags = fsrs::FLAG_SUSPENDED;
  for (Ordinal ordinal = 0; ordinal < 3; ordinal++) EXPECT(fixture.store.writeRecord(ordinal, suspended));

  fsrs::Now now{1700000000u, 200};
  Session session;
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.cards.size(), 0);
  EXPECT_EQ_U(session.summary.suspended, 3);
  EXPECT_EQ_U(session.summary.nextDueUnix, 0);
  EXPECT(flashcards::buildSession(fixture.store, SessionMode::CramAll, now, session));
  EXPECT_EQ_U(session.cards.size(), 0);
}

// --- StudyClock (the pure half) ----------------------------------------------

/**
 * R3a NIT-13. StudyClock.cpp itself needs an RTC, a settings blob and the DST
 * rule, none of which belong in a host test — but the arithmetic that can
 * actually be wrong (the −48 bias, the ×900, the clamp on a corrupt persisted
 * value) is a pure function in the header, and that is what is tested here.
 */
void testUtcOffsetComposition() {
  beginGroup("clock/utc offset");

  using flashcards::utcOffsetSecsFromQuarters;

  EXPECT_EQ_U(utcOffsetSecsFromQuarters(48), 0);             // UTC+0, the default
  EXPECT(utcOffsetSecsFromQuarters(52) == 3600);             // UTC+1
  EXPECT(utcOffsetSecsFromQuarters(44) == -3600);            // UTC-1
  EXPECT(utcOffsetSecsFromQuarters(49) == 900);              // a quarter-hour step is 900 s
  EXPECT(utcOffsetSecsFromQuarters(0) == -48 * 15 * 60);     // UTC-12, the low end
  EXPECT(utcOffsetSecsFromQuarters(70) == 5 * 3600 + 1800);  // UTC+5:30, India
  EXPECT(utcOffsetSecsFromQuarters(51) == 2700);             // UTC+0:45, Nepal-style quarter
  EXPECT(utcOffsetSecsFromQuarters(104) == 14 * 3600);       // UTC+14, the high end

  // A corrupt persisted byte clamps to UTC+14 rather than composing a wild
  // offset that would move the day number by days.
  EXPECT(utcOffsetSecsFromQuarters(105) == 14 * 3600);
  EXPECT(utcOffsetSecsFromQuarters(255) == 14 * 3600);

  // The clamp is constant-evaluable, so it costs nothing at run time.
  static_assert(utcOffsetSecsFromQuarters(48) == 0, "UTC+0 is the 48 bias point");
  static_assert(utcOffsetSecsFromQuarters(200) == 14 * 3600, "a corrupt offset clamps");

  // And the composed value is what a day number is actually computed from:
  // 03:59 local is still the previous study day, 04:00 is the new one.
  const int32_t indiaOffset = utcOffsetSecsFromQuarters(70);
  const uint32_t midnightUtc = 1700000000u - (1700000000u % 86400u);
  const uint32_t local0359 = midnightUtc + 3 * 3600 + 3599 - static_cast<uint32_t>(indiaOffset);
  const uint32_t local0400 = midnightUtc + 4 * 3600 - static_cast<uint32_t>(indiaOffset);
  EXPECT_EQ_U(fsrs::dayNumber(local0400, indiaOffset), fsrs::dayNumber(local0359, indiaOffset) + 1);
}

}  // namespace

int main(int argc, char** argv) {
  g_root = argc > 1 ? argv[1] : "/tmp/crosspoint-flashcard-tests";
  Storage.ensureDirectoryExists(g_root.c_str());
  makeDirs();

  testKeySort();
  testRealFixture();
  testDeckValidation();
  testDeckParameterBlock();
  testEmptyBack();
  testC0Sanitize();
  testStateCreation();
  testStateAdoptAndCounters();
  testStateRebuildsFromRubbish();
  testMerge();
  testMergeEdgeCases();
  testMergeCrashWindow();
  testTornKeyHeals();
  testMaximumDeck();
  testQueueBasics();
  testQueueInterleaveIsDeterministic();
  testQueueDailyLimits();
  testQueueLearnAhead();
  testLearnAheadAtDrain();
  testQueueModes();
  testQueueEmptyDeck();
  testUtcOffsetComposition();

  resetCard();
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    printf("FLASHCARD HOST TESTS FAILED\n");
    return 1;
  }
  printf("FLASHCARD HOST TESTS PASSED\n");
  return 0;
}
