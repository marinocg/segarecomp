#pragma once

// SEG-014-T002: MC68000 architecture semantics only (docs/architecture/
// post-seg007-architecture-refactor-contract.md, section 8.2 "cpu/m68k/").
// Relocated from include/segarecomp/m68k_pipeline.hpp (the instruction
// representation/EA/register-file portion) and include/segarecomp/moveq.hpp
// (DataRegister) per docs/architecture/seg-014-t001-symbol-migration-map.md's
// `cpu/m68k/` section. No field, value, or behavior changes; only the
// physical location and (for DataRegister) which header defines it changed.
//
// This header must never depend on Genesis/machine-specific content
// (forbidden dependency: cpu/m68k -> machine/genesis, cpu/m68k ->
// device/sega/genesis).

#include "segarecomp/core/provenance.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace segarecomp {

// Relocated from moveq.hpp. Reused far beyond MOVEQ (e.g. M68kRegisterWrite);
// not part of the temporary MOVEQ compatibility surface.
enum class DataRegister : std::uint8_t { d0, d1, d2, d3, d4, d5, d6, d7 };

// Address registers are a separate architectural register class.  Do not use
// DataRegister as an integer surrogate: MOVEA/LEA name An, never Dn.
enum class AddressRegister : std::uint8_t { a0, a1, a2, a3, a4, a5, a6, a7 };

// The profiles deliberately describe the already accepted slices; they are
// not a request to decode the rest of the MC68000 instruction set.
// `genesis_startup` additionally selects `TST.L (xxx).L` (SEG-007-T008,
// docs/references/tst-l-absolute-long-contract.md) through the same shared
// decode predicate every other selected genesis_startup form uses -- there is
// no separate per-instruction decode profile for it. This is a shared CPU
// *decode capability* fact only: the Genesis-specific fixed five-operation
// startup frontend graph (owned by machine/genesis-shaped code, not this
// header) still accepts only its own fixed role sequence and still rejects
// any other kind, including TST.L, at its fixed graph position.
// `general_startup` (SEG-007-T010) is one additional, deterministic decode
// selection policy: the union of `genesis_startup`'s forms (MOVEQ, both
// absolute-long MOVE forms, JSR, RTS, TST.L) and `direct_flow`'s forms
// (SUBQ.L #1,D0, BNE.short, BRA.short), selected through the exact same
// shared per-form predicates those two profiles already use -- it is an
// ingress/selection policy only, never a second decode implementation.
enum class M68kDecodeProfile { moveq, direct_flow, genesis_startup, general_startup };

// SEG-007-T023 ("common MC68000 startup/data-movement batch"): the reusable
// typed effective-address layer shared by every whitelisted TST/MOVE/MOVEA/
// CLR/LEA/JMP/JSR form, per docs/references/
// m68k-common-startup-data-movement-batch-contract.md. `unused` is the
// default for an operand position a given instruction/kind does not have
// (e.g. TST/CLR have no destination_ea beyond `unused`; JMP/JSR have no data
// destination_ea at all). Indexed modes (`d8(An,Xn)`, `d8(PC,Xn)`) are
// deliberately absent: they remain out of scope for this batch.
enum class M68kEaMode : std::uint8_t {
  unused,
  data_register,     // Dn
  address_register,  // An
  address_indirect,  // (An)
  address_postinc,   // (An)+
  address_predec,    // -(An)
  address_disp16,    // d16(An)
  absolute_word,      // (xxx).W
  absolute_long,      // (xxx).L
  pc_disp16,          // d16(PC)
  immediate,          // #<data>
  // SEG-007-T120: address register indirect with index and 8-bit
  // displacement, brief-format extension word only -- (d8,An,Xn). The
  // brief-format extension word is `D/A | Xn(3) | W/L | 0 0 | 0 | d8(8)`
  // per the Motorola M68000 Family Programmer's Reference Manual: `reg`
  // is the base An, `index_reg`/`index_is_address` select Xn, `index_is_long`
  // selects word (sign-extended) vs long index size, `displacement` carries
  // the sign-extended 8-bit displacement. The effective address is
  // `An + sign_extend(Xn by size) + d8`. Scale, base displacement, and
  // memory indirection do not exist on a base MC68000 and are rejected at
  // decode; this is an addressing-mode extension of the existing MOVE
  // family, not a new instruction kind.
  address_index8,
  // SEG-007-T124 / ADR-0009 (docs/decisions/0009-computed-indirect-control-
  // flow-target-resolution.md): brief-format PC-relative indexed addressing,
  // `(d8,PC,Xn)` -- mode field 111, register field 011. Distinct from
  // `address_index8` (never reused as an An-indexed base with a fabricated
  // An==PC convention): the ADR requires a separate typed EA mode and
  // separate control-EA legality so an indexed An base can never accidentally
  // admit this PC-relative form. Legal only as a JMP/JSR control EA
  // (`m68k_ea_jsr_jmp_control_modes`); LEA/PEA and every other selected form
  // stay out of scope. The effective address is
  // `pc_base_address + sign_extend(Xn by size) + d8`; scale, base
  // displacement, and memory indirection do not exist on a base MC68000 and
  // are rejected at decode, exactly like `address_index8`.
  pc_index8,
};
// `reg` is the 0-7 register number for the Dn/An/(An)/(An)+/-(An)/d16(An)
// modes. `displacement` is the raw signed 16-bit extension word for
// address_disp16/pc_disp16. `absolute_address` is the already-resolved,
// statically-foldable 32-bit target for absolute_word (sign-extended per the
// contract), absolute_long, and pc_disp16 (PC-relative, folded against the
// address of the displacement's own extension word, per the contract);
// meaningless for every other mode. `immediate_value` is the raw decoded
// immediate operand, sized/padded per the contract's byte/word/long rules;
// meaningless for every other mode. `extension_words` records how many 16-bit
// extension words this one operand consumed (0, 1, or 2) for provenance/
// length bookkeeping only.
struct M68kEffectiveAddress {
  M68kEaMode mode{M68kEaMode::unused};
  std::uint8_t reg{};
  std::int16_t displacement{};
  std::uint32_t absolute_address{};
  std::uint32_t immediate_value{};
  std::uint8_t extension_words{};
  // SEG-007-T120: meaningful only for `address_index8`. `index_reg` is the
  // 0-7 index register number; `index_is_address` selects An (true) vs Dn
  // (false); `index_is_long` selects long (true) vs sign-extended word
  // (false) index size. Zero/false for every other mode (harmless defaults,
  // appended so every existing positional aggregate initialization stays
  // valid).
  std::uint8_t index_reg{};
  bool index_is_address{};
  bool index_is_long{};
  // SEG-007-T124 / ADR-0009: the M68K program address of this operand's own
  // first extension word, set by decode for EVERY PC-relative EA
  // (`pc_disp16` and `pc_index8`; harmless zero default for every other
  // mode). The target-EA evaluator (both static discovery and the C11
  // runtime EA computation) consumes this field directly and must never
  // rederive a PC base from an emitter-local instruction length or host
  // pointer.
  std::uint32_t pc_base_address{};
};
// Per-instruction/per-operand-position legal-EA-mode bitmasks (decode-stage
// legality only; see the contract's per-mnemonic "Legal EA-mode set" facts).
using M68kEaLegalMask = std::uint16_t;
inline constexpr M68kEaLegalMask m68k_ea_dn = 1U << 0;
inline constexpr M68kEaLegalMask m68k_ea_an = 1U << 1;
inline constexpr M68kEaLegalMask m68k_ea_an_indirect = 1U << 2;
inline constexpr M68kEaLegalMask m68k_ea_an_postinc = 1U << 3;
inline constexpr M68kEaLegalMask m68k_ea_an_predec = 1U << 4;
inline constexpr M68kEaLegalMask m68k_ea_an_disp16 = 1U << 5;
inline constexpr M68kEaLegalMask m68k_ea_absolute_word = 1U << 6;
inline constexpr M68kEaLegalMask m68k_ea_absolute_long = 1U << 7;
inline constexpr M68kEaLegalMask m68k_ea_pc_disp16 = 1U << 8;
inline constexpr M68kEaLegalMask m68k_ea_immediate = 1U << 9;
// SEG-007-T120: brief-format (d8,An,Xn) indexed addressing.
inline constexpr M68kEaLegalMask m68k_ea_index8 = 1U << 10;
// SEG-007-T124 / ADR-0009: brief-format PC-relative indexed addressing,
// `(d8,PC,Xn)`. Never included in `m68k_ea_control_modes` (PEA stays exactly
// as narrow as before); the dedicated JMP/JSR mask below adds it for
// control-transfer targets, per the ADR's "distinct typed EA mode and
// control legality" rule. SEG-007-T215 additionally widens LEA's own
// control-EA set (below) to admit it as a plain address-computation source
// -- LEA never treats its source as a control-transfer target, so this is
// the same non-control widening precedent SEG-007-T135 already established
// for `(d8,An,Xn)`, not a change to JMP/JSR/Tier-1/Tier-2 admission.
inline constexpr M68kEaLegalMask m68k_ea_pc_index8 = 1U << 11;
// The "control addressing modes" set (contract § 2.3): memory operands
// without an associated size. LEA/JMP/JSR's target EA set, confirmed
// identical for all three.
inline constexpr M68kEaLegalMask m68k_ea_control_modes =
    m68k_ea_an_indirect | m68k_ea_an_disp16 | m68k_ea_absolute_word | m68k_ea_absolute_long | m68k_ea_pc_disp16;
