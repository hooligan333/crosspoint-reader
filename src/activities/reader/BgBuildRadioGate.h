#pragma once

#include <BlePageTurner.h>

#include "ChapterBuildGate.h"

// Fork (r8b, CROSSPOINT_BG_BUILD_TASK with the page-turner radio compiled in): what the
// core-0 background build task may do next to the Bluetooth page turner's radio. Pure, so
// it is host-tested in test/ble_radio_policy.
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

// STARTING a chapter layout (a speculative prebuild's fresh arm, or the resume of a
// parked partial): never next to a radio that runs, stops or starts (chapterBuildMayRun),
// nor while it asks for the heap (holdsHeap: also a start refused for memory and waiting
// to retry). A build that runs out of memory aborts the firmware, so this is the strict
// one, and a new parser is exactly what a refused start must not have to compete with.
inline bool buildMayRun(const bleturner::Status& radio, const bool radioHoldsHeap) {
  return chapterBuildMayRun(radio) && !radioHoldsHeap;
}

// A tick of a build ALREADY IN FLIGHT (the active section's buildSomeMore, a prebuild's
// Pump): only never next to a radio that runs, stops or starts. Not held back by a start
// refused for memory -- that build's parser already holds its heap, and freezing it would
// keep that heap for as long as the refusal lasts: every retry would see the same heap and
// be refused again, until the chapter boundary (r8b review m1). Letting it finish frees
// the parser, and the next retry sees that.
inline bool buildTickMayRun(const bleturner::Status& radio) { return chapterBuildMayRun(radio); }

// The idle steps that are not builds (HTML pre-inflate, image pre-decode): they check
// their own heap floors first and fail soft, so a radio that is steadily up -- its memory
// already taken, and visible to those floors -- does not stop them. They wait out a
// radio whose memory is in flux (starting, stopping), and a start refused for memory
// that is waiting to retry: those want the heap these steps would take.
//
// Advisory, not exclusion: the gate is checked BEFORE a step, not held during it, so a
// step can already be running when a radio start begins (r8b review m4). Both orders fail
// soft: a start that samples the step's transient heap is refused, which closes this gate
// (holdsHeap) until the step ends and the 5 s retry sees a quiet heap; a step that loses
// its heap to the radio fails its own allocation (malloc with fallbacks, PSRAM on S3).
inline bool idleWorkMayRun(const bleturner::Status& radio, const bool radioHoldsHeap) {
  if (radio.starting || radio.stopping) return false;
  return radio.running || !radioHoldsHeap;
}

}  // namespace bg_radio_gate
