#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "BmpSanity.h"

namespace {

struct Hdr {
  uint8_t b[bmp_sanity::kHeaderBytes] = {};
};

void put32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

void put16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

// A BITMAPINFOHEADER BMP header; returns the minimum file size it implies.
size_t makeHeader(Hdr& h, int32_t width, int32_t height, uint16_t bpp, uint32_t compression = 0,
                  uint32_t pixelOffset = 0, uint32_t dibSize = 40, uint32_t declaredSize = 0) {
  std::memset(h.b, 0, sizeof(h.b));
  h.b[0] = 'B';
  h.b[1] = 'M';
  if (pixelOffset == 0) pixelOffset = 14 + dibSize + (bpp <= 8 ? (4u << bpp) : 0);
  const int64_t absH = height < 0 ? -static_cast<int64_t>(height) : height;
  const size_t rowBytes = (static_cast<size_t>(width) * bpp + 31) / 32 * 4;
  const size_t total = pixelOffset + rowBytes * static_cast<size_t>(absH);
  put32(h.b + 2, declaredSize);
  put32(h.b + 10, pixelOffset);
  put32(h.b + 14, dibSize);
  put32(h.b + 18, static_cast<uint32_t>(width));
  put32(h.b + 22, static_cast<uint32_t>(height));
  put16(h.b + 26, 1);
  put16(h.b + 28, bpp);
  put32(h.b + 30, compression);
  return total;
}

}  // namespace

TEST(BmpSanity, AcceptsGeneratedOneBitThumbShape) {
  Hdr h;
  const size_t size = makeHeader(h, 240, -400, 1);  // the converters: offset 62, top-down
  put32(h.b + 2, static_cast<uint32_t>(size));
  EXPECT_TRUE(bmp_sanity::isSaneBmp(h.b, size));
}

TEST(BmpSanity, AcceptsVerbatimCompanionOfAnySupportedDepthAndSize) {
  for (const uint16_t bpp : {1, 2, 4, 8, 24, 32}) {
    Hdr h;
    const size_t size = makeHeader(h, 1200, 1600, bpp);  // far outside any thumb box
    EXPECT_TRUE(bmp_sanity::isSaneBmp(h.b, size)) << "bpp=" << bpp;
    EXPECT_TRUE(bmp_sanity::isSaneBmp(h.b, size + 100)) << "trailing bytes, bpp=" << bpp;
  }
}

TEST(BmpSanity, AcceptsTopDownAndBitfields32AndZeroDeclaredSize) {
  Hdr h;
  size_t size = makeHeader(h, 300, -500, 24);
  EXPECT_TRUE(bmp_sanity::isSaneBmp(h.b, size));
  size = makeHeader(h, 300, 500, 32, 3, 14 + 40 + 12);  // BI_BITFIELDS masks after the DIB header
  EXPECT_TRUE(bmp_sanity::isSaneBmp(h.b, size));
}

TEST(BmpSanity, RejectsTruncatedPixelArray) {
  Hdr h;
  const size_t size = makeHeader(h, 600, 800, 24);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size - 1));
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, bmp_sanity::kHeaderBytes));
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, bmp_sanity::kHeaderBytes - 1));
}

TEST(BmpSanity, RejectsWhatTheBitmapReaderRejects) {
  Hdr h;
  size_t size = makeHeader(h, 100, 100, 24);
  h.b[1] = 'X';
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "magic";

  size = makeHeader(h, 100, 100, 16);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "16 bpp";

  size = makeHeader(h, 100, 100, 8, 1);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "RLE8";

  size = makeHeader(h, 100, 100, 24, 3);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "bitfields on 24 bpp";

  size = makeHeader(h, 100, 100, 24, 0, 0, 12);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "OS/2 core header";

  size = makeHeader(h, 100, 100, 24);
  put16(h.b + 26, 2);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "planes";

  size = makeHeader(h, 0, 100, 24);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size + 4096)) << "zero width";

  size = makeHeader(h, 2049, 100, 24);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "wider than the reader's cap";

  size = makeHeader(h, 100, 3073, 1);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size)) << "taller than the reader's cap";

  size = makeHeader(h, 100, 100, 24, 0, 20);
  EXPECT_FALSE(bmp_sanity::isSaneBmp(h.b, size + 4096)) << "pixel data overlapping the headers";
}
