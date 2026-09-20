# C4 MOVEM adjacent-LEA constant-propagation contract (SEG-007-T075)

## Purpose and boundary

This is Checkpoint 1 of SEG-007-T075: a written soundness argument for one narrowly bounded
translation-time constant-propagation capability, produced and confirmed by this task's own direct
repository inspection before any production file changed. It is not a general data-flow or SSA
proposal; it states the exact conditions under which one specific producer/consumer instruction pair --
a `load_effective_address` (LEA) immediately followed, in the same straight-line block, by a
`movem_transfer` (`memory_to_registers`) whose register-indirect/postincrement-family source EA names
the register LEA just set -- may be lowered as literal constants instead of routed through
`genesis_route_access`.

It consumes, and does not relitigate:

- [`genesis-generalized-startup-runtime-bridge-contract.md`](genesis-generalized-startup-runtime-bridge-contract.md)
  §10 (the runtime ROM-read invariant: "the generated program never reads ROM bytes again at runtime")
  and §11 ("A read may remain a translation-time constant only when its target region and value are
  both proven immutable and read-only at discovery time"). This capability is an instance of §11's rule,
  not a relaxation of it: every value it folds is proven immutable/read-only by the same
  single-mapping-claim discipline `retain_fact` already applies to every other folded ROM fact.
- SEG-007-T074's evidence (authoritative, not re-derived here): the real Sonic ROM's accepted C4 prefix
  reaches a `movem_transfer` (`memory_to_registers`) through a register-indirect/`(An)+`-family EA whose
  base address register is set, by the immediately preceding same-block instruction, to a literal
  absolute ROM constant via `load_effective_address`, with no other write to that register in between;
  the routed lowering of that MOVEM currently reaches `GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY`
  because the runtime has no ROM-read success route (§10/§11).
- `docs/architecture/c4-move-predecrement-postincrement-commit-contract.md`, reused here only as a
  precedent for this project's own "diagnose, then state ten conditions in writing, then implement only
  if they hold as a direct reuse" discipline, not for any MOVE-specific mechanism.

## What was directly read to ground this contract

- `src/m68k_pipeline.cpp`: `case M68kIrKind::load_effective_address:` (the literal-constant lowering for
  `absolute_word`/`absolute_long`/`pc_disp16`, and `m68k_genesis_canonical_ea_address`,
  `m68k_is_statically_foldable_control_ea`); `case M68kIrKind::movem_transfer:` in full, including the
  `direct_flow`-only reference branch's own `rom_folded` local (which reads
  `memory->movem_transfer_regions`/`movem_transfer_values`, fields already declared on
  `M68kMemoryEmissionContext` but never populated by any real C4 caller today -- only by
  `tests/tools/m68k_batch_c_test_harness.cpp`'s differential-oracle harness), and the
  `memory->runtime_routing` branch's `address_indirect`/`address_disp16` (combined) and `address_postinc`
  sub-branches, which snapshot the working EA into one local exactly once and defer the architectural `An`
  writeback until after the whole per-slot loop.
- `src/m68k_pipeline_frontend.cpp`: `retain_fact` (the discovery-time per-instruction fact pass, whose
  own switch records a fact only for `move`/`movea`/`tst`/`clr`/`andi` -- never `movem`) and its call
  sites; `require_fact` and its call sites; `valid_c4_static_memory_fact` (the file-local, anonymous-
  namespace independent re-verification function `preflight_m68k_general_startup_c4` calls, structurally
  the precedent this task's own new re-verification function mirrors); the near-duplicate inline fact
  re-validation `emit_m68k_general_startup_runtime_c` performs immediately before building its own
  `facts` lookup map for the per-block emission loop; and that loop's own `case M68kIrKind::movem_transfer:`
  (which today unconditionally sets `routed.runtime_routing = true` for every MOVEM, with no fact lookup
  at all).
- `include/segarecomp/m68k_pipeline.hpp`: `M68kStaticBlock`/`BlockId`, `M68kStaticMemoryFact`,
  `FrontendAnalysis` (confirming `static_memory_facts`' own doc comment: "Verified static operand facts
  retained for C4 lowering. They are never reconstructed from a host pointer or source image after
  discovery."), `FrontendPartialProgram` (confirming neither it nor `FrontendAnalysis` retains the raw
  ROM image bytes -- `preflight_m68k_general_startup_c4` and `emit_m68k_general_startup_runtime_c` both
  take only a `FrontendPartialProgram`/`FrontendAnalysis`, never a `FrontendProgram`, so neither function
  has any image handle to re-read at C4 time), and `M68kMemoryEmissionContext::movem_transfer_regions`/
  `movem_transfer_values`.

## Decision

**Go.** The ten conditions below hold as a direct reuse of the existing fact machinery
(`M68kStaticMemoryFact`-shaped discovery-time fact / `valid_c4_static_memory_fact`-shaped independent
C4 re-verification) and the existing MOVEM lowering technique (the `direct_flow` reference branch's own
`rom_folded` per-slot literal, and the `runtime_routing` branch's existing snapshot-once/defer-writeback
discipline for `address_indirect`/`address_postinc`). No unresolved design fork was found:

- No ambiguity about which register a multi-write producer chain resolves to -- this capability never
  considers a multi-write chain at all; it requires the producer to be the single immediately-preceding
  retained instruction (condition 2), so there is exactly one candidate producer per consumer, or none.
- No aliasing question the existing fact machinery cannot already answer -- LEA computes an address only
  (it performs no memory access itself), so there is no read/write ordering hazard analogous to
  `c4-move-predecrement-postincrement-commit-contract.md`'s Q2/Q3 to resolve.
- No restructuring of `m68k_emit_ea_read`/`m68k_emit_ea_write`/`m68k_emit_runtime_ea_address` -- this
  capability adds one new `rom_folded`-style branch inside the existing `runtime_routing` MOVEM
  sub-branches, populating the existing (currently C4-unused) `movem_transfer_regions`/
  `movem_transfer_values` fields; it calls none of the three shared EA-address primitives differently
  than today, and does not touch them at all.

## The ten conditions (corrected against direct inspection)

1. **Same block.** Producer and consumer both appear in one `M68kStaticBlock::instructions` list (same
   `BlockId`). Confirmed representable directly: `FrontendAnalysis::static_blocks` already carries this
   exact shape, and block membership is unchanged by this capability.
2. **Immediate adjacency.** The consumer is at index `i` and the producer at index `i - 1` in that same
   `instructions` list, for some `i >= 1`. Because `static_blocks` retains a straight-line, gap-free,
   address-ordered sequence of exactly the retained instructions in that block (every other stage of this
   pipeline already relies on this construction), adjacency in this list is precisely "no intervening
   retained instruction of any kind between them" -- no separate gap check is needed beyond the index
   check itself.
3. **Producer shape.** The producer's decoded `kind` is `M68kInstructionKind::lea`, and its `source_ea`
   satisfies `m68k_is_statically_foldable_control_ea` (`absolute_word`/`absolute_long`/`pc_disp16`). Its
   resolved literal value is `producer.source_ea.absolute_address` itself -- the exact raw, decode-time
   field LEA's own `case M68kIrKind::load_effective_address:` emission writes, unmodified, into
   `UINT32_C(0x...)` for all three of these EA kinds. This is corrected from an earlier draft of this
   contract, which used `m68k_genesis_canonical_ea_address(producer.source_ea)` instead: that helper
   agrees with LEA's own raw literal for `absolute_long` and `pc_disp16` (both pass `absolute_address`
   through unchanged), but for `absolute_word` it additionally masks the value to 24 bits
   (`ea.absolute_address & 0x00FFFFFF`), while decode sign-extends a 16-bit `absolute_word` field to 32
   bits when populating `absolute_address` -- so for any `absolute_word` producer whose 16-bit value has
   bit 15 set, the masked helper's result differs from what LEA's own statement actually assigns to the
   address register. Using `producer.source_ea.absolute_address` directly removes that divergence for all
   three foldable EA kinds and keeps this the single, exclusive address-resolution formula this capability
   uses: this capability never introduces a second, independent address-resolution formula, and never
   calls `m68k_genesis_canonical_ea_address` for the producer's own resolved literal. (A raw
   `absolute_word` value with bit 15 set also cannot land inside any real ROM `MappingClaim` -- condition
   6's containment check below naturally rejects it, so this capability still folds nothing unsound for
   that case; it simply never folds it, exactly like every other out-of-claim address.) The producer's
   `destination_ea.reg` is the address register that literal is assigned to (LEA's destination is always
   an address register by decode; no register-class check is needed beyond that existing decode
   invariant).
