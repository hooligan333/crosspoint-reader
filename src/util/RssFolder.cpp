#include "RssFolder.h"

#ifdef CROSSPOINT_RSS_SYNC

#include <FsHelpers.h>

namespace {
constexpr const char* DEFAULT_FOLDER = "/RSS";
}  // namespace

std::string normalizeRssFolder(const std::string& value) {
  const size_t first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return DEFAULT_FOLDER;  // empty or whitespace only
  const size_t last = value.find_last_not_of(" \t");

  // normalisePath() pops a component per "..", drops empty ones (so "//a/"
  // becomes "a"), and returns "" for anything that walks out to the root. It
  // emits no leading '/', which is re-applied below.
  const std::string normalised = FsHelpers::normalisePath(value.substr(first, last - first + 1));
  if (normalised.empty()) return DEFAULT_FOLDER;
  return "/" + normalised;
}

#endif  // CROSSPOINT_RSS_SYNC
