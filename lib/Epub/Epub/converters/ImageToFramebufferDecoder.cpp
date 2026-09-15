#include "ImageToFramebufferDecoder.h"

#include <Arduino.h>
#include <Logging.h>

#include <climits>
#include <cstdint>

namespace {
// newlib-nano's printf knows only the h/l/L length modifiers: it prints "lld"
// as literal text AND leaves the argument unconsumed, which shifts every later
// one. These are diagnostics for an image that is already being rejected, so
// saturate into long and print "%ld". See the CONFIG_LIBC_NEWLIB_NANO_FORMAT
// entry in platformio.ini.
long clampToLong(const int64_t v) {
  if (v > static_cast<int64_t>(LONG_MAX)) return LONG_MAX;
  if (v < static_cast<int64_t>(LONG_MIN)) return LONG_MIN;
  return static_cast<long>(v);
}
}  // namespace

#ifdef CROSSPOINT_BG_IMAGE_DECODE
#include <atomic>

namespace {
std::atomic<bool> decodeAbort{false};
}  // namespace

void ImageToFramebufferDecoder::requestAbort(const bool abort) { decodeAbort.store(abort, std::memory_order_release); }

bool ImageToFramebufferDecoder::abortRequested() { return decodeAbort.load(std::memory_order_acquire); }
#endif

bool ImageToFramebufferDecoder::validateAndStoreDimensions(const int64_t width, const int64_t height,
                                                           ImageDimensions& out, const char* format) {
  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid %s dimensions: %ldx%ld", format, clampToLong(width), clampToLong(height));
    return false;
  }
  if (width > MAX_SOURCE_DIMENSION || height > MAX_SOURCE_DIMENSION) {
    LOG_ERR("IMG", "%s dimensions exceed supported limit: %ldx%ld (max %ld per dimension)", format,
            clampToLong(width), clampToLong(height), clampToLong(MAX_SOURCE_DIMENSION));
    return false;
  }

  const int64_t pixels = width * height;
  if (pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "%s too large (%ldx%ld = %ld pixels), max supported: %ld pixels", format, clampToLong(width),
            clampToLong(height), clampToLong(pixels), clampToLong(MAX_SOURCE_PIXELS));
    return false;
  }

  out.width = static_cast<int16_t>(width);
  out.height = static_cast<int16_t>(height);
  return true;
}

void ImageToFramebufferDecoder::yieldDuringDecode(uint32_t& lastYieldMs) {
  const uint32_t now = millis();
  if (now - lastYieldMs >= 250) {
    lastYieldMs = now;
    vTaskDelay(1);
  }
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}
