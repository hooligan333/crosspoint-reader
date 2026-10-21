#pragma once

#include <BlePageTurner.h>

// Fork (r8b): when the Bluetooth page turner keeps the chip out of automatic
// light sleep (CROSSPOINT_AUTO_LIGHT_SLEEP builds that also have the radio,
// FREEINK_CAP_BLE_HID_HOST). Pure, so it is host-tested in
// test/ble_radio_policy; main.cpp's loop() feeds the answer to
// HalPowerManager::noteBleActive() once per pass, the same way it refreshes the
// WiFi half of the no-light-sleep lock.
//
// The policy ("option 1", Erica 2026-10-21): a radio that is FULLY STOPPED
// changes nothing -- the device light-sleeps exactly as a build without the
// radio does. A radio that is up, or on its way up or down, blocks light sleep
// for as long as that lasts. Light sleep with a live link is deliberately not
// attempted.
namespace ble_light_sleep {

// Any radio state other than fully stopped. `starting` is the start in flight
// (its own task), `stopping` a teardown that has not finished; scanning,
// connecting and connected only ever occur with `running` set, and are named so
// that the answer does not depend on that.
inline bool radioActive(const bleturner::Status& s) {
  return s.starting || s.running || s.stopping || s.scanning || s.connecting || s.connected;
}

// holdsCpu is the page turner's own full-speed hold (Host::holdFullSpeed): set
// when a start begins, kept while the stack runs, dropped once a start fails
// with nothing left up or a stop finishes. It spans the same window as
// radioActive(); both are consulted, so either one alone keeps sleep off.
// compiledIn, idleStopped and readerDeferred describe a stopped radio and never
// block.
inline bool blocksLightSleep(const bool holdsCpu, const bleturner::Status& s) { return holdsCpu || radioActive(s); }

}  // namespace ble_light_sleep
