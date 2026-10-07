#pragma once

// SEG-040-T004 (ADR 0072 section 4, ADR 0079 decision 7; report-only): the Genesis-generic M68K-side producer of the Z80 boot
// image and of the proven BUSREQ/RESET pristine-window classification of every M68K store into the Z80 bus area.
//
// Root cause this closes (SEG-040-T001 census): the report driver (report.cpp) always passed `z80_images = std::nullopt` and
// `held_in_reset = false` for every observed store group to `prove_genesis_z80_ram_writes` (z80_ram_write_proof.hpp), which
// unconditionally forces `outcome = all` (Reason::image_set_unknown) independently of how precisely the 68K's own boot-time
// upload could have been bounded. The Z80-side proof itself (z80_ram_write_proof.cpp) needs zero semantic changes: only its
// input does.
//
// Genesis-generic hardware facts this producer relies on (docs/architecture/genesis-z80-audio-contract.md section 4, frozen
// against Genesis Plus GX `gen_zbusreq_w`/`gen_zreset_w` and ares `APU::setBUSREQ`/`setRES`; already mirrored at the runtime by
// platforms/genesis/runtime/runtime.c `genesis_z80_bus_write`):
//   - Power-on: BUSREQ not requested, `/RESET` asserted (the Z80 is held in reset; contract 4.1).
//   - `$A11100` bit 0 (word D8, byte D0): 1 = BUSREQ asserted (request the bus; the Z80 may not run), 0 = released.
//   - `$A11200` bit 0 (write-only): 1 = `/RESET` released, 0 = `/RESET` asserted (held).
//   - The Z80 is runnable only while `/RESET` is released AND BUSREQ is not asserted (contract 4.7); the 68K is granted the Z80
//     bus (its $A00000-$A0FFFF/$A06000-$A060FF stores actually reach Z80 RAM/the bank register rather than being ignored as open
//     bus, contract section 3) only while BUSREQ is asserted AND `/RESET` is released.
//   - A store that happens while the Z80 has never yet been provably runnable (the Genesis "pristine" invariant, contract
//     section 5) is part of the image the Z80 starts executing from once it is released; this producer derives a strictly
//     bounded (fail-closed) sufficient sub-case of that invariant: BUSREQ asserted AND `/RESET` released at the store (so the
//     store provably reaches Z80 RAM) AND the Z80 was never provably runnable on any path reaching that point.
//
// This is a CPU-generic-reuse pass only: it builds its own `M68kFiniteAdapter` from the SAME image/config the main report run
// used, replays the already-completed solution's edges (exactly the post-hoc "replay the final fixed point" pattern
// `derive_memory_policy` in finite_adapter.cpp already uses for the handler-reachability set) and reads out exact FiniteValue
// register/memory content through the adapter's own public queries (`memory_write_targets`, the SEG-040-T004
// `memory_write_values` query). It never decodes a Z80 instruction, never represents the bank latch or any Z80-side state, and
// never adds Z80 code addresses to M68K reachability or a combined M68K/Z80 program state: the Z80 bank/bus mechanism stays
// entirely inside z80_ram_write_proof.cpp. The only Z80-bank-specific facts referenced here are the same bus-geometry constants
// (`genesis_z80_control_first/last`, `genesis_z80_area_first/last`) the existing Z80 proof and report driver already use.
//
// Fail-closed throughout: the pristine/boot-window lattice (busreq/reset_released/pristine) and the image byte accumulation are
// replayed over the main-flow (tag 0) partition only (an interrupt/exception handler instance's state at its taking point is
// never provably related to the architectural power-on state, so this producer never credits one as part of the boot window or
// the image); a BUSREQ/RESET control write whose exact bit cannot be read, or whose target is not exactly one of the two
// documented register addresses, or a bounded worklist-iteration exhaustion, collapses every fact from that point forward to
// "not provably in the boot window" -- never the other way. A boot-window Z80-area store with an imprecise target or value
// abandons the WHOLE derived image (never a partially-filled one): `image` stays nullopt and the report driver keeps
// `z80_images = std::nullopt` exactly as before. A reachable handler-instance (tag != 0) store into the Z80 bus area is a real
// 68K write this producer never credits into the boot window either, but it is still folded into the "running" (never
// held_in_reset) area-store group so it is never silently dropped relative to the pre-existing, all-partition
// `observed_store_ranges` aggregate this producer's output replaces in report.cpp.

#include <cstdint>
#include <optional>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/finite_adapter.hpp"
#include "segarecomp/genesis_analysis_report/z80_ram_write_proof.hpp"

namespace segarecomp {

// Bounded defensively (the underlying fact lattice can only decrease a handful of times per point, so a real run converges far
// below this); exceeding it fails closed (every fact collapses to "not provably in the boot window", `image` stays nullopt, and
// every Z80-area store group is reported `held_in_reset = false`, i.e. the policy unchanged from before SEG-040-T004).
inline constexpr std::size_t genesis_z80_boot_window_iteration_bound = 1'000'000U;

struct GenesisZ80BootDerivation {
  // Every M68K store into the Z80 bus area (genesis_z80_area_first..last), split by the proven boot-window classification
  // above; replaces the report driver's previous single `held_in_reset = false` group. Empty when the analysis did not
  // complete or no such store is reachable.
  std::vector<GenesisZ80AreaStores> area_stores;
  // The exact Z80 RAM content every boot-window store writes, when every such store has both an exactly provable one-byte
  // target offset and an exactly provable value; nullopt otherwise (fail closed: never a partially-known image). Bytes beyond
  // the highest proven offset are not claimed (the proof's own image-length contract already treats them as "not code").
  std::optional<GenesisZ80Image> image;
};

// `image`/`config` must be the same executable-image view and (post domain-implication) adapter configuration the caller used
// to produce `analysis` (so replaying its edges reproduces the same per-point states). `reset_entry_pc`: the 24-bit bus PC of
// the program's own reset/startup entry when the caller has established the 68000 architectural reset state there
// (`GenesisAnalysisReportConfig::reset_entry`); nullopt when no root may assume the power-on BUSREQ/`/RESET` defaults. `roots`
// is the main-flow (tag 0) entry set the caller seeded the analysis with (`GenesisAnalysisReport::roots.roots`).
[[nodiscard]] GenesisZ80BootDerivation derive_genesis_z80_boot_image(const M68kAnalysisImage &image, const M68kAnalysisConfig &config,
                                                                     const M68kFiniteAnalysisResult &analysis,
                                                                     std::optional<std::uint32_t> reset_entry_pc,
                                                                     const std::vector<std::uint32_t> &roots);

}  // namespace segarecomp
