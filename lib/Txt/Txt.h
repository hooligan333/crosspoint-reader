#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <string_view>

class BookMetadataCache;

class Txt {
 public:
  static bool isTxtOrMd(std::string_view path);
  static bool validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize);
  static void invalidateCache(const std::string& cachePath);
  static bool streamTxtToHtml(const std::string& filepath, Print& out);
  static std::string findCompanionCoverImage(const std::string& filepath);
  static bool convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight = 0,
                                     bool cropped = false, bool originalThresholds = false);
  // Reuse test for a TXT/MD thumb: true for the empty "no usable cover" marker or
  // a structurally sane BMP of any size/depth (a companion BMP is copied verbatim,
  // so Epub::hasUsableThumbBmp's generated-shape check does not apply).
  static bool hasUsableThumbBmp(const std::string& thumbPath);
  static bool buildTxtCache(const std::string& filepath, const std::string& cachePath,
                            std::unique_ptr<BookMetadataCache>& bookMetadataCache);
};
