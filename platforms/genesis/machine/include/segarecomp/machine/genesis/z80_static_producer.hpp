#pragma once

// Static-first Genesis Z80 executable-image producer (ADR 0277, building on ADR 0073/0077/0256).
//
// The build-time materializer (z80_materialization.hpp) discovers each Z80 image by running the generated program until an unknown
// activation signature appears. This producer reconstructs the images whose causal upload is *provable from the cartridge alone*,
// before the first Z80 compilation, and returns them as ordinary Epoch records for the existing Registry. Nothing else changes: the
// content hash, the activation signature S1*, the RAM-backed structural guard and the fixed point are the existing ones; whatever the
// producer cannot establish is left to the materializer unchanged.
//
// What is proved (all of it, for an epoch to be produced):
//   * the walk reaches the epoch's BUSREQ / RESET writes with constant values (the hold window, its start and its runnable transition
//     follow the runtime's epoch tracker rule for rule: power-on state, window cleared at every runnable transition, an epoch is the
//     first runnable transition since the last /RESET release);
//   * every byte of the window is stored by a recognised, bounded mechanism whose source is immutable cartridge data and whose
//     destination range is constant inside the 8 KiB sound RAM:
//       - constant-valued stores, and a counted `MOVE.B (An)+,(Am)+ ; DBRA Dn` copy of constant operands from the ROM image;
//       - a call of the Kosinski decompressor whose M68K routine (position independent code lying in the ROM image) is one of the
//         recognised forms, with a constant ROM stream address and a constant RAM destination, decoded by the public Kosinski decoder.
//     An equal byte sequence is never taken as proof of a causal upload: bytes only enter the model through those mechanisms;
//   * everything else the walk meets that could change the Z80 window (a store through an unknown pointer while the 68K owns the
//     bus, an unknown-valued bus write, an unrecognised loop or decompressor feeding the window) is a typed refusal and ends that
//     path: nothing downstream of it is claimed.
//
// What is NOT proved, deliberately: asynchronous interrupts, 68K work-RAM state, Z80 execution effects on its own RAM between epochs
// (the model keeps the bytes the 68K left; the observed snapshot may differ outside the written extents - "carry-over", see
// compare_registries), and that the walk is exhaustive (it ends at a horizon: dynamic control flow, a budget). Exhaustiveness is
// established only by the existing verification pass, which must observe exactly the produced activation signatures in the produced
// order; any disagreement discards the static registry and the build proceeds with the unchanged fixed point (build_command).
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "segarecomp/machine/genesis/z80_images.hpp"

