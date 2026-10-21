// Fork (r8b): the two decisions the combo pair makes about the Bluetooth page turner's
// radio -- when it keeps the chip out of automatic light sleep (util/BleLightSleepPolicy.h,
// main.cpp -> HalPowerManager::noteBleActive) and what the core-0 background build task may
// run next to it (activities/reader/BgBuildRadioGate.h).
#include <gtest/gtest.h>

#include <initializer_list>

#include "activities/reader/BgBuildRadioGate.h"
#include "util/BleLightSleepPolicy.h"

namespace {

bleturner::Status stopped() { return bleturner::Status{}; }

// Every field that describes a radio that is not fully stopped.
enum class Active { Starting, Running, Stopping, Scanning, Connecting, Connected };

bleturner::Status with(const Active a) {
  auto s = stopped();
  s.compiledIn = true;
  switch (a) {
    case Active::Starting:
      s.starting = true;
      break;
    case Active::Running:
      s.running = true;
      break;
    case Active::Stopping:
      s.stopping = true;
      break;
    case Active::Scanning:
      s.scanning = true;
      break;
    case Active::Connecting:
      s.connecting = true;
      break;
    case Active::Connected:
      s.connected = true;
      break;
  }
  return s;
}

constexpr std::initializer_list<Active> kAllActive = {Active::Starting,   Active::Running,    Active::Stopping,
                                                      Active::Scanning,   Active::Connecting, Active::Connected};

// HalPowerManager's no-light-sleep lock as the loop drives it: one counted esp_pm lock,
// acquired/released only on a change of the composed want (setPmLockHeld), with the WiFi,
// USB and (r8b) Bluetooth halves ORed (updateNoLightSleepLock).
struct NoSleepLock {
  int count = 0;
  bool held = false;
  bool wifi = false;
  bool usb = false;
  bool ble = false;
  void update() {
    const bool want = wifi || usb || ble;
    if (want == held) return;
    count += want ? 1 : -1;
    held = want;
  }
  void noteBle(const bool holdsCpu, const bleturner::Status& s) {
    ble = ble_light_sleep::blocksLightSleep(holdsCpu, s);
    update();
  }
};

}  // namespace

// ---- light sleep -----------------------------------------------------------------

TEST(BleLightSleepPolicy, FullyStoppedRadioBlocksNothing) {
  EXPECT_FALSE(ble_light_sleep::blocksLightSleep(false, stopped()));
  // Compiled in, stopped for idleness, or refused for memory: all a stopped radio.
  auto s = stopped();
  s.compiledIn = true;
  EXPECT_FALSE(ble_light_sleep::blocksLightSleep(false, s));
  s.idleStopped = true;
  EXPECT_FALSE(ble_light_sleep::blocksLightSleep(false, s));
  s.idleStopped = false;
  s.readerDeferred = true;
  EXPECT_FALSE(ble_light_sleep::blocksLightSleep(false, s));
  EXPECT_FALSE(ble_light_sleep::radioActive(s));
}

TEST(BleLightSleepPolicy, EveryUpOrChangingStateBlocks) {
  for (const Active a : kAllActive) {
    EXPECT_TRUE(ble_light_sleep::radioActive(with(a))) << static_cast<int>(a);
    EXPECT_TRUE(ble_light_sleep::blocksLightSleep(false, with(a))) << static_cast<int>(a);
    EXPECT_TRUE(ble_light_sleep::blocksLightSleep(true, with(a))) << static_cast<int>(a);
  }
}

// Host::holdFullSpeed(true) runs before the start task exists (startAsync), so the hold can
// lead the status by a moment; it alone keeps sleep off.
TEST(BleLightSleepPolicy, FullSpeedHoldAloneBlocks) { EXPECT_TRUE(ble_light_sleep::blocksLightSleep(true, stopped())); }

// One session: a start, a link, the idle stop, then nothing -- the lock is taken once and
// returned once, and what is left afterwards is exactly the r8 composition.
TEST(BleLightSleepPolicy, SessionTakesAndReturnsTheLockOnce) {
  NoSleepLock lock;
  lock.noteBle(false, stopped());
  EXPECT_EQ(lock.count, 0);

  lock.noteBle(true, stopped());  // startAsync: hold first
  EXPECT_EQ(lock.count, 1);
  auto s = with(Active::Starting);
  lock.noteBle(true, s);
  s.starting = false;
  s.running = true;
  s.scanning = true;
  lock.noteBle(true, s);
  s.scanning = false;
  s.connected = true;
  lock.noteBle(true, s);
  EXPECT_EQ(lock.count, 1);
  EXPECT_TRUE(lock.held);

  s.connected = false;  // link lost; idle timeout then stops the radio
  s.running = false;
  s.stopping = true;
  lock.noteBle(true, s);
  EXPECT_EQ(lock.count, 1);
  s.stopping = false;
  s.idleStopped = true;
  lock.noteBle(false, s);  // suspend(): end() finished, holdFullSpeed(false)
  EXPECT_EQ(lock.count, 0);
  EXPECT_FALSE(lock.held);
}

