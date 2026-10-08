#pragma once

#include <cstddef>
#include <string>

class Epub {
 public:
  // Every read fails with no bytes: an image header probe finds nothing and a
  // whole-image extraction fails. Counted so a test can see each attempt.
  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output&, size_t, bool = false) const {
    ++reads;
    return false;
  }
  mutable int reads = 0;
};
