#pragma once
#ifdef CROSSPOINT_RSS_SYNC

#include <string>

/**
 * Canonical destination folder for the RSS mirror.
 *
 * Trims surrounding whitespace, resolves ".." and collapses duplicate/trailing
 * slashes through FsHelpers::normalisePath, and guarantees exactly one leading
 * '/'. Anything that would resolve to the card root — empty, whitespace only,
 * "/", "/.." — falls back to "/RSS": a mirror that DELETES files must never
 * point at the root.
 *
 * Shared rather than private to the settings editor because that editor is not
 * the only writer. rssDestFolder is a category-less SettingInfo::String, so the
 * web settings API and a hand-edited settings.json store whatever they are
 * given; RssSyncActivity therefore normalises again at the point of use.
 */
std::string normalizeRssFolder(const std::string& value);

#endif  // CROSSPOINT_RSS_SYNC
