#include "segarecomp/machine/genesis/z80_materialization.hpp"

namespace segarecomp::machine::genesis::z80 {
namespace {

[[nodiscard]] bool is_candidate(PassOutcome outcome) noexcept {
  return outcome == PassOutcome::window_complete || outcome == PassOutcome::guest_stop || outcome == PassOutcome::guest_complete;
}

[[nodiscard]] Failure terminal_failure(PassOutcome outcome) noexcept {
  switch (outcome) {
    case PassOutcome::budget_exhausted:
    case PassOutcome::wall_timeout:
      return Failure::materialization_budget_exhausted;
    case PassOutcome::z80_code_mismatch:
      return Failure::z80_code_mismatch;
    case PassOutcome::z80_stop:
      return Failure::z80_execution_unsupported;
    default:
      return Failure::materialization_pass_failed;
  }
}

[[nodiscard]] bool starts_with(const std::vector<PassEpoch>& longer, const std::vector<PassEpoch>& prefix) {
  if (prefix.size() > longer.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i)
    if (!(longer[i] == prefix[i])) return false;
  return true;
}

}  // namespace

const char* pass_outcome_name(PassOutcome outcome) noexcept {
  switch (outcome) {
    case PassOutcome::window_complete: return "window_complete";
    case PassOutcome::guest_stop: return "guest_stop";
    case PassOutcome::guest_complete: return "guest_complete";
    case PassOutcome::unknown_image: return "unknown_image";
    case PassOutcome::budget_exhausted: return "budget_exhausted";
    case PassOutcome::wall_timeout: return "wall_timeout";
    case PassOutcome::z80_code_mismatch: return "z80_code_mismatch";
    case PassOutcome::z80_stop: return "z80_stop";
    case PassOutcome::failed: return "failed";
  }
  return "failed";
}

const char* failure_name(Failure failure) noexcept {
  switch (failure) {
    case Failure::none: return "none";
    case Failure::z80_image_bound_exceeded: return "z80_image_bound_exceeded";
    case Failure::materialization_budget_exhausted: return "materialization_budget_exhausted";
    case Failure::materialization_nondeterministic: return "materialization_nondeterministic";
    case Failure::z80_image_compile_failed: return "z80_image_compile_failed";
    case Failure::materialization_no_convergence: return "materialization_no_convergence";
    case Failure::z80_code_mismatch: return "z80_code_mismatch";
    case Failure::z80_execution_unsupported: return "z80_execution_unsupported";
    case Failure::materialization_pass_failed: return "materialization_pass_failed";
  }
  return "materialization_pass_failed";
}

MaterializationSummary materialize(Registry& registry, PassRunner& runner) {
  MaterializationSummary summary;
  const auto fail = [&](Failure failure) {
    summary.failure = failure;
    summary.images = registry.images().size();
    return summary;
  };
  std::vector<PassEpoch> previous;  // the epoch list of the previous (unknown-image) run
  bool candidate_found = false;
  PassObservation candidate;
  for (std::size_t run = 0; run < kMaxDiscoveryRuns && !candidate_found; ++run) {
    if (!runner.prepare(registry)) return fail(Failure::z80_image_compile_failed);
    PassObservation observed = runner.run();
    ++summary.discovery_runs;
    ++summary.total_runs;
    // Determinism of the prefix: the previous run's epochs (its last one is the image just registered) recur identically.
    if (!starts_with(observed.epochs, previous)) return fail(Failure::materialization_nondeterministic);
    if (is_candidate(observed.outcome)) {
      candidate = std::move(observed);
      candidate_found = true;
      break;
    }
    if (observed.outcome != PassOutcome::unknown_image) return fail(terminal_failure(observed.outcome));
    if (!observed.unknown || observed.epochs.empty()) return fail(Failure::materialization_pass_failed);
    std::uint32_t bound = 0;
    const AddOutcome added = registry.add(*observed.unknown, bound);
    if (added == AddOutcome::bound_exceeded) return fail(Failure::z80_image_bound_exceeded);
    // The image must be new and its C++-derived signature must equal the one the generated program computed.
    if (added != AddOutcome::added || registry.images().back().signature != observed.epochs.back().signature)
      return fail(Failure::materialization_pass_failed);
    previous = std::move(observed.epochs);
  }
  if (!candidate_found) return fail(Failure::materialization_no_convergence);

  // The confirming run: the same program, run again, must reproduce the outcome class and the ordered epoch identities.
  const PassObservation confirmed = runner.run();
  ++summary.total_runs;
  if (!is_candidate(confirmed.outcome)) {
    return fail(confirmed.outcome == PassOutcome::unknown_image || confirmed.outcome == PassOutcome::budget_exhausted ||
                        confirmed.outcome == PassOutcome::wall_timeout
                    ? Failure::materialization_nondeterministic
                    : terminal_failure(confirmed.outcome));
  }
  if (confirmed.outcome != candidate.outcome || confirmed.epochs != candidate.epochs || confirmed.frames != candidate.frames)
    return fail(Failure::materialization_nondeterministic);
  summary.images = registry.images().size();
  summary.epochs = confirmed.epochs.size();
  summary.frames = confirmed.frames;
  summary.end = confirmed.outcome;
  return summary;
}

}  // namespace segarecomp::machine::genesis::z80