// The Bluetooth half composes with the others: its release never drops a lock WiFi or USB
// still wants, and a radio stop with WiFi up leaves the lock where r8 would have it.
TEST(BleLightSleepPolicy, ReleaseRestoresTheOtherHalves) {
  NoSleepLock lock;
  lock.wifi = true;
  lock.update();
  EXPECT_EQ(lock.count, 1);
  lock.noteBle(true, with(Active::Running));
  EXPECT_EQ(lock.count, 1);  // already held: counted once
  lock.noteBle(false, stopped());
  EXPECT_TRUE(lock.held);  // WiFi still wants it
  EXPECT_EQ(lock.count, 1);
  lock.wifi = false;
  lock.update();
  EXPECT_EQ(lock.count, 0);

  // A start that is refused leaves nothing up: hold dropped by finishAttempt().
  lock.noteBle(true, with(Active::Starting));
  EXPECT_EQ(lock.count, 1);
  auto refused = stopped();
  refused.readerDeferred = true;
  lock.noteBle(false, refused);
  EXPECT_EQ(lock.count, 0);
}

// ---- background build task -------------------------------------------------------

TEST(BgBuildRadioGate, BuildsRunNextToAStoppedRadio) {
  EXPECT_TRUE(bg_radio_gate::buildMayRun(stopped(), false));
  auto idle = stopped();
  idle.idleStopped = true;
  EXPECT_TRUE(bg_radio_gate::buildMayRun(idle, false));
  EXPECT_TRUE(bg_radio_gate::idleWorkMayRun(stopped(), false));
  EXPECT_TRUE(bg_radio_gate::idleWorkMayRun(idle, false));
}

TEST(BgBuildRadioGate, BuildsSkipEveryRadioThatHoldsOrTakesTheHeap) {
  for (const Active a : kAllActive) {
    if (a == Active::Scanning || a == Active::Connecting || a == Active::Connected) continue;  // only with running
    EXPECT_FALSE(bg_radio_gate::buildMayRun(with(a), true)) << static_cast<int>(a);
    // Even with the preference off (holdsHeap() false), a radio up is a radio up.
    EXPECT_FALSE(bg_radio_gate::buildMayRun(with(a), false)) << static_cast<int>(a);
  }
  // A start refused for memory, waiting to retry: it asks for the heap.
  auto deferred = stopped();
  deferred.readerDeferred = true;
  EXPECT_FALSE(bg_radio_gate::buildMayRun(deferred, true));
}

TEST(BgBuildRadioGate, IdleWorkWaitsOnlyForAHeapInFlux) {
  auto up = with(Active::Running);
  up.connected = true;
  EXPECT_TRUE(bg_radio_gate::idleWorkMayRun(up, true));  // steady: its memory is taken
  EXPECT_FALSE(bg_radio_gate::idleWorkMayRun(with(Active::Starting), true));
  EXPECT_FALSE(bg_radio_gate::idleWorkMayRun(with(Active::Stopping), true));
  auto deferred = stopped();
  deferred.readerDeferred = true;
  EXPECT_FALSE(bg_radio_gate::idleWorkMayRun(deferred, true));
}

// The deferred build is not lost: renderBook()'s makeRoomForChapterBuild() stops the radio and
// holds it down for the build (BuildRelease::Released; the deferral is cleared), so holdsHeap()
// turns false and the task's next pass builds.
TEST(BgBuildRadioGate, ARadioStoppedForTheBuildReopensTheGate) {
  auto up = with(Active::Running);
  up.connected = true;
  EXPECT_FALSE(bg_radio_gate::buildMayRun(up, true));
  auto released = stopped();
  released.compiledIn = true;
  EXPECT_TRUE(chapterBuildMayRun(released));
  EXPECT_TRUE(bg_radio_gate::buildMayRun(released, false));
  EXPECT_TRUE(bg_radio_gate::idleWorkMayRun(released, false));
}

// The two gates never disagree in the direction that matters: whenever a build may run, the
// idle steps may too.
TEST(BgBuildRadioGate, BuildGateIsTheStricter) {
  for (int bits = 0; bits < (1 << 9); ++bits) {
    bleturner::Status s{};
    s.starting = bits & 1;
    s.running = bits & 2;
    s.stopping = bits & 4;
    s.scanning = bits & 8;
    s.connecting = bits & 16;
    s.connected = bits & 32;
    s.idleStopped = bits & 64;
    s.readerDeferred = bits & 128;
    const bool holds = bits & 256;
    if (bg_radio_gate::buildMayRun(s, holds)) {
      EXPECT_TRUE(bg_radio_gate::idleWorkMayRun(s, holds)) << bits;
    }
  }
}
