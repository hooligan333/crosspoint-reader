#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>

// Heap figures for the app's allocation gates (build/prewarm pauses, decoder
// admission checks, retention-cache limits). The gates protect malloc-visible
// INTERNAL RAM — the pool whose exhaustion destabilizes the system. Note that
// on SPIRAM_USE_MALLOC builds, allocations above CONFIG_SPIRAM_MALLOC_
// ALWAYSINTERNAL are served PSRAM-first, so an internal-pressure gate is
// deliberately conservative for those: it may refuse an operation PSRAM could
// have absorbed. That is the intended trade — every refusal path is graceful,
// and admitting work while internal RAM is scarce is the failure mode these
// gates exist to prevent.
//
// Default: ESP.getFreeHeap()/getMaxAllocHeap(), which report MALLOC_CAP_INTERNAL —
// correct on internal-RAM-only boards and byte-identical to the historical
// behavior everywhere.
//
// CROSSPOINT_PSRAM_HEAP_GAUGE (opt-in, PSRAM boards with SPIRAM_USE_MALLOC):
// measure MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL instead. Bare INTERNAL
// over-counts there: CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL re-registers its
// pool with {DMA|INTERNAL} but no DEFAULT cap, so plain malloc can never touch
// it, yet ESP.getFreeHeap() counts it — with a 32 KB reserve a 32 KB gate floor
// mathematically cannot trip until malloc-visible internal RAM is fully
// exhausted, which is exactly the state the gates exist to prevent. Requiring
// DEFAULT excludes that phantom pool; requiring INTERNAL keeps the (huge,
// default-capable) PSRAM heap from masking internal pressure.
//
// Diagnostics deliberately stay on the raw ESP calls: the [MEM] prints and the
// web-server stats report the whole internal pool, not the gate's view of it.
#ifdef CROSSPOINT_PSRAM_HEAP_GAUGE
static inline size_t gateFreeHeap() {
  return heap_caps_get_free_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}
static inline size_t gateMaxAllocHeap() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}
#else
static inline size_t gateFreeHeap() { return ESP.getFreeHeap(); }
static inline size_t gateMaxAllocHeap() { return ESP.getMaxAllocHeap(); }
#endif
