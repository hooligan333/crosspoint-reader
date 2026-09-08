#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <cstdint>
#include <string>

/**
 * The staleness half of the deck feed contract (FLASHCARD_SPEC.md §1).
 *
 * Decks are MUTABLE — unlike the ePUBs the RSS mirror carries, a deck is
 * re-exported every time its cards change, under the same leaf filename — so
 * "the file is already there" is not a reason to skip it. The feed therefore
 * stamps each item's guid with the content hash of the very file it is
 * offering, and the device compares that against the header of the copy on the
 * card before deciding a checked row is up to date.
 *
 * Everything here is pure, header-only and free of SD/Arduino dependencies so
 * the decision rule can be unit tested on the host (test/deck_guid).
 */

// What the FEED claims about a deck, parsed once when the list is built.
struct DeckFeedStamp {
  uint64_t contentHash = 0;  // from the guid's "@<16 hex>" suffix, iff hasContentHash
  uint32_t sizeBytes = 0;    // <enclosure length>; 0 when the feed omits it
  bool hasContentHash = false;
};

// What the copy on the CARD actually is: filled from a single 64-byte CPDK
// header read. `readable` is false when the file will not open, is shorter
// than a header, or does not start with "CPDK".
struct LocalDeckStamp {
  uint64_t contentHash = 0;  // CPDK header, offset 12
  uint32_t sizeBytes = 0;    // on-disk length
  bool readable = false;
};

/**
 * Parses the content-hash suffix a deck feed appends to its guid:
 * `<leaf>@<16 lowercase hex digits>` — exactly 17 trailing bytes.
 *
 * Anything else is "no hash": a bare slug, an uppercase or short hash, a '@'
 * anywhere but that position, or a guid the parser's 128-byte cap truncated
 * (RssParser drops the END of an over-long guid, which is precisely where the
 * suffix lives, so an over-long guid degrades to no-hash rather than to a
 * WRONG hash). Callers fall back to the size comparison in that case.
 *
 * On false, `hashOut` is left untouched.
 */
inline bool parseDeckGuidHash(const std::string& guid, uint64_t& hashOut) {
  constexpr size_t SUFFIX_BYTES = 17;  // '@' + 16 hex digits
  if (guid.size() < SUFFIX_BYTES) return false;
  const size_t at = guid.size() - SUFFIX_BYTES;
  if (guid[at] != '@') return false;

  uint64_t value = 0;
  for (size_t i = at + 1; i < guid.size(); i++) {
    const char c = guid[i];
    uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint64_t>(c - 'a') + 10;
    } else {
      // Uppercase included: the format is pinned lowercase, and accepting both
      // would let two spellings of one hash read as two different decks.
      return false;
    }
    value = (value << 4) | digit;
  }
  hashOut = value;
  return true;
}

/**
 * True when the deck on the card no longer matches what the feed is offering,
 * i.e. the row must be re-downloaded even though its file is present.
 *
 * The re-download REPLACES the deck and leaves `.state/<leaf>.state` alone —
 * the study session reconciles it against the new content hash.
 */
inline bool deckIsStale(const DeckFeedStamp& feed, const LocalDeckStamp& local) {
  // A local file that cannot be read cannot be compared — and cannot be
  // studied either. Re-downloading is both the honest answer and the repair.
  if (!local.readable) return true;
  if (feed.hasContentHash) return local.contentHash != feed.contentHash;
  // Hashless guid (an older or hand-written feed): the byte count is the only
  // cheap signal left, and an edit that keeps the size slips past it. That is
  // a documented degradation, not a guarantee (FLASHCARD_SPEC.md §1).
  if (feed.sizeBytes == 0) return false;
  return local.sizeBytes != feed.sizeBytes;
}

#endif  // CROSSPOINT_FLASHCARDS
