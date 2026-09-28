#pragma once
#ifdef CROSSPOINT_PC_LINK

#include <HalMemory.h>
#include <PcLinkProtocol.h>

#include <memory>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

class InflateReader;

// PC Link: the reader as a USB thin-client display (lib/PcLink documents the
// protocol and the wear model). The host renders; this activity blits into the
// framebuffer, refreshes under pclink::RefreshPolicy and streams input back
// over the USB-Serial/JTAG console. Lifecycle mirrors UsbDriveActivity: the
// exclusive loop owns the port (main.cpp's CMD: parser does not run), sleep is
// held off for the session, and every exit reboots to Home.
class PcLinkActivity final : public Activity, private UiAppHost {
 public:
  PcLinkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~PcLinkActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The session is USB-attached and host-driven; the wait timeout below (not
  // auto-sleep) ends an abandoned one.
  bool preventAutoSleep() override { return true; }
  // Owns the serial port and the panel: no screenshots, shortcuts, sleep or
  // navigation while a host may be mid-frame.
  bool requiresExclusiveStorageLoop() const override { return true; }

 private:
  // Waiting: status screen, no host. Connected: the host owns the screen.
  // Idle: the host vanished (USB gone) — its last image stays up, untouched.
  enum class State : uint8_t { Waiting, Connected, Idle, StartError };

  static constexpr unsigned long HOST_WAIT_TIMEOUT_MS = 10UL * 60UL * 1000UL;
  static constexpr unsigned long EXIT_HOLD_MS = 1500;
  // One hold timer per forwarded logical button, plus the Home key.
  static constexpr uint8_t EXIT_HOLD_SLOTS = pclink::BUTTON_COUNT + 1;
  static constexpr unsigned long USB_GONE_MS = 5000;
  static constexpr unsigned long RX_BUDGET_MS = 30;
  static constexpr unsigned long TX_TIMEOUT_MS = 20;
  static constexpr size_t RX_CHUNK = 128;

  static void statusScreen(UiScreen& screen, void* user);
  void buildStatusScreen(UiScreen& screen) const;

  bool allocateSession();
  bool growSerialRxQueue();
  void pumpSerial();
  void handleFrame(const pclink::Frame& frame);
  void handleBlit(const pclink::Frame& frame);
  void runDueRefresh();
  void executeRefresh(pclink::RefreshMode mode);
  void forwardInput();
  void forwardSwipe(uint32_t now);
  bool exitHoldReached();
  bool homeKeyHeld() const;
  void watchUsbPresence();
  void send(pclink::MsgType type, const uint8_t* payload, uint32_t len);
  void sendAck(uint16_t seq, pclink::Status status);
  void enterWaiting(State next, bool repaint);
  void restartToHome();

  State state = State::Waiting;
  bool sessionEnded = false;  // a host said BYE: the status screen shows stats
  bool restartRequested = false;
  bool touchDown = false;  // INPUT touch-down already sent for this contact
  unsigned long waitStartedAt = 0;
  unsigned long usbGoneSince = 0;
  unsigned long holdSince[EXIT_HOLD_SLOTS]{};  // millis() at press; 0 = not held

  // PSRAM staging, allocated on enter and freed on exit — never in the loop.
  HalMemory::PsramBuffer rxFrame;  // one whole frame: FrameParser's buffer
  HalMemory::PsramBuffer gray;     // 2bpp canvas, 800x480
  HalMemory::PsramBuffer decoded;  // DEFLATE output, max BLIT + 1
  std::unique_ptr<pclink::FrameParser> parser;
  std::unique_ptr<InflateReader> inflater;
  uint8_t rxChunk[RX_CHUNK]{};
  size_t rxChunkLen = 0;
  size_t rxChunkPos = 0;

  pclink::CreditReporter credits{pclink::RX_WINDOW};
  pclink::RefreshPolicy policy;
  pclink::WearConfig config;
  uint16_t txSeq = 0;

  struct Stats {
    uint32_t frames = 0;
    uint32_t refreshes = 0;
    uint32_t errors = 0;
  } stats;
  char statsLine[96]{};
};

#endif  // CROSSPOINT_PC_LINK
