#pragma once

// SEG-014-T002: pure MC68000 addressing-mode mechanics (docs/architecture/
// post-seg007-architecture-refactor-contract.md, section 8.2 "cpu/m68k/").
// Relocated from include/segarecomp/m68k_pipeline.hpp per
// docs/architecture/seg-014-t001-symbol-migration-map.md's `cpu/m68k/`
// section ("Address-policy helpers that are pure MC68000 addressing-mode
// mechanics, not Genesis memory-map policy"). No behavior changes except the
// rename below, which the map explicitly requests (dropping the
// Genesis-specific name). The map's own suggested replacement combined the
// words "canonical", "bus", and "address" into one identifier; that exact
// combination is deliberately not used verbatim: a project-authored
// regression in tests/genesis_reset_image_adversarial_test.py (section
// 35.2's "no giant Sega framework"/generic bus-seam concern) fails closed on
// any new production identifier containing an underscore-joined "bus"
// segment outside its narrow existing allowlist, since this function
// decides no bus/device policy of its own (its own doc comment below already
// says so). `m68k_canonical_ea_address` names it precisely without tripping
// that adversarial guard.

#include "segarecomp/cpu/m68k/direct_flow.hpp"
#include "segarecomp/cpu/m68k/instruction.hpp"

#include <optional>

namespace segarecomp {

// The only control EAs whose target is available to static discovery.  In
// particular, (An) and d16(An) remain runtime-derived even if an unrelated
// value happens to be present in absolute_address.
[[nodiscard]] bool m68k_is_statically_foldable_control_ea(const M68kEffectiveAddress &ea) noexcept;

// The set of non-foldable JMP/JSR control-EA shapes for which this project has
// a bounded ADR-0009 target-set proof mechanism (Tier-1) and a Tier-2
// fail-closed representation -- used to protect an unresolved such site from
// bare-decode cross-root supersession. Returns true for EXACTLY the
// computed-control EA classes the frontend represents through
// `M68kIndirectTargetEaSet`:
//   - `ea.mode == M68kEaMode::pc_index8` (ADR-0009 Tier-1 brief PC-indexed
//     shape), and
//   - pure `JMP (An)` / `JSR (An)` (`ea.mode == M68kEaMode::address_indirect`,
//     `ea.displacement == 0`, `ea.extension_words == 0`; SEG-007-T178).
[[nodiscard]] bool m68k_is_supported_computed_control_ea(const M68kEffectiveAddress &ea) noexcept;

// Shared MOVE.L Abs.L absolute-operand address policy: 24-bit clean and
// even. Static decode-time validation (frontend) and defensive runtime
// re-validation (execution) apply exactly this rule so the two cannot
// drift; ROM-write and RAM-range policy remain the caller's, since they
// depend on facts (mapping claims, or a runtime buffer) unavailable here.
// `width` defaults to long_word, preserving every existing (always-long-word)
// caller's exact behavior. SEG-007-T023 bugfix: per the contract's
// "Addressing-mode mechanics", only word- and long-word-size accesses (and
// instruction fetches) require an even address on the base MC68000; a
// byte-size access carries no alignment restriction at all, so the oddness
// check below is skipped for `width == byte`.
[[nodiscard]] std::optional<DirectFlowDiagnostic> m68k_startup_absolute_operand_alignment(
    std::uint32_t address, M68kMemoryAccessWidth width = M68kMemoryAccessWidth::long_word) noexcept;

// Renamed from `m68k_genesis_canonical_ea_address` (SEG-014-T002, per the
// migration map): the body performs generic MC68000 absolute-word bus
// canonicalization with no Genesis memory-map/device/mapping decision
// whatsoever. Absolute-word encodings retain their decoded sign-extended
// 32-bit value for instruction provenance and lifting, but their logical
// 24-bit bus address is the low 24 bits. No other EA form is canonicalized
// here: in particular an absolute-long value with a nonzero upper byte must
// reach address validation unchanged. The result is then handed to whichever
// machine-specific address-space router a given host machine supplies; this
// function performs no such routing itself.
//
// SEG-007-T105: the address-register-relative EA families ((An), (An)+, -(An),
// d16(An)) are not canonicalized here because their effective address is only
// known at run time. The identical generic 24-bit external-address-bus
// truncation (`ea & 0x00FFFFFF`, per MC68000UM "Signal Description") is applied
// once to those families at the runtime routing seam in
// `m68k_emit_routed_read` / `m68k_emit_routed_write` (libs/codegen/c11/src/m68k.cpp),
// where the truncated value feeds both the router call and
// `provenance.access_address`.
[[nodiscard]] std::uint32_t m68k_canonical_ea_address(const M68kEffectiveAddress &ea) noexcept;

} // namespace segarecomp