namespace segarecomp::machine::genesis::z80 {

// The producer's typed result. `partial` means at least one Z80-affecting construct was refused; `complete` means every such construct
// on the explored region was resolved (the walk still ended at a horizon: see above); `none` means the walk finished with no epoch.
enum class StaticOutcome : std::uint8_t {
  complete,        // a non-empty ordered epoch list; no refused Z80-affecting construct
  partial,         // a proven, ordered epoch prefix/list plus at least one refused construct
  none,            // the walk completed and found no epoch (nothing statically proven)
  unsupported,     // an ambiguous or unreadable producer: no epoch is claimed
  resource_bound,  // a resource bound ended the walk before any epoch was proven
};

[[nodiscard]] const char* static_outcome_name(StaticOutcome outcome) noexcept;

// Why a Z80-affecting construct was refused (a bounded, sanitized, stable vocabulary; counted, never located).
enum class StaticRefusal : std::uint8_t {
  entry_unreadable,            // the reset vector does not name readable ROM code
  bus_write_value_unknown,     // a BUSREQ/RESET write whose value is not constant
  z80_store_value_unknown,     // a store into the sound RAM whose value is not constant
  z80_store_unknown_address,   // a store through an unknown pointer while the 68K may own the bus
  z80_store_form_unsupported,  // a store form that is neither a plain MOVE nor a recognised copy
  length_unknown,              // a counted copy whose count is not constant
  source_not_immutable,        // a copy/decoder source outside the ROM image
  destination_out_of_ram,      // an upload destination outside the 8 KiB sound RAM (or overlapping its mirror)
  loop_unproven,               // a store the walk could not prove executes exactly once per recorded occurrence (inside a loop)
  decompression_unproven,      // a decompression call whose routine is not a recognised form, or whose operands are not constant
  decode_failed,               // a recognised decoder rejected its stream (malformed, truncated or oversize)
  window_control_unknown,      // a branch whose outcome is not constant while the 68K owns the bus (the window's content would be path dependent)
  ambiguous_epoch_order,       // different paths reach different epoch sequences: only the common prefix is claimed
  image_bound,                 // more distinct images than the registry bound
  call_depth,                  // the bounded call depth was exceeded
  state_bound,                 // the bounded number of walk states was exceeded
  step_bound,                  // the bounded number of walk steps was exceeded
};

[[nodiscard]] const char* static_refusal_name(StaticRefusal refusal) noexcept;
// True when the refusal concerns a construct that may change the Z80 window (every refusal except the resource and ambiguity classes
// handled separately by the outcome rule).
[[nodiscard]] bool static_refusal_affects_epochs(StaticRefusal refusal) noexcept;

// The strongest mechanism that contributed bytes to a produced epoch (reported in provenance).
enum class StaticProof : std::uint8_t { rom_stores, rom_copy, kosinski };
[[nodiscard]] const char* static_proof_name(StaticProof proof) noexcept;

struct StaticEpoch {
  Epoch epoch;
  StaticProof proof = StaticProof::rom_stores;
};

// Bounds: constants, not options (no silent expansion to make a title pass).
inline constexpr std::uint32_t kStaticMaxSteps = 1'500'000;
inline constexpr std::uint32_t kStaticMaxStates = 64;   // distinct (RAM, bus, trace, call-stack) contexts
inline constexpr std::uint32_t kStaticMaxNodes = 400'000;
inline constexpr std::uint32_t kStaticMaxCallDepth = 8;
inline constexpr std::uint32_t kStaticMaxEpochs = 2 * kMaxImages;

struct StaticProduction {
  StaticOutcome outcome = StaticOutcome::none;
  std::vector<StaticEpoch> epochs;                  // ordered activation epochs (the runtime's on_epoch sequence), repeats included
  std::map<StaticRefusal, std::uint32_t> refusals;  // by class
  std::uint32_t steps = 0;                          // walk steps
  std::uint32_t nodes = 0;                          // (pc, context) nodes
  std::uint32_t contexts = 0;                       // distinct Z80 contexts
  std::uint32_t candidates = 0;                     // distinct epochs seen on any path (before the common-prefix rule)
  std::uint32_t copy_loops = 0;                     // counted ROM copies recognised
  std::uint32_t decoder_calls = 0;                  // recognised decompressor calls applied
  std::uint32_t horizon_paths = 0;                  // paths that ended at dynamic control flow, a stop, or a return without caller
  [[nodiscard]] bool proven() const noexcept { return !epochs.empty(); }
};

// Walks `rom` (the immutable cartridge image) from its reset vector. Deterministic: independent of host, thread count and address.
[[nodiscard]] StaticProduction produce_static_epochs(std::span<const std::uint8_t> rom);

// Registers the produced epochs, in order, in `registry` with static provenance (`Registry::add` rules, ordinals by first activation).
// Returns the number of images registered; stops at the registry bound (reported through `production.refusals`).
[[nodiscard]] std::size_t preload_registry(Registry& registry, StaticProduction& production);

// The recognised Kosinski decoder forms (digests of the routine's code; position-independent, no addresses). Exposed for tests/docs.
[[nodiscard]] std::size_t kosinski_routine_form_count() noexcept;

}  // namespace segarecomp::machine::genesis::z80
