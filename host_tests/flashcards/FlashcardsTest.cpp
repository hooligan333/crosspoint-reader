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
//   * gated allocations  -- C3 only: every gate driven into refusing
//   * cross-variant      -- the two builds reading each other's files
//
// BOTH CONFIGURATIONS. build.sh compiles and runs this file twice, with and
// without -DCROSSPOINT_FLASHCARDS_C3 (FLASHCARD_SPEC.md §7b.4), and everything
// below must pass in both. The C3 variant is an in-RAM change -- no resident
// card index, a merge that holds only (key, ordinal) pairs -- so the behavioural
// groups are written once and simply run twice. Only TWO divergences are
// documented and both are asserted per configuration rather than skipped:
//
//   * DECK_MAX_CARDS is 2000 rather than 40000 (deck/cap, state/max deck);
//   * the duplicate-key check is skipped on the C3 (deck/duplicate key), where
//     the group instead pins what the trust boundary actually costs.
//
// Anything else that came out differently between the two runs would be a bug,
// which is the whole point of not having a second suite.
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
#ifdef CROSSPOINT_FLASHCARDS_C3
// Only the C3 paths gate their allocations, and only this configuration can
// therefore drive a gate into refusing. stubs/HalHeapGauge.h makes the figure
// settable; the device header it stands in for is two esp_heap_caps calls.
#include "HalHeapGauge.h"
#endif

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

// --- v2 card images (CPDK v2, DECK_SERVER_SPEC.md §3.7) ----------------------
//
// Real JPEGs, not hand-rolled marker soup: the reader's open-time checks are
// structural, but loadImage() walks an actual JPEG's marker segments to its
// start-of-frame and refuses anything that is not the baseline single-component
// frame of exactly the table's size that §3.7's encoding contract promises. The
// three fixtures below are the three answers that walk can give, generated once
// with Pillow at quality 80 (the converter's own setting) and checked in as
// bytes so this suite stays a plain g++ build with no image toolchain.

// Baseline 8-bit grayscale, 16x12 -- what the converter is contracted to emit.
const uint8_t JPEG_GRAY_16x12[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x06, 0x04, 0x05, 0x06, 0x05, 0x04, 0x06, 0x06, 0x05, 0x06, 0x07, 0x07, 0x06,
    0x08, 0x0a, 0x10, 0x0a, 0x0a, 0x09, 0x09, 0x0a, 0x14, 0x0e, 0x0f, 0x0c, 0x10, 0x17, 0x14, 0x18, 0x18, 0x17, 0x14,
    0x16, 0x16, 0x1a, 0x1d, 0x25, 0x1f, 0x1a, 0x1b, 0x23, 0x1c, 0x16, 0x16, 0x20, 0x2c, 0x20, 0x23, 0x26, 0x27, 0x29,
    0x2a, 0x29, 0x19, 0x1f, 0x2d, 0x30, 0x2d, 0x28, 0x30, 0x25, 0x28, 0x29, 0x28, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00,
    0x0c, 0x00, 0x10, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04,
    0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33,
    0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6,
    0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xda, 0x00, 0x08, 0x01,
    0x01, 0x00, 0x00, 0x3f, 0x00, 0x93, 0x4a, 0xf8, 0x6f, 0xa3, 0xf8, 0x5f, 0x4a, 0xfe, 0xd2, 0xf1, 0x0d, 0xc5, 0xb6,
    0x9f, 0x64, 0x9c, 0x79, 0x93, 0x90, 0xbb, 0x98, 0x29, 0x6d, 0xaa, 0x3a, 0xb3, 0x10, 0xa7, 0x0a, 0xa0, 0x93, 0x8e,
    0x01, 0xa8, 0xa5, 0xf8, 0x80, 0x90, 0x5d, 0x25, 0xb7, 0x80, 0xb4, 0x08, 0xae, 0x63, 0x8d, 0xf0, 0xd7, 0xda, 0x9a,
    0x30, 0x49, 0x00, 0x2c, 0x0e, 0xc8, 0x94, 0x86, 0xc1, 0xf9, 0x18, 0x33, 0x30, 0x3d, 0x41, 0x41, 0xd6, 0xb9, 0x4f,
    0x0c, 0x68, 0xd6, 0xfa, 0xbd, 0xf9, 0xbe, 0xd5, 0x65, 0xb9, 0xbe, 0xbd, 0x97, 0x6f, 0x99, 0x71, 0x73, 0x29, 0x96,
    0x47, 0xc0, 0x00, 0x65, 0x9b, 0x24, 0xe0, 0x00, 0x3e, 0x80, 0x57, 0xb2, 0xc7, 0xa5, 0x58, 0x68, 0x3e, 0x0b, 0xd6,
    0xf5, 0x7b, 0x3b, 0x58, 0x9e, 0xe7, 0x4f, 0xd3, 0xe7, 0xbb, 0x89, 0x65, 0x19, 0x46, 0x78, 0xe3, 0x66, 0x01, 0x80,
    0xc1, 0xc6, 0x40, 0xce, 0x08, 0xaf, 0xff, 0xd9,
};

// Progressive grayscale, 16x12 -- SOF2. JPEGDEC decodes a progressive frame at
// a DC-only eighth scale, so this would draw a quarter-sized smear inside a
// full-sized reserved box; it is refused before the decoder ever sees it.
const uint8_t JPEG_PROGRESSIVE_16x12[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x06, 0x04, 0x05, 0x06, 0x05, 0x04, 0x06, 0x06, 0x05, 0x06, 0x07, 0x07, 0x06,
    0x08, 0x0a, 0x10, 0x0a, 0x0a, 0x09, 0x09, 0x0a, 0x14, 0x0e, 0x0f, 0x0c, 0x10, 0x17, 0x14, 0x18, 0x18, 0x17, 0x14,
    0x16, 0x16, 0x1a, 0x1d, 0x25, 0x1f, 0x1a, 0x1b, 0x23, 0x1c, 0x16, 0x16, 0x20, 0x2c, 0x20, 0x23, 0x26, 0x27, 0x29,
    0x2a, 0x29, 0x19, 0x1f, 0x2d, 0x30, 0x2d, 0x28, 0x30, 0x25, 0x28, 0x29, 0x28, 0xff, 0xc2, 0x00, 0x0b, 0x08, 0x00,
    0x0c, 0x00, 0x10, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x16, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x05, 0x06, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x01, 0x42, 0x56, 0xcb, 0xff, 0xc4, 0x00, 0x19, 0x10, 0x00, 0x02, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x01, 0x04, 0x05, 0x11, 0xff, 0xda, 0x00, 0x08, 0x01,
    0x01, 0x00, 0x01, 0x05, 0x02, 0x56, 0x6a, 0x6a, 0xaa, 0x74, 0x38, 0x55, 0x92, 0x2d, 0x38, 0x50, 0x22, 0x97, 0xff,
    0xc4, 0x00, 0x21, 0x10, 0x00, 0x02, 0x02, 0x01, 0x02, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x00, 0x03, 0x12, 0x04, 0x11, 0x13, 0x31, 0x41, 0x42, 0x51, 0x61, 0x81, 0xff, 0xda, 0x00, 0x08,
    0x01, 0x01, 0x00, 0x06, 0x3f, 0x02, 0xe2, 0x6a, 0x19, 0x6b, 0x4f, 0x26, 0x63, 0xa0, 0xa0, 0x30, 0x1d, 0xf6, 0x75,
    0xf9, 0x33, 0xb4, 0xb3, 0xb9, 0xe6, 0xcc, 0x77, 0x32, 0xeb, 0x51, 0x46, 0x55, 0xd6, 0x58, 0x6f, 0xe8, 0x4f, 0xff,
    0xc4, 0x00, 0x1b, 0x10, 0x01, 0x00, 0x02, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x11, 0x21, 0x00, 0x31, 0x51, 0x81, 0x61, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x3f, 0x21,
    0x80, 0x41, 0xf1, 0x96, 0x26, 0x0e, 0xb5, 0xa3, 0x00, 0xd2, 0x16, 0x6c, 0x6d, 0xa1, 0x7c, 0x65, 0x7c, 0xca, 0x9c,
    0x4b, 0x03, 0xd7, 0x13, 0x68, 0x1b, 0x45, 0x46, 0xf3, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x10,
    0x5f, 0xff, 0xc4, 0x00, 0x19, 0x10, 0x01, 0x01, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x11, 0x00, 0x21, 0x31, 0x71, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x3f, 0x10,
    0xe4, 0x55, 0xd4, 0x81, 0x41, 0xd4, 0x14, 0x05, 0x66, 0x87, 0x25, 0xbf, 0x21, 0x88, 0x43, 0x71, 0x0e, 0xa0, 0x3d,
    0x13, 0xdc, 0x77, 0xb7, 0x63, 0x94, 0x02, 0xd1, 0x60, 0x07, 0x81, 0x83, 0x33, 0x9d, 0xa5, 0x00, 0x11, 0x94, 0x2c,
    0x4c, 0xff, 0xd9,
};

// Baseline COLOUR, 16x12 -- three components where the contract promises one.
const uint8_t JPEG_RGB_16x12[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x06, 0x04, 0x05, 0x06, 0x05, 0x04, 0x06, 0x06, 0x05, 0x06, 0x07, 0x07, 0x06,
    0x08, 0x0a, 0x10, 0x0a, 0x0a, 0x09, 0x09, 0x0a, 0x14, 0x0e, 0x0f, 0x0c, 0x10, 0x17, 0x14, 0x18, 0x18, 0x17, 0x14,
    0x16, 0x16, 0x1a, 0x1d, 0x25, 0x1f, 0x1a, 0x1b, 0x23, 0x1c, 0x16, 0x16, 0x20, 0x2c, 0x20, 0x23, 0x26, 0x27, 0x29,
    0x2a, 0x29, 0x19, 0x1f, 0x2d, 0x30, 0x2d, 0x28, 0x30, 0x25, 0x28, 0x29, 0x28, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x07,
    0x07, 0x07, 0x0a, 0x08, 0x0a, 0x13, 0x0a, 0x0a, 0x13, 0x28, 0x1a, 0x16, 0x1a, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28,
    0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28,
    0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28,
    0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x0c, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00,
    0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00, 0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62,
    0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85,
    0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00, 0x1f, 0x01, 0x00,
    0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04,
    0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
    0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09,
    0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8,
    0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0xfa, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x6d, 0x86, 0x8f, 0x6d,
    0x61, 0xa1, 0xb4, 0x97, 0x11, 0xc3, 0x67, 0x60, 0x89, 0xe5, 0x97, 0x11, 0x9b, 0x6b, 0x79, 0x4f, 0x96, 0xec, 0x40,
    0xdc, 0x4c, 0xb2, 0x1c, 0x67, 0xf7, 0x40, 0x0d, 0xd8, 0x38, 0x19, 0x19, 0x28, 0xda, 0xa9, 0x86, 0xfa, 0x1b, 0x6b,
    0x28, 0x23, 0xf2, 0x1d, 0x80, 0x65, 0xbb, 0x49, 0x04, 0x52, 0xa6, 0xe7, 0x38, 0x4b, 0x68, 0x88, 0x6d, 0xa7, 0x0a,
    0x56, 0x47, 0x3b, 0xb8, 0xc1, 0x00, 0xe0, 0xd5, 0x4d, 0x2e, 0xcd, 0x24, 0xf1, 0x95, 0xd5, 0x99, 0x92, 0x5d, 0xc8,
    0xa1, 0x5e, 0xe9, 0xdb, 0xcc, 0xb8, 0x94, 0x79, 0x49, 0x8d, 0xd2, 0x3e, 0x4e, 0x54, 0x70, 0x0a, 0xe0, 0x81, 0xdf,
    0x81, 0x8d, 0xeb, 0x8b, 0x68, 0x61, 0xd3, 0x3c, 0x5b, 0x3c, 0x71, 0x85, 0x1a, 0x64, 0x57, 0x8e, 0xb1, 0x82, 0x40,
    0xb8, 0x68, 0xd6, 0x46, 0x06, 0x52, 0x0e, 0xe6, 0x04, 0xfd, 0xe1, 0x90, 0x0f, 0xa6, 0x79, 0xaa, 0xaa, 0xb9, 0x1b,
    0xe7, 0x6d, 0xdd, 0x27, 0xe5, 0xae, 0xdf, 0x35, 0xf7, 0x79, 0x15, 0x83, 0x52, 0xae, 0xbf, 0xd9, 0x92, 0xa7, 0x16,
    0xaf, 0x7d, 0xe5, 0xf7, 0xda, 0xc9, 0xfa, 0x7d, 0xe7, 0xff, 0xd9,
};

/** One image-table entry, plus the encoded bytes that go in the image blob. */
struct TestImage {
  const uint8_t* jpeg;
  size_t bytes;
  uint16_t width;
  uint16_t height;
  uint32_t reserved = 0;
  // Overrides for the malformed cases: when non-zero these replace what the
  // builder would otherwise compute, so a test can point an entry past the blob
  // or claim a size the JPEG does not have.
  uint32_t forceBlobOff = 0;
  uint32_t forceByteLen = 0;
  bool useForcedOffsets = false;
};

/** One placement-table entry, verbatim. The builder does NOT sort them. */
struct TestPlacement {
  uint16_t ordinal;
  uint8_t side;
  uint32_t textOffset;
  uint32_t imageIndex;
  uint8_t reserved = 0;
};

TestImage grayImage() { return TestImage{JPEG_GRAY_16x12, sizeof(JPEG_GRAY_16x12), 16, 12, 0, 0, 0, false}; }

