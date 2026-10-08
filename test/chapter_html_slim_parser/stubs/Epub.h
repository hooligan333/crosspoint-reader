#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Epub {
 public:
  static inline bool enableGifProbeForTest = false;
  std::vector<uint8_t> imageBytes;
  mutable size_t headerProbes = 0;
  mutable size_t fullExtractions = 0;
  // With no imageBytes every read fails with no bytes: an image header probe
  // finds nothing and a whole-image extraction fails. Every attempt is counted
  // in `reads` so a test can see it (the lending-gate tests).
  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output& output, size_t, bool allowEarlyStop = false) const {
    ++reads;
    if (imageBytes.empty()) return false;
    if (allowEarlyStop)
      ++headerProbes;
    else
      ++fullExtractions;
    const size_t written = output.write(imageBytes.data(), imageBytes.size());
    return written == imageBytes.size() || allowEarlyStop;
  }
  mutable int reads = 0;
};
