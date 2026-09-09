#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <cstdint>

/**
 * The one sort the flashcard layer needs, and the search that goes with it.
 *
 * Two callers, both on cold paths and both over card keys: the deck reader's
 * duplicate-key check at open (which must not be O(n²) over a 40000-card deck)
 * and the re-download merge, which sorts the old state's keys so the new deck's
 * index can be walked against it with a binary search (FLASHCARD_SPEC.md §3).
 *
 * Heapsort rather than `std::sort`: it is in-place, non-recursive (no stack
 * growth on a 40000-element array), has no O(n²) worst case to reason about,
 * and costs a few hundred bytes of IROM instead of an introsort instantiation.
 * The order is not stable, which neither caller needs — keys are unique in a
 * deck that passes validation, and the merge's payload rides along in lockstep.
 */
namespace flashcards {

/** Returned by findKey() when the key is not present. */
constexpr uint32_t KEY_NOT_FOUND = 0xFFFFFFFFu;

/**
 * Sorts `keys[0..count)` ascending. When `payload` is non-null it holds one
 * u16 per key (a card ordinal) and is permuted in lockstep, so the caller can
 * recover which record a key came from after the sort.
 */
void sortKeys(uint64_t* keys, uint16_t* payload, uint32_t count);

/** Index of `key` in a sorted array, or KEY_NOT_FOUND. */
uint32_t findKey(const uint64_t* keys, uint32_t count, uint64_t key);

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
