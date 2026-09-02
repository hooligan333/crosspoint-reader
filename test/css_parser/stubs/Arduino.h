#pragma once

#include <cstdint>

struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
  // HalHeapGauge.h defines gateMaxAllocHeap() alongside gateFreeHeap(); both
  // bodies are compiled even though the parser only calls the first.
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
