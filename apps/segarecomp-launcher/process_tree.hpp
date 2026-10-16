#pragma once

// SEG-069-T001: shutting down a child process TOGETHER with everything it started. `segarecomp build` runs the C compiler (and its own
// helpers) as grandchildren; killing only the direct child would leave them running and writing into the attempt's folder. Nothing
// here knows what the processes are: it is the same primitive for any spawned tool.
//
//   POSIX   : the descendant set is frozen (SIGSTOP, so it cannot fork again), found from the parent links of the process table, and then
//             killed with SIGKILL. No process group/session is needed, so the SDL spawn path stays unchanged.
//   Windows : the child is assigned to a kill-on-close Job Object right after it is created; terminating the job ends the whole tree.

#include <SDL3/SDL.h>

#include <memory>

namespace launcher {

class ProcessTree {
public:
  ProcessTree();
  ~ProcessTree();
  ProcessTree(const ProcessTree &) = delete;
  ProcessTree &operator=(const ProcessTree &) = delete;

  // Registers the freshly created root process (call once, right after creation, before it has started anything).
  void attach(SDL_Process *process);
  // Terminates the root and every descendant. Idempotent and safe before attach (then it is a no-op). Does not reap the root: the owner
  // still calls SDL_WaitProcess/SDL_DestroyProcess.
  void terminate();
  // The root has been reaped: from now on terminate() does nothing (its pid may be reused by an unrelated process).
  void release();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace launcher
