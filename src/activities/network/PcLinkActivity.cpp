#include "PcLinkActivity.h"

#ifdef CROSSPOINT_PC_LINK

#ifndef ENABLE_SERIAL_LOG
#error "CROSSPOINT_PC_LINK needs ENABLE_SERIAL_LOG: the protocol runs on the console Serial"
#endif

#include <Arduino.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <hal/usb_serial_jtag_ll.h>

#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;
using pclink::MsgType;
using pclink::Status;

namespace {

// Logical buttons forwarded as INPUT, indexed by pclink::ButtonCode.
constexpr MappedInputManager::Button FORWARDED_BUTTONS[pclink::BUTTON_COUNT] = {
    MappedInputManager::Button::Back, MappedInputManager::Button::Confirm, MappedInputManager::Button::Left,
    MappedInputManager::Button::Right, MappedInputManager::Button::Up,     MappedInputManager::Button::Down,
    MappedInputManager::Button::Power,
};

// Touch arrives in the renderer's logical (UI) orientation; the protocol speaks
// physical panel coordinates. Same transform as GfxRenderer's rotateCoordinates.
void logicalToPhysical(const GfxRenderer::Orientation o, const int x, const int y, uint16_t& px, uint16_t& py) {
  int ox = x;
  int oy = y;
  switch (o) {
    case GfxRenderer::Portrait:
      ox = y;
      oy = pclink::PANEL_HEIGHT - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      ox = pclink::PANEL_WIDTH - 1 - x;
      oy = pclink::PANEL_HEIGHT - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      ox = pclink::PANEL_WIDTH - 1 - y;
      oy = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      break;
  }
  const bool inside = ox >= 0 && oy >= 0 && ox < pclink::PANEL_WIDTH && oy < pclink::PANEL_HEIGHT;
  px = inside ? static_cast<uint16_t>(ox) : pclink::COORD_NONE;
  py = inside ? static_cast<uint16_t>(oy) : pclink::COORD_NONE;
}

HalDisplay::RefreshMode toHalMode(const pclink::RefreshMode mode) {
  switch (mode) {
    case pclink::RefreshMode::Full:
      return HalDisplay::FULL_REFRESH;
    case pclink::RefreshMode::Half:
    case pclink::RefreshMode::Gray:
      return HalDisplay::HALF_REFRESH;
    case pclink::RefreshMode::Fast:
      break;
  }
  return HalDisplay::FAST_REFRESH;
}

}  // namespace

PcLinkActivity::PcLinkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("PcLink", renderer, mappedInput), UiAppHost(renderer) {}

// Out of line: InflateReader is only forward-declared in the header.
PcLinkActivity::~PcLinkActivity() = default;

void PcLinkActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  app.setScreen(&PcLinkActivity::statusScreen, this);
  waitStartedAt = millis();
  if (!allocateSession() || !growSerialRxQueue()) {
    state = State::StartError;
  } else {
    LOG_INF("PCL", "PC Link ready (window %lu B)", static_cast<unsigned long>(pclink::RX_WINDOW));
  }
  requestUpdate();
}

void PcLinkActivity::onExit() {
  // Every exit reboots (restartToHome), which also restores the stock RX queue;
  // the staging buffers are released here regardless.
  parser.reset();
  inflater.reset();
  rxFrame.reset();
  gray.reset();
  decoded.reset();
  Activity::onExit();
}

bool PcLinkActivity::allocateSession() {
  // ~288 KB of PSRAM for the session: one whole receive frame (the parser's
  // buffer), the 2bpp canvas, and the DEFLATE output. All freed on exit.
  rxFrame = HalMemory::allocatePsram(pclink::MAX_FRAME);
  gray = HalMemory::allocatePsram(pclink::FB_2BPP_BYTES);
  decoded = HalMemory::allocatePsram(pclink::FB_2BPP_BYTES + 1);
  // ~1.2 KB of uzlib state, reused by every BLIT; heap, not the loop stack.
  inflater = makeUniqueNoThrow<InflateReader>();
  if (!rxFrame || !gray || !decoded || !inflater) {
    LOG_ERR("PCL", "OOM: PC Link staging buffers");
    return false;
  }
  parser = makeUniqueNoThrow<pclink::FrameParser>(rxFrame.get(), pclink::MAX_FRAME, pclink::MAX_PAYLOAD,
                                                  pclink::Direction::HostToDevice);
  if (!parser) {
    LOG_ERR("PCL", "OOM: PC Link parser");
    return false;
  }
  memset(gray.get(), 0xFF, pclink::FB_2BPP_BYTES);
  return true;
}

