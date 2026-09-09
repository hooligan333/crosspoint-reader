#include "CardImage.h"

#ifdef CROSSPOINT_FLASHCARDS

#include <Arduino.h>
#include <Epub/converters/DirectPixelWriter.h>
#include <Epub/converters/DitherUtils.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <GfxRenderer.h>
#include <HalHeapGauge.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>

namespace flashcards {
namespace {

// The JPEGDEC object is ~17 KB of internal decode buffers (Huffman tables, the
// MCU scratch and a 2 KB file buffer). The book path's admission gate, reused
// verbatim so both decoders refuse under the same pressure — on the C3 the free
// heap during a study session is routinely near 50 KB and fragmented, and the
// point of a gate is that the refusal is graceful.
constexpr size_t JPEG_DECODER_APPROX_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP_FOR_JPEG = JPEG_DECODER_APPROX_SIZE + 16 * 1024;

/**
 * What the draw callback needs, handed through JPEGDEC's user pointer rather
 * than a file-scope global — two decodes can never overlap here (one image at a
 * time, on the render task), but a global would make that a promise instead of
 * a fact.
 */
struct CardImageContext {
  GfxRenderer* renderer;
  int originX;    // logical X of the image's left edge
  int originY;    // logical Y of its top edge
  int clipWidth;  // columns to draw, from originX
  int clipHeight;
  uint32_t lastYieldMs;
};

/**
 * JPEGDEC hands back one decoded MCU block at a time, densely packed 8-bit grey
 * (stride `iWidth`, valid columns `iWidthUsed`). There is no scaling path here
 * at all: §3.7 pre-sizes every card image to at most 440 px and the device draws
 * it as delivered, so this is the book converter's 1:1 fast path with the
 * fixed-point machinery around it removed, plus a clip to the box the pager
 * reserved.
 */
int cardImageDrawCallback(JPEGDRAW* pDraw) {
  CardImageContext* const ctx = reinterpret_cast<CardImageContext*>(pDraw->pUser);
  if (ctx == nullptr || ctx->renderer == nullptr) return 0;
  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  const uint8_t* const pixels = reinterpret_cast<const uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int blockWidth = pDraw->iWidthUsed;
  const int blockHeight = pDraw->iHeight;
  if (pixels == nullptr || stride <= 0 || blockWidth <= 0 || blockHeight <= 0) return 1;

  // Clip in SOURCE coordinates: the block's own rows/columns, narrowed to the
  // part of the image that fits the reserved box. An image taller than its box
  // (a page-height cap) is cut off at the bottom rather than squashed, and one
  // wider than the column is cut off at the right edge.
  //
  // BOTH clips are LOAD-BEARING, not cosmetic (FLASHCARD_SPEC.md §2.2, I2 pins).
  // `DirectPixelWriter::writePixel()` is documented "no bounds checking — caller
  // guarantees coordinates are valid", and the device NEVER SCALES: the only
  // thing keeping an over-wide image inside the framebuffer is `colEnd` here.
  // The converter's 440 px cap makes an over-wide image unlikely rather than
  // impossible — the deck reader takes 440 on a narrower panel, and the column
  // is the body width, not the screen width — so this is the bound, not a
  // second opinion about one. Deleting either line is a framebuffer overrun.
  int rowEnd = blockHeight;
  if (pDraw->y + rowEnd > ctx->clipHeight) rowEnd = ctx->clipHeight - pDraw->y;
  int colEnd = blockWidth;
  if (pDraw->x + colEnd > ctx->clipWidth) colEnd = ctx->clipWidth - pDraw->x;
  if (rowEnd <= 0 || colEnd <= 0) return 1;

  DirectPixelWriter writer;
  writer.init(*ctx->renderer);
  for (int row = 0; row < rowEnd; row++) {
    const int outY = ctx->originY + pDraw->y + row;
    writer.beginRow(outY);
    const uint8_t* const source = pixels + static_cast<size_t>(row) * static_cast<size_t>(stride);
    for (int col = 0; col < colEnd; col++) {
      const int outX = ctx->originX + pDraw->x + col;
      writer.writePixel(outX, applyBayerDither4Level(source[col], outX, outY));
    }
  }
  return 1;
}

}  // namespace

CardImageDecoder::CardImageDecoder() = default;
// Out of line, and it has to be: `decoder` is a unique_ptr to a type the header
// only forward-declares, so the deleter can only be instantiated here.
CardImageDecoder::~CardImageDecoder() = default;

bool CardImageDecoder::acquire() {
  if (decoder) return true;
  if (gateFreeHeap() < MIN_FREE_HEAP_FOR_JPEG) {
    LOG_INF("DECK", "Card images off: %u B free, need %u", static_cast<unsigned>(gateFreeHeap()),
            static_cast<unsigned>(MIN_FREE_HEAP_FOR_JPEG));
    return false;
  }
  decoder = makeUniqueNoThrow<JPEGDEC>();
  if (!decoder) {
    LOG_ERR("DECK", "Card image decoder alloc failed (~%u B)", static_cast<unsigned>(JPEG_DECODER_APPROX_SIZE));
    return false;
  }
  return true;
}

void CardImageDecoder::release() { decoder.reset(); }

bool CardImageDecoder::draw(GfxRenderer& renderer, uint8_t* const jpeg, const size_t bytes, const int x, const int y,
                            const int maxWidth, const int maxHeight) {
  if (!decoder || jpeg == nullptr || bytes == 0 || maxWidth <= 0 || maxHeight <= 0) return false;

  // lastYieldMs starts at the decode's start time, as the helper's contract asks
  // (ImageToFramebufferDecoder.h): a 0 there would spend a yield on the very
  // first MCU block of every image.
  CardImageContext context{&renderer, x, y, maxWidth, maxHeight, millis()};
  // The bytes were already sniffed as a baseline single-component JPEG of the
  // table's dimensions (DeckFile::loadImage), so openRAM here is opening
  // something whose shape is known; a failure at this point is a decoder that
  // disagrees, and the caller's placeholder is the answer either way.
  if (decoder->openRAM(jpeg, static_cast<int>(bytes), cardImageDrawCallback) == 0) {
    LOG_ERR("DECK", "Card image open failed: %d", decoder->getLastError());
    return false;
  }
  decoder->setPixelType(EIGHT_BIT_GRAYSCALE);
  decoder->setUserPointer(&context);
  const bool ok = decoder->decode(0, 0, 0) != 0;
  decoder->close();
  if (!ok) LOG_ERR("DECK", "Card image decode failed: %d", decoder->getLastError());
  return ok;
}

void CardImageDecoder::drawPlaceholder(GfxRenderer& renderer, const int x, const int y, const int width,
                                       const int height) {
  if (width <= 0 || height <= 0) return;
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
}

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
