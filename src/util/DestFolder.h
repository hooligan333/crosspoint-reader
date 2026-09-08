#pragma once
#if defined(CROSSPOINT_RSS_SYNC) || defined(CROSSPOINT_FLASHCARDS)

#include <string>

/**
 * Canonical destination folder for a feed mirror (RSS enclosures, deck files).
 *
 * Trims surrounding whitespace, drops "." components, resolves ".." and
 * collapses duplicate/trailing slashes through FsHelpers::normalisePath, and
 * guarantees exactly one leading '/'. Anything that would resolve to the card
 * root — empty, whitespace only, "/", "/..", "/.", "/./.", "/a/../." — falls
 * back to `fallback`: a mirror that DELETES files must never point at the root.
 *
 * Shared rather than private to each settings editor because those editors are
 * not the only writers. Both destination folders are category-less
 * SettingInfo::String entries, so the web settings API and a hand-edited
 * settings.json store whatever they are given; the sync screens therefore
 * normalise again at the point of use.
 */
std::string normalizeDestFolder(const std::string& value, const char* fallback);

// The per-feature fallbacks, kept next to the normaliser so the settings
// default, the settings editor and the sync screen cannot drift apart.
#ifdef CROSSPOINT_RSS_SYNC
constexpr const char* RSS_DEFAULT_FOLDER = "/RSS";
#endif
#ifdef CROSSPOINT_FLASHCARDS
constexpr const char* DECK_DEFAULT_FOLDER = "/Decks";
#endif

#endif  // CROSSPOINT_RSS_SYNC || CROSSPOINT_FLASHCARDS