/**
 * A well-formed CPDK v2 file: header, optional FSRS block, image table,
 * placement table, card index, text blob, image blob.
 *
 * Deliberately a separate builder rather than a flag on buildDeck(): v1 files
 * must keep coming out of that one byte-identically, and every v1 assertion in
 * this suite is written against it.
 */
std::vector<uint8_t> buildDeckV2(const std::vector<TestCard>& cards, const std::vector<TestImage>& images,
                                 const std::vector<TestPlacement>& placements, const float* w = nullptr) {
  const bool hasParams = w != nullptr;
  const size_t imageTableStart = 64 + (hasParams ? 84u : 0u);
  const size_t placementStart = imageTableStart + 4 + images.size() * 16;
  const size_t indexStart = placementStart + 4 + placements.size() * 12;

  std::vector<uint8_t> out;
  out.resize(indexStart + cards.size() * 20, 0);
  // An explicit size guard, and it is here for the COMPILER. `indexStart` is
  // built from two container sizes, and g++ 11 loses the vector's allocation
  // size across that: it then reports the 64-byte header's own writes as
  // "writing 2 bytes into a region of size 0" and -Werror=stringop-overflow
  // fails the build. Restating the invariant the arithmetic already guarantees
  // (a CPDK file is at least its 64-byte header) hands the fact back.
  if (out.size() < 64) return out;
  memcpy(out.data(), "CPDK", 4);
  putU16(out, 4, 2);
  putU16(out, 6, static_cast<uint16_t>((hasParams ? 1 : 0) | 2));  // has_images is always set in v2
  putU32(out, 8, static_cast<uint32_t>(cards.size()));
  const char* title = "Test Deck";
  out[20] = static_cast<uint8_t>(strlen(title));
  memcpy(out.data() + 21, title, strlen(title));
  if (hasParams) memcpy(out.data() + 64, w, 84);

  // The image blob is written densely, and the table records where each image
  // landed in it. Density is part of the CONTRACT (§2.2 I1 pins), not merely
  // what the converter happens to do: the reader validates the tiling at open,
  // and that is what lets it trust the blob's derived start. A table that does
  // NOT tile is written by a test, either through TestImage's forced offsets or
  // by patching an entry's blob_off after the fact.
  std::vector<uint8_t> imageBlob;
  putU32(out, imageTableStart, static_cast<uint32_t>(images.size()));
  for (size_t i = 0; i < images.size(); i++) {
    const size_t entry = imageTableStart + 4 + i * 16;
    const uint32_t blobOff = static_cast<uint32_t>(imageBlob.size());
    const uint32_t byteLen = static_cast<uint32_t>(images[i].bytes);
    putU32(out, entry, images[i].useForcedOffsets ? images[i].forceBlobOff : blobOff);
    putU32(out, entry + 4, images[i].useForcedOffsets ? images[i].forceByteLen : byteLen);
    putU16(out, entry + 8, images[i].width);
    putU16(out, entry + 10, images[i].height);
    putU32(out, entry + 12, images[i].reserved);
    imageBlob.insert(imageBlob.end(), images[i].jpeg, images[i].jpeg + images[i].bytes);
  }

  putU32(out, placementStart, static_cast<uint32_t>(placements.size()));
  for (size_t i = 0; i < placements.size(); i++) {
    const size_t entry = placementStart + 4 + i * 12;
    putU16(out, entry, placements[i].ordinal);
    out[entry + 2] = placements[i].side;
    out[entry + 3] = placements[i].reserved;
    putU32(out, entry + 4, placements[i].textOffset);
    putU32(out, entry + 8, placements[i].imageIndex);
  }

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
  out.insert(out.end(), imageBlob.begin(), imageBlob.end());

  // v2 folds the image table's (blob_off, byte_len) pairs into the same FNV
  // pass as the card keys, so that an edit which only replaced a picture still
  // changes the guid the feed compares (§3.7). With no images the second half
  // is empty and v1 and v2 agree, which the group below asserts.
  std::vector<uint8_t> hashInput(cards.size() * 8);
  for (size_t i = 0; i < cards.size(); i++) memcpy(hashInput.data() + i * 8, &cards[i].key, 8);
  for (size_t i = 0; i < images.size(); i++) {
    const size_t entry = imageTableStart + 4 + i * 16;
    hashInput.insert(hashInput.end(), out.begin() + static_cast<long>(entry),
                     out.begin() + static_cast<long>(entry) + 8);
  }
  putU64(out, 12, fnv1a64(hashInput.data(), hashInput.size()));
  return out;
}

/**
 * Cards whose keys are deliberately NOT ascending with their ordinals.
 *
 * makeCards() below hands out keys in ordinal order, which quietly makes the
 * merge's sorted key array the identity permutation: `oldSlots[found] == found`
 * for every card, so a merge that dropped the oldSlots indirection entirely
 * would still pass. splitmix64's finaliser over a bijective input is itself a
 * bijection on 64 bits, so the keys stay unique — the deck reader's duplicate
 * check would say so on the default build — while their sorted order has
 * nothing whatever to do with the ordinal order (FLASHCARD_SPEC.md §7b.4 asks
 * for exactly this).
 */
std::vector<TestCard> makeShuffledCards(size_t count, uint64_t salt = 0) {
  std::vector<TestCard> cards;
  cards.reserve(count);
  for (size_t i = 0; i < count; i++) {
    char text[32];
    snprintf(text, sizeof(text), "front %u", static_cast<unsigned>(i));
    std::string front(text);
    snprintf(text, sizeof(text), "back %u", static_cast<unsigned>(i));
    uint64_t key = 0x9E3779B97F4A7C15ULL * (static_cast<uint64_t>(i) + 1) + salt;
    key ^= key >> 30;
    key *= 0xBF58476D1CE4E5B9ULL;
    key ^= key >> 27;
    key *= 0x94D049BB133111EBULL;
    key ^= key >> 31;
    cards.push_back(TestCard{key, front, std::string(text)});
  }
  return cards;
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

/** Disarms any injected I/O fault and zeroes the op counters (stubs/HalStorage.h). */
void resetIo() {
  hostIoFaults().reset();
  hostIoCounters().zero();
}

/**
 * Arms a short read AND a refused seek on the deck file, i.e. the card going
 * bad underneath an open deck. `allow` operations of each kind get through
 * first, so a test can let a deck open and validate and only then break it.
 */
void failDeckIo(int allow = 0) {
  hostIoFaults().reset();
  hostIoFaults().pathSuffix = DECK_LEAF;
  hostIoFaults().allowReads = allow;
  hostIoFaults().allowSeeks = allow;
}

/** Wipes the deck, its state and any stray temp so each group starts clean. */
void resetCard() {
  resetIo();
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
      {"version 3", DeckError::BadVersion, [](std::vector<uint8_t>& b) { putU16(b, 4, 3); }},
      // v2 is READ by this build now (CPDK v2, FLASHCARD_SPEC.md §2.2), so the
      // version alone is no longer a refusal -- but a v2 header that does not
      // set has_images is a file nobody wrote (an --images run that embedded
      // nothing is emitted as v1), and bit1 on a V1 header is still an unknown
      // bit, because v1 is exactly the version that does not know what it
      // means. Both directions are pinned here; the rest of the v2 matrix has
      // its own group below.
      {"version 2 without has_images", DeckError::BadImageTable, [](std::vector<uint8_t>& b) { putU16(b, 4, 2); }},
      {"has_images on a v1 header", DeckError::UnknownFlags, [](std::vector<uint8_t>& b) { putU16(b, 6, 0x0002); }},
      {"reserved flag", DeckError::UnknownFlags, [](std::vector<uint8_t>& b) { putU16(b, 6, 0x0004); }},
      {"zero cards", DeckError::BadCardCount, [](std::vector<uint8_t>& b) { putU32(b, 8, 0); }},
      // One past whatever this build's cap is: 40001 by default, 2001 on the C3
      // (FLASHCARD_SPEC.md §7b.3). The dedicated cap group below pins the
      // boundary itself from both sides.
      {"over the cap", DeckError::BadCardCount,
       [](std::vector<uint8_t>& b) { putU32(b, 8, flashcards::DECK_MAX_CARDS + 1); }},
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
  deck.close();
}

/**
 * The duplicate-key check: the ONE behavioural divergence between the two
 * builds (FLASHCARD_SPEC.md §7b.3). The default build refuses the deck; the C3
 * build skips the check and opens it, because the sorted key copy the check
 * needs is a contiguous 16 KB block and both upstream stages (convert_deck.py,
 * read_deck.py) already refuse a repeated key.
 *
 * What matters is that the C3 side is not merely "opens anyway": the state file
 * still keys every RECORD by ordinal, so the two twins get their own records
 * and their own schedules, and neither reads the other's. That is what is
 * asserted here — the trust boundary's actual blast radius, not just its
 * existence.
 */
void testDuplicateKeyPolicy() {
  beginGroup("deck/duplicate key");
  resetCard();
  makeDirs();

  std::vector<TestCard> cards = makeCards(4);
  cards[3].key = cards[0].key;  // ordinals 0 and 3 are now twins
  std::vector<uint8_t> bytes = buildDeck(cards);

  DeckFile deck;
  const DeckError error = installDeck(deck, bytes);
#ifdef CROSSPOINT_FLASHCARDS_C3
  EXPECT_EQ_U(static_cast<int>(error), static_cast<int>(DeckError::Ok));
  EXPECT(deck.isOpen());
  EXPECT_EQ_U(deck.keyAt(0), cards[0].key);
  EXPECT_EQ_U(deck.keyAt(3), cards[0].key);

  // Records are addressed by ordinal, so the twins do NOT share a schedule.
  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 500)), static_cast<int>(StateError::Ok));
  EXPECT(store.writeRecord(0, reviewCard(400, 390)));
  EXPECT(store.writeRecord(3, reviewCard(450, 440)));
  CardState back{};
  EXPECT_EQ_U(static_cast<int>(store.readRecord(0, back)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(back.due, 400);
  EXPECT_EQ_U(static_cast<int>(store.readRecord(3, back)), static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(back.due, 450);
  // And the per-record key echo still guards each ordinal: a record carrying
  // the twin key is accepted (it IS this ordinal's key), one carrying anything
  // else is healed, exactly as on the Pro.
  EXPECT(!store.keyMismatchSeen());
  store.close();
#else
  EXPECT_EQ_U(static_cast<int>(error), static_cast<int>(DeckError::DuplicateKey));
  EXPECT(!deck.isOpen());
  EXPECT_EQ_U(deck.cardCount(), 0);
#endif
  deck.close();
  resetCard();
}

/**
 * The card-count cap from both sides. Exactly at the cap must open; one past it
 * must be refused with BadCardCount, and no other error -- the study screen
 * distinguishes "this deck is too big for this reader" from "this file is
 * broken" only by that value.
 *
 * The deck built here is a header + index with a card count only; the blob is
 * empty and every slice is zero-length, which is legal (DECK_SERVER_SPEC §3.6.5)
 * and keeps a 40000-card default-config case from costing megabytes of fixture.
 */
void testCardCountCap() {
  beginGroup("deck/cap");
  resetCard();
  makeDirs();

  const uint32_t cap = flashcards::DECK_MAX_CARDS;
#ifdef CROSSPOINT_FLASHCARDS_C3
  EXPECT_EQ_U(cap, 2000);
#else
  EXPECT_EQ_U(cap, 40000);
#endif

  // Empty-sided cards, so the file is 64 + 20*count bytes and nothing more.
  std::vector<TestCard> atCap;
  atCap.reserve(cap);
  for (uint32_t i = 0; i < cap; i++) atCap.push_back(TestCard{0x2000000000000000ULL + i, "", ""});

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(atCap))), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(deck.cardCount(), cap);
  EXPECT_EQ_U(deck.keyAt(static_cast<Ordinal>(cap - 1)), atCap[cap - 1].key);
  deck.close();

  // One past the cap, as a genuine file rather than a doctored header, so the
  // refusal cannot be coming from a size check.
  std::vector<TestCard> overCap = atCap;
  overCap.push_back(TestCard{0x2000000000000000ULL + cap, "", ""});
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(overCap))), static_cast<int>(DeckError::BadCardCount));
  EXPECT(!deck.isOpen());
  deck.close();
  resetCard();
}

/**
 * Index validation must cover the WHOLE index, including whatever falls in the
 * final, short read. The C3 build streams the index in 128-entry chunks, so a
 * 300-card deck gives two full chunks and a 44-entry tail; corrupting an entry
 * in that tail is the case a "validate the first chunk and stop" bug survives.
 * The last card, the last card of the first chunk and the first of the second
 * are all probed. Runs in both configurations: the default build validates the
 * same entries out of the resident index and must agree.
 */
