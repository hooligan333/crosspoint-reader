#pragma once
// Host stand-in for lib/hal/HalHeapGauge.h.
//
// The real header is two inline calls into esp_heap_caps, so there is nothing
// to emulate — but the allocation GATES those two calls feed are the whole
// point of the CROSSPOINT_FLASHCARDS_C3 paths, and a gate that is never
// observed refusing is a gate nobody has tested. So the figure is a settable
// value: hostHeapGauge().maxAlloc = 4096 makes the next merge or session build
// take its out-of-memory branch, deterministically, with no real pressure on
// the host's heap and no way for a leak here to make an unrelated test flaky.
//
// Only compiled into the C3 configuration: the default build never includes
// this header, because the code that reaches for it is behind the flag.

#include <cstddef>
#include <cstdint>

struct HostHeapGauge {
  // Effectively unbounded by default, so every test that does not care about
  // the gates sees exactly the behaviour the device sees with plenty of room.
  size_t maxAlloc = SIZE_MAX;
  size_t freeHeap = SIZE_MAX;

  void reset() {
    maxAlloc = SIZE_MAX;
    freeHeap = SIZE_MAX;
  }
};

inline HostHeapGauge& hostHeapGauge() {
  static HostHeapGauge gauge;
  return gauge;
}

static inline size_t gateFreeHeap() { return hostHeapGauge().freeHeap; }
static inline size_t gateMaxAllocHeap() { return hostHeapGauge().maxAlloc; }
