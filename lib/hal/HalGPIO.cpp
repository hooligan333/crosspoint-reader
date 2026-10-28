#include <BatteryMonitor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <XteinkDetect.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <hal/gpio_ll.h>

#if FREEINK_DEVICE_LILYGO
#include <BoardT5S3.h>
#endif

#include <atomic>

// Global HalGPIO instance
HalGPIO gpio;

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

// ESP.restart() and panic restarts are CPU-only resets: they do not reset the
// GPIO block, so every pin's interrupt enable, interrupt type and light-sleep
// wake bit survive from the previous life. A level interrupt left armed there
// (the GT911 touch INT, held low while a report is latched) is already pending
// when the first attach of this boot installs the GPIO ISR service; with no
// per-pin handler registered yet, the service's dispatch loop clears and
// re-latches that level forever and the interrupt watchdog panics — into
// another CPU-only reset with the same GPIO state, i.e. a boot loop.
//
// Scrub ONLY the interrupt/wake configuration of every valid pin, before
// anything in this boot can attach an interrupt. Direction, pulls, IO-MUX and
// GPIO-matrix routing are left alone, so SD, display, flash/PSRAM and strapping
// pins are unaffected. The wake bit is cleared with the LL call rather than
// gpio_wakeup_disable(): the driver call also flips the pin's sleep-select
// (C3, CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND) and RTC-IO wake config, which
// are pin-behaviour changes this scrub must not make. Runs once per boot.
void clearStaleGpioInterrupts() {
  static bool done = false;
  if (done) return;
  done = true;
  for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
    if (!GPIO_IS_VALID_GPIO(pin)) continue;
    const auto num = static_cast<gpio_num_t>(pin);
    gpio_intr_disable(num);
    gpio_set_intr_type(num, GPIO_INTR_DISABLE);
    gpio_ll_wakeup_disable(&GPIO, num);
  }
}

}  // namespace

void HalGPIO::begin() {
  // First thing, before the touch probe or any display wait (EpdBus attaches a
  // BUSY interrupt on hosts without a busy-wait slice hook) can install the
  // GPIO ISR service against stale state from before a soft restart.
  clearStaleGpioInterrupts();
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
#if FREEINK_DEVICE_LILYGO
  // T5S3 IO48 key sits behind the PCA9535 expander. Mirrors BoardT5S3::begin(), which also
  // re-inits I2C/SPI and is unsafe to call here. inputMgr.begin() has started Wire.
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3) {
    BoardT5S3::setPca9535PinMode(PCA9535_IO12_BUTTON, INPUT);
    InputManager::setButtonHook(
        []() -> uint8_t { return BoardT5S3::readButton() ? static_cast<uint8_t>(1U << InputManager::BTN_DOWN) : 0; });
  }
#endif
}

