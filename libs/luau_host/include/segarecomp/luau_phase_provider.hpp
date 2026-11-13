#pragma once

// SEG-138-T001/T002 (ADR 0297): the Luau binding of the ONE mod phase scheduler. A Luau mod becomes an ordinary ABI 2 provider table (segarecomp_mod.h): the scheduler calls its `phase` entry exactly as it
// calls a native provider's, and the Luau callbacks (reset, frame_begin, input, post_input, frame_end, render) run inside the scheduler's phases through the same host services (memory, input, log). The
// scheduler is therefore the single authority for phase legality, ranges, alignment, budgets, staging and commit; this adapter only translates. It has no platform dependency: the scheduler's fault entry is
// injected as a plain function pointer.
#include "segarecomp/luau_api.hpp"
#include "segarecomp/luau_host.hpp"

#include <segarecomp_mod.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace segarecomp::luau {

class PhaseProvider {
 public:
  // Called when a Luau callback fails for a reason the scheduler did not see itself (a script error, budget, memory, stack or watchdog fault). `cause` is a static string (fault_name).
  using FaultSink = void (*)(const char *cause);

  // Loads `compiled` into a VM created for `identity` and prepares the provider table. Null (with `error` set to the stable class) when the VM cannot be created or the main chunk faults.
  [[nodiscard]] static std::unique_ptr<PhaseProvider> create(std::string_view identity, const CompiledMod &compiled, const Limits &limits, const HostConfig &host, FaultSink sink, std::string &error);
  ~PhaseProvider();
  PhaseProvider(const PhaseProvider &) = delete;
  PhaseProvider &operator=(const PhaseProvider &) = delete;

  [[nodiscard]] const SegaModProvider *provider() const;
  [[nodiscard]] void *state();
  // The phases whose callback the script defines (bit 1 << SegaModPhase): the scheduler's phase registration.
  [[nodiscard]] std::uint32_t phase_mask() const;
  // The catalogue ids of the capability names the VM was given (bitset by id).
  void capabilities(std::uint64_t out[2]) const;
  // The overlay requests of the last successful `render` callback (cleared when a render faults or is not defined).
  [[nodiscard]] const std::vector<OverlayOp> &overlay() const;
  // True once a watchdog fault happened: the run is host-timing dependent and not replay-verifiable (phases section 7).
  [[nodiscard]] bool nondeterministic() const;
  [[nodiscard]] Fault fault() const;

  struct Impl;   // implementation detail (named so the table's entry function can reach it)

 private:
  explicit PhaseProvider(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace segarecomp::luau