4. **Consumer shape.** The consumer's decoded `kind` is `M68kInstructionKind::movem`,
   `movem_direction == M68kMovemDirection::memory_to_registers`, and `source_ea.mode` is exactly
   `address_indirect` (`(An)`) or `address_postinc` (`(An)+`) -- the register-indirect/`(An)+`-family
   shape SEG-007-T074's own real-ROM diagnosis observed (quoted above: "register-indirect/`(An)+`-family
   EA"). `address_disp16` (`d16(An)`) is a different, wider family with no real-ROM evidence behind it
   here and is deliberately excluded (Non-goals). `source_ea.reg` names the exact same address register
   the producer's `destination_ea.reg` wrote.
5. **No intervening write.** No instruction of any kind writes that same address register between the
   producer and the consumer. This follows from conditions 1-2 alone (a gap-free straight-line block with
   nothing retained between the two instructions has no third instruction that could perform such a
   write); no separate scan is required.
6. **Whole-transfer single-claim containment.** Let `order = m68k_movem_transfer_order(consumer.
   movem_register_mask, M68kMovemTransferOrder::ascending)` (register-indirect/`(An)+` MOVEM
   memory-to-registers is never the reversed predecrement order -- decode's own legal-EA set never
   selects predecrement for that direction) and `width = consumer.size` (word or long only; MOVEM has no
   byte form). The complete range `[base, base + order.size() * width)`, where `base` is condition 3's
   resolved literal, must resolve inside exactly one `MappingClaim` from the accepted prefix's own
   `mapping_claims` (`claims(mapping_claims, base).size() == 1` and the claim's `target_end` covers the
   full range) -- the same single-claim discipline `retain_fact` already applies to every other folded ROM
   fact, extended here to cover the whole multi-slot span rather than only the base address.