// SEG-007-T124 / ADR-0009: JMP/JSR's own control-EA legal set, widened with
// the brief-format PC-relative indexed form beyond the shared
// `m68k_ea_control_modes` LEA/PEA still use unchanged. This is the ADR's
// required "distinct... control legality" -- an indexed An base
// (`m68k_ea_index8`) is never added here or to `m68k_ea_control_modes`;
// control addressing keeps its existing five plain forms plus exactly this
// one new PC-relative indexed form.
inline constexpr M68kEaLegalMask m68k_ea_jsr_jmp_control_modes = m68k_ea_control_modes | m68k_ea_pc_index8;
// SEG-007-T135 / ADR-0009 precedent: LEA's own control-EA legal set, widened
// with the brief-format address-register indexed form `(d8,An,Xn)`
// (`m68k_ea_index8`) beyond the shared `m68k_ea_control_modes`. Used only at
// LEA's own decode call site; the shared `m68k_ea_control_modes` and PEA's
// legal set are unchanged, exactly like the JMP/JSR precedent above. LEA
// computes an address value only -- no memory access of the addressed
// location and no condition-code change -- so admitting the indexed base
// here introduces no dereference or flag effect.
// SEG-007-T215: further widened with the brief-format PC-relative indexed
// form `(d8,PC,Xn)` (`m68k_ea_pc_index8`), the exact same non-control
// addressing-mode widening precedent as the `m68k_ea_index8` addition
// directly above -- LEA's source is never itself a control-transfer target
// (`m68k_is_statically_foldable_control_ea`/`m68k_is_supported_computed_
// control_ea` are unaffected), so this introduces no new indirect/computed-
// control admission and reuses the already-implemented `pc_index8` decode
// (SEG-007-T124) and shared runtime EA-computation helper
// (`m68k_emit_runtime_ea_address`, SEG-007-T136) verbatim. Found while
// classifying a real reachable Sonic startup block-reconstruction gap: an
// already-admitted static-discovery unit's own straight-line walk reached
// exactly this LEA form and failed to decode it, silently truncating that
// unit's own decoded range without registering any candidate-frontier fact
// (an ordinary, non-authoritative admitted-unit secondary discovery issue is
// dropped by design) -- this is a genuine CPU decode/lift/operand-shape gap
// for an existing supported instruction, not a block-reconstruction,
// stitching, or admission-policy defect.
inline constexpr M68kEaLegalMask m68k_ea_lea_control_modes =
    m68k_ea_control_modes | m68k_ea_index8 | m68k_ea_pc_index8;
// The "data alterable" set: MOVE/MOVEA/CLR's destination-alterable modes
// (MOVEA's destination is always the fixed An register, not this set, but
// this same 8-mode set is reused by plain MOVE and CLR destinations).
inline constexpr M68kEaLegalMask m68k_ea_data_alterable =
    m68k_ea_dn | m68k_ea_an_indirect | m68k_ea_an_postinc | m68k_ea_an_predec | m68k_ea_an_disp16 |
    m68k_ea_absolute_word | m68k_ea_absolute_long;
// Data-alterable excluding Dn: the destination set for the reverse ordinary
// AND/OR forms.  These forms are memory RMW operations, never register RMW.
inline constexpr M68kEaLegalMask m68k_ea_memory_alterable =
    m68k_ea_an_indirect | m68k_ea_an_postinc | m68k_ea_an_predec | m68k_ea_an_disp16 |
    m68k_ea_absolute_word | m68k_ea_absolute_long;
