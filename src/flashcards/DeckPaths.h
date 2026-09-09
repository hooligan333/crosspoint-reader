#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <string>

/**
 * Where a deck and its scheduling state live on the card (FLASHCARD_SPEC.md §3).
 *
 * The state file is `<destFolder>/.state/<leaf>.state` where `<leaf>` INCLUDES
 * the `.deck` extension — `/Decks/.state/spanish.deck.state`. Two independent
 * users of that rule (FlashcardSyncActivity's delete-on-untick and the study
 * session's reader/writer) have to spell it identically or a deck's state is
 * either orphaned or deleted out from under it, so the rule lives here once
 * rather than as a pair of string constants that agree by convention.
 *
 * Header-only and free of every dependency but std::string: the sync screen
 * must be able to name a state file without pulling the study layer (and the
 * scheduler with it) into its translation unit.
 *
 * **Callers must pass an already-normalized `destFolder`** — leading slash, no
 * trailing one, i.e. whatever `normalizeDestFolder()` returned for the setting.
 * Nothing here normalizes, so a trailing slash produces `/Decks//.state/...`
 * and the two users of that rule stop agreeing on the path.
 */
namespace flashcards {

constexpr const char* DECK_EXTENSION = ".deck";
constexpr const char* STATE_DIR_LEAF = "/.state";
constexpr const char* STATE_SUFFIX = ".state";

/** The hidden directory holding every deck's state file. Created on first study. */
inline std::string stateDirFor(const std::string& destFolder) { return destFolder + STATE_DIR_LEAF; }

/** `deckLeaf` is the deck's filename WITH its `.deck` extension. */
inline std::string statePathFor(const std::string& destFolder, const std::string& deckLeaf) {
  return destFolder + STATE_DIR_LEAF + "/" + deckLeaf + STATE_SUFFIX;
}

inline std::string deckPathFor(const std::string& destFolder, const std::string& deckLeaf) {
  return destFolder + "/" + deckLeaf;
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
