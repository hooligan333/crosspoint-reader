#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>

#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
#include <esp_pm.h>
#endif

#include "HalGPIO.h"

#if FREEINK_DEVICE_PAPERMONO
#include <M5Pm1.h>
#endif

HalPowerManager powerManager;  // Singleton instance

// GPIO13 controls the X4 battery latch and the X3 SD power rail on the C3
// Xteink boards. Other boards use it for unrelated signals, including the
// X4 Pro display chip select.
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
#if !BOARD_HAS_PSRAM
#error \
    "CROSSPOINT_AUTO_LIGHT_SLEEP assumes a PSRAM board: min DFS = LOW_POWER_FREQ (80) keeps APB pinned at 80 MHz. On a non-PSRAM board LOW_POWER_FREQ is 10, and a 10 MHz APB under the lock-less peripheral drivers (SPI.writeBytes, Wire, LEDC) is unvalidated -- review before enabling."
#endif
  // Automatic light sleep: let esp_pm scale the clock between LOW_POWER_FREQ
  // and the boot clock, and light-sleep the chip whenever every task is idle
  // (FreeRTOS tickless idle) instead of burning the loop's delays awake. Needs
  // CONFIG_PM_ENABLE + CONFIG_FREERTOS_USE_TICKLESS_IDLE in the env's
  // sdkconfig; without them esp_pm_configure() fails and everything below
  // degrades to a logged no-op — including the manual DFS this replaces, so
  // the flag and the sdkconfig must ship together (they do, in [env:x4pro-combo]).
  esp_pm_config_t pmCfg = {};
  pmCfg.max_freq_mhz = normalFreq > 0 ? normalFreq : 240;
  pmCfg.min_freq_mhz = LOW_POWER_FREQ;
  pmCfg.light_sleep_enable = true;
  const esp_err_t pmErr = esp_pm_configure(&pmCfg);
  if (pmErr != ESP_OK) {
    LOG_ERR("PWR", "esp_pm_configure failed (%d); auto light sleep unavailable", static_cast<int>(pmErr));
    return;
  }

  esp_pm_lock_handle_t active = nullptr;
  esp_pm_lock_handle_t render = nullptr;
  esp_pm_lock_handle_t noSleep = nullptr;
  esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "cpActive", &active);
  esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "cpRender", &render);
  esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "cpAwake", &noSleep);
  if (active == nullptr || render == nullptr || noSleep == nullptr) {
    // Locks are the only way back to full speed (and the only way to hold sleep
    // off for WiFi/USB) once this config is live, so a config applied without
    // them would strand the device at the floor. Revert to a fixed-max
    // no-sleep config and disable the feature for this session.
    LOG_ERR("PWR", "esp_pm lock creation failed; auto light sleep disabled");
    esp_pm_config_t fixed = {};
    fixed.max_freq_mhz = pmCfg.max_freq_mhz;
    fixed.min_freq_mhz = pmCfg.max_freq_mhz;
    fixed.light_sleep_enable = false;
    esp_pm_configure(&fixed);
    if (active != nullptr) esp_pm_lock_delete(active);
    if (render != nullptr) esp_pm_lock_delete(render);
    if (noSleep != nullptr) esp_pm_lock_delete(noSleep);
    return;
  }

  pmActiveLock = active;
  pmRenderLock = render;
  pmNoSleepLock = noSleep;
  // Boot continues at full speed; the first setPowerSaving(true) releases it.
  setPmLockHeld(pmActiveLock, pmActiveHeld, true);
  LOG_INF("PWR", "Auto light sleep on (DFS %d-%d MHz)", pmCfg.min_freq_mhz, pmCfg.max_freq_mhz);
#endif
}

#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
void HalPowerManager::setPmLockHeld(void* const lock, bool& held, const bool want) {
  if (lock == nullptr || held == want) {
    return;
  }
  auto* const handle = static_cast<esp_pm_lock_handle_t>(lock);
  if (want) {
    esp_pm_lock_acquire(handle);
  } else {
    esp_pm_lock_release(handle);
  }
  held = want;
}

void HalPowerManager::updateNoLightSleepLock() {
  setPmLockHeld(pmNoSleepLock, pmNoSleepHeld, pmWifiBlocked || pmUsbBlocked);
}

void HalPowerManager::noteUsbConnected(const bool connected) {
  pmUsbBlocked = connected;
  updateNoLightSleepLock();
}