// MOVE/MOVEA's full 9-mode legal source set (An/d16(PC)/immediate legal
// already on the base MC68000, unlike TST's restricted set below).
inline constexpr M68kEaLegalMask m68k_ea_move_source =
    m68k_ea_dn | m68k_ea_an | m68k_ea_an_indirect | m68k_ea_an_postinc | m68k_ea_an_predec | m68k_ea_an_disp16 |
    m68k_ea_absolute_word | m68k_ea_absolute_long | m68k_ea_pc_disp16 | m68k_ea_immediate;
// SEG-021-T005: MOVE/MOVEA/CLR/NOT/TST family-level legal-EA contract, written
// from the Motorola M68000 Family Programmer's Reference Manual (MOVE, MOVEA,
// CLR, NOT, TST entries, base MC68000 columns) and independent of any game
// history or of the T001 legal-form dataset (production never reads it).
//   MOVE/MOVEA source: every mode -- Dn, An (word/long only; byte An is
//     rejected by the MOVE decoder), (An), (An)+, -(An), d16(An), (d8,An,Xn),
//     abs.W, abs.L, d16(PC), (d8,PC,Xn), #imm.
//   MOVE destination: data alterable -- Dn, (An), (An)+, -(An), d16(An),
//     (d8,An,Xn), abs.W, abs.L (MOVEA's destination is the fixed An field).
//   CLR/NOT operand: data alterable, same eight modes as the MOVE destination.
//   TST operand: data alterable on the base MC68000 (no An, PC-relative or
//     immediate; those are 68020+).
inline constexpr M68kEaLegalMask m68k_ea_move_family_source =
    m68k_ea_dn | m68k_ea_an | m68k_ea_an_indirect | m68k_ea_an_postinc | m68k_ea_an_predec | m68k_ea_an_disp16 |
    m68k_ea_index8 | m68k_ea_absolute_word | m68k_ea_absolute_long | m68k_ea_pc_disp16 | m68k_ea_pc_index8 |
    m68k_ea_immediate;
inline constexpr M68kEaLegalMask m68k_ea_data_alterable_with_index =
    m68k_ea_dn | m68k_ea_an_indirect | m68k_ea_an_postinc | m68k_ea_an_predec | m68k_ea_an_disp16 | m68k_ea_index8 |
    m68k_ea_absolute_word | m68k_ea_absolute_long;
inline constexpr M68kEaLegalMask m68k_ea_move_family_destination = m68k_ea_data_alterable_with_index;
inline constexpr M68kEaLegalMask m68k_ea_clr_not_operand = m68k_ea_data_alterable_with_index;
// SEG-007-T137: brief-format address-register indexed addressing,
// `(d8,An,Xn)`, is likewise a legal source operand for the shared
// ADD/SUB/CMP/AND/OR/ADDA/SUBA/CMPA register-form family on the base
// MC68000 (M68000PM/AD Rev. 1 §2/§4 source-operand tables for this family
// list every addressing mode `m68k_ea_move_source` already carries plus this
// brief-format indexed mode). A source read through this mode is a plain
// runtime-routed data read via the existing shared `m68k_emit_runtime_ea_address`
// helper and the existing owned-cartridge-region read mechanism (ADR-0006),
// never a computed control-flow target, so it introduces no new EA-
// computation or dispatch logic. This mask is consumed only at this shared
// family's decode call sites; it is never merged into
// `m68k_ea_jsr_jmp_control_modes`/`m68k_ea_lea_control_modes`, which keep
// their own distinct, narrower control-EA legality per ADR-0009, and it is
// kept separate from `m68k_ea_move_family_source` (MOVE/MOVEA's own primary
// source set, which also admits the PC-relative indexed form this family
// does not).
inline constexpr M68kEaLegalMask m68k_ea_arithmetic_logical_indexed_source =
    m68k_ea_move_source | m68k_ea_index8;
// SEG-021-T006: ADD/ADDA/SUB/SUBA/CMP/CMPA source set, written from the Motorola
// M68000 Family Programmer's Reference Manual (base MC68000 columns; independent of the
// T001 dataset): every addressing mode, i.e. exactly `m68k_ea_move_family_source`. ADD/SUB/CMP
// (ordinary, reverse and immediate forms) destinations are `m68k_ea_data_alterable_with_index`.
inline constexpr M68kEaLegalMask m68k_ea_add_sub_cmp_source = m68k_ea_move_family_source;
// ADD/SUB `Dn,<ea>` (opmode 4-6): memory alterable only. The Dn (and An) encodings of this opmode range are
// ADDX/SUBX, a different instruction owned by a later task, never an ADD/SUB Dn,Dn form.
inline constexpr M68kEaLegalMask m68k_ea_reverse_arithmetic_destination =
    m68k_ea_data_alterable_with_index & ~m68k_ea_dn;
// SEG-021-T007: AND/OR source set, written from the Motorola M68000 Family Programmer's Reference
// Manual (base MC68000 columns; independent of the T001 dataset): every data addressing mode --
// all modes except An -- for every size, including `(d8,An,Xn)`, `(d8,PC)`, `(d8,PC,Xn)` and #imm.
inline constexpr M68kEaLegalMask m68k_ea_and_or_source = m68k_ea_move_family_source & ~m68k_ea_an;
// AND/OR `Dn,<ea>` (opmode 4-6): memory alterable only (Dn/An encodings of that opmode range are
// ABCD/SBCD/EXG, not AND/OR); EOR `Dn,<ea>` and ANDI/ORI/EORI destinations: data alterable.
inline constexpr M68kEaLegalMask m68k_ea_and_or_reverse_destination =
    m68k_ea_data_alterable_with_index & ~m68k_ea_dn;
inline constexpr M68kEaLegalMask m68k_ea_eor_destination = m68k_ea_data_alterable_with_index;
inline constexpr M68kEaLegalMask m68k_ea_logical_immediate_destination = m68k_ea_data_alterable_with_index;
// SEG-007-T248: ADDQ/SUBQ's data-alterable (non-An) destination set, widened
// to also admit the brief-format `(d8,An,Xn)` indexed mode -- the same base-
// MC68000 addressing-mode extension MOVE's own destination mask already
// applies (`m68k_ea_move_family_destination` above). ADDQ/SUBQ's An
// destination case is decoded through a separate, unrelated `m68k_ea_an`
// mask (never index8-eligible) and is unaffected by this addition.
inline constexpr M68kEaLegalMask m68k_ea_addq_subq_destination =
    m68k_ea_data_alterable | m68k_ea_index8;
