#pragma once

// Build-time Z80 image materialization fixed point (SEG-032-T008; ADR 0073 decision 4-5, contract sections 5-7).
//
// The loop is platform logic only: it does not know how a program is compiled or run. A `PassRunner` supplies those two
// operations ("build the program for this registry" and "run it headless from reset, with no input, under the bounds"), the
// production runner is `segarecomp build`'s process-based one and the tests use scripted runners. Flow:
//   registry (initially empty) -> prepare -> run -> unknown image: register it, prepare again, run again, ...
//   a run that ends with no unknown epoch is the candidate; one further run must reproduce the same outcome class and the same
//   ordered epoch identities exactly (the confirming run). Every bound is a constant, every violation a typed failure, and a
//   failure leaves no program (the caller never links the final executable).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "segarecomp/machine/genesis/m68k_copy_alias.hpp"
#include "segarecomp/machine/genesis/z80_images.hpp"

namespace segarecomp::machine::genesis::z80 {

// Bounds, frozen from the measured workloads (ADR 0073 T008 record). kMaxImages (z80_images.hpp) is the image bound.
inline constexpr std::size_t kMaxImagesCeiling = 16;
inline constexpr std::uint32_t kObservationFrames = 600;          // virtual frames of guest time observed per run
inline constexpr std::uint32_t kPassInstructionBudget = 100'000'000;  // retired dispatches per run (600 frames need under 20 M)
inline constexpr std::uint32_t kPassWallSeconds = 120;            // wall timeout per run
inline constexpr std::size_t kMaxDiscoveryRuns = kMaxImages + 1;  // runs that may end in an unknown image, plus the completing run
static_assert(kObservationFrames >= 600, "the observation window must cover the boot-time handoffs of the authorized workloads");
static_assert(kMaxImages <= kMaxImagesCeiling);

enum class PassOutcome : std::uint8_t {
  window_complete,   // the observation window ended
  guest_stop,        // the program stopped earlier on a non-Z80 typed stop (the window ended there)
  guest_complete,    // the program completed earlier
  unknown_image,     // an image epoch with an unregistered activation signature (snapshot present)
  budget_exhausted,  // the instruction budget ended the run
  wall_timeout,      // the wall timeout ended the run
  z80_code_mismatch, // a registered image's compiled bytes differ from the live RAM (under-determined signature)
  z80_stop,          // any other typed Z80 stop
  failed,            // the pass produced no usable report
};

[[nodiscard]] const char* pass_outcome_name(PassOutcome outcome) noexcept;

struct PassEpoch {
  Digest signature{};
  std::uint32_t extents = 0;
  friend bool operator==(const PassEpoch&, const PassEpoch&) = default;
};

// The Z80 was isolated by a structural code mutation during the run (contract section 18); the run itself continued.
struct SoundFault {
  std::uint32_t epochs = 0;         // image epochs seen when the fault latched
  std::uint64_t master_ticks = 0;   // guest master time of the fault
  friend bool operator==(const SoundFault&, const SoundFault&) = default;
};

struct PassObservation {
  PassOutcome outcome = PassOutcome::failed;
  std::uint32_t frames = 0;           // virtual frames reached
  std::vector<PassEpoch> epochs;      // every image epoch seen, activation order
  std::optional<Epoch> unknown;       // present exactly for PassOutcome::unknown_image
  std::optional<SoundFault> sound_fault;  // present when the Z80 sound CPU was isolated (structural_code_mismatch)
  // SEG-028-T005 (ADR 0077): the private M68K stop record (stop PC, stop class, work RAM), present for PassOutcome::guest_stop when
  // the pass wrote it. Consumed only by the build's ADR 0049 alias preparation; never reported.
  std::optional<m68k_alias::GuestStopRecord> guest_stop;
};

// Which operation of PassRunner::prepare failed (a short stable name such as "emit", "unit-compile", "pass-link") and a bounded,
// sanitized diagnostic text. Informational only: the typed Failure is unchanged.
struct PrepareFailure {
  std::string stage;
  std::string detail;
};

class PassRunner {
 public:
  virtual ~PassRunner() = default;
  // Builds the program for the registry (emit, compile only what changed, link). False = z80_image_compile_failed.
  virtual bool prepare(const Registry& registry) = 0;
  // After a false prepare(): the failed operation. Runners that do not report one return empty strings.
  [[nodiscard]] virtual PrepareFailure prepare_failure() const { return {}; }
  // One bounded run of the most recently prepared program.
  virtual PassObservation run() = 0;
};

enum class Failure : std::uint8_t {
  none,
  z80_image_bound_exceeded,
  materialization_budget_exhausted,
  materialization_nondeterministic,
  z80_image_compile_failed,
  materialization_no_convergence,
  z80_code_mismatch,
  z80_execution_unsupported,
  materialization_pass_failed,
};

[[nodiscard]] const char* failure_name(Failure failure) noexcept;

struct MaterializationSummary {
  Failure failure = Failure::none;
  PrepareFailure prepare_failure;   // set exactly for z80_image_compile_failed
  std::size_t images = 0;
  std::size_t discovery_runs = 0;   // runs that were needed to reach the candidate (the completing run included)
  std::size_t total_runs = 0;       // discovery runs plus the confirming run
  std::size_t epochs = 0;           // image epochs of the confirmed run
  std::uint32_t frames = 0;         // virtual frames reached by the confirmed run
  PassOutcome end = PassOutcome::failed;
  std::optional<SoundFault> sound_fault;  // set: Genesis audio degraded (z80_audio_outcome = structural_code_mismatch)
  std::optional<m68k_alias::GuestStopRecord> guest_stop;  // the confirmed run's M68K stop record (end == guest_stop)
  [[nodiscard]] bool ok() const noexcept { return failure == Failure::none; }
};

// Runs the fixed point. On success `registry` is the converged image set (images in first-activation order).
[[nodiscard]] MaterializationSummary materialize(Registry& registry, PassRunner& runner);

}  // namespace segarecomp::machine::genesis::z80