bool PcLinkActivity::growSerialRxQueue() {
  // HWCDC's RX queue is 256 B by default and the ISR drops whatever does not
  // fit. 4 KB (internal heap) backs the advertised credit window + control
  // slack. Resizing deletes and recreates the queue the RX ISR writes into, so
  // the OUT-packet interrupt is masked across it; the USB FIFO NAKs the host
  // meanwhile, so nothing is lost. Restored by the reboot on exit.
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
  delay(2);  // let an ISR already in flight on the other core finish
  const size_t got = logSerial.setRxBufferSize(pclink::RX_QUEUE_BYTES);
  if (got == 0) logSerial.setRxBufferSize(256);  // keep the console alive
  usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
  if (got == 0) LOG_ERR("PCL", "OOM: serial RX queue");
  return got != 0;
}

void PcLinkActivity::loop() {
  if (restartRequested) return;

  // Exit: hold any button, or the Home key, for EXIT_HOLD_MS — in every state.
  // Checked before forwardInput(), and restartToHome() reboots synchronously,
  // so the held button's release is never forwarded (see exitHoldReached()).
  if (exitHoldReached()) {
    restartToHome();
    return;
  }
  // Without a host, a single Back/Power press or any Home-key action exits too.
  // homeButtonAction(), not wasHomeGesture(): with CROSSPOINT_HOME_TAP_GO_BACK
  // no Home-key gesture maps to the Home action on the default bindings.
  if (state != State::Connected &&
      (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
       mappedInput.wasPressed(MappedInputManager::Button::Power) ||
       mappedInput.homeButtonAction() != HomeButtonAction::Ignore)) {
    restartToHome();
    return;
  }
  if (state == State::StartError) return;

  if (state == State::Connected) forwardInput();
  pumpSerial();
  if (restartRequested) return;
  runDueRefresh();
  watchUsbPresence();

  if (state != State::Connected && millis() - waitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("PCL", "No host; leaving PC Link");
    restartToHome();
  }
}

void PcLinkActivity::pumpSerial() {
  const unsigned long start = millis();
  while (true) {
    if (rxChunkPos < rxChunkLen) {
      rxChunkPos += parser->feed(rxChunk + rxChunkPos, rxChunkLen - rxChunkPos);
    }
    while (parser->hasFrame()) {
      handleFrame(parser->frame());
      parser->release();
    }
    if (parser->takeRejects() > 0) {
      ++stats.errors;
      uint8_t err[4];
      pclink::wr16(err, pclink::SEQ_NONE);
      err[2] = static_cast<uint8_t>(Status::Crc);
      err[3] = 0;
      send(MsgType::Err, err, sizeof(err));
    }
    if (credits.reportDue()) {
      uint8_t credit[4];
      pclink::wr32(credit, credits.total());
      credits.reported();
      send(MsgType::Credit, credit, sizeof(credit));
    }
    if (rxChunkPos < rxChunkLen) continue;  // parser paused on a frame; feed the rest
    if (millis() - start >= RX_BUDGET_MS) return;
    const int avail = logSerial.available();
    if (avail <= 0) return;
    const size_t want = static_cast<size_t>(avail) < RX_CHUNK ? static_cast<size_t>(avail) : RX_CHUNK;
    rxChunkLen = logSerial.read(rxChunk, want);
    rxChunkPos = 0;
    credits.consumed(static_cast<uint32_t>(rxChunkLen));
  }
}

