#pragma once

#include <atomic>
#include <cstdint>

// Fixed-scalar active-reading summary. The owner reports turn outcomes before
// render and completed pages after render; no allocation or I/O occurs here.
class ReaderSession {
 public:
  void noteTurn(bool forward, bool succeeded);
  void onRenderComplete(uint32_t monotonicMs, int64_t trustedEpoch, int progressBp);
  void reset();

  bool isEmitWorthy() const { return startEpoch > 0 && duration >= 10 && endTime() > startEpoch; }
  int64_t startTime() const { return startEpoch; }
  int64_t endTime() const { return startEpoch > 0 ? startEpoch + duration : 0; }
  uint32_t durationSeconds() const { return duration; }
  uint16_t startProgressBp() const { return startProgress; }
  uint16_t endProgressBp() const { return endProgress; }

 private:
  static uint16_t clampProgress(int progressBp);

  uint32_t anchorMs = 0;
  int64_t anchorEpoch = 0;
  int64_t startEpoch = 0;
  uint32_t duration = 0;
  uint16_t anchorProgress = 0;
  uint16_t startProgress = 0;
  uint16_t endProgress = 0;
  bool hasAnchor = false;
  // The one field that crosses tasks: the loop task writes it in noteTurn()
  // WITHOUT the render lock (a blocking acquire there parks page turns behind
  // the fork's background section builds); everything else is only touched on
  // the render side under the lock.
  std::atomic<bool> pendingForward{false};
};