// TST's base-MC68000 legal operand set: excludes An, d16(PC), and immediate
// (all three are 68020+-only for TST specifically; see the contract's
// correction). SEG-007-T248: adds `m68k_ea_index8` -- the brief-format
// `(d8,An,Xn)` indexed mode is an ordinary base-MC68000 data addressing mode
// TST already supports architecturally (Motorola M68000 Family Programmer's
// Reference Manual, TST instruction description: legal modes are Dn, (An),
// (An)+, -(An), d16(An), (d8,An,Xn), Abs.W, Abs.L -- excluding only An,
// Immediate, and both PC-relative forms). This mask was never revisited when
// SEG-007-T120 introduced `m68k_ea_index8` and widened several other
// instructions' masks (MOVE's primary source/destination, the arithmetic/
// logical indexed source set, LEA's control-EA set); TST was simply missed.
// Decode is the sole owner of this addressing-mode legality fact -- no lift,
// EA-calculation, or emission change is needed: `M68kIrKind::test_operand`'s
// shared C4 lowering and `m68k_emit_ea_read`/`m68k_emit_runtime_ea_address`
// already handle `address_index8` identically to `address_disp16` (see
// SEG-007-T120/T136 and this same task's AOT-safety-predicate carve-out).
inline constexpr M68kEaLegalMask m68k_ea_tst_operand = m68k_ea_data_alterable_with_index;
// SEG-007-T025 (Batch C, C3): BTST's project-selected read-only destination
// set. BTST is architecturally broader than BCHG/BCLR/BSET (it may also
// read via d16(PC), which the three mutating forms may never target), but
// T025 deliberately excludes an immediate destination even though the base
// ISA's dynamic (Dn bit-number source) BTST form architecturally permits
// one (pinned Musashi's own legend: "A+-DXWLdxI") -- see the batch
// contract's bit-operation section for the exact scope decision. This is
// exactly `m68k_ea_data_alterable` (Dn plus the six memory-alterable T023
// forms BCHG/BCLR/BSET also use) plus `m68k_ea_pc_disp16`.
// SEG-021-T008: bit-operation legal-EA contract, written from the Motorola M68000 Family Programmer's
// Reference Manual (BTST/BCHG/BCLR/BSET entries, base MC68000 columns; independent of the T001 dataset):
//   BCHG/BCLR/BSET (both bit-number forms): data alterable -- Dn, (An), (An)+, -(An), d16(An), (d8,An,Xn),
//     abs.W, abs.L (never An, PC-relative or immediate).
//   BTST with a static (#n) bit number: the data-alterable set plus d16(PC) and (d8,PC,Xn); no immediate.
//   BTST with a dynamic (Dn) bit number: the static set plus #imm.
inline constexpr M68kEaLegalMask m68k_ea_bit_modify_destination = m68k_ea_data_alterable_with_index;
inline constexpr M68kEaLegalMask m68k_ea_bit_test_destination =
    m68k_ea_data_alterable_with_index | m68k_ea_pc_disp16 | m68k_ea_pc_index8;
inline constexpr M68kEaLegalMask m68k_ea_bit_test_dynamic_destination =
    m68k_ea_bit_test_destination | m68k_ea_immediate;
// SEG-007-T025 (Batch C, C5a/C5b/C5c): MOVEM's legal EA sets, verified
// against M68000PM/AD Rev. 1 Sec 4 MOVEM entry and pinned Musashi's
// m68k_in.c opcode table ("re ." row legend "A..DXWL...": An indirect,
// d16(An), absolute.w, absolute.l; the dedicated "re pd" row additionally
// selects -(An) for register->memory; "er ." row identical to "re .", plus
// the dedicated "er pi" row selecting (An)+ and the dedicated "er pcdi" row
// selecting d16(PC), both for the memory->register direction only).
// Register->memory legally EXCLUDES PC-relative entirely (there is no legal
// MOVEM store through PC-relative addressing) and EXCLUDES postincrement
// (never legal for register->memory); memory->register legally EXCLUDES
// predecrement (never legal for memory->register). Indexed modes
// (d8(An,Xn)/d8(PC,Xn)) remain permanently out of the project's EA tranche,
// matching every other selected control-EA form.
inline constexpr M68kEaLegalMask m68k_ea_movem_register_to_memory =
    m68k_ea_an_indirect | m68k_ea_an_disp16 | m68k_ea_absolute_word | m68k_ea_absolute_long | m68k_ea_an_predec;
inline constexpr M68kEaLegalMask m68k_ea_movem_memory_to_register =
    m68k_ea_control_modes | m68k_ea_an_postinc;