void PcLinkActivity::handleFrame(const pclink::Frame& frame) {
  ++stats.frames;
  switch (frame.type) {
    case MsgType::Hello: {
      if (state != State::Connected) {
        // Adopt whatever is on the glass as the session's starting canvas.
        RenderLock lock;
        pclink::grayFromFramebuffer(display.getFrameBuffer(), gray.get());
        policy.setConfig(config);
        policy.begin(millis());
        state = State::Connected;
        sessionEnded = false;
      }
      uint8_t caps[pclink::CAPS_MAX_PAYLOAD];
      const size_t n = pclink::buildCaps(caps, sizeof(caps), config, credits.total(), CROSSPOINT_VERSION);
      credits.reported();
      send(MsgType::Caps, caps, static_cast<uint32_t>(n));
      return;
    }
    case MsgType::Ping: {
      uint8_t pong[8];
      memcpy(pong, frame.payload, 4);
      pclink::wr32(pong + 4, credits.total());
      credits.reported();
      send(MsgType::Pong, pong, sizeof(pong));
      return;
    }
    default:
      break;
  }

  if (state != State::Connected) {
    sendAck(frame.seq, frame.type == MsgType::Bye ? Status::Ok : Status::NotReady);
    return;
  }

  switch (frame.type) {
    case MsgType::Blit:
      handleBlit(frame);
      return;
    case MsgType::Refresh: {
      const uint8_t mode = frame.payload[0];
      if (mode > static_cast<uint8_t>(pclink::RefreshMode::Gray)) {
        sendAck(frame.seq, Status::BadFormat);
        return;
      }
      policy.request(static_cast<pclink::RefreshMode>(mode), frame.seq);
      sendAck(frame.seq, Status::Ok);
      return;
    }
    case MsgType::Clear: {
      if (frame.payload[0] > 1) {
        sendAck(frame.seq, Status::BadFormat);
        return;
      }
      {
        RenderLock lock;
        pclink::clearCanvases(frame.payload[0] == 1, display.getFrameBuffer(), gray.get());
      }
      sendAck(frame.seq, Status::Ok);
      return;
    }
    case MsgType::Config: {
      const bool clamped = pclink::applyConfig(config, frame.payload);
      policy.setConfig(config);
      sendAck(frame.seq, clamped ? Status::Clamped : Status::Ok);
      uint8_t state8[8];
      pclink::encodeConfig(config, state8);
      send(MsgType::ConfigState, state8, sizeof(state8));
      return;
    }
    case MsgType::Bye:
      sendAck(frame.seq, Status::Ok);
      sessionEnded = true;
      // The one device-initiated paint: the host ended the session, so the
      // status screen (with its stats) comes back.
      enterWaiting(State::Waiting, true);
      return;
    default:
      sendAck(frame.seq, Status::UnknownType);
      return;
  }
}

void PcLinkActivity::handleBlit(const pclink::Frame& frame) {
  pclink::BlitHeader blit{};
  Status status = pclink::parseBlit(frame.payload, frame.len, blit);
  const uint8_t* pixels = blit.data;
  if (status == Status::Ok && blit.enc == pclink::Encoding::Deflate) {
    if (pclink::inflateExact(*inflater, blit.data, blit.dataLen, decoded.get(), blit.decodedLen)) {
      pixels = decoded.get();
    } else {
      status = Status::DecodeFail;
    }
  }
  if (status != Status::Ok) {
    ++stats.errors;
    sendAck(frame.seq, status);
    return;
  }
  {
    // Staged only: nothing reaches the glass until a REFRESH.
    RenderLock lock;
    pclink::applyBlit(blit, pixels, display.getFrameBuffer(), gray.get());
  }
  sendAck(frame.seq, Status::Ok);
}

void PcLinkActivity::runDueRefresh() {
  if (state != State::Connected) return;
  pclink::RefreshPolicy::Action action{};
  if (!policy.due(millis(), action)) return;
  const unsigned long start = millis();
  {
    RenderLock lock;
    executeRefresh(action.mode);
  }
  const unsigned long done = millis();
  policy.executed(action, static_cast<uint32_t>(done));
  ++stats.refreshes;
  uint8_t payload[8];
  pclink::buildRefreshDone(payload, action, static_cast<uint32_t>(done - start));
  send(MsgType::RefreshDone, payload, sizeof(payload));
}

