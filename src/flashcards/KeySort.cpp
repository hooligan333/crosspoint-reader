#include "KeySort.h"

#ifdef CROSSPOINT_FLASHCARDS

namespace flashcards {
namespace {

void swapAt(uint64_t* keys, uint16_t* payload, uint32_t a, uint32_t b) {
  const uint64_t key = keys[a];
  keys[a] = keys[b];
  keys[b] = key;
  if (payload != nullptr) {
    const uint16_t value = payload[a];
    payload[a] = payload[b];
    payload[b] = value;
  }
}

/** Restores the max-heap property at `root` over `[0, size)`. */
void siftDown(uint64_t* keys, uint16_t* payload, uint32_t root, uint32_t size) {
  while (true) {
    const uint32_t left = 2 * root + 1;
    if (left >= size) return;
    uint32_t largest = left;
    const uint32_t right = left + 1;
    if (right < size && keys[right] > keys[left]) largest = right;
    if (keys[root] >= keys[largest]) return;
    swapAt(keys, payload, root, largest);
    root = largest;
  }
}

}  // namespace

void sortKeys(uint64_t* keys, uint16_t* payload, uint32_t count) {
  if (count < 2) return;
  // Build the max-heap bottom-up, then pull the largest to the back one at a
  // time: the array is left ascending, in place, with no recursion.
  for (uint32_t node = count / 2; node > 0; node--) {
    siftDown(keys, payload, node - 1, count);
  }
  for (uint32_t end = count; end > 1; end--) {
    swapAt(keys, payload, 0, end - 1);
    siftDown(keys, payload, 0, end - 1);
  }
}

uint32_t findKey(const uint64_t* keys, uint32_t count, uint64_t key) {
  uint32_t low = 0;
  uint32_t high = count;  // exclusive
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    if (keys[mid] == key) return mid;
    if (keys[mid] < key) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return KEY_NOT_FOUND;
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