void testIndexValidationAcrossChunks() {
  beginGroup("deck/index chunks");
  resetCard();
  makeDirs();

  const size_t count = 300;
  const std::vector<TestCard> cards = makeCards(count);
  const std::vector<uint8_t> good = buildDeck(cards);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, good)), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(deck.cardCount(), count);
  deck.close();

  // Ordinals at, either side of, and well past the 128-entry chunk boundary.
  const size_t probes[] = {0, 127, 128, 129, 255, 256, count - 2, count - 1};
  for (size_t ordinal : probes) {
    std::vector<uint8_t> bytes = good;
    // front_off far outside the blob: the slice check must catch it wherever
    // the entry happens to sit in the stream.
    putU32(bytes, 64 + ordinal * 20 + 8, 0x40000000u);
    const DeckError error = installDeck(deck, bytes);
    if (error != DeckError::SliceOutOfRange) {
      ++g_failures;
      printf("FAIL [%s] ordinal %u: got %s, want %s\n", g_group, static_cast<unsigned>(ordinal),
             flashcards::deckErrorName(error), flashcards::deckErrorName(DeckError::SliceOutOfRange));
    }
    ++g_checks;
    EXPECT(!deck.isOpen());

    // Same ordinal, this time only the LENGTH is over the 4096-byte cap.
    bytes = good;
    putU16(bytes, 64 + ordinal * 20 + 12, 5000);
    ++g_checks;
    if (installDeck(deck, bytes) != DeckError::SliceOutOfRange) {
      ++g_failures;
      printf("FAIL [%s] ordinal %u length cap not caught\n", g_group, static_cast<unsigned>(ordinal));
    }
  }
  deck.close();

  // A truncated index (the file stops mid-chunk) must read as ShortFile rather
  // than being validated out of whatever the last read left in the buffer.
  std::vector<uint8_t> truncated = good;
  truncated.resize(64 + 200 * 20);
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, truncated)), static_cast<int>(DeckError::ShortFile));
  deck.close();
  resetCard();
}

/**
 * The window cache behind the C3 build's keyAt()/loadSide(). Every ordinal in a
 * 300-card deck must resolve to the right key and the right text, walked
 * forwards, backwards and at random, because a stale window would only show up
 * as the WRONG card rather than as an error. Cheap and correct on the default
 * build too, where it just re-checks the resident index.
 */
void testOnDemandIndexAccess() {
  beginGroup("deck/index access");
  resetCard();
  makeDirs();

  const size_t count = 300;
  const std::vector<TestCard> cards = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));

  char text[flashcards::DECK_MAX_SLICE_BYTES + 1];
  uint16_t length = 0;
  bool forwardOk = true;
  for (size_t i = 0; i < count; i++) {
    if (deck.keyAt(static_cast<Ordinal>(i)) != cards[i].key) forwardOk = false;
  }
  EXPECT(forwardOk);

  bool backwardOk = true;
  for (size_t i = count; i > 0; i--) {
    if (deck.keyAt(static_cast<Ordinal>(i - 1)) != cards[i - 1].key) backwardOk = false;
  }
  EXPECT(backwardOk);

  // A deliberately window-hostile walk: every step crosses a 32-entry boundary.
  bool scatterOk = true;
  for (size_t step = 0; step < count; step++) {
    const size_t i = (step * 37) % count;
    if (deck.keyAt(static_cast<Ordinal>(i)) != cards[i].key) scatterOk = false;
    if (!deck.loadSide(static_cast<Ordinal>(i), CardSide::Front, text, sizeof(text), length)) scatterOk = false;
    if (cards[i].front != text) scatterOk = false;
    if (!deck.loadSide(static_cast<Ordinal>(i), CardSide::Back, text, sizeof(text), length)) scatterOk = false;
    if (cards[i].back != text) scatterOk = false;
  }
  EXPECT(scatterOk);

  // Out of range stays out of range however the entry is fetched.
  EXPECT_EQ_U(deck.keyAt(static_cast<Ordinal>(count)), 0);
  EXPECT(!deck.loadSide(static_cast<Ordinal>(count), CardSide::Front, text, sizeof(text), length));
  EXPECT_EQ_U(length, 0);
  deck.close();
  resetCard();
}

/**
 * C3b CRITICAL-1, at the reader's end of it: `keyAtChecked()` is the accessor
 * with an error channel (FLASHCARD_SPEC.md §7b.3 pin a). keyAt()'s "0 on
 * failure" cannot tell a real key of 0 from a card that would not read, and the
 * state layer treats a key that disagrees as a TORN record and heals it — so on
 * the C3, where a key IS a disk read, the difference between the two accessors
 * is the difference between one refused session and a wiped deck.
 *
 * The contract is asserted in both configurations; only the failing-read half
 * is C3-specific, because the Pro's index is resident and cannot fail.
 */
void testKeyAtChecked() {
  beginGroup("deck/keyAtChecked");
  resetCard();
  makeDirs();

  // 40 cards spans two of the C3's 32-entry index windows, so the walk below
  // crosses a refill.
  const size_t count = 40;
  const std::vector<TestCard> cards = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));

  bool everyKeyOk = true;
  for (size_t i = 0; i < count; i++) {
    uint64_t key = 0;
    if (!deck.keyAtChecked(static_cast<Ordinal>(i), key)) everyKeyOk = false;
    if (key != cards[i].key) everyKeyOk = false;
  }
  EXPECT(everyKeyOk);

  // Out of range is a refusal in both builds, and leaves the caller's variable
  // exactly as it was rather than zeroing it into a plausible-looking key.
  uint64_t untouched = 0xD15EA5EDD15EA5EDULL;
  EXPECT(!deck.keyAtChecked(static_cast<Ordinal>(count), untouched));
  EXPECT_EQ_U(untouched, 0xD15EA5EDD15EA5EDULL);
  EXPECT_EQ_U(deck.keyAt(static_cast<Ordinal>(count)), 0);

#ifdef CROSSPOINT_FLASHCARDS_C3
  // The window currently holds ordinals 32..39 (the walk ended there). Asking
  // for ordinal 0 must refill, and with the card refusing every seek and read
  // that refill fails: a REFUSAL, not a zero.
  uint64_t key = 0xABCDULL;
  failDeckIo();
  EXPECT(!deck.keyAtChecked(0, key));
  EXPECT_EQ_U(key, 0xABCDULL);
  EXPECT(hostIoFaults().seekFailures + hostIoFaults().readFailures > 0);

  // And the window is EMPTY afterwards, so the entries it used to hold are not
  // served either: a refill that failed part-way may already have overwritten
  // some of those bytes, and nothing in the buffer can be trusted until a read
  // has completed. (Without this, ordinal 32 would come back happily from a
  // window the failed refill left standing.)
  EXPECT(!deck.keyAtChecked(32, key));
  EXPECT_EQ_U(key, 0xABCDULL);

  resetIo();
  EXPECT(deck.keyAtChecked(0, key));  // the card comes back, and so does the deck
  EXPECT_EQ_U(key, cards[0].key);
#endif

  deck.close();
  resetCard();
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

// --- DeckFile: CPDK v2 card images -------------------------------------------

/**
 * The v2 open-time refusal matrix: one case per rejection `read_deck.py`'s
 * `_validate_images()` raises, minus the four that are deliberately NOT open's
 * job (see the end of this comment).
 *
 * Both configurations run every case. The two tables are streamed rather than
 * held on BOTH builds -- the Pro's resident-index trick does not scale to a
 * table the format lets grow to 6.4 MB (DeckFile.h's v2 note) -- so there is one
 * code path here and "parity" is not a separate group: it is the fact that this
 * group runs twice and has to give the same answers both times.
 *
 * Moved to loadImage(), and covered in testImageBytes(): the frame sniff
 * (baseline / one component / dimensions agreeing with the table). Sniffing at
 * open costs one SD seek per image -- minutes of them at the format's cap -- and
 * buys a refused DECK where a refused PICTURE is the right answer.
 */
