#include <BatteryMonitor.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>

#include <atomic>

#ifdef CROSSPOINT_TOUCH_INT_WAKE
#include <driver/gpio.h>
#endif

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

}  // namespace X3GPIO

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

}  // namespace

void HalGPIO::begin() {
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
}

void HalGPIO::update() {
  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::hasHomeKey() const { return BoardConfig::hasHomeKey(); }

bool HalGPIO::wasHomeKeyTapped() const { return inputMgr.wasHomeKeyTapped(); }

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
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  if (BoardConfig::ACTIVE.usbDetect >= 0) {
    return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  // No digital USB-detect line (e.g. Sticky, whose PWR_IN_VOLT is an analog
  // divider): infer external power from charging state instead. BatteryMonitor
  // picks the board's best source — charger IC status, gauge Current() sign, or
  // a /STAT pin — and reports false on boards with no battery telemetry at all.
  // Caveat: charge termination at 100% reads as "not connected".
  static const BatteryMonitor battery;
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
  return isXteinkDevice() || BoardConfig::isPaperMono() || BoardConfig::isSticky();
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

void addWakePin(const int8_t pin, const bool activeLow) {
  if (pin < 0) {
    return;
  }
  for (uint8_t i = 0; i < wakePinCount; ++i) {
    if (wakePins[i].pin == static_cast<gpio_num_t>(pin)) return;  // shared pin (confirm/power)
  }
  if (wakePinCount >= MAX_WAKE_PINS) {
    // Silently dropping a pin would leave the wait deaf to that input for up to
    // a full second. Fail the whole mechanism closed instead; the caller then
    // keeps the stock 50 ms poll, which sees every button.
    wakePinOverflow = true;
    return;
  }
  wakePins[wakePinCount++] = {static_cast<gpio_num_t>(pin), activeLow};
}

// True while the pin sits at the level it would wake on.
bool wakePinAsserted(const WakePin& p) { return digitalRead(p.pin) == (p.activeLow ? LOW : HIGH); }

}  // namespace

void HalGPIO::beginInputWake() {
  wakePinCount = 0;
  wakePinOverflow = false;
  wakeUsable = false;

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

  const int8_t touchIrq = inputMgr.touchWakeIrqPin();
  addWakePin(touchIrq, inputMgr.touchWakeIrqActiveLow());

  // Every input the loop can act on has to be something this can arm, or the
  // wait would be deaf to the rest of them for up to its full cap. Boards that
  // fail that test keep the stock poll:
  //   * XteinkAdcLadder (X4, X3): the nav keys are resistor steps on a single
  //     ADC pin, found by sampling — there is no per-key level to interrupt on;
  //   * a board button hook (LilyGo T5 S3's user button on its PCA9535): the
  //     key is behind an I2C expander, invisible to a GPIO interrupt, and the
  //     expander's own INT line is not modeled here;
  //   * a live touch panel with no level-holding INT: a contact would have no
  //     way to signal during a long wait (see FREEINK_GT911_INT_WAKE, which is
  //     what makes touchWakeIrqPin() report a pin at all);
  //   * more wake pins than the table holds (see addWakePin).
  const bool navKeysAreGpio =
      BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder && !InputManager::hasButtonHook();
  wakeUsable = wakePinCount > 0 && !wakePinOverflow && navKeysAreGpio && (!inputMgr.hasTouch() || touchIrq >= 0);
  LOG_INF("PWR", "Input wake: %u pin(s), touch INT %d, gpioKeys=%d usable=%d", static_cast<unsigned>(wakePinCount),
          static_cast<int>(touchIrq), static_cast<int>(navKeysAreGpio), static_cast<int>(wakeUsable));
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
  uint8_t armedCount = 0;
  for (uint8_t i = 0; i < wakePinCount; ++i) {
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

#endif  // CROSSPOINT_TOUCH_INT_WAKE
