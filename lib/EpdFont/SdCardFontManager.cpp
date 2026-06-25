#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

#include <new>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Select the physical point size closest to the built-in reader sizes. Some
  // CJK font packs only ship larger sizes, so ordinal selection can make
  // MEDIUM load 18pt+ and produce oversized pages on small devices.
  const SdCardFontFileInfo* selected = family.findClosestReaderSize(fontSizeEnum);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  if (!loadFile(family, selected->path.c_str(), selected->pointSize, renderer)) return false;

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
  return true;
}

bool SdCardFontManager::loadAllSizes(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }
  if (family.files.empty()) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  bool loadedAny = false;
  for (const auto& file : family.files) {
    loadedAny = loadFile(family, file.path.c_str(), file.pointSize, renderer) || loadedAny;
  }
  if (!loadedAny) return false;

  loadedFamilyName_ = family.name;
  loadedPointSize_ = loaded_.front().size;
  return true;
}

bool SdCardFontManager::loadFile(const SdCardFontFamilyInfo& family, const char* path, uint8_t pointSize,
                                 GfxRenderer& renderer) {
  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", path);
    return false;
  }

  if (!font->load(path)) {
    LOG_ERR("SDMGR", "Failed to load %s", path);
    delete font;
    return false;
  }

  int fontId = computeFontId(font->contentHash(), family.name.c_str(), pointSize);
  // Guard against collision with built-in font IDs (astronomically unlikely
  // with FNV-1a hashes, but provides a safety net)
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, path);
    delete font;
    return false;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u", path, pointSize, fontId, font->styleCount());

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);
  return true;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  return loaded_.front().fontId;
}

int SdCardFontManager::getFontIdForPointSize(const std::string& familyName, uint8_t pointSize) const {
  if (familyName != loadedFamilyName_) return 0;
  for (const auto& lf : loaded_) {
    if (lf.size == pointSize) return lf.fontId;
  }
  return 0;
}
