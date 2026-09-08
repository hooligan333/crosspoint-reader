#include "DestFolder.h"

#if defined(CROSSPOINT_RSS_SYNC) || defined(CROSSPOINT_FLASHCARDS)

#include <FsHelpers.h>

#include <string_view>

std::string normalizeDestFolder(const std::string& value, const char* fallback) {
  const size_t first = value.find_first_not_of(" \t");
  if (first == std::string::npos) return fallback;  // empty or whitespace only
  const size_t last = value.find_last_not_of(" \t");
  const std::string trimmed = value.substr(first, last - first + 1);

  // "." components are dropped HERE rather than in FsHelpers::normalisePath,
  // which treats "." as an ordinary directory name — correct for its other
  // callers (ePUB hrefs, where the behaviour is long settled) but wrong here:
  // "/." would survive as a non-empty path and aim a folder that DELETES files
  // straight at the card root. Doing it before normalisePath also keeps ".."
  // honest, since "/a/./.." must walk out to the root rather than pop the "."
  // and leave "a".
  std::string filtered;
  filtered.reserve(trimmed.size());
  size_t start = 0;
  for (size_t i = 0; i <= trimmed.size(); i++) {
    if (i != trimmed.size() && trimmed[i] != '/') continue;
    const std::string_view component(trimmed.data() + start, i - start);
    if (!component.empty() && component != ".") {
      if (!filtered.empty()) filtered += '/';
      filtered.append(component);
    }
    start = i + 1;
  }

  // normalisePath() pops a component per "..", drops empty ones (so "//a/"
  // becomes "a"), and returns "" for anything that walks out to the root. It
  // emits no leading '/', which is re-applied below.
  const std::string normalised = FsHelpers::normalisePath(filtered);
  if (normalised.empty()) return fallback;
  return "/" + normalised;
}

#endif  // CROSSPOINT_RSS_SYNC || CROSSPOINT_FLASHCARDS
