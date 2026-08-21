#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  // Pass seamless=true on any path where the panel already shows the
  // content it should after begin() returns (silent reboot's popup,
  // sleep-wake with a restored buffer). Skips the wakeup-gated
  // requestResync() and defuses the SDK's X3 _x3InitialFullSyncsRemaining
  // counter; otherwise the first two paints get promoted to FULL
  // (~770ms each on X3).
  void begin(bool seamless = false);

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  // Non-blocking refresh (shadow-free): starts the panel waveform and returns
  // while the panel refreshes on its own. The framebuffer must stay untouched
  // until waitRefreshComplete(), and the caller must rebuild the differential
  // baseline before the next differential update (the tiled grayscale cleanup
  // does). Panels without deferral fall back to a blocking refresh.
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH);
  // Block until a pending deferred refresh completes (no-op when none is).
  void waitRefreshComplete();
  // True when displayBufferAsync() genuinely overlaps (panel driver defers);
  // false where it falls back to a blocking refresh.
  bool supportsAsyncRefresh() const;
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Output polarity. The framebuffer remains in normal polarity; inversion is
  // applied by the display driver while sending it to the panel.
  void setInverted(bool inverted);
  bool toggleInverted();
  bool isInverted() const;

  // Power management
  void deepSleep();

#ifdef FREEINK_UC8179_RAIL_POWEROFF
  // Panel analog rails (booster / VGH / VGL / VSH / VSL / VCOM). On the UC8179
  // they latch ON at the first refresh and stay on for the whole reading
  // session, because nothing on this device ever passes turnOff.
  //
  // controllerIdle(): park them once display work is quiescent. Idempotent and
  // free (no SPI) when they are already down. Call only from the main loop with
  // the render lock held — it talks to the panel.
  // beginDisplayWork(): command them back up the moment a render is queued, so
  // the ~127 ms ramp overlaps CPU-side composition instead of stalling the
  // refresh. Returns immediately when already powered. Called from
  // ActivityManager, which owns that edge for every render on the device.
  void controllerIdle();
  void beginDisplayWork();
#endif

#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
  // Passthrough to EpdBus::setBusyWaitSliceHook (see its contract). Exposed
  // through the HAL so main.cpp can force the SDK's level-polled refresh wait
  // under automatic light sleep without reaching into SDK classes directly.
  void setBusyWaitSliceHook(bool (*sliceHook)(int8_t busyPin, uint8_t busyLevel));
#endif

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Lend the framebuffer's ~48 KB STORAGE to a memory-hungry phase (chapter
  // builds) without freeing it: the allocation never moves, so repeated loans
  // cannot fragment the heap (free+realloc measurably did). No display calls
  // between lend and return; the panel keeps its last refreshed image. The
  // buffer comes back white — redraw fully. Returns nullptr if already lent.
  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut);
  void returnFrameBufferStorage();

  // X3 grayscale preconditioning (OEM "AA-pre-BW(mid)" settle pass), windowed
  // to the gray region in physical panel coordinates (no-arg = full frame).
  // Call after the BW base frame is displayed and before the grayscale planes
  // are written; no-op on X4. See EInkDisplay::preconditionGrayscale.
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows. On X3, HALF fallback first requests a resync to match
  // displayBuffer(HALF); FAST fallback keeps the OEM differential base waveform
  // ("AA-pre-BW(mid)"). Other panels display normally with `fallback` mode.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);

  // Absolute 4-level grayscale in ONE panel activation, using the controller's
  // OEM "factory" waveform: it paints a finished 4-gray frame from any prior
  // screen state, so no B/W base activation has to precede it. The planes must
  // have been written with the absolute (darkness-code) encoding, not the
  // refinement one. supportsFactoryGrayscale() gates the path.
  void displayGrayBufferFactory(bool turnOffScreen = false);
  bool supportsFactoryGrayscale() const;

  // True when a FAST_REFRESH issued right after a grayscale page fully re-drives
  // the gray charge that pass left on the panel (UC8179 substitutes the OEM
  // gray-exit transition for the plain differential update). Callers that would
  // otherwise force a clean refresh on the page after grayscale content can keep
  // their ordinary refresh cadence. See PanelDriver::fastAfterGrayscaleSafe().
  bool fastAfterGrayscaleSafe() const;

#ifdef FREEINK_UC8179_DOUBLE_GRAY_PRE
  // One-shot hint for the NEXT base refresh: spend extra panel time equalizing
  // black depth across the frame, so the page being replaced cannot ghost
  // through the new page's grays. Call immediately before the base display of a
  // page with large gray areas (an image page); text pages leave it unset and
  // keep the cheaper single-pass transition. Panels without such a pass ignore
  // it. See PanelDriver::requestDeepGrayEqualize().
  void requestDeepGrayEqualize();
#endif

  // Tiled grayscale: stream one band of a plane (lsbPlane selects LSB/MSB RAM)
  // straight to the controller; supportsStripGrayscale() gates the path. See
  // EInkDisplay::writeGrayscalePlaneStrip.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);
  bool supportsStripGrayscale() const;

  // True when displayGrayscaleBase() defers the base activation so the gray
  // planes join it in a single waveform (Paper Mono). Callers should then route
  // the base of a grayscale page through displayGrayscaleBase() instead of
  // displayBuffer(): a separate B/W refresh first makes the gray pass re-drive
  // the whole text body (a visible flash).
  bool combinesGrayscaleBase() const;

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  EInkDisplay einkDisplay;
};

extern HalDisplay display;
