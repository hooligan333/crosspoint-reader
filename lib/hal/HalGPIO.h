#pragma once

#include <Arduino.h>
#include <InputManager.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }
  bool isXteinkDevice() const;

  // True when the board's page buttons sit on the left/right screen edges
  // (X3, X4 Pro) rather than an off-screen vertical rocker. Drives side-hint
  // placement and the flipped large-step direction in selection activities.
  // Keyed off the active BoardConfig profile, not the X3/X4 runtime detection.
  bool hasEdgeSideButtons() const;

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;
  bool hasTouch() const;
  // Capacitive Home key reported by the touch controller (X4 Pro). The tap
  // event fires on release and excludes a long hold.
  bool hasHomeKey() const;
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;
#ifdef CROSSPOINT_TOUCH_INT_WAKE
  // Home key currently held, as a level rather than an edge. A motionless hold
  // produces no new touch frames, so its long-press threshold is timed by
  // update() against the wall clock: callers that may stop polling have to
  // check this or they stretch that threshold by however long they stay away.
  // Flag-gated: the SDK-side accessor exists only on the INT-wake fork branch.
  bool isHomeKeyDown() const;
#endif
  bool wasTouchTap(float& nx, float& ny) const;
  bool wasTouchDown(float& nx, float& ny) const;
  // Raw release edge, reported even when the contact was not a tap (swipe end,
  // drag-off). Snapshot builders forward it so interaction routing can clear
  // pressed state.
  bool wasTouchReleased() const;
  bool isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const;
  bool isTouchHeldAt(float& nx, float& ny) const;
  // One-shot long-press, fired by the SDK classifier while the finger is still
  // down (stationary contact held past its threshold). Position = touch-down
  // point. Callers that act on it should suppressTouchContact() so the lift
  // cannot also tap.
  bool wasTouchLongPress(float& nx, float& ny) const;
  // Ignore the remainder of the current contact (its continued hold and its
  // release edge). Self-clears once the contact ends.
  void suppressTouchContact();
  unsigned long lastTouchHeldMs() const;
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;
  bool wasTouchActivity() const;
  void setSharedConfirmPowerShortPressEmitsPower(bool enabled);

  // Verify that the physical power button remains held through input debounce.
  // Returns true if verification succeeded, false if device should return to sleep.
  // Should only be called when wakeup reason is PowerButton.
  bool verifyPowerButtonWakeup();

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

#ifdef CROSSPOINT_TOUCH_INT_WAKE
  // --- Interrupt-woken idle wait ---------------------------------------------
  // Lets the main loop block for hundreds of ms instead of re-polling every
  // 50 ms, so tickless light sleep gets one long window instead of twenty short
  // ones. Only a waker: the wake pins feed an ISR that unblocks the loop task,
  // and the existing update() poll then classifies every press, gesture and
  // debounce exactly as before.

  // Attach the wake ISRs and latch the CALLING task as the one they notify —
  // must run on the task that later calls waitForInput() (loopTask, i.e. from
  // setup()), after begin() has probed the touch controller.
  void beginInputWake();

  // False when no wake source could be armed, or when this board has inputs no
  // GPIO level can represent: ADC-ladder nav keys, a button behind an I2C
  // expander, or a live touch panel whose INT cannot hold a level. Callers then
  // keep polling, which sees every input.
  bool inputWakeAvailable() const;

  // Block up to maxMs, returning as soon as a wake pin asserts. Arms the pins as
  // light-sleep wake sources for the duration of the call ONLY — esp_sleep's
  // GPIO trigger bit is shared with deep sleep, and deep-sleep entry always
  // happens outside this call. Returns true when a pin (not the timeout) ended
  // the wait. A pin already sitting at its wake level (an INT stuck low after
  // an I2C fault, a button held down in a bag) would fire the instant it was
  // armed, so it is left unarmed and the round falls back to the loop's stock
  // 50 ms cadence — which is also the floor for how fast this can return. A
  // held level can therefore neither busy-loop the caller nor hide an input
  // behind the full cap.
  bool waitForInput(uint32_t maxMs);
#endif

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