7. **Independent C4 re-verification.** A new typed producer/consumer fact
   (`M68kMovemAdjacentLeaFact`, discovery-time, populated in a separate additive pass alongside
   `retain_fact` -- never inside it, and never changing `retain_fact`'s own per-instruction fact shape or
   call sites) carries only: the producer and consumer `InstructionProvenance`, the shared address
   register, the resolved literal base, and one resolved literal register value per `order` slot. A new
   function structurally parallel to `valid_c4_static_memory_fact` (file-local, anonymous namespace,
   `valid_c4_movem_adjacent_lea_fact`) independently re-derives conditions 1-6 from the decoded/lifted
   instruction stream, the block list, and the mapping-claim list -- never merely trusting the
   discovery-time record for the producer/consumer identity, the shared register, the resolved base, the
   EA shape, the adjacency, or the mapping-claim containment. Consistent with the existing
   `M68kStaticMemoryFact` precedent (whose own re-verification never re-reads `program.image.bytes` for a
   `raw_cartridge_rom` fact's `immutable_value`) and with the fact that neither
   `preflight_m68k_general_startup_c4` nor `emit_m68k_general_startup_runtime_c` is ever given a ROM image
   handle at all (`FrontendPartialProgram` carries only `accepted_prefix`/`frontiers`, never the source
   image), the per-slot resolved literal *values* themselves are trusted from the discovery-time record,
   exactly like every other folded ROM fact in this codebase; only the *identity, shape, adjacency, and
   mapping-claim containment* that justify trusting those values are independently re-derived.
8. **Preserved MOVEM semantics.** The fold changes only how each slot's source value is obtained (a
   literal constant instead of a routed `genesis_route_access` read); it never changes `order` (register
   order), WORD sign extension / LONG non-extension (both still lowered through the one shared
   `m68k_sign_extend_expr` formula), or the consumer's own address-register postincrement writeback
   arithmetic (still computed on the same snapshot-once-defer-writeback-last local the `runtime_routing`
   branch already uses for `address_postinc`; a folded slot still advances that local by `width` exactly
   like a routed slot does).
