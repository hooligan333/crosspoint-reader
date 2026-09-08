// Host-side cover for the two decisions in the deck sync flow that are pure
// logic and that get a file wrong if they are wrong: the guid content-hash
// parse plus the staleness comparison it feeds (FLASHCARD_SPEC.md §1), and the
// destination-folder root guard (a mirror that DELETES files must never point
// at the card root).

#include <gtest/gtest.h>

#include <string>

#include "DeckGuid.h"
#include "DestFolder.h"

namespace {

constexpr const char* FALLBACK = "/Decks";

// RssParser caps a <guid> at this many bytes and truncates the TAIL, which is
// where the hash suffix lives (lib/RssParser/RssParser.cpp).
constexpr size_t PARSER_GUID_CAP = 128;

std::string guidFor(const std::string& leaf, const char* hex) { return leaf + "@" + hex; }

// --- parseDeckGuidHash --------------------------------------------------

TEST(DeckGuid, ParsesTheSuffixOfAWellFormedGuid) {
  uint64_t hash = 0;
  EXPECT_TRUE(parseDeckGuidHash("spanish-verbs.deck@0123456789abcdef", hash));
  EXPECT_EQ(hash, 0x0123456789abcdefULL);
}

TEST(DeckGuid, KeepsLeadingZeroesAndTheTopBit) {
  uint64_t hash = 1;
  EXPECT_TRUE(parseDeckGuidHash("a.deck@0000000000000000", hash));
  EXPECT_EQ(hash, 0ULL);
  EXPECT_TRUE(parseDeckGuidHash("a.deck@ffffffffffffffff", hash));
  EXPECT_EQ(hash, 0xffffffffffffffffULL);
  EXPECT_TRUE(parseDeckGuidHash("a.deck@8000000000000001", hash));
  EXPECT_EQ(hash, 0x8000000000000001ULL);
}

TEST(DeckGuid, ABareSlugCarriesNoHashAndLeavesTheOutputAlone) {
  uint64_t hash = 0xdeadbeef;
  EXPECT_FALSE(parseDeckGuidHash("spanish-verbs", hash));
  EXPECT_EQ(hash, 0xdeadbeefULL);
  EXPECT_FALSE(parseDeckGuidHash("", hash));
  EXPECT_EQ(hash, 0xdeadbeefULL);
}

TEST(DeckGuid, RejectsEverySuffixThatIsNotExactlySeventeenBytes) {
  uint64_t hash = 0;
  EXPECT_FALSE(parseDeckGuidHash("a.deck@0123456789abcde", hash));    // 15 digits
  EXPECT_FALSE(parseDeckGuidHash("a.deck@0123456789abcdef0", hash));  // 17 digits
  EXPECT_FALSE(parseDeckGuidHash("a.deck@", hash));
  EXPECT_FALSE(parseDeckGuidHash("@", hash));
}

TEST(DeckGuid, RejectsUppercaseHex) {
  // The format is pinned lowercase; accepting both spellings would let one
  // hash read as two different values across servers.
  uint64_t hash = 0;
  EXPECT_FALSE(parseDeckGuidHash("a.deck@0123456789ABCDEF", hash));
  EXPECT_FALSE(parseDeckGuidHash("a.deck@0123456789abcdeF", hash));
}

TEST(DeckGuid, RejectsNonHexAndAMisplacedAt) {
  uint64_t hash = 0;
  EXPECT_FALSE(parseDeckGuidHash("a.deck@0123456789abcdeg", hash));
  EXPECT_FALSE(parseDeckGuidHash("a.deck@0123456789abcde ", hash));
  EXPECT_FALSE(parseDeckGuidHash("a@deck-0123456789abcdef", hash));
  EXPECT_FALSE(parseDeckGuidHash("@0123456789abcdef.deck", hash));
}

TEST(DeckGuid, AnEmptyLeafStillParses) {
  // Degenerate but harmless: the device identifies a deck by its enclosure
  // filename, never by the guid's leaf half.
  uint64_t hash = 0;
  EXPECT_TRUE(parseDeckGuidHash("@0123456789abcdef", hash));
  EXPECT_EQ(hash, 0x0123456789abcdefULL);
}

TEST(DeckGuid, TheLongestLeafThatStillCarriesAHashIsOneHundredEleven) {
  // 128-byte cap - 17-byte suffix. With the server's own 64-char leaf limit
  // (DECK_SERVER_SPEC.md §2) there is 47 bytes of slack, so a real feed never
  // comes close.
  uint64_t hash = 0;
  const std::string longestLeaf(PARSER_GUID_CAP - 17, 'a');
  const std::string fits = guidFor(longestLeaf, "0123456789abcdef");
  ASSERT_EQ(fits.size(), PARSER_GUID_CAP);
  EXPECT_TRUE(parseDeckGuidHash(fits, hash));
  EXPECT_EQ(hash, 0x0123456789abcdefULL);
}

TEST(DeckGuid, AGuidThatOverflowsTheParserCapDegradesToNoHashNotAWrongOne) {
  // RssParser truncates the tail, so an over-long guid loses hex digits off the
  // end. The parse must fail rather than accept a shifted window.
  uint64_t hash = 0xdeadbeef;
  const std::string tooLong = guidFor(std::string(PARSER_GUID_CAP - 16, 'a'), "0123456789abcdef");
  const std::string truncated = tooLong.substr(0, PARSER_GUID_CAP);
  EXPECT_FALSE(parseDeckGuidHash(truncated, hash));
  EXPECT_EQ(hash, 0xdeadbeefULL);
}

// --- deckIsStale --------------------------------------------------------

DeckFeedStamp feedWithHash(const uint64_t hash, const uint32_t size = 4957) {
  DeckFeedStamp feed;
  feed.contentHash = hash;
  feed.hasContentHash = true;
  feed.sizeBytes = size;
  return feed;
}

DeckFeedStamp feedWithoutHash(const uint32_t size) {
  DeckFeedStamp feed;
  feed.sizeBytes = size;
  return feed;
}

LocalDeckStamp localDeck(const uint64_t hash, const uint32_t size = 4957) {
  LocalDeckStamp local;
  local.contentHash = hash;
  local.sizeBytes = size;
  local.readable = true;
  return local;
}

TEST(DeckStaleness, MatchingHashIsFresh) {
  EXPECT_FALSE(deckIsStale(feedWithHash(0x1122334455667788), localDeck(0x1122334455667788)));
}

TEST(DeckStaleness, MismatchedHashIsStale) {
  EXPECT_TRUE(deckIsStale(feedWithHash(0x1122334455667788), localDeck(0x1122334455667789)));
}

TEST(DeckStaleness, HashWinsOverSize) {
  // The re-export that motivates this whole mechanism — an edited card, same
  // byte count — is invisible to the size check and caught by the hash.
  EXPECT_TRUE(deckIsStale(feedWithHash(0xaaaa, 4957), localDeck(0xbbbb, 4957)));
  // And the converse: a size the feed got wrong must not force a pointless
  // re-download of a deck whose content is provably identical.
  EXPECT_FALSE(deckIsStale(feedWithHash(0xaaaa, 4957), localDeck(0xaaaa, 9999)));
}

TEST(DeckStaleness, AnUnreadableLocalFileIsAlwaysStale) {
  // Truncated, empty, not a CPDK file, or gone between the list and the sync:
  // re-downloading is the repair, so it must not be reported as up to date.
  const LocalDeckStamp unreadable;
  EXPECT_TRUE(deckIsStale(feedWithHash(0xaaaa), unreadable));
  EXPECT_TRUE(deckIsStale(feedWithoutHash(4957), unreadable));
  EXPECT_TRUE(deckIsStale(feedWithoutHash(0), unreadable));
}

TEST(DeckStaleness, HashlessGuidFallsBackToTheEnclosureLength) {
  EXPECT_FALSE(deckIsStale(feedWithoutHash(4957), localDeck(0x1234, 4957)));
  EXPECT_TRUE(deckIsStale(feedWithoutHash(5000), localDeck(0x1234, 4957)));
}

TEST(DeckStaleness, HashlessGuidWithNoLengthCannotTellAndKeepsTheLocalFile) {
  // Nothing to compare: guessing "stale" here would re-download every deck on
  // every sync for a feed that publishes neither signal.
  EXPECT_FALSE(deckIsStale(feedWithoutHash(0), localDeck(0x1234, 4957)));
}

// --- normalizeDestFolder ------------------------------------------------

TEST(DestFolder, KeepsAnOrdinaryFolder) {
  EXPECT_EQ(normalizeDestFolder("/Decks", FALLBACK), "/Decks");
  EXPECT_EQ(normalizeDestFolder("Decks", FALLBACK), "/Decks");
  EXPECT_EQ(normalizeDestFolder("  /Decks/  ", FALLBACK), "/Decks");
  EXPECT_EQ(normalizeDestFolder("//a//b//", FALLBACK), "/a/b");
  EXPECT_EQ(normalizeDestFolder("/a/../b", FALLBACK), "/b");
}

TEST(DestFolder, AnythingThatResolvesToTheRootFallsBack) {
  EXPECT_EQ(normalizeDestFolder("", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("   ", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("/", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("///", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("/..", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("/a/..", FALLBACK), FALLBACK);
}

TEST(DestFolder, DotComponentsDoNotSurviveTheRootGuard) {
  // "." is an ordinary component to FsHelpers::normalisePath, so without the
  // filtering in normalizeDestFolder these would all aim at the card root.
  EXPECT_EQ(normalizeDestFolder("/.", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder(".", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("/./.", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("/a/../.", FALLBACK), FALLBACK);
  EXPECT_EQ(normalizeDestFolder("/./..", FALLBACK), FALLBACK);
  // "." must also not soak up a "..", which would leave "/a" instead of root.
  EXPECT_EQ(normalizeDestFolder("/a/./..", FALLBACK), FALLBACK);
  // And it must not disturb a legitimate folder.
  EXPECT_EQ(normalizeDestFolder("/./Decks/.", FALLBACK), "/Decks");
}

}  // namespace