void PcLinkActivity::executeRefresh(const pclink::RefreshMode mode) {
  if (mode != pclink::RefreshMode::Gray) {
    renderer.displayBuffer(toHalMode(mode));
    return;
  }
  // Overlay grayscale, as SleepActivity's non-absolute path runs it: the B/W
  // base (already in the framebuffer — BLITs keep it in step) on a HALF
  // waveform, which the gray nudge LUT is calibrated against; then the two
  // planes, built in the framebuffer and handed to the controller; then the
  // base restored so the next differential update has the right baseline.
  uint8_t* fb = display.getFrameBuffer();
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  pclink::buildGrayPlane(gray.get(), fb, false);
  renderer.copyGrayscaleLsbBuffers();
  pclink::buildGrayPlane(gray.get(), fb, true);
  renderer.copyGrayscaleMsbBuffers();
  renderer.displayGrayBuffer();
  pclink::buildGrayBase(gray.get(), fb);
  renderer.cleanupGrayscaleWithFrameBuffer();
}

void PcLinkActivity::forwardInput() {
  const uint32_t now = millis();
  uint8_t payload[12];
  if (config.inputMask & pclink::INPUT_MASK_BUTTONS) {
    for (uint8_t code = 0; code < pclink::BUTTON_COUNT; ++code) {
      const auto button = FORWARDED_BUTTONS[code];
      if (mappedInput.wasPressed(button)) {
        pclink::buildInput(payload, pclink::INPUT_SRC_BUTTON, code, 1, pclink::COORD_NONE, pclink::COORD_NONE, now);
        send(MsgType::Input, payload, sizeof(payload));
      }
      if (mappedInput.wasReleased(button)) {
        pclink::buildInput(payload, pclink::INPUT_SRC_BUTTON, code, 0, pclink::COORD_NONE, pclink::COORD_NONE, now);
        send(MsgType::Input, payload, sizeof(payload));
      }
    }
  }
  if (config.inputMask & pclink::INPUT_MASK_TOUCH) {
    int x = 0;
    int y = 0;
    uint16_t px = pclink::COORD_NONE;
    uint16_t py = pclink::COORD_NONE;
    // wasScreenTouchDown() is a level (true on every frame of a held tap
    // candidate); INPUT carries edges only.
    if (!touchDown && mappedInput.wasScreenTouchDown(x, y)) {
      touchDown = true;
      logicalToPhysical(renderer.getOrientation(), x, y, px, py);
      pclink::buildInput(payload, pclink::INPUT_SRC_TOUCH, 0, pclink::INPUT_STATE_DOWN, px, py, now);
      send(MsgType::Input, payload, sizeof(payload));
    }
    if (mappedInput.wasScreenTapped(x, y)) {
      touchDown = false;
      logicalToPhysical(renderer.getOrientation(), x, y, px, py);
      pclink::buildInput(payload, pclink::INPUT_SRC_TOUCH, 0, pclink::INPUT_STATE_UP, px, py, now);
      send(MsgType::Input, payload, sizeof(payload));
    } else if (mappedInput.wasScreenTouchReleased()) {
      // Swipes and drags end here, without a tap position. The coordless
      // release only closes a touch-down the host was sent; a quick flick
      // never sent one, so it gets no orphan release (its SWIPE says it all).
      if (touchDown) {
        pclink::buildInput(payload, pclink::INPUT_SRC_TOUCH, 0, pclink::INPUT_STATE_UP, pclink::COORD_NONE,
                           pclink::COORD_NONE, now);
        send(MsgType::Input, payload, sizeof(payload));
      }
      touchDown = false;
      // After the release, with the same t_ms: a host that predates SWIPE has
      // already classified the release its own way and ignores this event.
      forwardSwipe(now);
    }
  }
}