// SEG-007-T088: MOVE to SR's project-selected source set. Excludes
// address-register direct (m68k_ea_an) as an architectural fact: verified
// against the exact encoding-structure fact in pinned Musashi's own
// disassembler opcode table (`m68kdasm.c`'s `g_opcode_info`: mask 0xffc0,
// base 0x46c0, legal-EA legend 0xbff, whose bit 10 -- An-direct -- is
// unset), the same encoding-structure citation style SEG-007-T025's
// shift/rotate work already relies on (Musashi is consulted here only for
// this structural encoding fact, never as a hardware-semantics or
// privilege-behavior authority; see the console-developer research
// discipline this milestone already follows). Additionally, as a
// deliberate project scope decision (not an architectural exclusion), this
// task additionally excludes every memory-operand source mode that is
// otherwise architecturally legal here -- the three absolute/PC-relative
// control-addressing forms (abs.W, abs.L, d16(PC)), which require this
// project's separate C4 static-memory-fact/gap-tracking system (already
// established for TST/CLR/MOVE's own absolute operands), and the four
// An-indirect-family memory forms ((An)/(An)+/-(An)/d16(An)), which this
// task's own general_startup C4 block-emission dispatcher deliberately
// groups with write_moveq/write_user_stack_pointer's shared plain,
// non-runtime-routed `M68kMemoryEmissionContext` (no `ram_array`
// configured there) rather than threading a new routed/fact-checked memory
// path for this one new kind -- neither this task extends to this new kind.
// Only the two operand positions with a fully self-contained, register-
// file-only or literal-constant C lowering remain selected: Dn and #imm.
// This mirrors this milestone's own established precedent for deliberately
// narrowing an otherwise-legal architectural EA set to a project-scoped
// subset (BTST's own excluded-immediate-destination precedent; MOVEM's
// runtime-routing-instead-of-folding precedent). A later task may widen
// MOVE to SR's decode to recognize the remaining seven architecturally-legal
// forms once it also wires proper runtime routing/fact-tracking for this
// new kind; until then they remain unrecognized and fall through to
// `valid_but_unsupported_instruction` like any other neighboring form.
// Indexed forms (d8(An,Xn)/d8(PC,Xn)) are not part of any existing
// `m68k_ea_*` mask in this project at all and remain permanently out of
// scope project-wide, matching every other selected EA form.
inline constexpr M68kEaLegalMask m68k_ea_move_to_sr_source = m68k_ea_dn | m68k_ea_immediate;
// SEG-007-T116: MOVE from SR's project-selected destination set. The public
// Motorola M68000 Family Programmer's Reference Manual (1988, `M1`) documents
// MOVE from SR (opcode word 0100 0000 11 mmm rrr, 0x40C0-0x40FF) as a word
// operation whose destination is any data-alterable addressing mode (Dn plus
// the six memory-alterable modes). This task deliberately narrows that set to
// data-register-direct only -- the exact shape the authorized generated-native
// route reaches -- for the same reason SEG-007-T088 narrowed
// m68k_ea_move_to_sr_source to Dn/#imm: a memory destination would require
// threading this new kind through this project's C4 static-memory-fact /
// runtime-routed memory-access machinery, which this task does not wire up.
// The excluded memory forms remain unrecognized and fall through to
// `valid_but_unsupported_instruction` like any other neighboring form; a later
// task may widen this once it also wires that routing/fact machinery for this
// kind. An-direct is never a legal MOVE from SR destination on any 68000-family
// part (it is not in the data-alterable set) and is excluded architecturally.
inline constexpr M68kEaLegalMask m68k_ea_move_from_sr_destination = m68k_ea_dn;
// SEG-007-T118: MOVE <ea>,CCR's project-selected source set. The public Motorola
// M68000 Family Programmer's Reference Manual (1988, `M1`) documents MOVE to CCR
// (opcode word 0100 0100 11 mmm rrr, 0x44C0-0x44FF) as a word operation whose
// source is any data addressing mode (Dn plus every memory mode plus the
// PC-relative modes and immediate); the source is read as a word and only its
// low-order byte is copied into the CCR (the upper byte is ignored). This task
// deliberately narrows that set to data-register-direct only -- the exact shape
// the authorized generated-native route reaches -- for the same reason
// SEG-007-T088 narrowed m68k_ea_move_to_sr_source and SEG-007-T116 narrowed
// m68k_ea_move_from_sr_destination to Dn: a memory or PC-relative source would
// require threading this new kind through this project's C4 static-memory-fact /
// runtime-routed memory-access machinery, which this task does not wire up. The
// excluded modes remain unrecognized and fall through to
// `valid_but_unsupported_instruction` like any other neighboring form; a later
// task may widen this once it also wires that routing/fact machinery for this
// kind.
inline constexpr M68kEaLegalMask m68k_ea_move_to_ccr_source = m68k_ea_dn;

enum class M68kMemoryAccessWidth { byte = 1, word = 2, long_word = 4 };
enum class M68kMemoryAccessDirection { read, write };