void testImageValidation() {
  beginGroup("deck/v2 validation");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(4);
  const std::vector<TestImage> images = {grayImage(), grayImage()};
  const std::vector<TestPlacement> placements = {
      TestPlacement{0, 0, 0, 0},
      TestPlacement{0, 1, 6, 1},
      TestPlacement{2, 0, 3, 0},
  };
  const std::vector<uint8_t> good = buildDeckV2(cards, images, placements);

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, good)), static_cast<int>(DeckError::Ok));
  EXPECT(deck.hasImages());
  EXPECT_EQ_U(deck.cardCount(), 4);
  EXPECT_EQ_U(deck.maxImageBytes(), sizeof(JPEG_GRAY_16x12));
  // The text blob still reads correctly with an image blob sitting behind it.
  // That boundary is DERIVED (file_size - the image table's total length), not
  // stored, and getting it wrong would hand the text renderer JPEG bytes.
  char text[flashcards::DECK_MAX_SLICE_BYTES + 1];
  uint16_t length = 0;
  EXPECT(deck.loadSide(3, CardSide::Back, text, sizeof(text), length));
  EXPECT_STR(text, "back 3");
  EXPECT(deck.loadSide(0, CardSide::Front, text, sizeof(text), length));
  EXPECT_STR(text, "front 0");
  deck.close();

  // An image-free deck hashes identically under both versions (§3.7), so a
  // converter switched to --images does not churn every guid it emits.
  EXPECT_EQ_U(getU64(buildDeckV2(cards, {}, {}), 12), getU64(buildDeck(cards), 12));

  struct Case {
    const char* name;
    DeckError expected;
    std::vector<uint8_t> (*build)();
  };
  const Case cases[] = {
      // -- the image table --
      {"image_count 0", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 64, 0);
         return b;
       }},
      // Four images per SIDE over two sides is eight per card, so 4 cards cap
      // the table at 32 and 33 is the first refusal. The old 4x-total bound was
      // unsatisfiable beside the per-side rule (FLASHCARD_SPEC.md §2.2 I1 pins),
      // and 32 passing here is half of what this case pins.
      {"image_count over 8x cards", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 64, 33);  // 2 x 4 per side x 4 cards is the cap
         return b;
       }},
      // Cut inside the two-entry image table: the count is in range, the bytes
      // it names are not in the file. Both sections have to be proved to FIT
      // before a single entry of either is read (§3.7 device check 2).
      {"image table cut short", DeckError::ShortFile,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         b.resize(64 + 4 + 20);  // image_count is read; its 32 bytes of entries are not there
         return b;
       }},
      {"image byte_len 0", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68 + 4, 0);
         return b;
       }},
      // Two byte_len cases, and the second is the one that means something.
      // Patching the length on a 1 KB deck is refused by the 64 KB cap, but it
      // would ALSO be refused by "this image runs past the file" -- so on its
      // own it never proves the cap exists. The second builds a file that really
      // does hold 70000 bytes of image, so every other check passes and only the
      // cap can refuse it.
      {"image byte_len over 64 KB", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68 + 4, 64 * 1024 + 1);
         return b;
       }},
      {"image byte_len over 64 KB in a file long enough to hold it", DeckError::BadImageTable,
       [] {
         static const std::vector<uint8_t> oversized(64 * 1024 + 1, 0x5A);
         const TestImage huge{oversized.data(), oversized.size(), 16, 12, 0, 0, 0, false};
         return buildDeckV2(makeCards(1), {huge}, {TestPlacement{0, 0, 0, 0}});
       }},
      // A v2 header with has_images CLEARED, on a file whose image and placement
      // sections are otherwise perfectly well formed -- so nothing downstream
      // can refuse it and the flag check is the only thing standing between this
      // file and an open deck. A v2 file always carries images (an --images run
      // that embedded nothing is written as v1), so this is a file nobody wrote.
      {"v2 header without has_images, sections intact", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()},
                                              {TestPlacement{0, 0, 0, 0}, TestPlacement{1, 1, 2, 1}});
         putU16(b, 6, 0);  // format_version stays 2; only the has_images bit goes
         return b;
       }},
      {"image reserved non-zero", DeckError::BadImageTable,
       [] {
         std::vector<TestImage> bad = {grayImage(), grayImage()};
         bad[1].reserved = 1;
         return buildDeckV2(makeCards(4), bad, {TestPlacement{0, 0, 0, 0}});
       }},
      {"image width 0", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU16(b, 68 + 8, 0);
         return b;
       }},
      {"image width over 440", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU16(b, 68 + 8, 441);
         return b;
       }},
      {"image height 0", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU16(b, 68 + 10, 0);
         return b;
       }},
      {"image height over 440", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU16(b, 68 + 10, 441);
         return b;
       }},
      {"image end wraps past the file", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68, 0xFFFFFF00u);  // blob_off + byte_len must not fold back inside
         return b;
       }},
      // -- the blob must TILE (§2.2 I1 pins) --------------------------------
      //
      // The image blob has no length field: it runs to EOF, so its start is
      // derived and every blob_off is read against that derived start. Nothing
      // inside an entry can notice the blob having shifted. Requiring the table
      // to tile the blob exactly -- entry 0 at offset 0, each entry beginning
      // where the last one ended -- is what turns "the pictures came out wrong"
      // into a refusal. Four ways a table can fail to tile, and each is one
      // patched u32 on an otherwise good two-image deck.
      {"first image not at blob offset 0", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68, 1);
         return b;
       }},
      {"gap between two images", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68 + 16, static_cast<uint32_t>(sizeof(JPEG_GRAY_16x12)) + 1);
         return b;
       }},
      // Two entries naming the same bytes. Legal-looking (both ends are inside
      // the file) and the reason "every end is in range" was never enough.
      {"two images aliasing the same blob bytes", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68 + 16, 0);
         return b;
       }},
      // Table order must BE blob order: the two entries swapped describe the
      // same set of bytes and the same total, and only the tiling walk sees it.
      {"table order does not match blob order", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68, static_cast<uint32_t>(sizeof(JPEG_GRAY_16x12)));
         putU32(b, 68 + 16, 0);
         return b;
       }},
      // THE one this closes. A single junk byte appended to the download slides
      // the EOF-anchored image blob by one: before the boundary check every
      // image read a byte off, every frame sniff failed, and the card drew a
      // full set of placeholder boxes with nothing logged and nothing refused.
      {"one stray byte appended after the image blob", DeckError::BadImageTable,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         b.push_back(0x00);
         return b;
       }},
      // The blob's start is file_size - blob_length; stretching the only entry
      // moves that start in front of the text blob, which is the one arrangement
      // §3.7 refuses outright. ONE image on purpose: with a second entry behind
      // it the stretch would break the dense tiling first and this would stop
      // being a test of the blob/index overlap.
      {"image blob overlaps the card index", DeckError::ShortFile,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 68 + 4, static_cast<uint32_t>(b.size()) - 4);
         return b;
       }},
      // -- the placement table --
      {"placement table cut short", DeckError::ShortFile,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         b.resize(64 + 4 + 32 + 4 + 6);  // placement_count is read; its 12 bytes are not there
         return b;
       }},
      {"placement_count over 8x cards", DeckError::BadPlacement,
       [] {
         std::vector<uint8_t> b = buildDeckV2(makeCards(4), {grayImage(), grayImage()}, {TestPlacement{0, 0, 0, 0}});
         putU32(b, 64 + 4 + 2 * 16, 33);
         return b;
       }},
      {"placement reserved non-zero", DeckError::BadPlacement,
       [] {
         std::vector<TestPlacement> bad = {TestPlacement{0, 0, 0, 0, 1}};
         return buildDeckV2(makeCards(4), {grayImage()}, bad);
       }},
      {"placement side 2", DeckError::BadPlacement,
       [] { return buildDeckV2(makeCards(4), {grayImage()}, {TestPlacement{0, 2, 0, 0}}); }},
      {"placement ordinal past the deck", DeckError::BadPlacement,
       [] { return buildDeckV2(makeCards(4), {grayImage()}, {TestPlacement{4, 0, 0, 0}}); }},
      {"placement image_index past the table", DeckError::BadPlacement,
       [] { return buildDeckV2(makeCards(4), {grayImage()}, {TestPlacement{0, 0, 0, 1}}); }},
      {"placement text_offset past the side", DeckError::BadPlacement,
       [] { return buildDeckV2(makeCards(4), {grayImage()}, {TestPlacement{0, 0, 8, 0}}); }},
      {"placements out of order by ordinal", DeckError::BadPlacement,
       [] {
         return buildDeckV2(makeCards(4), {grayImage(), grayImage()},
                            {TestPlacement{2, 0, 0, 0}, TestPlacement{0, 0, 0, 1}});
       }},
      {"placements out of order by side", DeckError::BadPlacement,
       [] {
         return buildDeckV2(makeCards(4), {grayImage(), grayImage()},
                            {TestPlacement{0, 1, 0, 0}, TestPlacement{0, 0, 0, 1}});
       }},
      {"placements out of order by offset", DeckError::BadPlacement,
       [] {
         return buildDeckV2(makeCards(4), {grayImage(), grayImage()},
                            {TestPlacement{0, 0, 5, 0}, TestPlacement{0, 0, 4, 1}});
       }},
      {"five placements on one side", DeckError::BadPlacement,
       [] {
         std::vector<TestPlacement> bad;
         for (int i = 0; i < 5; i++) bad.push_back(TestPlacement{0, 0, 0, 0});
         return buildDeckV2(makeCards(4), {grayImage()}, bad);
       }},
  };

  for (const Case& testCase : cases) {
    const DeckError error = installDeck(deck, testCase.build());
    if (error != testCase.expected) {
      ++g_failures;
      printf("FAIL [%s] case \"%s\": got %s, want %s\n", g_group, testCase.name, flashcards::deckErrorName(error),
             flashcards::deckErrorName(testCase.expected));
    }
    ++g_checks;
    EXPECT(!deck.isOpen());
  }

  // The ACCEPT side of both section caps, and the half that pins the number.
  // "33 is refused" is equally true of a 4x cap; only "32 is accepted" says the
  // bound is 2 x DECK_MAX_IMAGES_PER_SIDE x cards. 4 cards, four images a side
  // on both sides of every one of them: 32 placements naming 32 images, which
  // is the largest v2 file 4 cards can legally describe.
  {
    std::vector<TestImage> full;
    std::vector<TestPlacement> everySlot;
    for (uint16_t ordinal = 0; ordinal < 4; ordinal++) {
      for (uint8_t side = 0; side < 2; side++) {
        for (uint32_t slot = 0; slot < flashcards::DECK_MAX_IMAGES_PER_SIDE; slot++) {
          everySlot.push_back(TestPlacement{ordinal, side, slot, static_cast<uint32_t>(full.size())});
          full.push_back(grayImage());
        }
      }
    }
    EXPECT_EQ_U(full.size(), 32u);
    EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, full, everySlot))),
                static_cast<int>(DeckError::Ok));
    flashcards::DeckImagePlacement capFound[flashcards::DECK_MAX_IMAGES_PER_SIDE];
    EXPECT_EQ_U(deck.imagesForSide(3, CardSide::Back, capFound, 4), 4);
    deck.close();
  }

  // 64 KB EXACTLY is the cap, not one byte over it -- the other half of the
  // oversized case above, and what makes that one a boundary rather than a
  // blanket refusal of large pictures.
  {
    const std::vector<uint8_t> atCap(64 * 1024, 0x5A);
    const TestImage large{atCap.data(), atCap.size(), 16, 12, 0, 0, 0, false};
    EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(makeCards(1), {large}, {TestPlacement{0, 0, 0, 0}}))),
                static_cast<int>(DeckError::Ok));
    EXPECT_EQ_U(deck.maxImageBytes(), 64u * 1024u);
    deck.close();
  }

  // Legal-but-odd, and read_deck.py only WARNS about it: an image no placement
  // names. The converter never writes one, but a deck carrying one still opens.
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, images, {TestPlacement{0, 0, 0, 0}}))),
              static_cast<int>(DeckError::Ok));
  // A tie is legal too: two images at one (ordinal, side, text_offset).
  EXPECT_EQ_U(static_cast<int>(installDeck(
                  deck, buildDeckV2(cards, images, {TestPlacement{1, 0, 2, 0}, TestPlacement{1, 0, 2, 1}}))),
              static_cast<int>(DeckError::Ok));
  // Four on one side is the cap, not one over it.
  {
    std::vector<TestPlacement> four;
    for (int i = 0; i < 4; i++) four.push_back(TestPlacement{1, 0, 2, 0});
    EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, images, four))), static_cast<int>(DeckError::Ok));
  }
  // Four on the front AND four on the back of the same card: eight placements
  // that share an ordinal are eight, not a per-card cap of four.
  {
    std::vector<TestPlacement> eight;
    for (int i = 0; i < 4; i++) eight.push_back(TestPlacement{1, 0, 2, 0});
    for (int i = 0; i < 4; i++) eight.push_back(TestPlacement{1, 1, 2, 1});
    EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, images, eight))),
                static_cast<int>(DeckError::Ok));
  }
  deck.close();
}

/** The per-card placement query: counts, order, boundaries, and the v1 no-op. */
void testImageQueries() {
  beginGroup("deck/v2 placements");
  resetCard();
  makeDirs();

  std::vector<TestCard> cards = makeCards(6);
  cards[4].front.clear();  // an image-only front: length 0 with a placement at 0
  const std::vector<TestImage> images = {grayImage(), grayImage(), grayImage(), grayImage(),
                                         grayImage(), grayImage(), grayImage()};
  // Sorted by (ordinal, side, text_offset), ties included, exactly as the
  // converter writes them.
  const std::vector<TestPlacement> placements = {
      TestPlacement{1, 0, 0, 0},                                                        // above all the text
      TestPlacement{2, 0, 0, 1}, TestPlacement{2, 0, 3, 2}, TestPlacement{2, 0, 3, 3},  // a tie
      TestPlacement{2, 0, 7, 4},                                                        // four on one side
      TestPlacement{3, 1, 6, 5},  // text_offset == back_len: below all the text
      TestPlacement{4, 0, 0, 6},  // the image-only side
  };

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, images, placements))),
              static_cast<int>(DeckError::Ok));

  flashcards::DeckImagePlacement found[flashcards::DECK_MAX_IMAGES_PER_SIDE];
  // Cards with nothing on them, on both sides of the ones that have some: the
  // binary search has to land on the right run, not merely near it.
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 0);
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Back, found, 4), 0);
  EXPECT_EQ_U(deck.imagesForSide(5, CardSide::Front, found, 4), 0);
  EXPECT_EQ_U(deck.imagesForSide(5, CardSide::Back, found, 4), 0);
  EXPECT_EQ_U(deck.imagesForSide(1, CardSide::Back, found, 4), 0);
  EXPECT_EQ_U(deck.imagesForSide(2, CardSide::Back, found, 4), 0);
  EXPECT_EQ_U(deck.imagesForSide(3, CardSide::Front, found, 4), 0);

  EXPECT_EQ_U(deck.imagesForSide(1, CardSide::Front, found, 4), 1);
  EXPECT_EQ_U(found[0].textOffset, 0);
  EXPECT_EQ_U(found[0].image.width, 16);
  EXPECT_EQ_U(found[0].image.height, 12);
  EXPECT_EQ_U(found[0].image.byteLength, sizeof(JPEG_GRAY_16x12));

  EXPECT_EQ_U(deck.imagesForSide(2, CardSide::Front, found, 4), 4);
  EXPECT_EQ_U(found[0].textOffset, 0);
  EXPECT_EQ_U(found[1].textOffset, 3);
  EXPECT_EQ_U(found[2].textOffset, 3);  // the tie survives, in table order
  EXPECT_EQ_U(found[3].textOffset, 7);
  // Distinct table entries, so consecutive images sit one encoded image apart.
  EXPECT_EQ_U(found[1].image.fileOffset - found[0].image.fileOffset, sizeof(JPEG_GRAY_16x12));

  EXPECT_EQ_U(deck.imagesForSide(3, CardSide::Back, found, 4), 1);
  EXPECT_EQ_U(found[0].textOffset, 6);  // == back_len ("back 3")

  EXPECT_EQ_U(deck.imagesForSide(4, CardSide::Front, found, 4), 1);
  EXPECT_EQ_U(found[0].textOffset, 0);

  // A caller with room for fewer takes fewer; an out-of-range ordinal takes
  // none. Neither is a read failure.
  EXPECT_EQ_U(deck.imagesForSide(2, CardSide::Front, found, 2), 2);
  EXPECT_EQ_U(deck.imagesForSide(static_cast<Ordinal>(cards.size()), CardSide::Front, found, 4), 0);

  // A card the query has already visited answers the same way a second time --
  // the placement window is a cache, and a cache that went stale would show up
  // here first.
  EXPECT_EQ_U(deck.imagesForSide(2, CardSide::Front, found, 4), 4);
  EXPECT_EQ_U(found[3].textOffset, 7);

  // A v1 deck answers 0 for every card, and does it WITHOUT TOUCHING THE CARD:
  // the op counters are what makes "an image-free deck never pays" a fact.
  deck.close();
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));
  EXPECT(!deck.hasImages());
  EXPECT_EQ_U(deck.maxImageBytes(), 0);
  resetIo();
  for (size_t ordinal = 0; ordinal < cards.size(); ordinal++) {
    EXPECT_EQ_U(deck.imagesForSide(static_cast<Ordinal>(ordinal), CardSide::Front, found, 4), 0);
    EXPECT_EQ_U(deck.imagesForSide(static_cast<Ordinal>(ordinal), CardSide::Back, found, 4), 0);
  }
  EXPECT_EQ_U(hostIoCounters().deckReads, 0);
  EXPECT_EQ_U(hostIoCounters().deckSeeks, 0);
  deck.close();
}

/**
 * The placement window is a CACHE, and a failed refill must leave it holding no
 * claim at all.
 *
 * imagesForSide() binary-searches the on-disk placement table through a
 * 16-entry window, so a query that walks from one window into another refills
 * it mid-search. If that refill fails -- a bad sector under the table, which is
 * the whole reason these reads have an error path -- the window must be marked
 * EMPTY, not left describing the block it was about to load. Otherwise the next
 * query is answered out of a buffer whose contents belong to a different part of
 * the table: not a failure the caller can see, but four wrong pictures on a card.
 *
 * The op counters are what makes "invalidated" checkable rather than asserted:
 * a query into a window that is genuinely resident costs no window read, and one
 * into an invalidated window costs exactly one more read than that.
 */
void testPlacementWindowFault() {
  beginGroup("deck/v2 placement window");
  resetCard();
  makeDirs();

  // 24 placements over 6 cards -- two windows of 16, so the search really does
  // cross from one to the other. One image, named by all of them: an image with
  // several placements is legal, and it keeps the image table out of the way.
  const std::vector<TestCard> cards = makeCards(6);
  std::vector<TestPlacement> placements;
  for (uint16_t ordinal = 0; ordinal < 6; ordinal++) {
    for (uint32_t slot = 0; slot < flashcards::DECK_MAX_IMAGES_PER_SIDE; slot++) {
      placements.push_back(TestPlacement{ordinal, 0, slot, 0});
    }
  }
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, {grayImage()}, placements))),
              static_cast<int>(DeckError::Ok));

  flashcards::DeckImagePlacement found[flashcards::DECK_MAX_IMAGES_PER_SIDE];
  // Card 0 lives in the first window; loading it leaves that window resident.
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 4);
  resetIo();
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 4);
  const long warmReads = hostIoCounters().deckReads;  // image-table reads only

  // Card 5 is in the SECOND window, so the search refills -- into a card that
  // has just gone bad. The query fails softly (no images, deck still open).
  failDeckIo();
  EXPECT_EQ_U(deck.imagesForSide(5, CardSide::Front, found, 4), 0);
  EXPECT(hostIoFaults().seekFailures + hostIoFaults().readFailures > 0);
  EXPECT(deck.isOpen());

  // Back on a healthy card, the first window must be READ AGAIN rather than
  // served out of the buffer the failed refill was aiming at.
  resetIo();
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 4);
  EXPECT_EQ_U(found[0].textOffset, 0);
  EXPECT_EQ_U(found[3].textOffset, 3);
  EXPECT(hostIoCounters().deckReads > warmReads);

  // ... and the window that faulted is usable again too.
  EXPECT_EQ_U(deck.imagesForSide(5, CardSide::Front, found, 4), 4);
  EXPECT_EQ_U(found[0].textOffset, 0);
  EXPECT_EQ_U(found[3].textOffset, 3);
  deck.close();
}

