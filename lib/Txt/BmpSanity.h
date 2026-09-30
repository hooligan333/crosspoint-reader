#pragma once

#include <cstddef>
#include <cstdint>

// Structural sniff for a BMP that was copied verbatim (a TXT/MD companion cover),
// so it may be any size or depth -- unlike the fork's generated EPUB thumbs, whose
// exact 1-bit, box-bounded shape Epub::hasUsableThumbBmp checks. Mirrors what
// Bitmap::parseHeaders will accept, plus a truncation check, so a file passing
// here is one the renderer can actually open. Pure over the first
// kHeaderBytes of the file and its on-disk size, so it is host-testable.
namespace bmp_sanity {

constexpr size_t kHeaderBytes = 34;  // file header (14) + DIB fields through biCompression

inline uint32_t readLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t readLE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// `h` must hold at least kHeaderBytes bytes.
inline bool isSaneBmp(const uint8_t* h, const size_t fileSize) {
  if (fileSize < kHeaderBytes || h[0] != 'B' || h[1] != 'M') return false;
  const uint32_t pixelOffset = readLE32(h + 10);
  const uint32_t dibSize = readLE32(h + 14);
  const auto width = static_cast<int32_t>(readLE32(h + 18));
  const auto rawHeight = static_cast<int32_t>(readLE32(h + 22));
  const uint16_t planes = readLE16(h + 26);
  const uint16_t bpp = readLE16(h + 28);
  const uint32_t compression = readLE32(h + 30);

  // Same acceptance as Bitmap::parseHeaders.
  if (dibSize < 40 || planes != 1) return false;
  if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return false;
  if (!(compression == 0 || (bpp == 32 && compression == 3))) return false;
  const int64_t height = rawHeight < 0 ? -static_cast<int64_t>(rawHeight) : rawHeight;  // negative = top-down
  if (width < 1 || width > 2048 || height < 1 || height > 3072) return false;

  // The pixel array must start after the headers and fit inside the file: a copy
  // cut short by a reset or a full card fails here. bfSize is not trusted (some
  // writers leave it 0); the uncompressed geometry gives the real minimum.
  if (pixelOffset < 14 + static_cast<uint64_t>(dibSize)) return false;
  const uint64_t rowBytes = (static_cast<uint64_t>(width) * bpp + 31) / 32 * 4;
  return static_cast<uint64_t>(fileSize) >= pixelOffset + rowBytes * static_cast<uint64_t>(height);
}

}  // namespace bmp_sanity
