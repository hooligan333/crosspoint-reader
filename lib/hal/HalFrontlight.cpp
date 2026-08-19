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
// from applyLit(), i.e. from the main loop task, so the lazy create and the
// held flag need no synchronization; every acquire is matched by exactly one
// release because the state is transition-edged.
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

// Night Light ladder: SDK perceptual levels whose quadratic curve lands on
// duties {8, 6, 4, 2, 1} at a 10-bit PWM (the X4 Pro profile) — i.e. about
// 0.8 / 0.6 / 0.4 / 0.2 / 0.1 % of full, the spread validated on hardware.
// Level 1 is the smallest non-zero duty on any LEDC board, so the ladder stays
// monotonic at other PWM resolutions even though the exact percentages shift.
// Not valid for the PM1 PMIC path — see supportsNightLight().
constexpr uint8_t DIM_LEVELS[HalFrontlight::DIM_STEP_COUNT] = {23, 19, 15, 9, 1};
}  // namespace

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on, const uint8_t dimStep) {
  if (!manager.present()) return;

  manager.begin();
  lastBrightness = brightness > 100 ? 100 : brightness;
  dimStepIdx = dimStep > DIM_STEP_COUNT ? DIM_STEP_COUNT : dimStep;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  applyLit();
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% dim=%u %s", lastBrightness, manager.colorTemperature(), dimStepIdx,
          lit ? "on" : "off");
}

void HalFrontlight::applyLit() {
  if (!lit) {
    manager.setBrightness(0);
  } else if (dimStepIdx > 0) {
    manager.setBrightnessLevel(DIM_LEVELS[dimStepIdx - 1]);
  } else {
    manager.setBrightness(lastBrightness);
  }
  // Single funnel for the lit state, so the sleep-blink guard has exactly one
  // call site. A dim step is always a non-zero duty (the ladder bottoms out at
  // SDK level 1), so "emitting" is lit with either a step or a non-zero percent.
  frontlightPmLock(lit && (dimStepIdx > 0 || lastBrightness > 0));
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  dimStepIdx = 0;
  if (lit) applyLit();
}

void HalFrontlight::setDimStep(const uint8_t step) {
  dimStepIdx = step > DIM_STEP_COUNT ? DIM_STEP_COUNT : step;
  if (lit) applyLit();
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  applyLit();
}