/**
 * loadImage(): the bounds, and the frame sniff that open() deliberately does
 * not do. This is where read_deck.py's encoding-contract rejections land, and
 * the point of every one of them is that a bad picture is a bad PICTURE.
 */
void testImageBytes() {
  beginGroup("deck/v2 image bytes");
  resetCard();
  makeDirs();

  const std::vector<TestCard> cards = makeCards(3);
  const std::vector<TestImage> images = {
      grayImage(),
      TestImage{JPEG_PROGRESSIVE_16x12, sizeof(JPEG_PROGRESSIVE_16x12), 16, 12, 0, 0, 0, false},
      TestImage{JPEG_RGB_16x12, sizeof(JPEG_RGB_16x12), 16, 12, 0, 0, 0, false},
      TestImage{JPEG_GRAY_16x12, sizeof(JPEG_GRAY_16x12), 20, 12, 0, 0, 0, false},  // the table lies about the width
  };
  const std::vector<TestPlacement> placements = {
      TestPlacement{0, 0, 0, 0},
      TestPlacement{0, 1, 0, 1},
      TestPlacement{1, 0, 0, 2},
      TestPlacement{1, 1, 0, 3},
  };

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, images, placements))),
              static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(deck.maxImageBytes(), sizeof(JPEG_RGB_16x12));  // the largest of the four

  std::vector<uint8_t> buffer(deck.maxImageBytes() + 8);
  flashcards::DeckImagePlacement found[flashcards::DECK_MAX_IMAGES_PER_SIDE];

  // The baseline grayscale one reads back byte for byte.
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 1);
  EXPECT(deck.loadImage(found[0].image, buffer.data(), buffer.size()));
  EXPECT_EQ_U(memcmp(buffer.data(), JPEG_GRAY_16x12, sizeof(JPEG_GRAY_16x12)), 0);

  // A buffer one byte short of the encoded length is refused rather than filled
  // part way: the decoder would then read whatever was behind it.
  EXPECT(!deck.loadImage(found[0].image, buffer.data(), found[0].image.byteLength - 1));
  EXPECT(!deck.loadImage(found[0].image, nullptr, buffer.size()));

  // The three encoding-contract refusals.
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Back, found, 4), 1);
  EXPECT(!deck.loadImage(found[0].image, buffer.data(), buffer.size()));  // progressive
  EXPECT_EQ_U(deck.imagesForSide(1, CardSide::Front, found, 4), 1);
  EXPECT(!deck.loadImage(found[0].image, buffer.data(), buffer.size()));  // three components
  EXPECT_EQ_U(deck.imagesForSide(1, CardSide::Back, found, 4), 1);
  EXPECT(!deck.loadImage(found[0].image, buffer.data(), buffer.size()));  // dimensions disagree
  // ... and the deck is still open and still readable after all three.
  EXPECT(deck.isOpen());
  char text[flashcards::DECK_MAX_SLICE_BYTES + 1];
  uint16_t length = 0;
  EXPECT(deck.loadSide(2, CardSide::Front, text, sizeof(text), length));
  EXPECT_STR(text, "front 2");

  // Truncated JPEGs, cut at every point the frame walk has to survive. This is
  // the realistic corruption for a blob addressed by offset and length, and the
  // walk steps through attacker-shaped data with only `bytes` to stop it: each
  // cut below lands in a different one of its bounds checks, and NONE of them
  // may read past the buffer or claim the frame is fine. (The cuts are the SOI
  // pair alone, a marker with no length, the middle of APP0, the last byte
  // before the SOF marker, the SOF marker with its length missing, the middle of
  // the frame header, and one byte short of the complete frame segment. 102 is
  // the first length that carries a whole SOF0, and it is deliberately not
  // here.)
  {
    const size_t cuts[] = {2, 4, 20, 89, 91, 95, 101};
    for (const size_t cut : cuts) {
      DeckFile cutDeck;
      const TestImage truncated{JPEG_GRAY_16x12, cut, 16, 12, 0, 0, 0, false};
      const DeckError opened =
          installDeck(cutDeck, buildDeckV2(makeCards(1), {truncated}, {TestPlacement{0, 0, 0, 0}}));
      // The DECK still opens: open() never sniffs a frame (§2.2 -- one seek per
      // image is minutes of them at the format's cap), so a truncated picture is
      // a picture that will not draw, not a deck that will not study.
      EXPECT_EQ_U(static_cast<int>(opened), static_cast<int>(DeckError::Ok));
      flashcards::DeckImagePlacement cutFound[flashcards::DECK_MAX_IMAGES_PER_SIDE];
      EXPECT_EQ_U(cutDeck.imagesForSide(0, CardSide::Front, cutFound, 4), 1);
      EXPECT_EQ_U(cutFound[0].image.byteLength, cut);
      // EXACTLY as many bytes as the image claims, and no more. A generously
      // sized buffer would let the frame walk step past `bytes` into slack that
      // happens to be readable, and a bounds check deleted from that walk would
      // then be invisible to every assertion here. Sized to the byte, an
      // over-read is a heap over-read: this loop is the fixture an address
      // sanitizer needs, and the reason the cuts land where they do.
      std::vector<uint8_t> exact(cut);
      EXPECT(!cutDeck.loadImage(cutFound[0].image, exact.data(), exact.size()));
      cutDeck.close();
    }
  }
  // Reinstall the four-image deck the rest of this group works on: the cut decks
  // above wrote over the same path.
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeckV2(cards, images, placements))),
              static_cast<int>(DeckError::Ok));

  // A read that fails under an open deck costs the picture, never the card: the
  // C3 variant reaches the card for every one of these calls, so this is the
  // path that actually runs there (FLASHCARD_SPEC.md §7b.4).
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 1);
  failDeckIo();
  EXPECT(!deck.loadImage(found[0].image, buffer.data(), buffer.size()));
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 0);
  resetIo();
  EXPECT_EQ_U(deck.imagesForSide(0, CardSide::Front, found, 4), 1);
  EXPECT(deck.loadImage(found[0].image, buffer.data(), buffer.size()));
  deck.close();
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

  // C3b NIT-13. The repair has to be ON DISK, not merely applied on the way
  // back out: readRecord() normalizes too, so asserting only through it would
  // pass just as happily if the merge had carried the 0x77 byte straight into
  // the new file. This byte is what says the MERGE repaired the carried record
  // (FLASHCARD_SPEC.md §4), and it is the same assertion in both builds.
  merged.close();
  EXPECT(readBytes(statePath(), raw));
  EXPECT_EQ_U(raw.size(), stateBytes(7));
  EXPECT_EQ_U(raw[recAt(0) + 8 + 14], static_cast<uint8_t>(CardPhase::New));
}

/**
 * C3b MUST-3. Every other merge fixture here builds its keys with makeCards(),
 * which hands them out in ascending ordinal order — and that quietly makes the
 * merge's sorted key array the identity permutation, so `oldSlots[found]` and
 * `found` are the same number and a merge that dropped the indirection
 * altogether would pass. This one shuffles the keys, rotates the deck, drops a
 * block from the middle and appends unseen cards, so nothing lines up with
 * anything: the only thing that can carry a schedule to the right ordinal is
 * the key.
 */
void testShuffledKeyMerge() {
  beginGroup("state/merge shuffled keys");
  resetCard();
  makeDirs();

  const size_t count = 300;
  const std::vector<TestCard> v1 = makeShuffledCards(count);
  // The fixture is worth nothing unless the keys really are out of order, so
  // that is checked rather than assumed: neither sorted nor reverse-sorted.
  size_t ascendingPairs = 0;
  for (size_t i = 1; i < count; i++) {
    if (v1[i].key > v1[i - 1].key) ascendingPairs++;
  }
  EXPECT(ascendingPairs > 30);
  EXPECT(ascendingPairs < count - 31);

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v1))), static_cast<int>(DeckError::Ok));
  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
  bool seeded = true;
  for (size_t i = 0; i < count; i++) {
    if (!store.writeRecord(static_cast<Ordinal>(i),
                           reviewCard(static_cast<uint16_t>(600 + i % 250), static_cast<uint16_t>(500 + i % 90)))) {
      seeded = false;
    }
  }
  EXPECT(seeded);
  store.close();

  // v2: rotate by 97 so no ordinal keeps its place, drop twenty from the
  // middle, append fifteen the deck has never seen.
  std::vector<TestCard> v2;
  for (size_t k = 0; k < count; k++) {
    const size_t i = (k + 97) % count;
    if (i >= 140 && i < 160) continue;
    v2.push_back(v1[i]);
  }
  const size_t kept = v2.size();
  EXPECT_EQ_U(kept, count - 20);
  for (size_t i = 0; i < 15; i++) {
    v2.push_back(TestCard{0x9000000000000000ULL + i, "added front", "added back"});
  }

  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v2))), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), v2.size());

  bool carriedOk = true;
  size_t carried = 0;
  for (size_t o = 0; o < v2.size(); o++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(o), state) != RecordStatus::Ok) {
      carriedOk = false;
      continue;
    }
    size_t from = SIZE_MAX;  // which v1 ordinal owned this key
    for (size_t i = 0; i < count; i++) {
      if (v1[i].key == v2[o].key) {
        from = i;
        break;
      }
    }
    if (from == SIZE_MAX) {
      if (state.state != CardPhase::New || state.due != 0 || state.reps != 0) carriedOk = false;
    } else {
      carried++;
      if (state.state != CardPhase::Review || state.due != 600 + from % 250 || state.lastReviewDay != 500 + from % 90) {
        carriedOk = false;
      }
    }
  }
  EXPECT(carriedOk);
  EXPECT_EQ_U(carried, kept);
  EXPECT(!store.keyMismatchSeen());
  store.close();
  deck.close();
  resetCard();
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

/**
 * C3b CRITICAL-1 regression. On the C3 a deck key is a DISK READ, so keyAt()'s
 * "0 when it did not work" collides with the self-heal above: a record checked
 * against a failed read looks torn, gets rewritten as a fresh New card, and one
 * bad sector under the deck's index therefore wipes the schedule of every card
 * in the deck while the session reports success. Measured before the fix: 100
 * of 100 records overwritten.
 *
 * The rule (FLASHCARD_SPEC.md §7b.3 pin a) is that a failed key read is an I/O
 * ERROR — it aborts the operation and heals NOTHING — and the assertion that
 * matters is the same in both builds: after the fault, the state file has not
 * moved by a byte.
 */
void testIoFaultsDoNotHeal() {
  beginGroup("state/io faults");
  resetCard();
  makeDirs();

  // 200 cards is more than one C3 index window, so a scan cannot get through
  // on whatever the window happened to be holding.
  const size_t count = 200;
  const std::vector<TestCard> cards = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));
  {
    StateStore seed;
    EXPECT_EQ_U(static_cast<int>(seed.open(deck, decksDir(), DECK_LEAF, 800)), static_cast<int>(StateError::Ok));
    bool seeded = true;
    for (size_t i = 0; i < count; i++) {
      if (!seed.writeRecord(static_cast<Ordinal>(i), reviewCard(790, 780))) seeded = false;
    }
    EXPECT(seeded);
  }

  std::vector<uint8_t> before;
  EXPECT(readBytes(statePath(), before));

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 800)), static_cast<int>(StateError::Ok));

  // --- the deck's index stops answering ---------------------------------------
  fsrs::Now now{1700000000u, 800};
  Session session;
  failDeckIo();
  const bool built = flashcards::buildSession(store, SessionMode::Due, now, session);
#ifdef CROSSPOINT_FLASHCARDS_C3
  EXPECT(!built);
  EXPECT_EQ_U(session.cards.size(), 0);
  EXPECT(hostIoFaults().seekFailures + hostIoFaults().readFailures > 0);

  // readRecord reports the I/O error rather than a key mismatch, so no caller
  // can mistake it for a card that needs restarting...
  CardState state = reviewCard(555, 550);
  EXPECT_EQ_U(static_cast<int>(store.readRecord(0, state)), static_cast<int>(RecordStatus::IoError));
  EXPECT_EQ_U(static_cast<int>(state.state), static_cast<int>(CardPhase::New));
  // ...and a write refuses rather than stamping a 0 key onto a live record,
  // which would leave it reading as torn for ever afterwards.
  EXPECT(!store.writeRecord(1, reviewCard(700, 690)));
#else
  // The Pro holds the index in RAM, so a card that stopped answering is not
  // even reached: the same fault is invisible on that build.
  EXPECT(built);
