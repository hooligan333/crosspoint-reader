#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  // boundOutputToTarget emits only the centered targetWidth×targetHeight window of the
  // scaled image (the *WithSize thumbnail APIs promise "max" dimensions); without it,
  // crop mode writes the full fill-scaled image and consumers crop/letterbox at draw time
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool boundOutputToTarget = false);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
