#pragma once

// Failure bookkeeping for the core-0 background build task (CROSSPOINT_BG_BUILD_TASK),
// kept free of reader state so the host tests can drive it.
//
// Upstream #4040's loop-task build tick latches a spine on its FIRST failed build step: from then
// on that spine is built only as far as the reader goes, so a parse error that would recur cannot
// rebuild-and-fail in a loop. The fork's task also builds while a child screen (dictionary,
// footnotes, frontlight panel, go-to-percent) is stacked on the reader -- upstream's tick never
// runs there -- and those screens hold heap, so a transient OOM or SD hiccup is likelier. A latch
// costs the rest of the chapter its background build, its prebuild and the page turner's radio
// release, so the task latches on the SECOND failure of the same spine instead: the first one
// takes the ordinary reset-and-rebuild path, which a transient error survives and a deterministic
// one fails again almost at once.
namespace bg_build_fail {

// The task's failure report names the spine it was building (read under the RenderLock it held
// for the step). The loop task acts on it only while that spine is still the open one: the reader
// can have turned to another chapter between the failed step and the loop pass that consumes the
// report, and that section must neither be reset nor latched.
inline bool appliesTo(const int failedSpine, const bool haveSection, const int currentSpine) {
  return haveSection && failedSpine >= 0 && failedSpine == currentSpine;
}

class Latch {
 public:
  // A failure of `spine` that appliesTo() the open section. True: latch it now.
  bool onFailure(const int spine) {
    if (lastFailedSpine_ == spine) {
      lastFailedSpine_ = -1;
      return true;
    }
    lastFailedSpine_ = spine;
    return false;
  }
  // The build of `spine` completed: an earlier failure was transient.
  void onBuildComplete(const int spine) {
    if (lastFailedSpine_ == spine) lastFailedSpine_ = -1;
  }
  // The reader is in `spine`: a remembered failure of another chapter is forgotten.
  void onSpine(const int spine) {
    if (lastFailedSpine_ >= 0 && lastFailedSpine_ != spine) lastFailedSpine_ = -1;
  }
  int lastFailedSpine() const { return lastFailedSpine_; }

 private:
  int lastFailedSpine_ = -1;
};

}  // namespace bg_build_fail