#endif
  EXPECT(!store.keyMismatchSeen());

  // --- the state file itself stops answering ----------------------------------
  // Same rule, both builds: a scan that cannot read is a failed scan, never a
  // deck full of torn records.
  hostIoFaults().reset();
  hostIoFaults().pathSuffix = ".state";
  hostIoFaults().allowReads = 0;
  EXPECT(!flashcards::buildSession(store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.cards.size(), 0);
  CardState unread = reviewCard(444, 440);
  EXPECT_EQ_U(static_cast<int>(store.readRecord(0, unread)), static_cast<int>(RecordStatus::IoError));
  EXPECT(hostIoFaults().readFailures > 0);
  EXPECT(!store.keyMismatchSeen());
  store.close();

  // THE assertion, and it holds in both builds: not one byte of the state file
  // moved while the card was misbehaving.
  resetIo();
  std::vector<uint8_t> after;
  EXPECT(readBytes(statePath(), after));
  EXPECT(before == after);

  // And with the card answering again, every schedule is still there.
  StateStore again;
  EXPECT_EQ_U(static_cast<int>(again.open(deck, decksDir(), DECK_LEAF, 800)), static_cast<int>(StateError::Ok));
  EXPECT(flashcards::buildSession(again, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.summary.dueAvailable, count);
  EXPECT(!again.keyMismatchSeen());
  again.close();
  deck.close();

  // --- creation and the merge, with the deck's index unreadable ---------------
  // Both of those lay down a whole file's worth of keys read from the deck, so
  // both must refuse rather than write records carrying a 0 key — which every
  // later session would judge torn and heal away.
  resetCard();
  makeDirs();
  DeckFile writing;
  EXPECT_EQ_U(static_cast<int>(installDeck(writing, buildDeck(cards))), static_cast<int>(DeckError::Ok));
  {
    StateStore creating;
    failDeckIo();
    const StateError created = creating.open(writing, decksDir(), DECK_LEAF, 800);
    resetIo();
#ifdef CROSSPOINT_FLASHCARDS_C3
    EXPECT_EQ_U(static_cast<int>(created), static_cast<int>(StateError::ReadFailed));
    EXPECT(!creating.isOpen());
#else
    EXPECT_EQ_U(static_cast<int>(created), static_cast<int>(StateError::Ok));  // resident index, nothing to fail
#endif
  }

  resetCard();
  makeDirs();
  EXPECT_EQ_U(static_cast<int>(installDeck(writing, buildDeck(cards))), static_cast<int>(DeckError::Ok));
  {
    StateStore seed;
    EXPECT_EQ_U(static_cast<int>(seed.open(writing, decksDir(), DECK_LEAF, 800)), static_cast<int>(StateError::Ok));
    for (size_t i = 0; i < count; i++) seed.writeRecord(static_cast<Ordinal>(i), reviewCard(790, 780));
  }
  const std::vector<TestCard> reversed(cards.rbegin(), cards.rend());
  EXPECT_EQ_U(static_cast<int>(installDeck(writing, buildDeck(reversed))), static_cast<int>(DeckError::Ok));
  std::vector<uint8_t> beforeMerge;
  EXPECT(readBytes(statePath(), beforeMerge));
  {
    StateStore merging;
    failDeckIo();
    const StateError result = merging.open(writing, decksDir(), DECK_LEAF, 800);
    resetIo();
#ifdef CROSSPOINT_FLASHCARDS_C3
    EXPECT_EQ_U(static_cast<int>(result), static_cast<int>(StateError::ReadFailed));
    EXPECT(!merging.isOpen());
    // The merge abandons before the remove→rename, so the old state is still
    // the state — and the half-built temp is gone rather than waiting to be
    // adopted as a complete file by the next open().
    std::vector<uint8_t> afterMerge;
    EXPECT(readBytes(statePath(), afterMerge));
    EXPECT(beforeMerge == afterMerge);
    EXPECT(!fileExists(statePath() + ".tmp"));
#else
    EXPECT_EQ_U(static_cast<int>(result), static_cast<int>(StateError::Ok));
#endif
  }

  writing.close();
  resetCard();
}

/**
 * R3a NIT-16: a deck AT this build's cap — 40000 by default, 2000 on the C3 —
 * exercising both chunked readers. Written against DECK_MAX_CARDS rather than a
 * literal so it stays the cap test in both configurations; on the C3 it is also
 * the merge-at-the-cap fixture that testMergeAtCap() below builds on.
 */
void testMaximumDeck() {
  beginGroup("state/max deck");
  resetCard();
  makeDirs();

  const size_t count = flashcards::DECK_MAX_CARDS;
  const std::vector<TestCard> cards = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(deck.cardCount(), count);
  // The index is read in chunks (16 KB resident reads by default, 2560-byte
  // streamed ones on the C3) and the ordinals at both ends must still resolve.
  EXPECT_EQ_U(deck.keyAt(0), cards[0].key);
  EXPECT_EQ_U(deck.keyAt(static_cast<Ordinal>(count - 1)), cards[count - 1].key);

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), count);

  std::vector<uint8_t> raw;
  EXPECT(readBytes(statePath(), raw));
  EXPECT_EQ_U(raw.size(), stateBytes(count));
  // Record chunks are 128 records; check one at a chunk boundary, one mid-deck
  // and the very last, so a short final chunk would show up here.
  EXPECT_EQ_U(getU64(raw, recAt(128)), cards[128].key);
  EXPECT_EQ_U(getU64(raw, recAt(count / 2 + 1)), cards[count / 2 + 1].key);
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
  EXPECT_EQ_U(session.summary.newCards, 20);  // the daily allowance, not every card in the deck
  EXPECT_EQ_U(session.summary.newAvailable, count - 1);
  EXPECT_EQ_U(session.cards.size(), 21);
  store.close();
  resetCard();
}

/**
 * A re-download merge at this build's cap, which is where the C3's merge shape
 * is actually under load: (key, ordinal) pairs only — 20 KB at 2000 cards — and
 * one seek+read into the OLD state file per matched card, 2000 of them. The
 * default build runs the same assertions through its resident payload buffer.
 *
 * The v2 deck drops the first 50 cards, keeps the middle in a different order
 * and appends 50 new ones, so a merge that quietly relied on ordinals lining up
 * would carry the wrong schedules rather than none.
 */
void testMergeAtCap() {
  beginGroup("state/merge at cap");
  resetCard();
  makeDirs();

  const size_t count = flashcards::DECK_MAX_CARDS;
  const std::vector<TestCard> v1 = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v1))), static_cast<int>(DeckError::Ok));

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  // Every 7th card gets a schedule keyed to its ordinal, so a mismatch after the
  // merge names the card it came from.
  bool seeded = true;
  for (size_t i = 0; i < count; i += 7) {
    if (!store.writeRecord(static_cast<Ordinal>(i),
                           reviewCard(static_cast<uint16_t>(600 + i % 300), static_cast<uint16_t>(500 + i % 100)))) {
      seeded = false;
    }
  }
  EXPECT(seeded);
  store.close();

  // v2: drop the first 50, reverse the survivors, append 50 unseen cards.
  std::vector<TestCard> v2;
  v2.reserve(count);
  for (size_t i = count; i > 50; i--) v2.push_back(v1[i - 1]);
  for (size_t i = 0; i < 50; i++) {
    v2.push_back(TestCard{0x7000000000000000ULL + i, "new front", "new back"});
  }
  EXPECT_EQ_U(v2.size(), count);
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v2))), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), count);

  // Every v2 ordinal must carry exactly the schedule its KEY had in v1.
  bool carriedOk = true;
  bool freshOk = true;
  size_t carried = 0;
  for (size_t i = 0; i < count; i++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(i), state) != RecordStatus::Ok) {
      carriedOk = false;
      continue;
    }
    // Which v1 ordinal was this key?
    size_t from = SIZE_MAX;
    if (i < count - 50) from = count - 1 - i;  // the reversed survivors
    if (from != SIZE_MAX && from % 7 == 0) {
      carried++;
      if (state.state != CardPhase::Review || state.due != 600 + from % 300 ||
          state.lastReviewDay != 500 + from % 100) {
        carriedOk = false;
      }
    } else if (state.state != CardPhase::New || state.due != 0 || state.reps != 0) {
      freshOk = false;
    }
  }
  EXPECT(carriedOk);
  EXPECT(freshOk);
  // The survivors are v1 ordinals 50..count-1; the seeded ones are those
  // divisible by 7. Counted independently of the merge that is under test.
  size_t expectedCarried = 0;
  for (size_t o = 50; o < count; o++) {
    if (o % 7 == 0) expectedCarried++;
  }
  EXPECT_EQ_U(carried, expectedCarried);
  EXPECT(!store.keyMismatchSeen());
  store.close();
  resetCard();
}

/** A well-formed CPST v1 file holding one record per entry of `keys`/`states`. */
std::vector<uint8_t> buildStateFile(const std::vector<uint64_t>& keys, const std::vector<CardState>& states,
                                    uint64_t contentHash, uint16_t day, uint16_t newToday, uint16_t revToday) {
  std::vector<uint8_t> out(stateBytes(keys.size()), 0);
  memcpy(out.data(), "CPST", 4);
  putU16(out, 4, flashcards::CPST_VERSION);
  putU32(out, 8, static_cast<uint32_t>(keys.size()));
  putU64(out, 12, contentHash);
  putU32(out, 20, day);  // counters_day
  putU16(out, 24, newToday);
  putU16(out, 26, revToday);
  putU32(out, 28, day);  // last_seen_day
  for (size_t i = 0; i < keys.size(); i++) {
    putU64(out, recAt(i), keys[i]);
    memcpy(out.data() + recAt(i) + 8, &states[i], 20);
  }
  return out;
}

/**
 * C3b MUST-2, the merge's half. The C3 merge fetches each carried card out of
 * the old file rather than out of RAM, so it re-checks the key echo of what
 * came back against what the sorted key array promised. The fault that check
 * exists for is not a read that FAILS — a failed read leaves nothing to copy
 * and the card simply restarts — it is a seek that reports success and lands
 * somewhere else, after which a full-length read hands back the wrong records.
 *
 * The rule it enforces is the one that matters to a person: a merge may cost a
 * card its history, but it must NEVER give a card somebody else's. Every schedule
 * here is distinct, so a foreign one is identifiable.
 */
void testMergeKeyEchoUnderMisseek() {
  beginGroup("state/merge key echo");
  resetCard();
  makeDirs();

  const size_t count = 300;
  const std::vector<TestCard> v1 = makeShuffledCards(count, 0x3300);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v1))), static_cast<int>(DeckError::Ok));
  {
    StateStore seed;
    EXPECT_EQ_U(static_cast<int>(seed.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
    bool seeded = true;
    for (size_t i = 0; i < count; i++) {
      // due is unique per v1 ordinal, so a payload that came from the wrong
      // record names the record it came from.
      if (!seed.writeRecord(static_cast<Ordinal>(i), reviewCard(static_cast<uint16_t>(1000 + i), 900))) seeded = false;
    }
    EXPECT(seeded);
  }

  std::vector<TestCard> v2;
  v2.reserve(count);
  for (size_t k = 0; k < count; k++) v2.push_back(v1[(k + 61) % count]);
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v2))), static_cast<int>(DeckError::Ok));

  // Every seek into the old state after the first lands one record late. (The
  // first is the sequential key pass, which must be allowed to load real keys —
  // the point is a merge that KNOWS which key it wants and is handed the wrong
  // record anyway.)
  hostIoFaults().reset();
  hostIoFaults().pathSuffix = ".state";
  hostIoFaults().allowSeeksBeforeSkew = 1;
  hostIoFaults().seekSkewBytes = static_cast<long>(REC);

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), count);
  resetIo();

  bool noForeignSchedule = true;
  for (size_t o = 0; o < count; o++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(o), state) != RecordStatus::Ok) noForeignSchedule = false;
    const size_t from = (o + 61) % count;  // the v1 ordinal this key came from
    const bool restarted = state.state == CardPhase::New && state.due == 0;
    const bool itsOwn = state.state == CardPhase::Review && state.due == 1000 + from;
    if (!restarted && !itsOwn) noForeignSchedule = false;
  }
  EXPECT(noForeignSchedule);
  EXPECT(!store.keyMismatchSeen());
  store.close();

  // And with the card seeking straight again, the same merge carries every
  // schedule to the right ordinal: the echo costs nothing when nothing is wrong.
  resetCard();
  makeDirs();
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v1))), static_cast<int>(DeckError::Ok));
  {
    StateStore seed;
    EXPECT_EQ_U(static_cast<int>(seed.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
    for (size_t i = 0; i < count; i++) {
      seed.writeRecord(static_cast<Ordinal>(i), reviewCard(static_cast<uint16_t>(1000 + i), 900));
    }
  }
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v2))), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
  bool allItsOwn = true;
  for (size_t o = 0; o < count; o++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(o), state) != RecordStatus::Ok) allItsOwn = false;
    if (state.state != CardPhase::Review || state.due != 1000 + (o + 61) % count) allItsOwn = false;
  }
  EXPECT(allItsOwn);
  store.close();
  deck.close();
  resetCard();
}

/**
 * C3b SHOULD-7 (FLASHCARD_SPEC.md §7b.3 pin d). An SD card moves from a Pro to
 * the C3 and brings a state file with MORE records than the C3's own 2000-card
 * deck cap. Clamping the OLD side of the merge at the READING build's cap would
 * drop every record past that quietly — a thousand cards losing their history
 * with nothing said. The old side is bounded by the FORMAT's cap instead, and
 * an old side too big for the heap is a typed refusal with the file untouched.
 *
 * The carrying half runs in both builds: 3000 old records are inside the Pro's
 * cap too, so the two must agree about every schedule.
 */
