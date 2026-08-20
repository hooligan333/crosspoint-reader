#pragma once

// Host stub for the IDF heap-capabilities API. lib/hal/HalHeapGauge.h includes
// it unconditionally, and CssParser.cpp uses that gauge for the low-memory
// cache-retry gate, so the host test needs a definition to compile against.
// Values are the IDF's; the sizes are the "plenty of room" answers the parser's
// gate expects on a host run.

#include <cstddef>
#include <cstdint>

constexpr uint32_t MALLOC_CAP_INTERNAL = 1 << 11;
constexpr uint32_t MALLOC_CAP_DEFAULT = 1 << 12;

inline size_t heap_caps_get_free_size(uint32_t) { return SIZE_MAX; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return SIZE_MAX; }