9. **Live-base-guarded source selection.** Conditions 1-8 authorize embedding both the validated
   immutable values and the producer-derived base in the consumer's one generated body. At the MOVEM
   instruction boundary that body snapshots the live architectural `An` state once. Only when that
   snapshot equals the validated base does every slot consume its direct `UINT32_C(0x...)` literal, with
   no executed `genesis_route_access`; a different live base (including direct interior entry that did
   not execute the adjacent producer) makes every slot use the existing generic routed read. Both paths
   reuse the same per-slot sign-extension/register-write and MOVEM working-EA owners; this is not a
   second instruction body, predecessor token, or alternate compiled-entry set.
10. **Fail-closed, all-or-nothing default.** Any shape not satisfying every one of conditions 1-9 --
     including a consumer with no matching fact at all -- remains routed through the existing
     `memory->runtime_routing` MOVEM path exactly as today. At runtime a transfer selects the immutable
     source in full (every slot) or the routed source in full based on condition 9's one base snapshot;
     the fact is only ever recorded, and only ever consulted at emission, as one complete
     `order.size()`-length value vector, never a partial one. A forged or stale
    `M68kMovemAdjacentLeaFact` (fields that no longer match the decoded/lifted stream, the block list, or
    the mapping-claim list) fails independent re-verification and rejects the whole translation
    (`/* translation rejected: ... */`), exactly like a forged `M68kStaticMemoryFact` does today -- never
    a silent fall-back-to-routing for that one instruction, since a fact that fails re-verification is
    itself evidence of an inconsistent retained prefix, not merely an absent optimization opportunity.

## Acceptance shape for the implementation that follows in this same task

- **New type:** `M68kMovemAdjacentLeaFact` (header-declared, alongside `M68kStaticMemoryFact`) and
  `FrontendAnalysis::movem_adjacent_lea_facts`.
- **New discovery-time pass:** an additive loop over `prefix.static_blocks`, run in the same
  `analyze_m68k_frontend` scope `retain_fact` already runs in, that populates
  `movem_adjacent_lea_facts` for every adjacent pair satisfying conditions 1-6. `retain_fact` itself, its
  existing per-instruction fact shape, and its existing call sites are unchanged.
- **New re-verification function:** `valid_c4_movem_adjacent_lea_fact`, called from both
  `preflight_m68k_general_startup_c4` (rejecting the whole preflight for any invalid/forged fact, mirroring
  the existing `static_memory_facts` loop) and `emit_m68k_general_startup_runtime_c` (rejecting the whole
  translation for any invalid/forged fact, mirroring the existing inline `static_memory_facts`
  re-validation there, before building a lookup map keyed by the consumer's source address).
- **New MOVEM sub-branch:** inside `emit_m68k_operation_c`'s `case M68kIrKind::movem_transfer:`
  `memory->runtime_routing` branch, the `address_indirect` and `address_postinc` sub-branches use the
  existing `M68kMemoryEmissionContext::movem_transfer_regions`/`movem_transfer_values` fields plus the
  validated producer base. The one emitted MOVEM body compares its live working-EA snapshot to that
  base and selects all immutable values on equality or the existing routed `emit_slot` calls otherwise.
  The C4 emission loop populates this state only for a consumer instruction with a fact that passed
  re-verification. Every other MOVEM shape falls through to the existing routed calls unchanged.
- **Untouched:** `retain_fact`'s existing per-instruction fact shape and call sites, `require_fact`'s
  existing gates, the `direct_flow`-only `rom_folded` reference branch, and every other already-shipped
  C4 lowering's fold-vs-route behavior.

## Explicit non-claims

This contract makes no claim about `address_disp16` MOVEM sources, `registers_to_memory` MOVEM,
`write_movea` producers, or any producer other than `load_effective_address`. It does not widen
`retain_fact`'s per-instruction fact shape for any existing kind, and it does not relax
`docs/architecture/genesis-generalized-startup-runtime-bridge-contract.md` §10/§11 for any case outside
the ten conditions above.