void testOversizedOldState() {
  beginGroup("state/oversized old state");
  resetCard();
  makeDirs();

  constexpr size_t OLD_RECORDS = 3000;
  constexpr size_t DECK_CARDS = 2000;
  const std::vector<TestCard> cards = makeShuffledCards(DECK_CARDS, 0x5000);

  // The deck's own keys sit in old slots 1000..2999, REVERSED — so the records
  // a 2000-record clamp would have dropped are real cards, not filler.
  std::vector<uint64_t> oldKeys(OLD_RECORDS, 0);
  std::vector<CardState> oldStates(OLD_RECORDS);
  for (size_t s = 0; s < OLD_RECORDS; s++) {
    oldStates[s] = reviewCard(static_cast<uint16_t>(700 + s % 200), static_cast<uint16_t>(600 + s % 80));
    oldKeys[s] = s < OLD_RECORDS - DECK_CARDS ? 0xF000000000000000ULL + s : cards[OLD_RECORDS - 1 - s].key;
  }

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(cards))), static_cast<int>(DeckError::Ok));
  const std::vector<uint8_t> oldFile = buildStateFile(oldKeys, oldStates, deck.contentHash() ^ 0xFFFFULL, 900, 4, 9);
  EXPECT(writeBytes(statePath(), oldFile));

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), DECK_CARDS);
  // A merge is not a new day, however big the old file was.
  EXPECT_EQ_U(store.newToday(), 4);
  EXPECT_EQ_U(store.reviewsToday(), 9);

  bool allCarried = true;
  for (size_t o = 0; o < DECK_CARDS; o++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(o), state) != RecordStatus::Ok) {
      allCarried = false;
      continue;
    }
    const size_t slot = OLD_RECORDS - 1 - o;  // where this card's key sat in the old file
    if (state.state != CardPhase::Review || state.due != 700 + slot % 200 || state.lastReviewDay != 600 + slot % 80) {
      allCarried = false;
    }
  }
  EXPECT(allCarried);
  EXPECT(!store.keyMismatchSeen());
  store.close();

#ifdef CROSSPOINT_FLASHCARDS_C3
  // The same file with a heap that cannot hold 3000 keys: a TYPED refusal, and
  // the old file still byte for byte what it was. Never a quiet truncation to
  // whatever would have fitted.
  EXPECT(writeBytes(statePath(), oldFile));
  hostHeapGauge().maxAlloc = OLD_RECORDS * sizeof(uint64_t) + 1024 - 1;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 900)),
              static_cast<int>(StateError::OutOfMemory));
  EXPECT(!store.isOpen());
  hostHeapGauge().reset();
  std::vector<uint8_t> after;
  EXPECT(readBytes(statePath(), after));
  EXPECT(oldFile == after);
  EXPECT(!fileExists(statePath() + ".tmp"));
#endif

  deck.close();
  resetCard();
}

/**
 * C3b SHOULD-4 (FLASHCARD_SPEC.md §7b.3 pin c). The C3 merge does not hold the
 * old payloads, so every carried card is a read out of the old file. One read
 * per card is ~2000 random reads for a full deck — seconds on the loop task,
 * against a 5 s task WDT — so the merge carries a read-ahead window aligned to
 * its own size. This group is what says the window is actually there: it counts
 * the operations the merge performs, through the same stub seam the fault
 * injection uses, and prints the figures.
 *
 * Both builds run it. The Pro's number is the floor (its merge reads the old
 * file exactly once, in chunks); the C3's has to stay in the same order of
 * magnitude rather than the deck's card count.
 */
void testMergeReadAheadCounts() {
  beginGroup("state/merge read-ahead");
  const size_t count = 2000;

  // --- the ordinary case: the re-export kept the card order -------------------
  resetCard();
  makeDirs();
  const std::vector<TestCard> v1 = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v1))), static_cast<int>(DeckError::Ok));
  {
    StateStore seed;
    EXPECT_EQ_U(static_cast<int>(seed.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  }
  std::vector<TestCard> ordered(v1.begin(), v1.end() - 50);
  for (size_t i = 0; i < 50; i++) {
    ordered.push_back(TestCard{0x8000000000000000ULL + i, "added front", "added back"});
  }
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(ordered))), static_cast<int>(DeckError::Ok));

  StateStore store;
  hostIoCounters().zero();
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  const long orderedReads = hostIoCounters().stateReads;
  const long orderedSeeks = hostIoCounters().stateSeeks;
  store.close();
  printf("  [%s] merge of %u cards, old ordinals in order: %ld reads / %ld seeks on the old state\n", g_group,
         static_cast<unsigned>(count), orderedReads, orderedSeeks);
  // One read per matched card would be ~2000. The window has to keep this in
  // the tens, which is where the Pro's chunked pass already is.
  EXPECT(orderedReads < 200);
  EXPECT(orderedSeeks < 200);

  // --- the worst case: the re-export scattered them ---------------------------
  resetCard();
  makeDirs();
  const std::vector<TestCard> s1 = makeShuffledCards(count, 0x7000);
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(s1))), static_cast<int>(DeckError::Ok));
  {
    StateStore seed;
    EXPECT_EQ_U(static_cast<int>(seed.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  }
  std::vector<TestCard> s2;
  s2.reserve(count);
  for (size_t k = 0; k < count; k++) s2.push_back(s1[(k * 37) % count]);  // 37 is coprime with 2000
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(s2))), static_cast<int>(DeckError::Ok));

  hostIoCounters().zero();
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 1000)), static_cast<int>(StateError::Ok));
  const long shuffledReads = hostIoCounters().stateReads;
  const long shuffledSeeks = hostIoCounters().stateSeeks;
  printf("  [%s] merge of %u cards, old ordinals scattered: %ld reads / %ld seeks on the old state\n", g_group,
         static_cast<unsigned>(count), shuffledReads, shuffledSeeks);
  // Every step here crosses a window, so this is the floor the read-ahead can
  // fall back to and not a regression: it must not EXCEED one read per card.
  EXPECT(shuffledReads <= static_cast<long>(count) + 100);
  EXPECT(shuffledSeeks <= static_cast<long>(count) + 100);

  // The scattering must not have cost a single schedule, whatever it cost in
  // reads: the numbers above are only meaningful if the merge was correct.
  bool carriedOk = true;
  for (size_t o = 0; o < count; o++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(o), state) != RecordStatus::Ok) carriedOk = false;
    if (state.state != CardPhase::New) carriedOk = false;  // the seed left every card New
  }
  EXPECT(carriedOk);
  EXPECT(!store.keyMismatchSeen());
  store.close();
  deck.close();
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

// --- gated allocations (C3 only) ---------------------------------------------

#ifdef CROSSPOINT_FLASHCARDS_C3
/**
 * FLASHCARD_SPEC.md §7b.3: every allocation on the C3 paths is gated on the
 * largest free block and reports a typed error rather than aborting. A gate
 * nobody has watched refuse is a gate nobody has tested, so stubs/HalHeapGauge.h
 * makes the figure settable and this group drives each gate into its refusal.
 *
 * Two things are asserted every time: the failure is the TYPED one the study
 * screen can render, and the store/queue is left in a state a retry recovers
 * from once the heap figure comes back.
 */
void testGatedAllocationFailures() {
  beginGroup("c3/gated allocs");
  resetCard();
  makeDirs();

  // 1000 cards, chosen so the Cram-all ordinal buffer (2000 B) is LARGER than
  // the review picker (200 picks x 8 B = 1600 B). Below about 800 cards it is
  // the smaller of the two, and a gauge low enough to refuse it would refuse
  // the picker first — the cram gate would then never be what is under test.
  const std::vector<TestCard> v1 = makeCards(1000);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v1))), static_cast<int>(DeckError::Ok));

  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)), static_cast<int>(StateError::Ok));
  EXPECT(store.writeRecord(5, reviewCard(650, 640)));
  store.close();

  // --- the merge's (key, ordinal) pairs -------------------------------------
  // A v2 deck with a different content hash, so open() takes the merge path.
  std::vector<TestCard> v2(v1.rbegin(), v1.rend());
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v2))), static_cast<int>(DeckError::Ok));

  std::vector<uint8_t> before;
  EXPECT(readBytes(statePath(), before));

  hostHeapGauge().maxAlloc = 512;  // far below the 8000 B key array for 1000 cards
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)),
              static_cast<int>(StateError::OutOfMemory));
  EXPECT(!store.isOpen());
  EXPECT_EQ_U(store.recordCount(), 0);
  // The refusal happens before a byte is written: the old state is intact and
  // no half-built temp is left behind for the next open() to adopt.
  std::vector<uint8_t> after;
  EXPECT(readBytes(statePath(), after));
  EXPECT(before == after);
  EXPECT(!fileExists(statePath() + ".tmp"));

  // With room again, the same call merges — nothing was poisoned by the refusal.
  hostHeapGauge().reset();
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), v1.size());
  CardState carried{};
  // v1 ordinal 5 is v2 ordinal (count - 1 - 5) after the reversal.
  EXPECT_EQ_U(static_cast<int>(store.readRecord(static_cast<Ordinal>(v1.size() - 1 - 5), carried)),
              static_cast<int>(RecordStatus::Ok));
  EXPECT_EQ_U(carried.due, 650);

  // --- the session buffers ---------------------------------------------------
  fsrs::Now now{1700000000u, 700};
  Session session;

  // A gauge BETWEEN the two asks: enough for the pickers (1600 B + 1 KB
  // headroom = 2624), not enough for the Cram-all ordinal buffer (2000 B + 1 KB
  // = 3024). So Cram must be refused for exactly the buffer this round is
  // about, while a Due session over the same store still builds — which is what
  // proves the refusal was the ordinal buffer and not a blanket failure.
  hostHeapGauge().maxAlloc = 2800;
  EXPECT(!flashcards::buildSession(store, SessionMode::CramAll, now, session));
  EXPECT_EQ_U(session.cards.size(), 0);
  EXPECT(flashcards::buildSession(store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.summary.reviews, 1);

  // Lower still: now the review picker itself is refused, and every scheduled
  // mode fails with it (NewOnly resets the pickers too, even though it will not
  // use them).
  hostHeapGauge().maxAlloc = 1200;  // < 200 picks * 8 B + 1 KB headroom
  EXPECT(!flashcards::buildSession(store, SessionMode::Due, now, session));
  EXPECT(!flashcards::buildSession(store, SessionMode::NewOnly, now, session));
  EXPECT(!flashcards::buildSession(store, SessionMode::CramAll, now, session));

  hostHeapGauge().reset();
  EXPECT(flashcards::buildSession(store, SessionMode::CramAll, now, session));
  EXPECT_EQ_U(session.cards.size(), v1.size());
  EXPECT(flashcards::buildSession(store, SessionMode::Due, now, session));
  EXPECT_EQ_U(session.summary.reviews, 1);

  // DeckFile::open() has NO allocation to gate on this build — the index is
  // never held — so a gauge pinned at zero must not stop a deck opening.
  hostHeapGauge().maxAlloc = 0;
  DeckFile starved;
  EXPECT_EQ_U(static_cast<int>(starved.open(deckPath())), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(starved.keyAt(0), v2[0].key);
  char text[64];
  uint16_t length = 0;
  EXPECT(starved.loadSide(0, CardSide::Front, text, sizeof(text), length));
  EXPECT_STR(text, v2[0].front.c_str());
  starved.close();

  hostHeapGauge().reset();
  store.close();
  resetCard();
}

/**
 * C3b SHOULD-6. The group above drives each gate into refusing from far away;
 * this one stands on the edge of every gate. Each allocation is checked at
 * EXACTLY what it needs (which must succeed) and at one byte less (which must
 * not), so the arithmetic in the gate itself — the block's own size, plus the
 * 1 KB of slack, compared with >= and not > — is pinned rather than inferred.
 * FLASHCARD_SPEC.md §7b.3 pin e: each block is gated for its own size.
 */
void testGateBoundaries() {
  beginGroup("c3/gate boundaries");
  resetCard();
  makeDirs();

  const size_t count = 1000;
  const std::vector<TestCard> v1 = makeCards(count);
  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v1))), static_cast<int>(DeckError::Ok));
  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)), static_cast<int>(StateError::Ok));
  EXPECT(store.writeRecord(5, reviewCard(650, 640)));
  store.close();

  // A v2 with a different content hash, so open() takes the merge path.
  const std::vector<TestCard> v2(v1.rbegin(), v1.rend());
  EXPECT_EQ_U(static_cast<int>(installDeck(deck, buildDeck(v2))), static_cast<int>(DeckError::Ok));

  // --- the merge's key array: 1000 keys plus the mandatory slack --------------
  // The need-1 case has to come first: the exactly-enough case MERGES, and a
  // merged file has the deck's hash, so a second open() would only adopt.
  const size_t mergeNeed = count * sizeof(uint64_t) + 1024;
  hostHeapGauge().maxAlloc = mergeNeed - 1;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)),
              static_cast<int>(StateError::OutOfMemory));
  EXPECT(!fileExists(statePath() + ".tmp"));
  hostHeapGauge().maxAlloc = mergeNeed;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, decksDir(), DECK_LEAF, 700)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), count);
  hostHeapGauge().reset();

  // --- the session's three blocks --------------------------------------------
  // A fresh Session each time: OrdinalBuffer::reserve keeps a block that is
  // already big enough, which would take the gate out of the picture.
  fsrs::Now now{1700000000u, 700};
  const size_t pickerNeed = flashcards::DAILY_REVIEW_LIMIT * 8 + 1024;  // Pick is {u32, u16} = 8 B
  const size_t cramNeed = count * sizeof(Ordinal) + 1024;

  {
    Session session;
    hostHeapGauge().maxAlloc = pickerNeed - 1;
    EXPECT(!flashcards::buildSession(store, SessionMode::Due, now, session));
  }
  {
    Session session;
    hostHeapGauge().maxAlloc = pickerNeed;
    EXPECT(flashcards::buildSession(store, SessionMode::Due, now, session));
    EXPECT_EQ_U(session.summary.reviews, 1);
  }
  {
    Session session;
    hostHeapGauge().maxAlloc = cramNeed - 1;
    EXPECT(!flashcards::buildSession(store, SessionMode::CramAll, now, session));
    EXPECT_EQ_U(session.cards.size(), 0);
  }
  {
    Session session;
    hostHeapGauge().maxAlloc = cramNeed;
    EXPECT(flashcards::buildSession(store, SessionMode::CramAll, now, session));
    EXPECT_EQ_U(session.cards.size(), count);
  }

  hostHeapGauge().reset();
  store.close();
  deck.close();
  resetCard();
}
#endif  // CROSSPOINT_FLASHCARDS_C3