void HalPowerManager::noteWifiEnabled(const bool enabled) {
  pmWifiBlocked = enabled;
  updateNoLightSleepLock();
}
#endif

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
  // PM mode: "power saving" means releasing the interactive CPU_FREQ_MAX lock
  // and letting DFS drop to the floor with tickless light sleep between wakes.
  // Render Locks hold their own counted lock, so no mode bookkeeping is
  // consulted here — the PM subsystem arbitrates the concurrent requests.
  if (pmActiveLock == nullptr) {
    return;  // esp_pm unavailable; no manual DFS either (see begin())
  }
  // Sleep is also suppressed outright while WiFi is up. Capping the clock is
  // not enough on this path: unlike the manual-DFS branch below, where "no
  // power saving" simply meant staying at full speed, light sleep here would
  // halt the whole chip and drop the association. Sampled from the same
  // WiFi.getMode() read the branch above already does, on the same loop task
  // that owns the USB half of this lock, so the two never race.
  pmWifiBlocked = wifiMode != WIFI_MODE_NULL;
  updateNoLightSleepLock();
  if (enabled == pmActiveHeld) {  // the lock state is about to change
    if (enabled) {
      LOG_DBG("PWR", "Going to low-power mode (pm)");
    } else {
      LOG_DBG("PWR", "Restoring full speed (pm)");
    }
  }
  setPmLockHeld(pmActiveLock, pmActiveHeld, !enabled);
  isLowPower = enabled;
#else
  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
#endif
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
  // esp_pm's auto light sleep keeps ESP_SLEEP_WAKEUP_TIMER armed: esp_pm_configure()
  // with light_sleep_enable enables the RTC timer trigger globally, and every tickless
  // window re-arms it with that window's duration. esp_sleep's wakeup config is shared
  // with deep sleep — and the S3 deep-sleep path applies it with no minimum-duration
  // guard — so "off" would become a nap of whatever the last light-sleep window was,
  // and the device would boot-loop instead of powering down. Reconfiguring with light
  // sleep disabled is the documented way out: it disarms the timer source itself.
  // Done first, so nothing can re-arm the timer during the power-button-release wait
  // at the bottom of this function.
  esp_pm_config_t pmOff = {};
  pmOff.max_freq_mhz = normalFreq > 0 ? normalFreq : 240;
  pmOff.min_freq_mhz = pmOff.max_freq_mhz;
  pmOff.light_sleep_enable = false;
  esp_pm_configure(&pmOff);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);  // belt and braces
#endif
#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  if (gpio.isXteinkDevice()) {
    // GPIO13 gates the battery MOSFET on both Xteink C3 boards; driving it low
    // is the battery power-off (the SDK wake source still handles USB power).
    // Release any surviving pad hold first: hold_en survives deep sleep via
    // the SDK's deepSleep() (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and a held pad silently ignores the drive.
    gpio_hold_dis(XTEINK_C3_GPIO13);
    gpio_set_direction(XTEINK_C3_GPIO13, GPIO_MODE_OUTPUT);
    gpio_set_level(XTEINK_C3_GPIO13, 0);
    gpio_hold_en(XTEINK_C3_GPIO13);
  }
#endif

  // Hold every configured power-latch pin HIGH through deep sleep. These are
  // keep-alive enables (the X4 Pro's master peripheral rail on GPIO1, the
  // Sticky's PWR_HOLD/PWR_LOCK): deepSleep() isolates all pads
  // (esp_sleep_config_gpio_isolate), so a latch without an armed hold loses its
  // output driver and floats — on the X4 Pro the latch drops as soon as
  // external power leaves (serial/pogo adapter unplugged), and the next power-
  // button press cold-boots instead of fast-waking. holdPowerRails() asserted
  // the latches at boot but arms no sleep hold; arm it here instead. Skips
  // XTEINK_C3_GPIO13: it IS power.latch0 on the C3 Xteink boards, where the
  // block above drives it LOW on purpose (battery power-off).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || static_cast<gpio_num_t>(pin) == XTEINK_C3_GPIO13) continue;
    const auto g = static_cast<gpio_num_t>(pin);
    // Release any surviving pad hold first: a held pad silently ignores the
    // drive below (same trap as the GPIO13 block above).
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    gpio_hold_en(g);
  }

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  freeink::PowerManager::powerDownRailsForSleep();

#if FREEINK_DEVICE_PAPERMONO
  // Its power button is behind the M5PM1 PMIC rather than an ESP GPIO, so
  // normal GPIO deep sleep would have no wake source. Ask the PMIC to shut the
  // device down; a button click then restarts it through a cold boot.
  if (freeink::m5pm1::requestShutdown()) {
    delay(1000);  // allow the PMIC firmware time to drop power
  }
#endif

  // Waits for the power button to be physically released (so holding it doesn't
  // immediately wake the device again), then arms the wake source and sleeps.
  freeink::PowerManager::deepSleepUntilPowerButton();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t percent = 0;
    if (!battery.readPercentageChecked(percent)) {
      return _batteryCachedPercent;
    }
    _batteryCachedPercent = percent;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
HalPowerManager::Lock::Lock() {
  // esp_pm locks are counted, so concurrent Locks compose naturally — the
  // manual-DFS path's one-Lock-at-a-time limitation does not apply here. valid
  // records whether this instance took a reference, so the dtor returns exactly
  // the ones the ctor took.
  valid = powerManager.pmRenderLock != nullptr;
  if (valid) {
    esp_pm_lock_acquire(static_cast<esp_pm_lock_handle_t>(powerManager.pmRenderLock));
  }
}

HalPowerManager::Lock::~Lock() {
  if (valid) {
    esp_pm_lock_release(static_cast<esp_pm_lock_handle_t>(powerManager.pmRenderLock));
  }
}
#else
HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
#endif
