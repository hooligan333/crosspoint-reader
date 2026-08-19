#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  enum class Mode { Blocking, Try };
  explicit RenderLock(Mode mode = Mode::Blocking);
  explicit RenderLock(Activity&);  // Activity argument retained for compatibility.
  // Non-blocking acquisition for background and deferrable work. A background
  // task must NEVER park on the rendering mutex: ActivityManager calls onExit()
  // while holding the RenderLock, and onExit joins the background build task —
  // a task blocked inside the blocking ctor at that moment would deadlock the
  // exit. The loop task's deferrable chores use it too: a blocking acquire that
  // loses the peek→acquire race parks the loop behind an entire page render,
  // stalling input polling for its duration. Check locked() before touching
  // guarded state; on false, back off and retry the next pass.
  struct TryAcquire {};
  explicit RenderLock(TryAcquire);
  bool locked() const { return isLocked; }
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  bool ownsLock() const { return isLocked; }
  void unlock();
  static bool peek();
};
