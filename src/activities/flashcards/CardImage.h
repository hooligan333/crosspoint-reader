#pragma once
#ifdef CROSSPOINT_FLASHCARDS

#include <cstddef>
#include <cstdint>
#include <memory>

class GfxRenderer;
class JPEGDEC;

/**
 * The study screen's card-image plumbing: one encoded JPEG in RAM, drawn into
 * the framebuffer at its delivered size (FLASHCARD_SPEC.md §2.2).
 *
 * This is deliberately THIN. The firmware already links a JPEG decoder for book
 * images — bitbank2/JPEGDEC, pinned in `[base]`'s lib_deps and therefore present
 * in every environment, C3 and S3 alike — so nothing here vendors a codec or
 * adds one to the image. What it does not reuse is
 * `Epub/converters/JpegToFramebufferConverter`, and for one reason: that class
 * decodes from a FILE PATH, and a card image is a byte range inside the middle
 * of a `.deck` file. Its scaling, `.pxc` pixel cache and dimension probing are
 * all things this screen must not do anyway — §3.7 pre-sizes every image to
 * <= 440 px and the device "never scales, it draws as delivered". So the decode
 * runs from RAM through `JPEGDEC::openRAM()`, and the two header-only pieces of
 * that pipeline that ARE the right thing to share — `DirectPixelWriter` (the
 * orientation/render-mode framebuffer writer) and `DitherUtils` (the 4-level
 * Bayer quantizer that maps 8-bit grey onto this panel) — are used verbatim.
 *
 * Memory. The `JPEGDEC` object is ~17 KB of internal decode buffers, so it is
 * acquired ONCE per study session rather than per image, and only for a deck
 * that actually has pictures: `acquire()` gates on free heap first and reports
 * failure instead of aborting, and a session that cannot have it simply draws
 * placeholder boxes. Nothing in the render path allocates. The encoded bytes
 * live in a buffer the caller owns for the same reason — one buffer, sized to
 * the deck's largest image at session start, reused by every image drawn.
 */
namespace flashcards {

class CardImageDecoder {
 public:
  CardImageDecoder();
  ~CardImageDecoder();
  CardImageDecoder(const CardImageDecoder&) = delete;
  CardImageDecoder& operator=(const CardImageDecoder&) = delete;

  /**
   * Claims the ~17 KB decoder, gated on free heap. False leaves the object
   * unusable but valid — every caller draws a placeholder and studies on.
   */
  bool acquire();
  void release();
  bool ready() const { return decoder != nullptr; }

  /**
   * Decodes `bytes` of baseline grayscale JPEG and draws it with its top-left at
   * (`x`, `y`), clipped to `maxWidth` x `maxHeight`. Never scales.
   *
   * False on a decoder that was never acquired, a refused open, or a decode that
   * failed part way — the caller draws a placeholder over whatever landed.
   */
  bool draw(GfxRenderer& renderer, uint8_t* jpeg, size_t bytes, int x, int y, int maxWidth, int maxHeight);

  /**
   * The outlined box that stands in for an image this session could not draw —
   * the same shape ImageBlock::renderPlaceholder() uses in the reader, so a
   * failed picture looks the same wherever the user meets one.
   */
  static void drawPlaceholder(GfxRenderer& renderer, int x, int y, int width, int height);

 private:
  std::unique_ptr<JPEGDEC> decoder;
};

}  // namespace flashcards

#endif  // CROSSPOINT_FLASHCARDS