enum class M68kInstructionKind {
  moveq, subq_l_1_d0, bne_short, bra_short, rts,
  // SEG-007-T047 / ADR-0020 §9: RTE, the architected encoding 0x4E73, selected
  // only under the general_startup policy, beside rts. It consumes exactly the
  // basic MC68000 exception stack frame this task's IRQ6 entry constructs (SR
  // at SP, PC at SP+2). RTR/TRAP/TRAPV/ILLEGAL remain out of scope / rejected.
  // Static translation only: no runtime instruction fetch of any kind.
  rte,
  // SEG-007-T023 shared whitelist forms. Each `kind` spans every selected
  // size/EA-mode combination for its mnemonic. The EA-mode/size variability lives in
  // M68kDecodedInstruction::size/source_ea/destination_ea, not in the kind.
  tst, cmp, cmpi, cmpa, add, adda, addi, addq, sub, suba, subi, subq, logical_and, andi, logical_or, ori, eor, eori, move, movea, clr, lea, jmp, jsr,
  // SEG-007-T168: NOT <ea> (0100 0110 ss mmmrrr, 0x4600-0x46FF), the unary
  // logical-complement sibling of AND/OR/EOR (`logical_and`/`logical_or`/
  // `eor`) and CLR (`clr`). Its legal destination EA set is exactly CLR's own
  // `m68k_ea_data_alterable` (M68000PM/AD Rev. 1 §4, "not" entry, "Data
  // Alterable" addressing category) -- data-register-direct plus every
  // memory-alterable mode including `(An)+`/`-(An)`. `destination_ea` carries
  // the sole read-then-written operand; there is no `source_ea` (matching the
  // `clr`/`swap`/`ext_w` convention for a kind with no second operand).
  // Unlike `clr` (write-only), NOT reads the destination's current value,
  // complements it, and writes the result back -- a genuine one-address
  // read-modify-write, exactly like `shift_rotate`'s memory form.
  not_operand,
  // NEG.W Dn (0100 0100 01 000 rrr). This bounded capability deliberately
  // excludes byte/long and every memory-EA form.
  negate_word,
  // SEG-007-T025 (Batch C, C1): SWAP Dn, EXT.W Dn, EXT.L Dn. Register-only
  // forms -- no EA mode beyond Dn, no memory access. `destination_ea` (not
  // `source_ea`) carries the single Dn operand these read-then-write,
  // matching the existing `clr` convention of naming a write-only-shaped
  // slot the destination even where, as here, the same slot is also read.
  swap, ext_w, ext_l,
  // SEG-007-T025 (Batch C, C2): PEA <ea>, LINK An,#<word displacement>,
  // UNLK An. PEA has no destination_ea (its one EA is source_ea, the address
  // to compute and push -- matching the existing jmp/jsr convention). LINK's
  // `destination_ea` carries its An operand and `source_ea` carries its
  // decoded raw (not yet sign-extended) word-displacement immediate fact;
  // UNLK's `destination_ea` carries its sole An operand, matching swap/ext.
  pea, link, unlk,
  // SEG-007-T025 (Batch C, C3): BTST/BCHG/BCLR/BSET. `source_ea` carries the
  // bit-number fact -- either `{data_register, reg}` (dynamic Dn source) or
  // `{immediate, ..., raw_bit_number, ...}` (static source, raw/not yet
  // masked, exactly like every other selected immediate operand); it is
  // never a memory EA. `destination_ea` carries the Dn-or-memory destination
  // (the existing project EA vocabulary, no new mode); its mode determines
  // operation width (`size`): `data_register` -> long_word (32-bit), any
  // memory mode -> byte (8-bit) -- there is no word-destination bit-
  // operation form. BTST never writes destination_ea; BCHG/BCLR/BSET always
  // do (see M68kIrKind's matching bit_test/bit_change/bit_clear/bit_set and
  // m68k_evaluate_bit_operation, the one shared semantic owner).
  btst, bchg, bclr, bset,
  // SEG-007-T025 (Batch C, C4): the general (unconditional or conditional)
  // branch, generalizing BRA and every Bcc condition into one truthful
  // decode identity rather than one kind per condition (contract: "generic
  // branch identity"). `condition` (a new typed fact -- see
  // M68kDecodedInstruction::condition) distinguishes BRA (`always`) from
  // each selected Bcc condition; `size` records the displacement width
  // (byte or word); `source_ea` carries the raw (not yet sign-extended)
  // displacement immediate, exactly like every other selected immediate
  // operand. This is the `general_startup`-only generalized form; the
  // pre-existing `bne_short`/`bra_short` compatibility kinds above remain
  // the sole selected forms for the frozen `direct_flow` profile, and both
  // ultimately derive their target from the one shared
  // `m68k_branch_target` formula (contract: "branch target formula").
  branch,
  // BSR: a direct call sharing the project's one call/continuation/callee
  // identity owner (M68kStaticCall and friends) with JSR -- never a second
  // return-identity formula. `source_ea` carries the raw displacement
  // immediate exactly like `branch` above; `size` records its width.
  bsr,
  // DBcc Dn,<word displacement>. `destination_ea` (mode data_register,
  // matching the existing swap/ext/link/unlk convention for a sole register
  // operand) is the decremented register; `condition` is the shared typed
  // condition fact (DBT/DBF included); `source_ea` carries the raw word
  // displacement immediate; `size` is always word (DBcc has no byte form).
  dbcc,
  // SEG-007-T025 (Batch C, C5): MOVEM.W/L, register-list<->memory. One
  // truthful representation (contract: "one truthful MOVEM identity") spans
  // every selected width/direction/EA combination -- never one kind per
  // combination. `movem_direction` distinguishes registers_to_memory from
  // memory_to_registers; `movem_register_mask` carries the raw 16-bit
  // register-list extension word (decoded, never re-derived); the memory
  // operand's EA lives in `source_ea` (memory_to_registers: memory is READ)
  // or `destination_ea` (registers_to_memory: memory is WRITTEN) -- the
  // register list itself has no EA representation, matching the existing
  // `unused` convention for a kind's absent operand position.
  movem,
  // SEG-007-T025 (Batch C, C6): register-form shift/rotate. One truthful
  // representation (contract: "one truthful shift/rotate representation")
  // spans every selected family/width/count-source combination -- never one
  // kind per combination (e.g. never `asl_byte_immediate`). `shift_rotate_kind`
  // (below) distinguishes the 8 base-MC68000 families; `size` records the
  // operation width (byte/word/long_word; there is no memory-form width
  // ambiguity here since C6 never selects a memory destination). `source_ea`
  // carries the count fact -- either `{immediate, ..., resolved 1-8 count,
  // ...}` (the encoded 3-bit field already resolved at decode time per the
  // documented 0->8 rule; never a second interpretation elsewhere) or
  // `{data_register, count_reg, ...}` (the Dn holding the runtime count,
  // masked to its low 6 bits only at evaluation time, never at decode time,
  // since it depends on live register state); `destination_ea` carries the
  // sole Dn destination (matching the existing swap/ext/unlk convention for
  // a read-then-written register operand). C6 selects Dn destinations only.
  //
  // SEG-007-T025 (Batch C, C7a): the memory-WORD forms sharing this same
  // primary opcode family (`ss==11`, structurally disjoint from every C6
  // register form) are now ALSO selected through this SAME
  // `M68kInstructionKind::shift_rotate` identity -- never a second
  // `memory_asl`/`memory_asr`/... kind, matching the contract's "keep one
  // instruction family identity" rule. For a memory form: `source_ea` is
  // unused/default (the count is architecturally fixed to exactly 1 -- there
  // is no count field in this word shape at all); `destination_ea` carries
  // the single memory EA, restricted to `m68k_ea_memory_alterable`
  // ((An)/(An)+/-(An)/d16(An)/absolute.w/absolute.l -- never Dn/An-direct/
  // PC-relative/immediate/indexed); `size` is always `word` (there is no
  // BYTE or LONG memory-form shift/rotate). Lift distinguishes the register
  // form (`destination_ea.mode == data_register`, lowered to
  // `M68kIrKind::shift_rotate_register`) from the memory form (every other
  // destination mode, lowered to `M68kIrKind::shift_rotate_memory`, a
  // genuine one-address read-modify-write) purely from `destination_ea`,
  // never a second decoded fact.
  shift_rotate,
  // MOVE An,USP retains its encoded An source as source_ea.
  move_an_to_usp,
  // SEG-007-T088: MOVE <ea>,SR (opcode word 0100 0110 11 mmm rrr, 0x46C0-
  // 0x46FF), a privileged System Control Group instruction per the public
  // Motorola M68000 Family Programmer's Reference Manual (1988), the same
  // `M1` citation already used by this milestone's prior CPU-decode-gap
  // tasks (see docs/references/genesis-rom-startup-contract.md); word size
  // only. `source_ea` carries the decoded source operand (see
  // m68k_ea_move_to_sr_source below for the project-selected legal set);
  // there is no `destination_ea` (the destination is the fixed SR
  // pseudo-register this project's runtime already models generically as
  // its existing 16-bit `sr`/`status_register` field -- no new persistent-
  // state concept). See the MOVE to SR compatibility policy appended to
  // docs/architecture/genesis-move-an-usp-startup-compatibility-policy.md
  // for the narrow, explicitly-labeled no-privilege-check project policy
  // this decode/lift/emission selects, mirroring SEG-007-T085's own MOVE
  // An,USP precedent.
  move_to_sr,
  // SEG-007-T114: NOP (opcode word 0100 1110 0111 0001 = 0x4E71), a fixed,
  // no-operand, no-extension-word System Control Group instruction, per the
  // public Motorola M68000 Family Programmer's Reference Manual (1988) NOP
  // entry -- the same `M1` citation this milestone's prior CPU-decode-gap
  // tasks already use (see docs/references/genesis-rom-startup-contract.md
  // and docs/references/m68k-nop-contract.md). The PRM states: "No operation
  // occurs ... The processor state, other than the program counter, is
  // unaffected." Condition codes X/N/Z/V/C are not affected; NOP is
  // unprivileged; there is no memory or device access, no exception, and no
  // interrupt effect. The only architectural effect is that the program
  // counter advances past the single instruction word. There is no
  // `source_ea` or `destination_ea` (NOP has no operands). Accepted only by
  // the `general_startup` decode profile; `genesis_startup` still rejects
  // 0x4E71 as a CPU frontier. See docs/references/m68k-nop-contract.md for
  // the bounded no-operand System Control family inventory and why only NOP
  // is in scope for the existing shared decode/lift/IR/C11 owner.
  nop,
  // SEG-007-T116: MOVE from SR (opcode word 0100 0000 11 mmm rrr, 0x40C0-
  // 0x40FF), a System Control Group instruction per the public Motorola
  // M68000 Family Programmer's Reference Manual (1988, `M1`, the same
  // citation SEG-007-T059/T085/T088/T114 already use; see
  // docs/references/genesis-rom-startup-contract.md and
  // docs/references/m68k-move-from-sr-contract.md). Word size only; the source
  // is the fixed Status Register (this project's existing generic 16-bit
  // `sr`/`status_register` runtime field -- no new persistent state); the
  // destination is any data-alterable EA, deliberately narrowed here to
  // data-register-direct (see m68k_ea_move_from_sr_destination). On the
  // original MC68000 MOVE from SR is UNPRIVILEGED (it became privileged only
  // from the MC68010) and affects NO condition codes (X/N/Z/V/C unchanged) --
  // contrast MOVE to SR, which overwrites the whole SR. A register (or
  // ordinary memory) destination raises no exception. `destination_ea`
  // carries the decoded operand (matching the `clr` convention for a
  // write-shaped sole operand); there is no `source_ea`. Accepted only by the
  // `general_startup` decode profile; `genesis_startup`/`direct_flow` still
  // reject 0x40C0-0x40FF. See the MOVE from SR compatibility policy appended
  // to docs/architecture/genesis-move-an-usp-startup-compatibility-policy.md.
  move_from_sr,
  // SEG-007-T118: MOVE <ea>,CCR (opcode word 0100 0100 11 mmm rrr, 0x44C0-
  // 0x44FF), a System Control Group instruction per the public Motorola M68000
  // Family Programmer's Reference Manual (1988, `M1`, the same citation
  // SEG-007-T059/T085/T088/T114/T116 already use; see
  // docs/references/genesis-rom-startup-contract.md and
  // docs/references/m68k-move-to-ccr-contract.md). Word size only; the source is
  // any data addressing mode, deliberately narrowed here to data-register-direct
  // (see m68k_ea_move_to_ccr_source); the source word is read and only its
  // low-order byte is copied into the CCR (the upper byte is ignored). The
  // destination is the implied CCR -- the low byte of this project's existing
  // generic 16-bit `sr`/`status_register` runtime field; the upper (system)
  // byte of SR is untouched -- no new persistent state. MOVE to CCR is
  // UNPRIVILEGED on every 68000-family part and affects ALL condition codes
  // (X/N/Z/V/C all taken from source bits 4..0) -- contrast MOVE from SR, which
  // is a pure SR read affecting no condition codes, and MOVE to SR, which
  // overwrites the whole 16-bit SR. A register (or ordinary memory) source
  // raises no exception. `source_ea` carries the decoded operand; there is no
  // `destination_ea`. Accepted only by the `general_startup` decode profile;
  // `genesis_startup`/`direct_flow` still reject 0x44C0-0x44FF. See the MOVE to
  // CCR compatibility policy appended to
  // docs/architecture/genesis-move-an-usp-startup-compatibility-policy.md.
  move_to_ccr,
  // SEG-007-T220: MULS.W <ea>,Dn (opcode word 1100 ddd 111 mmmrrr,
  // 0xC1C0-0xC1FF/0xCFC0-0xCFFF depending on Dn), the signed 16x16->32
  // multiply per the public Motorola M68000 Family Programmer's Reference
  // Manual (1988, `M1`) MULS entry. Shares the exact opcode line (1100) and
  // register/mode/reg field layout as the already-selected AND/EOR register
  // family (`logical_and`/`eor`, opmode field bits 8-6), but is decoded
  // separately: `m68k_decode_general_logical`'s shared `decode_register_
  // logical` helper explicitly declines opmode 3 (MULU) and opmode 7 (MULS)
  // for every family, so MULS/MULU never reach that shared body. Only the
  // reached form, MULS.W (opmode 111), is selected; MULU.W (opmode 011)
  // remains unsupported. The legal source EA set is exactly the same
  // "data addressing modes without An-direct" set the AND/OR/EOR
  // register-form source already uses for opmode<=2
  // (`m68k_ea_arithmetic_logical_indexed_source & ~m68k_ea_an`), per the
  // manual's MULS source-operand table -- deliberately excluding
  // `m68k_ea_pc_index8`: the brief-format PC-relative indexed source stays
  // an established fail-closed non-goal for this exact opcode line/opmode
  // (an existing regression pins this down), matching the evidenced
  // runtime-reached shape (register-direct/immediate), not a speculative
  // widening. `source_ea`
  // carries the decoded word-size source; `destination_ea` carries the
  // fixed `{data_register, Dn}` destination (the manual's MULS is always
  // Dn-destination, never a memory destination) -- matching the existing
  // `add`/`logical_and` two-operand convention. `size` is always word (the
  // source read width; the product itself is always a full 32-bit Dn
  // write, matching ADDA's own established "read narrower than the
  // register it widens into" shape rather than a same-width uniform
  // EA-to-EA family). Condition codes: N/Z set from the full 32-bit
  // product, V=0, C=0, X unaffected -- the exact same CCR contract as the
  // AND/OR/EOR logical family (`M68kLogicalResultSpecification`), reused
  // verbatim rather than a second hand-written flag rule.
  multiply_signed_word,
  // SEG-007-T222: MULU.W <ea>,Dn (opcode word 1100 ddd 011 mmmrrr,
  // 0xC0C0-0xC0FF/0xCEC0-0xCEFF depending on Dn) -- the unsigned sibling of
  // `multiply_signed_word` on the exact same opcode line/opmode field
  // (opmode 011, the form `decode_register_logical`'s shared body already
  // declines for every logical family). Same legal source EA set, same
  // fixed `{data_register, Dn}` destination, same word-size source /
  // full-32-bit-Dn-write shape, and the same excluded `m68k_ea_pc_index8`
  // non-goal as MULS.W -- see that entry's doc comment for the shared
  // rationale, not repeated here. CCR: N/Z from the full 32-bit unsigned
  // product, V=0, C=0, X unaffected -- reuses `M68kLogicalResultSpecification`
  // exactly like MULS.W.
  multiply_unsigned_word,
  // SEG-007-T222: DIVS.W <ea>,Dn (opcode word 1000 ddd 111 mmmrrr,
  // 0x81C0-0x81FF/0x8FC0-0x8FFF depending on Dn) -- signed 32-bit Dn
  // dividend / signed 16-bit sign-extended source-EA divisor, per the public
  // Motorola M68000 Family Programmer's Reference Manual DIVS entry (see
  // docs/decisions/0037-synchronous-mc68000-divide-by-zero-vector-5-
  // exception-entry.md). Same legal source EA set / excluded
  // `m68k_ea_pc_index8` non-goal as MULS.W/MULU.W. `source_ea` carries the
  // decoded word-size divisor; `destination_ea` is always
  // `{data_register, Dn}` (the full 32-bit dividend/result register).
  // Divisor == 0: raises the synchronous vector-5 CPU exception (ADR-0037);
  // Dn is completely unchanged, no CCR update. Quotient overflow (does not
  // fit in a signed 16-bit range, including 0x8000_0000 / -1): Dn completely
  // unchanged, V=1 (defined, project-chosen deterministic N/Z -- see
  // M68kDivisionResultSpecification), C=0, X unaffected. Normal case:
  // signed 16-bit quotient in Dn's low word, signed 16-bit remainder (same
  // sign as the dividend) in Dn's high word; N/Z from the signed quotient;
  // V=0; C=0; X unaffected.
  divide_signed_word,
  // SEG-007-T222: DIVU.W <ea>,Dn (opcode word 1000 ddd 011 mmmrrr,
  // 0x80C0-0x80FF/0x8EC0-0x8EFF depending on Dn) -- the unsigned sibling of
  // `divide_signed_word` through the SAME shared `M68kDivisionResultSpecification`
  // semantic owner (signedness is an explicit parameter, never a duplicated
  // implementation). Unsigned 32-bit dividend / unsigned 16-bit divisor;
  // same divide-by-zero/overflow/CCR contract as DIVS.W with unsigned
  // comparison (quotient > 0xFFFF is the overflow condition).
  divide_unsigned_word,
};
// SEG-007-T025 (Batch C, C5): MOVEM's direction bit (contract: "one truthful
// MOVEM identity"). Never combined with M68kCondition -- MOVEM reads no
// condition and writes no CCR/SR bit at all.
enum class M68kMovemDirection { registers_to_memory, memory_to_registers };
// SEG-007-T025 (Batch C, C6): the 8 base-MC68000 register-form shift/rotate
// families (contract: "one truthful shift/rotate representation"). C6a
// selected `lsl`/`lsr`; C6b widened this same enum (never a parallel,
// differently-shaped representation) with `asl`/`asr`; C6c widened it with
// `rol`/`ror`; C6d completes it with `roxl`/`roxr`, on this same task
// branch -- all 8 base-MC68000 register-form families are now represented.
enum class M68kShiftRotateKind { lsl, lsr, asl, asr, rol, ror, roxl, roxr };

