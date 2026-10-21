#pragma once

#include <BlePageTurner.h>

#include "ChapterBuildGate.h"

// Fork (r8b, CROSSPOINT_BG_BUILD_TASK with the page-turner radio compiled in): what the
// core-0 background build task may do next to the Bluetooth page turner's radio. Pure, so
// it is host-tested in test/ble_chapter_build_gate.
//
// Upstream builds chapters on its loop task and arbitrates with the radio in two places:
// a build the reader is WAITING on (renderBook) calls makeRoomForChapterBuild(), which
// stops the radio and keeps it down (Why::BuildNeedsHeap) until that chapter is built and
// its page shown; background work (backgroundBuildWanted(), the deferred partial
// extension) simply does not run while bleturner::holdsHeap() -- the radio holds the heap
// or is asking for it. The fork's task runs on the other core and never waits (it must
// not block core 0, and onExit() joins it), so it takes the second half of that contract:
// it SKIPS a tick the radio forbids and tries again on a later pass. A build the reader
// needs still reaches renderBook(), whose makeRoomForChapterBuild() stops the radio;
// that makes holdsHeap() false (radio down and held down, deferral cleared), and these
// gates open again for the rest of the chapter.
namespace bg_radio_gate {

// A chapter build tick (the active section's, or a speculative prebuild's start or
// tick): never next to a radio that runs, stops or starts (chapterBuildMayRun), nor
// while it asks for the heap (holdsHeap: also a start refused for memory and waiting to
// retry). A build that runs out of memory aborts the firmware, so this is the strict one.
inline bool buildMayRun(const bleturner::Status& radio, const bool radioHoldsHeap) {
  return chapterBuildMayRun(radio) && !radioHoldsHeap;
}

// The idle steps that are not builds (HTML pre-inflate, image pre-decode): they check
// their own heap floors first and fail soft, so a radio that is steadily up -- its memory
// already taken, and visible to those floors -- does not stop them. They wait out a
// radio whose memory is in flux (starting, stopping), and a start refused for memory
// that is waiting to retry: those want the heap these steps would take.
inline bool idleWorkMayRun(const bleturner::Status& radio, const bool radioHoldsHeap) {
  if (radio.starting || radio.stopping) return false;
  return radio.running || !radioHoldsHeap;
}

}  // namespace bg_radio_gate
