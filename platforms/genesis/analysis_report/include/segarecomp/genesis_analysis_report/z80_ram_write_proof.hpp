#pragma once

// SEG-030-T010 (ADR 0079 decision 7, record T010; report-only): the Genesis-owned proof that bounds the Z80's stores into 68K work
// RAM. Its result is the only credited input that may replace the blanket external-writer rule of the M68K memory domain.
//
// Invariant: a precise bound OR `all` with typed reasons. The Z80 is never assumed not to write RAM; runtime observation is never an
// input (it may only falsify); the diagnostic `--assume-no-z80-ram-writes` ablation is separate and never consults this proof.
//
// Inputs (data contract):
// - The Z80 image set: every byte content the Z80 RAM ($0000-$1FFF, mirrored at $2000-$3FFF) can hold when the Z80 leaves reset.
//   nullopt is `image_set_unknown`. Only a static derivation may supply a set; none exists yet for 68K-uploaded drivers, so the
//   report driver passes nullopt. A byte beyond an image's length is not code (the analysis never invents bytes).
// - The 68K stores that may touch the Z80 area ($A00000-$A0FFFF), grouped: a group is a merged set of known bus ranges plus a count
//   of Unknown-target stores, and whether the group is proven to happen while /RESET holds the Z80. A store under /RESET is part of
//   the image the Z80 starts from (the image-set definition above); every other store may happen while the Z80 runs or between two
//   of its instructions (BUSREQ only pauses it).
//
// The Z80 side is a bounded flow-sensitive constant analysis over each image from the architectural reset state
// (`docs/architecture/genesis-z80-audio-contract.md` section 4 rule 6: PC 0, SP = AF = $FFFF, IM 0, interrupts disabled), not a value-
// set analysis: each 8-bit register, IX, IY and SP is one constant or Unknown; the bank latch is nine three-valued bits (Unknown at
// entry: the 68K may have written it and Z80 /RESET does not change it); IFF and IM are small sets; return slots (two stack bytes
// written by CALL/RST/interrupt pushes or PUSH) hold a bounded set of 16-bit values. Loads always yield Unknown (Z80 RAM is
// writable by the 68K and by the Z80 itself). Every reachable instruction must decode from image bytes.
//
// Store classification (per byte): Z80 RAM ($0000-$3FFF, `& $1FFF`) is local data unless it hits a control byte (a reachable
// instruction byte or a return slot read by RET/RETI/RETN/POP-free return): then it is `self_modifying_store`; YM2612, bank register
// (latch update), PSG/VDP window and unused space never reach 68K RAM; the bank window ($8000-$FFFF) maps to 68K
// `bank << 15 | (address & $7FFF)` for every completion of the latch: a completion at or above $1C0 adds the work-RAM byte
// (`& $FFFF`, mirrored), a completion inside the Z80 area ($140-$141) is `window_store_into_z80_area`. An Unknown address is
// `store_target_unknown`.
//
// Interrupts (contract section 8): INT is taken at any point where interrupts may be enabled; IM 0 and IM 1 both enter $0038 (the
// acknowledge byte is $FF), IM 2 or an Unknown mode is `interrupt_mode_unbounded`; the push uses the interrupted SP. NMI is not
// connected. Computed jumps need a known register; returns need a known SP whose slot holds a bounded value set.
//
// The 68K side: a known store (not under /RESET) into Z80 RAM that overlaps a control byte is `m68k_store_into_z80_code`; an
// Unknown-target store (not under /RESET) is `m68k_store_into_z80_ram_unbounded`; any 68K store that may touch the bank register
// (or has an Unknown target, /RESET or not) makes the latch Unknown at every Z80 point.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "segarecomp/cpu/m68k/analysis/abstract_memory.hpp"

namespace segarecomp {

enum class GenesisZ80RamWrites : std::uint8_t { none, ranges, all };
[[nodiscard]] const char *genesis_z80_ram_writes_name(GenesisZ80RamWrites writes) noexcept;

// Closed vocabulary; every reason forces `all`.
enum class GenesisZ80ProofReason : std::uint8_t {
  image_set_unknown,
  image_invalid,
  undecodable_code,
  store_target_unknown,
  stack_pointer_unknown,
  indirect_control,
  return_unbounded,
  interrupt_mode_unbounded,
  self_modifying_store,
  window_store_into_z80_area,
  m68k_store_into_z80_code,
  m68k_store_into_z80_ram_unbounded,
  analysis_bound,
  proof_not_stable,
};
[[nodiscard]] const char *genesis_z80_proof_reason_name(GenesisZ80ProofReason reason) noexcept;

struct GenesisZ80Image {
  std::vector<std::uint8_t> bytes;  // Z80 RAM content from $0000 (at most 8 KiB)
  std::string provenance;           // generic provenance class (never a path or commercial identifier)
};

struct GenesisZ80AreaStores {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;  // known 68K bus ranges [first, last)
  std::size_t unknown_target_stores{};
  bool held_in_reset{};  // proven: every store of the group happens while /RESET holds the Z80
};

// The 68K Z80 area (stores the proof consumes) and the Genesis work-RAM region of the M68K analysis view.
inline constexpr std::uint32_t genesis_z80_area_first = UINT32_C(0xA00000);
inline constexpr std::uint32_t genesis_z80_area_last = UINT32_C(0xA10000);
inline constexpr std::uint32_t genesis_z80_bank_ram_first = 0x1C0U;  // bank values selecting $E00000-$FFFFFF

struct GenesisZ80RamWriteProof {
  GenesisZ80RamWrites outcome{GenesisZ80RamWrites::all};
  std::vector<M68kAsyncRange> work_ram;      // physical work-RAM ranges the Z80 may write (outcome `ranges`)
  std::set<GenesisZ80ProofReason> reasons;   // non-empty iff outcome `all`
  std::vector<std::string> image_hashes;     // SHA-256 of each analysed image, in input order
  // Aggregate measures.
  std::size_t reachable_instructions{};
  std::size_t store_sites{};          // reachable instructions (and interrupt entries) that store
  std::size_t window_store_sites{};
  std::size_t bank_register_store_sites{};
  std::size_t interrupt_entries{};
  std::size_t m68k_known_ranges{};
  std::size_t m68k_unknown_target_stores{};
  bool bank_volatile{};               // a 68K store may write the bank latch

  // The M68K memory-domain bound: nullopt (`all`) or the work-RAM ranges (empty for `none`).
  [[nodiscard]] std::optional<std::vector<M68kAsyncRange>> bound() const;
};

// Runs the proof. `images` nullopt: image set unknown.
[[nodiscard]] GenesisZ80RamWriteProof prove_genesis_z80_ram_writes(const std::optional<std::vector<GenesisZ80Image>> &images,
                                                                   const std::vector<GenesisZ80AreaStores> &m68k_stores);

// Aggregate JSON object (classes and counts only; never an address).
[[nodiscard]] std::string format_genesis_z80_ram_write_proof(const GenesisZ80RamWriteProof &proof, const char *credited_bound);

}  // namespace segarecomp