void PcLinkActivity::forwardSwipe(const uint32_t now) {
  // The left-edge Back gesture already went out as the Back button.
  if (mappedInput.wasBackGesture()) return;
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  // HalGPIO directly (as homeKeyHeld() does): MappedInputManager::wasSwipe()
  // only yields the direction in the UI's logical frame, while the protocol
  // speaks physical coords and wants the start point.
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return;
  // Same native -> logical -> physical path as taps, so SWIPE and touch
  // coordinates always share one frame.
  int lx = 0;
  int ly = 0;
  uint16_t sx = pclink::COORD_NONE;
  uint16_t sy = pclink::COORD_NONE;
  uint16_t ex = pclink::COORD_NONE;
  uint16_t ey = pclink::COORD_NONE;
  const auto orientation = renderer.getOrientation();
  renderer.tapToLogical(nxs, nys, lx, ly);
  logicalToPhysical(orientation, lx, ly, sx, sy);
  renderer.tapToLogical(nxe, nye, lx, ly);
  logicalToPhysical(orientation, lx, ly, ex, ey);
  if (ex == pclink::COORD_NONE || sx == pclink::COORD_NONE) return;  // off-panel: no trustworthy direction
  const auto code = pclink::swipeCode(sx, sy, ex, ey);
  uint8_t payload[12];
  pclink::buildInput(payload, pclink::INPUT_SRC_TOUCH, static_cast<uint8_t>(code), pclink::INPUT_STATE_SWIPE, sx, sy,
                     now);
  send(MsgType::Input, payload, sizeof(payload));
}

bool PcLinkActivity::homeKeyHeld() const {
#ifdef CROSSPOINT_TOUCH_INT_WAKE
  return gpio.isHomeKeyDown();
#else
  // No level accessor on this SDK: the SDK's own Home long-press event
  // (HOME_KEY_LONG_PRESS_MS) stands in for the hold.
  return false;
#endif
}

bool PcLinkActivity::exitHoldReached() {
  // Timed here against millis() from the button LEVELS, not with
  // MappedInputManager::wasLongPressed(): that one watches only the named
  // logical button, and on the X4 Pro logical Back has no physical key at all
  // (its three keys are Up, Down and Power; Back is only the edge-swipe
  // gesture), while its getHeldTime() reads 0 on any frame a Home-key action
  // is latched. Every forwarded logical button counts, so the page-turn keys
  // exit whatever the side-button layout or orientation maps them to.
  //
  // A press that reaches the threshold has already been forwarded as a
  // down-edge; its up-edge is deliberately never sent. Host tools act on
  // release (pclink.py pages on state 0), so a synthetic up would fire an
  // action on the way out; the reboot drops the port, which ends the session
  // on the host side anyway.
  const unsigned long now = millis();
  bool reached = false;
  for (uint8_t slot = 0; slot < EXIT_HOLD_SLOTS; ++slot) {
    const bool held = slot < pclink::BUTTON_COUNT ? mappedInput.isPressed(FORWARDED_BUTTONS[slot]) : homeKeyHeld();
    if (!held) {
      holdSince[slot] = 0;
    } else if (holdSince[slot] == 0) {
      holdSince[slot] = now == 0 ? 1 : now;
    } else if (now - holdSince[slot] >= EXIT_HOLD_MS) {
      reached = true;
    }
  }
#ifndef CROSSPOINT_TOUCH_INT_WAKE
  if (gpio.hasHomeKey() && gpio.wasHomeKeyLongPressed()) reached = true;
#endif
  if (reached) LOG_INF("PCL", "Exit hold");
  return reached;
}

void PcLinkActivity::watchUsbPresence() {
  if (state != State::Connected) return;
  // HWCDC's SOF watchdog flaps briefly on a healthy link; only a sustained
  // absence ends the session. No repaint: the host's last image stays up.
  if (logSerial) {
    usbGoneSince = 0;
    return;
  }
  const unsigned long now = millis();
  if (usbGoneSince == 0) {
    usbGoneSince = now == 0 ? 1 : now;
    return;
  }
  if (now - usbGoneSince >= USB_GONE_MS) {
    LOG_INF("PCL", "USB host gone; session idle");
    enterWaiting(State::Idle, false);
  }
}

void PcLinkActivity::enterWaiting(const State next, const bool repaint) {
  state = next;
  usbGoneSince = 0;
  waitStartedAt = millis();
  if (repaint) {
    snprintf(statsLine, sizeof(statsLine), tr(STR_PC_LINK_STATS), static_cast<unsigned long>(stats.frames),
             static_cast<unsigned long>(stats.refreshes), static_cast<unsigned long>(policy.scrubs()),
             static_cast<unsigned long>(stats.errors));
    requestUpdate();
  }
}