void HalGPIO::update() {
  inputMgr.update();
  const uint8_t pageButtons = inputMgr.capacitivePageButtonMask();
  capacitivePagePressed = (pageButtons & ~previousCapacitivePageButtons) != 0;
  previousCapacitivePageButtons = pageButtons;
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

void HalGPIO::prepareForDeepSleep() {
  if (!inputMgr.prepareForDeepSleep()) LOG_ERR("GPIO", "Failed to put touch controller to sleep");
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

bool HalGPIO::rawInputActive() {
  if (inputMgr.isPowerButtonPhysicallyPressed()) return true;
#if FREEINK_DEVICE_LILYGO
  if (BoardConfig::ACTIVE.board == BoardConfig::Board::LilyGoT5S3 && BoardT5S3::readButton()) return true;
#endif
  InputManager::ButtonAdcSample g1{}, g2{};
  inputMgr.readButtonAdc(g1, g2);
  // The Xteink ladder idles at the ADC full-scale rail (~4095); every button band sits below 3900.
  constexpr int kIdleRailMin = 4000;
  return (g1.raw >= 0 && g1.raw < kIdleRailMin) || (g2.raw >= 0 && g2.raw < kIdleRailMin);
}

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyPressed() const { return inputMgr.wasHomeKeyPressed(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

bool HalGPIO::hasToggleSwitch() const { return inputMgr.hasToggleSwitch(); }

bool HalGPIO::isToggleSwitchOn() const { return inputMgr.isToggleSwitchOn(); }

bool HalGPIO::wasHomeKeyLongPressed() const { return inputMgr.wasHomeKeyLongPressed(); }

#ifdef CROSSPOINT_TOUCH_INT_WAKE
// InputManager::isHomeKeyDown() exists only on the fork SDK carrying the
// GT911 INT-wake work; keep this passthrough behind the same flag so
// flags-off builds of the app still compile against the upstream SDK.
bool HalGPIO::isHomeKeyDown() const { return inputMgr.isHomeKeyDown(); }
#endif

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::wasTouchReleased() const { return inputMgr.wasTouchReleased(); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

bool HalGPIO::wasTouchLongPress(float& nx, float& ny) const { return inputMgr.wasTouchLongPress(nx, ny); }

void HalGPIO::suppressTouchContact() { inputMgr.suppressTouchContact(); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::hasEdgeSideButtons() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Classic;
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::verifyPowerButtonWakeup() {
  // M5Paper v1.1: the classic ESP32's reset-to-setup() latency exceeds a normal
  // wheel click, so a click wake is always released before this samples and
  // verification would re-sleep on every wake. Its wheel has hard external
  // pull-ups, so the ghost-wake debounce this implements is not needed.
  if (BoardConfig::isPaperMono() || BoardConfig::isM5PaperV11() || BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }

  constexpr unsigned long POWER_WAKE_STABILITY_MS = 10;
  const bool heldAtFirstSample = inputMgr.isPowerButtonPhysicallyPressed();
  const unsigned long sampleStart = millis();
  inputMgr.update();
  while (millis() - sampleStart < POWER_WAKE_STABILITY_MS || inputMgr.isDebouncePending()) {
    delay(1);
    inputMgr.update();
  }
  return heldAtFirstSample && inputMgr.isPowerButtonPhysicallyPressed();
}

bool HalGPIO::isUsbConnected() const {
  if (!deviceIsX3() && BoardConfig::ACTIVE.usbDetect >= 0) {
    return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  // X3 uses GPIO20 for I2C, not USB detection. Boards without a digital
  // USB-detect line (e.g. Sticky, whose PWR_IN_VOLT is an analog
  // divider): infer external power from charging state instead. BatteryMonitor
  // picks the board's best source — charger IC status, gauge Current() sign, or
  // a /STAT pin — and reports false on boards with no battery telemetry at all.
  // A charger IC that reports its input rail (BQ25896, SGM41562, M5 PMIC)
  // answers directly; otherwise infer from charging state, where charge
  // termination at 100% reads as "not connected".
  static const BatteryMonitor battery;
  bool known = false;
  const bool present = battery.isExternalPowerPresent(&known);
  if (known) return present;
  return battery.isCharging();
}

bool HalGPIO::coldBootImpliesPowerButton() const {
  // Xteink-style power topology: the power button energizes the rail until
  // firmware latches it, so a no-USB POWERON can only be a still-held button
  // boot, and plugging USB into an off device should charge-sleep, not boot.
  // Everything else boots on any cold boot: boards with no USB detection at
  // all (M5Paper v1.1, PaperColor, Murphy, de-link) would misread USB and
  // post-flash boots as battery button boots, and STAT-only boards like the
  // EEGO A4 misread them the same way once the charger terminates at 100%
  // (STAT inactive reads as "no USB").
  // The Metalio sleeps powered off and only its button turns it back on.
  return isXteinkDevice() || BoardConfig::isPaperMono() || BoardConfig::isSticky() || BoardConfig::isMetalioEInk4();
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected &&
      coldBootImpliesPowerButton()) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}

#ifdef CROSSPOINT_TOUCH_INT_WAKE

namespace {

struct WakePin {
  gpio_num_t pin;
  bool activeLow;
};

constexpr uint8_t MAX_WAKE_PINS = 8;
WakePin wakePins[MAX_WAKE_PINS];
uint8_t wakePinCount = 0;
bool wakePinOverflow = false;
bool wakeUsable = false;
// Index of the touch INT in wakePins, or -1. The SLOT is reserved once, at
// setup, but whether it is ARMED is decided again before every wait (and every
// anyWakePinAsserted() read) from InputManager::touchWakeIrqPin(): the SDK
// withdraws the line (-1) while it cannot read or clear the GT911 status
// register — a pending report would then hold INT asserted indefinitely and a
// level wake on it would pin the loop awake — and offers it again once a poll
// gets through. A withdrawn slot is treated exactly like an already-asserted
// pin: left unarmed, and the round falls back to the stock LOOP_FLOOR_MS
// cadence, so touch keeps working through the ordinary poll while the buttons
// still wake the wait. Recovery re-arms on the next wait, with no reboot.
int8_t touchWakeSlot = -1;
// Written once on loopTask (before any wake interrupt is ever enabled), read
// from the ISR: atomic so the ISR/task sharing is formally synchronised, not
// merely benign-by-ordering. Relaxed is enough — there is no dependent data.
std::atomic<TaskHandle_t> wakeNotifyTask{nullptr};

// The stock idle cadence of the loop tail, and the floor this wait may not
// undercut. A LEVEL interrupt on a pin that is ALREADY asserted when it is
// armed — a GT911 INT stuck low after an I2C fault, a button held down in a
// bag — fires the instant gpio_intr_enable() runs, so ulTaskNotifyTake()
// returns immediately and the loop would spin at full rate with no delay at
// all: the opposite of what this is for, and light sleep never engages. A
// return faster than IMMEDIATE_RETURN_MS is taken as that case and padded back
// out to LOOP_FLOOR_MS, which bounds the loop to today's cadence while still
// letting the poll run right away.
constexpr uint32_t LOOP_FLOOR_MS = 50;
constexpr uint32_t IMMEDIATE_RETURN_MS = 20;

// The Arduino GPIO ISR service is installed without ESP_INTR_FLAG_IRAM, which
// is what makes the flash-resident gpio_intr_disable() call in the handler
// below safe (the allocator then masks this interrupt for the duration of every
// flash operation, so it never runs with the cache disabled). Fail the build
// loudly if that ever flips rather than ship a cache-disabled crash.
#if CONFIG_ARDUINO_ISR_IRAM
#error \
    "CROSSPOINT_TOUCH_INT_WAKE needs a non-IRAM GPIO ISR service: inputWakeIsr calls flash-resident gpio_intr_disable()."
#endif

// Level-triggered by necessity: light sleep clock-gates the GPIO edge detector,
// so gpio_wakeup_enable() accepts nothing else. A level interrupt re-latches its
// status bit for as long as the line is held, re-entering the handler until the
// source clears — so the handler must mask its own pin, and waitForInput()
// re-enables it for the next wait.
void IRAM_ATTR inputWakeIsr(void* arg) {
  gpio_intr_disable(static_cast<gpio_num_t>(reinterpret_cast<intptr_t>(arg)));
  TaskHandle_t task = wakeNotifyTask.load(std::memory_order_relaxed);
  if (task == nullptr) {
    return;
  }
  BaseType_t higherPriorityWoken = pdFALSE;
  vTaskNotifyGiveFromISR(task, &higherPriorityWoken);
  if (higherPriorityWoken) {
    portYIELD_FROM_ISR();
  }
}

// Returns the pin's slot in wakePins, or -1 when it was not added.
int8_t addWakePin(const int8_t pin, const bool activeLow) {
  if (pin < 0) {
    return -1;
  }
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    if (wakePins[i].pin == static_cast<gpio_num_t>(pin)) return static_cast<int8_t>(i);  // shared (confirm/power)
  }
  if (wakePinCount >= MAX_WAKE_PINS) {
    // Silently dropping a pin would leave the wait deaf to that input for up to
    // a full second. Fail the whole mechanism closed instead; the caller then
    // keeps the stock 50 ms poll, which sees every button.
    wakePinOverflow = true;
    return -1;
  }
  wakePins[wakePinCount] = {static_cast<gpio_num_t>(pin), activeLow};
  return static_cast<int8_t>(wakePinCount++);
}

// True while the pin sits at the level it would wake on.
bool wakePinAsserted(const WakePin& p) { return digitalRead(p.pin) == (p.activeLow ? LOW : HIGH); }

// Whether the reserved touch slot may be armed right now. A plain getter on
// InputManager state (no I2C), so asking it on every wait costs nothing; the
// state it reads is written by update() on this same loop task.
bool touchWakeSlotLive(const InputManager& input) {
  return touchWakeSlot < 0 || input.touchWakeIrqPin() == static_cast<int8_t>(wakePins[touchWakeSlot].pin);
}

}  // namespace

void HalGPIO::beginInputWake() {
  clearStaleGpioInterrupts();  // no-op after begin(); guards a caller that skips it
  wakePinCount = 0;
  wakePinOverflow = false;
  wakeUsable = false;
  touchWakeSlot = -1;

  // Button pin modes already belong to InputManager::begin() (INPUT_PULLUP for
  // the nav keys, powerActiveHigh for power) and the touch INT's to the GT911
  // driver; this only reads their polarity. ADC-ladder boards multiplex the nav
  // keys onto ADC pins, where only power is a real GPIO.
  const auto& in = BoardConfig::ACTIVE.input;
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    for (const int8_t pin : {in.back, in.confirm, in.left, in.right, in.up, in.down}) {
      addWakePin(pin, true);
    }
  }
  addWakePin(in.power, !in.powerActiveHigh);

  // Not cached as a decision: touchWakeIrqPin() can read -1 right now only
  // because the post-config status clear failed (the SDK withdraws the line
  // until a poll clears it), so reserve the slot whenever the SDK is able to
  // offer the line at all, and let each wait re-ask (see touchWakeSlot). A
  // controller that refused the level mode outright leaves the slot reserved
  // but never armed: every wait then runs at the stock LOOP_FLOOR_MS cadence.
  // Polarity is the board profile's, a constant, so it is read once.
  int8_t touchIrq = inputMgr.touchWakeIrqPin();
#if FREEINK_TOUCH_GT911 && FREEINK_GT911_INT_WAKE
  if (touchIrq < 0 && inputMgr.hasTouch() &&
      BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::Gt911) {
    touchIrq = BoardConfig::ACTIVE.touch.irq;
  }
#endif
  const uint8_t countBeforeTouch = wakePinCount;
  const int8_t touchSlot = addWakePin(touchIrq, inputMgr.touchWakeIrqActiveLow());
  // Only a slot of its own: a touch INT sharing a button's GPIO stays a button.
  touchWakeSlot = wakePinCount > countBeforeTouch ? touchSlot : -1;

  // Every input the loop can act on has to be something this can arm, or the
  // wait would be deaf to the rest of them for up to its full cap. Boards that
  // fail that test keep the stock poll:
  //   * XteinkAdcLadder (X4, X3): the nav keys are resistor steps on a single
  //     ADC pin, found by sampling — there is no per-key level to interrupt on;
  //   * a board button hook (LilyGo T5 S3's user button on its PCA9535): the
  //     key is behind an I2C expander, invisible to a GPIO interrupt, and the
  //     expander's own INT line is not modeled here;
  //   * a live touch panel with no INT that could ever hold a level: a contact
  //     would have no way to signal during a long wait (see
  //     FREEINK_GT911_INT_WAKE, which is what makes touchWakeIrqPin() report a
  //     pin at all, and touchWakeSlot for an INT that is only withdrawn for now);
  //   * more wake pins than the table holds (see addWakePin).
  const bool navKeysAreGpio =
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder && !InputManager::hasButtonHook();
  wakeUsable = wakePinCount > 0 && !wakePinOverflow && navKeysAreGpio && (!inputMgr.hasTouch() || touchIrq >= 0);
  LOG_INF("PWR", "Input wake: %u pin(s), touch INT %d (offered now %d), gpioKeys=%d usable=%d",
          static_cast<unsigned>(wakePinCount), static_cast<int>(touchIrq), static_cast<int>(inputMgr.touchWakeIrqPin()),
          static_cast<int>(navKeysAreGpio), static_cast<int>(wakeUsable));
  if (!wakeUsable) {
    wakePinCount = 0;
    return;
  }

  for (uint8_t i = 0; i < wakePinCount; ++i) {
    attachInterruptArg(wakePins[i].pin, inputWakeIsr, reinterpret_cast<void*>(static_cast<intptr_t>(wakePins[i].pin)),
                       wakePins[i].activeLow ? ONLOW : ONHIGH);
    gpio_intr_disable(wakePins[i].pin);  // enabled only inside waitForInput()
  }
  wakeNotifyTask = xTaskGetCurrentTaskHandle();
}

bool HalGPIO::inputWakeAvailable() const { return wakeUsable && wakeNotifyTask != nullptr; }

bool HalGPIO::waitForInput(const uint32_t maxMs) {
  if (!inputWakeAvailable()) {
    // Deliberately NOT delay(maxMs): with no wake source at all that would be a
    // silent stall of up to the caller's full cap. The caller's gate keeps this
    // unreachable today (it asks for no wait when the wait is unavailable), so
    // this is a floor for a future caller, not a live path.
    delay(LOOP_FLOOR_MS);
    return false;
  }

  const uint32_t startMs = millis();

  // Arm only the pins that are currently released. One already sitting at its
  // wake level would interrupt the moment it is enabled and keep doing so until
  // the line lets go, which is the busy loop LOOP_FLOOR_MS exists to bound;
  // leaving it disarmed costs nothing, because the poll that runs immediately
  // after this call is what reads its state anyway.
  // The touch INT is re-asked for every wait; a withdrawn one is left out the
  // same way, so this round runs at the stock cadence (see touchWakeSlot).
  const bool touchLive = touchWakeSlotLive(inputMgr);
  uint8_t armedCount = 0;
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    if (i == touchWakeSlot && !touchLive) continue;
    if (wakePinAsserted(wakePins[i])) continue;
    gpio_wakeup_enable(wakePins[i].pin, wakePins[i].activeLow ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    gpio_intr_enable(wakePins[i].pin);
    ++armedCount;
  }
  if (armedCount == 0) {
    // Every wake-capable input is held down: nothing left that could change
    // state and signal it, so skip the long wait and keep the stock cadence.
    delay(LOOP_FLOOR_MS);
    return false;
  }
  // With a pin left out, fall back to the stock cadence for this round rather
  // than the caller's full cap: an asserted line is state the poll has not
  // consumed yet (a GT911 INT still held because update() skipped its I2C
  // slot, a button mid-press), and a long wait would sit on it for up to a
  // second. The pins that ARE armed can still end the wait sooner.
  const uint32_t waitMs = armedCount == wakePinCount ? maxMs : LOOP_FLOOR_MS;
  esp_sleep_enable_gpio_wakeup();

  // Not cleared before the take: the previous wait drained its own slot below,
  // so a count pending here came from outside this mechanism — returning early
  // on it costs one extra poll, which is cheaper than dropping a real input.
  const uint32_t woken = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    gpio_intr_disable(wakePins[i].pin);
    gpio_wakeup_disable(wakePins[i].pin);
  }
  // Drop anything the ISR posted between the take and the disarm above: this
  // task's notification index 0 is shared with
  // ActivityManager::requestUpdateAndWait(), which blocks on it for the render
  // task's ack, and a leftover wake notification there would make it return
  // before the render had actually happened. Nothing is lost — the pin that
  // fired is still asserted for the poll that follows.
  ulTaskNotifyTake(pdTRUE, 0);

  const uint32_t elapsedMs = millis() - startMs;
  if (elapsedMs < IMMEDIATE_RETURN_MS) {
    delay(LOOP_FLOOR_MS - elapsedMs);
  }
  return woken != 0;
}

bool HalGPIO::anyWakePinAsserted() const {
  // A withdrawn touch INT may be held asserted by a report the poll cannot
  // clear; counting it would hold the caller in its 10 ms slice poll for as
  // long as the I2C fault lasts. It is not a wake source this round, so it is
  // not a contact in progress either — touch is still read by the poll.
  const bool touchLive = touchWakeSlotLive(inputMgr);
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    if (i == touchWakeSlot && !touchLive) continue;
    if (wakePinAsserted(wakePins[i])) return true;
  }
  return false;
}

#endif  // CROSSPOINT_TOUCH_INT_WAKE
