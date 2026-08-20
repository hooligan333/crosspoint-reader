#pragma once

// Host stub for the IDF heap-capabilities API. lib/hal/HalHeapGauge.h includes
// it unconditionally and SdCardFont.cpp includes the gauge, so the host test
// needs a definition to compile against. Values are the IDF's. The gauge only
// calls these under CROSSPOINT_PSRAM_HEAP_GAUGE, which host builds never set;
// the suite's heap answers come from its ESP stub (HalStorage.h).

#include <cstddef>
#include <cstdint>

constexpr uint32_t MALLOC_CAP_INTERNAL = 1 << 11;
constexpr uint32_t MALLOC_CAP_DEFAULT = 1 << 12;

inline size_t heap_caps_get_free_size(uint32_t) { return SIZE_MAX; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return SIZE_MAX; }