void PcLinkActivity::send(const MsgType type, const uint8_t* payload, const uint32_t len) {
  uint8_t frame[pclink::FRAME_OVERHEAD + pclink::CAPS_MAX_PAYLOAD];
  const size_t n = pclink::encodeFrame(frame, sizeof(frame), type, txSeq++, payload, len);
  // The console TX path gives up after 1 ms per write (main.cpp's load-bearing
  // setTxTimeoutMs(1)); retry briefly so a burst of log output from another
  // task does not truncate the frame. A lost frame is healed host-side.
  size_t off = 0;
  const unsigned long start = millis();
  while (off < n && millis() - start < TX_TIMEOUT_MS) {
    off += logSerial.write(frame + off, n - off);
  }
}

void PcLinkActivity::sendAck(const uint16_t seq, const Status status) {
  uint8_t ack[8];
  pclink::buildAck(ack, seq, status, credits.total());
  credits.reported();
  send(MsgType::Ack, ack, sizeof(ack));
}

void PcLinkActivity::render(RenderLock&&) {
  // While a host owns the screen (or its last image is parked there), the
  // framebuffer is its canvas: never paint over it.
  if (state == State::Connected || state == State::Idle) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PC_LINK));
  renderUi();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void PcLinkActivity::statusScreen(UiScreen& screen, void* user) {
  static_cast<PcLinkActivity*>(user)->buildStatusScreen(screen);
}

void PcLinkActivity::buildStatusScreen(UiScreen& screen) const {
  const char* message = tr(STR_PC_LINK_WAITING);
  const char* detail = tr(STR_PC_LINK_HINT);
  const char* secondaryDetail = nullptr;
  if (state == State::StartError) {
    message = tr(STR_PC_LINK_START_ERROR);
    detail = nullptr;
  } else if (sessionEnded) {
    message = tr(STR_PC_LINK_ENDED);
    detail = statsLine;
    secondaryDetail = tr(STR_PC_LINK_HINT);
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), static_cast<int16_t>(metrics.contentSidePadding),
      static_cast<int16_t>(metrics.buttonHintsHeight), static_cast<int16_t>(metrics.contentSidePadding)});

  auto messageStyle = screen.theme().smallText;
  messageStyle.align = fui::TextAlign::Center;
  messageStyle.bold = true;
  messageStyle.maxLines = 2;
  auto detailStyle = screen.theme().smallText;
  detailStyle.align = fui::TextAlign::Center;
  detailStyle.maxLines = 3;

  const fui::Rect body = screen.body();
  const int16_t messageHeight = fui::measureWrappedText(screen.target(), message, messageStyle, body.width).height;
  const int16_t detailHeight =
      detail ? fui::measureWrappedText(screen.target(), detail, detailStyle, body.width).height : 0;
  const int16_t secondaryDetailHeight =
      secondaryDetail ? fui::measureWrappedText(screen.target(), secondaryDetail, detailStyle, body.width).height : 0;
  const int16_t gap = detail ? screen.theme().spaceMd : 0;
  const int16_t secondaryGap = secondaryDetail ? screen.theme().spaceMd : 0;
  const int16_t totalHeight =
      static_cast<int16_t>(messageHeight + gap + detailHeight + secondaryGap + secondaryDetailHeight);
  if (body.height > totalHeight) screen.spacer(static_cast<int16_t>((body.height - totalHeight) / 2));

  screen.target().text(screen.takeTop(messageHeight, gap), message, messageStyle);
  if (detail) screen.target().text(screen.takeTop(detailHeight, secondaryGap), detail, detailStyle);
  if (secondaryDetail) screen.target().text(screen.takeTop(secondaryDetailHeight), secondaryDetail, detailStyle);
}

void PcLinkActivity::restartToHome() {
  if (restartRequested) return;
  restartRequested = true;
  LOG_INF("PCL", "PC Link exit: %lu frames, %lu refreshes", static_cast<unsigned long>(stats.frames),
          static_cast<unsigned long>(stats.refreshes));
  // A reboot, as every exclusive-loop activity exits: it also restores the
  // stock HWCDC RX queue. The GPIO scrub in HalGPIO::begin() covers the
  // CPU-only reset on the X4 Pro.
  restartToHomeAfterStorageHandoff();
}

#endif  // CROSSPOINT_PC_LINK
