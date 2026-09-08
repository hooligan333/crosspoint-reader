#pragma once
// Minimal stand-in for the Arduino String that FsHelpers.h names in its
// convenience overloads. The functions under test here take std::string /
// std::string_view, so this only has to satisfy the compiler.

#include <cstddef>
#include <string>

class String {
 public:
  String() = default;
  explicit String(const char* value) : value_(value) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }

 private:
  std::string value_;
};