// --- cross-variant compatibility ---------------------------------------------

/**
 * FLASHCARD_SPEC.md §7b.3: the on-disk formats are IDENTICAL between the two
 * builds; only the in-RAM strategy differs. A deck and state written by one must
 * be fully usable by the other, including through a re-download merge.
 *
 * That cannot be shown inside one process, so build.sh runs each binary once
 * with `--produce` (writing a deck, a v2 deck and a state file with known
 * schedules under build/card/xvariant/) before either runs its suite, and the
 * suite then consumes the OTHER configuration's artifacts. Both directions are
 * therefore covered by the pair of runs.
 */

#ifdef CROSSPOINT_FLASHCARDS_C3
constexpr const char* THIS_VARIANT = "c3";
constexpr const char* OTHER_VARIANT = "std";
#else
constexpr const char* THIS_VARIANT = "std";
constexpr const char* OTHER_VARIANT = "c3";
#endif

constexpr size_t CROSS_CARDS = 12;
constexpr uint16_t CROSS_DAY = 4321;
constexpr uint16_t CROSS_NEW_TODAY = 3;
constexpr uint16_t CROSS_REV_TODAY = 11;
/** v2 keeps v1's cards 3..11 in reverse and appends three unseen ones. */
constexpr size_t CROSS_DROPPED = 3;

std::string xDir() { return g_root + "/xvariant"; }
std::string xStatePath(const std::string& leaf) { return xDir() + "/.state/" + leaf + ".state"; }

/** The deck both variants write, byte for byte: same keys, same text, same order. */
std::vector<TestCard> crossCardsV1() {
  std::vector<TestCard> cards;
  cards.reserve(CROSS_CARDS);
  for (size_t i = 0; i < CROSS_CARDS; i++) {
    char text[40];
    snprintf(text, sizeof(text), "x front %u", static_cast<unsigned>(i));
    std::string front(text);
    snprintf(text, sizeof(text), "x back %u", static_cast<unsigned>(i));
    cards.push_back(TestCard{0x5A5A000000000000ULL + i * 0x0101ULL, front, std::string(text)});
  }
  return cards;
}

std::vector<TestCard> crossCardsV2() {
  const std::vector<TestCard> v1 = crossCardsV1();
  std::vector<TestCard> v2;
  v2.reserve(CROSS_CARDS);
  for (size_t i = CROSS_CARDS; i > CROSS_DROPPED; i--) v2.push_back(v1[i - 1]);
  for (size_t i = 0; i < CROSS_DROPPED; i++) {
    v2.push_back(TestCard{0x6B6B000000000000ULL + i, "fresh front", "fresh back"});
  }
  return v2;
}

/** Which v1 ordinal a v2 ordinal came from, or SIZE_MAX for a card v2 added. */
size_t crossV1OrdinalOf(size_t v2Ordinal) {
  if (v2Ordinal >= CROSS_CARDS - CROSS_DROPPED) return SIZE_MAX;
  return CROSS_CARDS - 1 - v2Ordinal;
}

/**
 * The schedule ordinal `i` is written with. Every float is an exact binary
 * fraction, so the records compare bit for bit across the two builds rather
 * than approximately.
 */
CardState crossExpectedState(size_t i) {
  CardState card{};
  switch (i % 4) {
    case 0:
      return card;  // left New
    case 1:
      card.state = CardPhase::Review;
      card.due = static_cast<uint32_t>(4000 + i);
      card.lastReviewDay = static_cast<uint16_t>(3900 + i);
      card.stability = 1.25f * static_cast<float>(i + 1);
      card.difficulty = 2.5f + 0.125f * static_cast<float>(i);
      card.reps = static_cast<uint16_t>(i + 2);
      card.lapses = static_cast<uint8_t>(i % 3);
      return card;
    case 2:
      card.state = CardPhase::Learning;
      card.due = 1700000000u + static_cast<uint32_t>(i) * 60u;
      card.step = static_cast<uint8_t>(i % 2);
      card.stability = 0.5f;
      card.difficulty = 5.0f;
      card.reps = 1;
      return card;
    default:
      card.state = CardPhase::Review;
      card.due = static_cast<uint32_t>(4200 + i);
      card.lastReviewDay = static_cast<uint16_t>(4100 + i);
      card.stability = 30.0f;
      card.difficulty = 7.75f;
      card.reps = 9;
      card.lapses = 2;
      card.flags = fsrs::FLAG_SUSPENDED;
      return card;
  }
}

bool sameState(const CardState& a, const CardState& b) {
  return a.state == b.state && a.due == b.due && a.lastReviewDay == b.lastReviewDay && a.stability == b.stability &&
         a.difficulty == b.difficulty && a.step == b.step && a.reps == b.reps && a.lapses == b.lapses &&
         a.flags == b.flags;
}

/** `--produce`: lay down this configuration's artifacts for the other one to read. */
bool produceCrossVariantArtifacts() {
  Storage.ensureDirectoryExists(xDir().c_str());
  Storage.ensureDirectoryExists((xDir() + "/.state").c_str());

  const std::string leaf = std::string(THIS_VARIANT) + ".deck";
  const std::string v2Leaf = std::string(THIS_VARIANT) + "-v2.deck";
  if (!writeBytes(xDir() + "/" + leaf, buildDeck(crossCardsV1()))) return false;
  if (!writeBytes(xDir() + "/" + v2Leaf, buildDeck(crossCardsV2()))) return false;

  DeckFile deck;
  if (deck.open(xDir() + "/" + leaf) != DeckError::Ok) return false;
  StateStore store;
  if (store.open(deck, xDir(), leaf, CROSS_DAY) != StateError::Ok) return false;
  for (size_t i = 0; i < CROSS_CARDS; i++) {
    if (!store.writeRecord(static_cast<Ordinal>(i), crossExpectedState(i))) return false;
  }
  if (!store.writeCounters(CROSS_NEW_TODAY, CROSS_REV_TODAY)) return false;
  store.close();
  printf("produced cross-variant artifacts for \"%s\"\n", THIS_VARIANT);
  return true;
}

void testCrossVariantCompatibility() {
  beginGroup("cross-variant");

  const std::string otherDeck = xDir() + "/" + OTHER_VARIANT + ".deck";
  const std::string otherV2 = xDir() + "/" + OTHER_VARIANT + "-v2.deck";
  const std::string otherState = xStatePath(std::string(OTHER_VARIANT) + ".deck");
  if (!fileExists(otherDeck) || !fileExists(otherV2) || !fileExists(otherState)) {
    printf("SKIP [%s]: no \"%s\" artifacts under %s (run build.sh, which produces both first)\n", g_group,
           OTHER_VARIANT, xDir().c_str());
    return;
  }

  // The strongest single assertion available: the two builds, given the same
  // deck and the same writes, produced BYTE-IDENTICAL state files. Anything
  // that had drifted in the CPST layout, the 448-byte reserved gap, the header
  // counters or the record packing shows up here first.
  std::vector<uint8_t> mine;
  std::vector<uint8_t> theirs;
  EXPECT(readBytes(xStatePath(std::string(THIS_VARIANT) + ".deck"), mine));
  EXPECT(readBytes(otherState, theirs));
  EXPECT_EQ_U(mine.size(), stateBytes(CROSS_CARDS));
  EXPECT(mine == theirs);

  // Work copies, so the artifacts stay pristine for whichever run comes after.
  const std::string workLeaf = std::string(THIS_VARIANT) + "-work.deck";
  EXPECT(copyFile(otherDeck, xDir() + "/" + workLeaf));
  EXPECT(copyFile(otherState, xStatePath(workLeaf)));

  DeckFile deck;
  EXPECT_EQ_U(static_cast<int>(deck.open(xDir() + "/" + workLeaf)), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(deck.cardCount(), CROSS_CARDS);

  // Same content hash: this must ADOPT, not merge, and must carry the other
  // build's daily counters across untouched.
  StateStore store;
  EXPECT_EQ_U(static_cast<int>(store.open(deck, xDir(), workLeaf, CROSS_DAY)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), CROSS_CARDS);
  EXPECT_EQ_U(store.newToday(), CROSS_NEW_TODAY);
  EXPECT_EQ_U(store.reviewsToday(), CROSS_REV_TODAY);
  EXPECT_EQ_U(store.countersDay(), CROSS_DAY);

  bool adoptedOk = true;
  for (size_t i = 0; i < CROSS_CARDS; i++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(i), state) != RecordStatus::Ok) adoptedOk = false;
    if (!sameState(state, crossExpectedState(i))) adoptedOk = false;
  }
  EXPECT(adoptedOk);
  EXPECT(!store.keyMismatchSeen());  // every record's key echo matched the other build's deck
  store.close();

  // Now the re-download merge, over a v2 deck the OTHER build wrote.
  EXPECT(copyFile(otherV2, xDir() + "/" + workLeaf));
  EXPECT_EQ_U(static_cast<int>(deck.open(xDir() + "/" + workLeaf)), static_cast<int>(DeckError::Ok));
  EXPECT_EQ_U(static_cast<int>(store.open(deck, xDir(), workLeaf, CROSS_DAY)), static_cast<int>(StateError::Ok));
  EXPECT_EQ_U(store.recordCount(), CROSS_CARDS);
  // A merge is not a new day: the counters survive it.
  EXPECT_EQ_U(store.newToday(), CROSS_NEW_TODAY);
  EXPECT_EQ_U(store.reviewsToday(), CROSS_REV_TODAY);

  bool mergedOk = true;
  size_t carried = 0;
  for (size_t i = 0; i < CROSS_CARDS; i++) {
    CardState state{};
    if (store.readRecord(static_cast<Ordinal>(i), state) != RecordStatus::Ok) mergedOk = false;
    const size_t from = crossV1OrdinalOf(i);
    if (from == SIZE_MAX) {
      if (!sameState(state, CardState{})) mergedOk = false;
    } else {
      if (!sameState(state, crossExpectedState(from))) mergedOk = false;
      carried++;
    }
  }
  EXPECT(mergedOk);
  EXPECT_EQ_U(carried, CROSS_CARDS - CROSS_DROPPED);
  EXPECT(!store.keyMismatchSeen());
  store.close();
  deck.close();

  remove((xDir() + "/" + workLeaf).c_str());
  remove(xStatePath(workLeaf).c_str());
  remove((xStatePath(workLeaf) + ".tmp").c_str());
}

}  // namespace

int main(int argc, char** argv) {
  g_root = argc > 1 ? argv[1] : "/tmp/crosspoint-flashcard-tests";
  Storage.ensureDirectoryExists(g_root.c_str());
  makeDirs();

  // build.sh runs each configuration once in this mode, before either runs its
  // suite, so that each suite has the OTHER one's files to read (see
  // testCrossVariantCompatibility).
  if (argc > 2 && strcmp(argv[2], "--produce") == 0) {
    if (produceCrossVariantArtifacts()) return 0;
    printf("FAILED to produce cross-variant artifacts for \"%s\"\n", THIS_VARIANT);
    return 1;
  }

  printf("configuration: %s\n", THIS_VARIANT);

  testKeySort();
  testRealFixture();
  testDeckValidation();
  testDuplicateKeyPolicy();
  testCardCountCap();
  testIndexValidationAcrossChunks();
  testOnDemandIndexAccess();
  testKeyAtChecked();
  testDeckParameterBlock();
  testEmptyBack();
  testC0Sanitize();
  testImageValidation();
  testImageQueries();
  testPlacementWindowFault();
  testImageBytes();
  testStateCreation();
  testStateAdoptAndCounters();
  testStateRebuildsFromRubbish();
  testMerge();
  testMergeEdgeCases();
  testShuffledKeyMerge();
  testMergeCrashWindow();
  testTornKeyHeals();
  testIoFaultsDoNotHeal();
  testMaximumDeck();
  testMergeAtCap();
  testMergeKeyEchoUnderMisseek();
  testOversizedOldState();
  testMergeReadAheadCounts();
  testQueueBasics();
  testQueueInterleaveIsDeterministic();
  testQueueDailyLimits();
  testQueueLearnAhead();
  testLearnAheadAtDrain();
  testQueueModes();
  testQueueEmptyDeck();
  testUtcOffsetComposition();
#ifdef CROSSPOINT_FLASHCARDS_C3
  testGatedAllocationFailures();
  testGateBoundaries();
#endif
  testCrossVariantCompatibility();

  resetCard();
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    printf("FLASHCARD HOST TESTS FAILED\n");
    return 1;
  }
  printf("FLASHCARD HOST TESTS PASSED\n");
  return 0;
}
