#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
  // esp_pm handles, created in begin() once esp_pm_configure() succeeds
  // (nullptr = PM unavailable and every user below degrades to a no-op).
  // esp_pm locks are COUNTED, so concurrent holders compose naturally and the
  // manual-DFS path's one-Lock-at-a-time limitation does not apply here. The
  // price is that every acquire must be matched by exactly one release: the two
  // edge-managed locks go through setPmLockHeld(), which owns that pairing, and
  // pmRenderLock is paired by HalPowerManager::Lock's ctor/dtor.
  void* pmActiveLock = nullptr;   // esp_pm_lock_handle_t, CPU_FREQ_MAX, held while interactive
  void* pmRenderLock = nullptr;   // CPU_FREQ_MAX, backs HalPowerManager::Lock instances
  void* pmNoSleepLock = nullptr;  // NO_LIGHT_SLEEP, held while WiFi or a USB host needs the chip awake
  bool pmActiveHeld = false;      // pmActiveLock is currently acquired
  bool pmNoSleepHeld = false;     // pmNoSleepLock is currently acquired
  bool pmWifiBlocked = false;     // WiFi is up (sampled by setPowerSaving)
  bool pmUsbBlocked = false;      // a USB host is attached (see noteUsbConnected)
#if defined(FREEINK_CAP_BLE_HID_HOST) && FREEINK_CAP_BLE_HID_HOST
  bool pmBleBlocked = false;  // the page turner's radio is up or changing state (see noteBleActive)
#endif
  // Acquire/release `lock` so that `held` mirrors `want`, at most once per
  // transition. Single point of truth for the counted-lock balance.
  static void setPmLockHeld(void* lock, bool& held, bool want);
  // Re-evaluate pmNoSleepLock from pmWifiBlocked || pmUsbBlocked (|| pmBleBlocked).
  void updateNoLightSleepLock();
#endif

 public:
#if BOARD_HAS_PSRAM
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  // wakeOnHeldButton: arm the wake without waiting for the power button's
  // release, so a press that is still down wakes the device straight back up
  // through the normal power-button wake path.
  void startDeepSleep(HalGPIO& gpio, bool wakeOnHeldButton = false) const;

#if FREEINK_DEVICE_PICCO
  // While USB powers the device, light-sleep instead of deep sleep (deep sleep
  // cannot observe the charger), calling onStatus whenever the charging state
  // or percentage changes. Returns true when the power button ended the hold,
  // false when USB went away or was never present.
  bool holdWhileUsbPowered(HalGPIO& gpio, void (*onStatus)(bool charging, uint16_t percent)) const;
#endif

#ifdef CROSSPOINT_AUTO_LIGHT_SLEEP
  // Report the USB host state so automatic light sleep can be suppressed while
  // one is attached: a sleep window drops an enumerated CDC link, which would
  // make the serial console unusable exactly when it is wanted. Call on every
  // plug/unplug edge and once at boot with the initial state — this is a
  // level, not an event, and a missed edge leaves the lock in the wrong state.
  //
  // Note this is a SECOND line of defence, not the only one: the env's
  // CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y makes the USB-Serial-JTAG driver hold
  // its own no-light-sleep lock while enumerated, which is what actually covers
  // the X4 Pro (whose BoardConfig leaves usbDetect unassigned, so
  // HalGPIO::isUsbConnected() can only ever answer false there) and setup(),
  // which runs before the first call to this.
  void noteUsbConnected(bool connected);
  // Refresh the WiFi half of the no-light-sleep lock. setPowerSaving() samples
  // WiFi too, but is not called on every loop pass; loop() calls this one
  // unconditionally so the lock can never go stale between mode changes.
  // loopTask-only, like every other writer of the pm* flags.
  void noteWifiEnabled(bool enabled);
#if defined(FREEINK_CAP_BLE_HID_HOST) && FREEINK_CAP_BLE_HID_HOST
  // Fork (r8b): the Bluetooth half of the same lock. loop() passes
  // ble_light_sleep::blocksLightSleep() every pass: true while the page
  // turner's radio is up or starting/stopping, false once it is fully stopped
  // (and then this half is as if the radio did not exist). Like the WiFi half
  // it is a level refreshed per pass, not an edge.
  //
  // With the radio compiled in, setPowerSaving() can also run on the render
  // task (the page turner's Host::holdFullSpeed, from a chapter build that
  // stops the radio), and it re-evaluates this lock too. So in these builds
  // the three note*() setters take modeMutex like setPowerSaving() does, which
  // keeps the counted lock's acquire/release pairing single-writer.
  void noteBleActive(bool active);
#endif
#ifdef CROSSPOINT_PM_STATS
  // Light-sleep residency counters (entries + total slept ms since boot),
  // maintained by the idle-task sleep-exit callback. See HalPowerManager.cpp.
  void getLightSleepStats(uint32_t& entries, uint32_t& sleptMs) const;
#endif
#endif

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
