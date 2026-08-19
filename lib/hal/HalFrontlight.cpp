#include "HalFrontlight.h"

#include <Logging.h>

#if defined(CROSSPOINT_AUTO_LIGHT_SLEEP) && !defined(FREEINK_FRONTLIGHT_LS)
#include <esp_pm.h>
#endif

HalFrontlight HalFrontlight::instance;

namespace {

#if defined(CROSSPOINT_AUTO_LIGHT_SLEEP) && !defined(FREEINK_FRONTLIGHT_LS)
// Automatic light sleep without an RC_FAST-clocked LEDC: the APB-fed PWM stops
// during every sleep window, so a lit frontlight would visibly blink dark as
// the idle loop sleeps. Hold a no-light-sleep PM lock while the light is
// actually emitting — no sleep savings while lit, the deliberate trade for
// boards that cannot use the SDK's FREEINK_FRONTLIGHT_LS (which keeps the PWM
// running through sleep and makes this whole path compile away). Called only
// from the main loop task, so the lazy create and the held flag need no
// synchronization; every acquire is matched by exactly one release because the
// state is transition-edged.
void frontlightPmLock(const bool emitting) {
  static esp_pm_lock_handle_t lock = nullptr;
  static bool held = false;
  if (lock == nullptr && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "cpLight", &lock) != ESP_OK) {
    return;
  }
  if (emitting == held) {
    return;
  }
  if (emitting) {
    esp_pm_lock_acquire(lock);
  } else {
    esp_pm_lock_release(lock);
  }
  held = emitting;
}
#else
inline void frontlightPmLock(bool) {}
#endif

}  // namespace

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  if (!manager.present()) return;

  manager.begin();
  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
  frontlightPmLock(lit && lastBrightness > 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  if (lit) manager.setBrightness(lastBrightness);
  frontlightPmLock(lit && lastBrightness > 0);
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  manager.setBrightness(lit ? lastBrightness : 0);
  frontlightPmLock(lit && lastBrightness > 0);
}