// SEG-007-T025 (Batch C, C4): the one shared typed condition representation
// for Bcc and DBcc (contract: "one shared condition-code owner"). `always`/
// `never` are BRA/BSR's and DBT/DBF's condition-field slots (0000/0001);
// Bcc's own decoder never constructs either (those bit patterns select BRA/
// BSR instead, per the batch contract). No condition consults X. Evaluated
// from the current SR by m68k_evaluate_condition and lowered to an
// equivalent C boolean expression by m68k_condition_c_expr -- the single
// owner both host semantics and generated C consume; no per-condition
// formula is hand-written a second time anywhere else.
enum class M68kCondition { always, never, hi, ls, cc, cs, ne, eq, vc, vs, pl, mi, ge, lt, gt, le };

struct M68kDecodedInstruction {
  InstructionProvenance provenance{};
  M68kInstructionKind kind{M68kInstructionKind::moveq};
  DataRegister destination{DataRegister::d0};
  std::int8_t operand{};
  // The single verified owner of the complete selected instruction span.
  // provenance.bytes/length independently retain the inherited primary-word
  // storage; raw_bytes additionally retains every verified byte (2 for a
  // primary-only form, 6 for a selected extension form) so no other stage
  // re-reads the source image for bytes already verified here.  Empty for
  // profiles/forms that never populate it.
  std::vector<std::uint8_t> raw_bytes;
  // The big-endian 32-bit value of raw_bytes[2..5] for a verified six-byte
  // selected form; zero otherwise.  Decoded and verified only here.
  std::uint32_t extension{};
  // SEG-007-T023: operand size and typed EA facts for the shared whitelist
  // kinds above. `unused` for whichever operand position a given kind does not have (e.g. TST/CLR
  // have no destination_ea; JMP/JSR have no destination_ea at all -- their
  // one EA is source_ea, the jump/call target).
  M68kMemoryAccessWidth size{M68kMemoryAccessWidth::long_word};
  M68kEffectiveAddress source_ea{};
  M68kEffectiveAddress destination_ea{};
  // SEG-007-T025 (Batch C, C4): meaningful only for `branch`/`bsr`/`dbcc`;
  // `always` (harmless default) for every other kind. Placed last so every
  // existing positional aggregate-initialization call site elsewhere in the
  // codebase remains valid unchanged (it simply default-initializes here).
  M68kCondition condition{M68kCondition::always};
  // SEG-007-T025 (Batch C, C5): meaningful only for `movem`; harmless
  // defaults (`registers_to_memory`/`0`) for every other kind. Placed last,
  // matching `condition`'s own placement rule, so every existing positional
  // aggregate-initialization call site elsewhere in the codebase remains
  // valid unchanged.
  M68kMovemDirection movem_direction{M68kMovemDirection::registers_to_memory};
  std::uint16_t movem_register_mask{};
  // SEG-007-T025 (Batch C, C6): meaningful only for `shift_rotate`; harmless
  // default (`lsl`) for every other kind. Placed last, matching
  // `condition`/`movem_direction`'s own placement rule.
  M68kShiftRotateKind shift_rotate_kind{M68kShiftRotateKind::lsl};
};

} // namespace segarecomp
