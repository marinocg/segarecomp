# ADR 0022: Generation-Time Immutable In-Cartridge Offset-Table Descriptor Finite-Value Producer

- Status: Accepted
- Date: 2026-09-03
- Amends: ADR 0009 (adds one additive input producer for its existing
  `M68kFiniteIndexValueSet`; changes nothing downstream of that fact)
- Related: ADR 0006 (bounds/immutability arithmetic precedent only), ADR 0002,
  ADR 0003

## Context

SEG-007-T160 (PR #272) classified the runtime-selected Sonic Phase-B frontier
as an unresolved indirect control-flow target reached inside the IRQ6/VBlank
handler's retained static prefix: a program-counter-relative brief-format
indexed subroutine call (`call_general`, `M68kEaMode::pc_index8`, word-width
data-register index) whose index register is first narrowed by a bounded word
`ANDI #mask` (mask popcount within ADR-0009's `<= 8` bound) and then
**overwritten by a 16-bit value read from an immutable in-cartridge offset
table**, immediately before the computed call.

ADR-0009's accepted `M68kFiniteIndexValueSet` producer models only `MOVEQ` and
bounded word `ANDI`. A table-load write to the index register is "any other
write," which fails closed to `reached_unresolved_direct_edge` /
`unresolved_indirect_target`. ADR-0009 §"Target-set proof and boundaries"
explicitly anticipates exactly this gap:

> A future producer may prove a finite index value set from an immutable table
> descriptor, but that is an alternative input to `M68kFiniteIndexValueSet`,
> not a prerequisite or a different dispatch path. It must satisfy the same
> finite-value and per-candidate EA evaluation contract.

This ADR is that anticipated producer.

## Decision

Add one **generation-time immutable in-cartridge offset-table descriptor
finite-value producer** as an additive transfer rule inside ADR-0009's existing
bounded forward register-value analysis (`analyze_finite_index_values` in
`src/cpu/m68k/static_discovery.cpp`). It produces a finite `M68kDnValueState`
for a destination data register from a proven immutable cartridge-data read; it
is otherwise identical in type, cap, ordering, merge, and fail-closed behavior
to the two producers ADR-0009 already names. Everything downstream of the
resulting `M68kFiniteIndexValueSet` — per-candidate `M68kIndirectTargetEaSet` EA
evaluation, target admission/walk, `indirect_branch`/`indirect_call` edges,
candidate-specific JSR frames, C4 `indirect_targets_<source>[]` emission, the
`m68k_indirect_target_member` runtime guard, and `genesis_dispatch` — is reused
**unchanged**. No ceiling of any kind is widened. No new dispatcher, no second
target-set representation, no new frontier class.

### Producer proof obligation

The producer yields a finite value set for data register `Dd` only when every
one of the following is established; any failure yields `unknown` (ADR-0009's
default/fail-closed state) exactly as "any other write" does today:

1. **Recognised shape.** The writing instruction is `MOVE.W <ea>,Dd` where
   `<ea>` is `M68kEaMode::pc_index8` with a data-register word index
   (`!index_is_address && !index_is_long`) — base-MC68000 brief format, implicit
   scale 1. Any other instruction, size, destination mode, index kind, or EA
   mode is not this producer and falls through to the existing rules.
2. **Finite bounded selector.** The incoming fixed-point state for the index
   (selector) register is `finite` and non-empty. Its member count is the
   element-count bound. In the reached shape that bound originates from the
   preceding bounded word `ANDI #mask` (`popcount(mask) <= 8`), but the producer
   only requires a finite non-empty incoming set from any ADR-0009 producer, so
   there is no separate "selector-bound vs entry-count" field that could
   mismatch — the selector set *is* the enumerated entry set.
3. **Descriptor.** One explicit immutable table descriptor is derived:
   - base + provenance: `pc_base_address` of the `MOVE.W` source EA (the address
     of its own brief extension word, set by decode for every PC-relative EA)
     plus the sign-extended 8-bit displacement; the selector is the only
     variable term.
   - entry width: 2 bytes (word read).
   - indexing stride: the word selector value is the byte offset into the table
     (implicit scale 1). Only the proven selector positions are inspected; no
     uniform-stride assumption is made.
   - element count / selector bound: the selector set cardinality (`<= 256`).
   - complete mapped bounds: `[min_entry_addr, max_entry_addr + 2)` must lie
     entirely inside exactly one mapping claim (exactly one claim in
     `mapping_claims` may overlap *any* byte of that half-open interval, and it
     must fully contain the interval; an interior-only second overlap, a
     conflicting `>1` overlap, a partial overlap, or an absent mapping all fail
     closed) with a structurally valid affine image mapping, verified with the
     existing
     `MappingClaim` arithmetic (`target_end > target_begin`,
     `image_end - image_begin == target_end - target_begin`, range within the
     generation-time image). Every mapping claim reaching general-startup
     discovery is an immutable cartridge-image region by construction
     (ADR-0006 §2: "ownership and immutability are implicit; only mapping and
     bounds need independent re-verification"), so this reuses exactly the same
     trust `instruction_source` already relies on — cited as precedent for the
     bounds/immutability arithmetic only.
   - generation-time immutability: the bytes are cartridge-image bytes consumed
     once at generation time.
   - deterministic entry interpretation: see below.
4. **Bounded descriptor.** `max_entry_addr - min_entry_addr + 2 <= 0x10000`.
5. **Address arithmetic.** Every entry address
   `canonical(pc_base_address + signed_d8 + sign_extend_16(selector))` is
   `>= 0`, `<= 0x00FFFFFF` (24-bit), and **even** (a `MOVE.W` from an odd
   address is architecturally invalid on the MC68000). Any overflow or 24-bit
   canonical-address overflow in the entry-address or table-address arithmetic
   fails closed.
6. **Entry read.** Each proven entry's 2 bytes are read from the
   generation-time image, big-endian, through the narrow
   `read_immutable_cartridge_bytes` environment fact method (a single bounded
   read of the whole descriptor range; a partially-mapped, unmapped, or
   conflicting (`>1` claim) range fails closed).
7. **Cap.** The ordered deduplicated value set has `<= 256` members (ADR-0009's
   existing `M68kFiniteIndexValueSet` cap); more fails closed to `unknown`.

The produced set is the sorted, deduplicated set of the 16-bit entry values,
folded into `M68kDnValueState{finite=true, values=...}` for `Dd`. It then
participates in ADR-0009's merge, cap, and fixed-point exactly like any other
finite state.

### Deterministic 16-bit-entry interpretation (sign-extension + relative base)

The 16-bit table entry becomes the **index term of the following
`JSR/JMP (d8,PC,Xn)`**, so its interpretation is exactly ADR-0009's existing
per-candidate EA rule with no new semantics:

- The entry value is written to `Dd` as a raw low word. ADR-0009's
  `M68kIndirectTargetEaSet` construction already evaluates
  `canonical(extension_word_address + signed_d8 + index_value)` with
  `index_value = sign_extend_16(Dn)` for a **word** index. The resolved target
  is therefore `canonical(pc_base_address_of_the_control_instruction +
  signed_d8_of_the_control_instruction + sign_extend_16(entry))`.
- Relative base: the control instruction's own brief extension-word address
  (`M68kEffectiveAddress::pc_base_address`), never the table base, never an
  emitter-local instruction length, never the `MOVE.W`'s base.
- Signedness: signed (16-bit two's-complement, sign-extended to 32 bits) — the
  single deterministic choice, identical to every other word Dn index in
  ADR-0009.

This is the only interpretation the pipeline represents. An unsigned
interpretation, a table-relative base, or a long entry width is **not**
represented; a site requiring any of those does not match producer condition 1
(or fails ADR-0009's own word/long index rule) and retains the existing
fail-closed frontier. There is no configuration knob and no ambiguity: exactly
one semantics exists.

### ADR-0006 vs ADR-0009 boundary (pinned)

This producer is a **generation-time image-inspection finite-value producer
living as an ADR-0009 input**, not a runtime cartridge-data ownership mechanism
and not an ADR-0006 redesign:

- The table bytes are consumed **once, at generation time**, to compute a
  finite value set. The generated executable never reads the table for
  dispatch, never fetches or decodes a target opcode, and gains no new runtime
  read path. Dispatch uses only ADR-0009's sorted candidate-address literal
  array and the existing `m68k_indirect_target_member` guard.
- The incidental `MOVE.W (d8,PC,Xn),Dn` instruction that loads a table entry is
  still lowered by C11 exactly as it is today (SEG-007-T136: an ordinary
  runtime-routed data read through the existing owned-cartridge-region
  mechanism). This ADR adds no runtime data-access call site, no new
  `GenesisOwnedCartridgeRegion`, and does not change `genesis_route_access`
  behavior. The producer only makes discovery *prove* the finite value the load
  yields on each path.
- The new descriptor proof reuses the pipeline's existing `MappingClaim`
  mapping/bounds arithmetic and the implicit `raw_cartridge_rom`
  ownership/immutability discovery already establishes — the same trust
  ADR-0006 §2 relies on — cited as precedent for the bounds/immutability
  arithmetic only, not as an ADR-0006 ownership extension.
- ADR-0006 is not amended, reopened, or extended. No real ownership invariant
  found during implementation required it.

### Ownership and seam

- `include/segarecomp/cpu/m68k/static_discovery.hpp`: one new narrow fact
  method on `M68kStaticDiscoveryEnvironment`,
  `read_immutable_cartridge_bytes(base, length) -> optional<{bytes, claim}>`,
  answering a single question about one already-identified address range. It
  makes no walk/recurse/successor/frame decision.
- `src/machine/genesis/frontend.cpp`
  (`M68kGeneralStartupEnvironment`): implements that method by iterating
  `program_.mapping_claims` once, collecting every claim whose target interval
  overlaps the half-open requested interval `[base, base+length)`, requiring
  exactly one such claim, and requiring that claim to pass
  `structurally_valid_mapping_claim` affine arithmetic and to fully contain
  the interval. This rejects a second claim that overlaps only an interior
  descriptor subrange, which an endpoint-only check would miss.
- `src/cpu/m68k/static_discovery.cpp`: the additive transfer rule inside
  `m68k_apply_finite_value_transfer` / `analyze_finite_index_values`. Nothing
  else in static discovery changes.
- Everything from `M68kFiniteIndexValueSet` onward (facts, edges, frames, C4
  validation/emission, runtime guard, dispatcher) is reused unchanged.

### Fail-closed list

The producer yields `unknown` (retaining the existing
`reached_unresolved_direct_edge` at the source instruction, projected as
`unresolved_indirect_target`), or — once a set is proven — the existing
`m68k_indirect_target_member` non-member stop naming the source control
instruction with PC/A7 unchanged, for at least:

- selector not finitely proven; unsupported/ambiguous index-register
  provenance; long-width or otherwise unsupported index form;
- writing instruction not `MOVE.W`, destination not a data register, or source
  EA not `pc_index8`;
- table source not resolvable through exactly one mapping claim (conflicting,
  interior-only-overlapping, or absent mapping); descriptor range unmapped or
  only partially mapped against the generation-time image;
- descriptor range wider than the bounded limit;
- integer overflow or 24-bit canonical-address overflow in entry-address or
  table-address arithmetic;
- odd (word-misaligned) entry address, or any architecturally invalid target;
- resulting value set exceeding the existing 256-member cap;
- a later write invalidating the proven index value (ADR-0009's existing
  merge/fixed-point already forces `unknown`);
- a runtime-computed EA not in the statically proven candidate set (existing
  `m68k_indirect_target_member` non-member stop).

## Consequences

SEG-007-T161 implements and tests exactly this producer with synthetic legal
fixtures: a positive resolution through a synthetic immutable offset table to
ordinary translated blocks; deterministic byte-identical two-run generation;
adversarial-negative fail-closed cases for each condition above; and a
pinned-Musashi differential for the EA/index/sign-extension semantics,
confirming existing direct/indirect control-transfer coverage is unaffected.

### Test-coverage boundary (which failure conditions each suite proves)

The SEG-007-T161 producer negative tests do **not** independently re-prove
every ADR-0022 *and* ADR-0009 failure condition. Coverage splits as follows.

- **NEW ADR-0022 producer-proof regressions — covered by the T161 producer
  negative tests** (`general_startup_rejects_unprovable_immutable_offset_table_forms`,
  `general_startup_rejects_interior_overlap_of_the_immutable_offset_table_descriptor`,
  and the positive fold test): recognised `MOVE.W (d8,PC,Xn),Dn` shape;
  finite non-empty selector; the single-covering-claim obligation for the
  whole descriptor range — **including the interior-overlap case where a
  second structurally valid claim overlaps only an interior descriptor
  subrange while neither endpoint is ambiguous**; conflicting (`>1`) and
  partially mapped descriptor ranges; bounded descriptor limit; entry-address
  arithmetic (even/24-bit/overflow); the whole-descriptor immutable read via
  `read_immutable_cartridge_bytes`; the 256-member cap.
- **INHERITED ADR-0009 coverage — proved by the pre-existing ADR-0009 /
  SEG-007-T124 tests, not re-proven here**: per-candidate `M68kIndirectTargetEaSet`
  target admission rejecting odd / unmapped / mid-instruction candidate
  targets; the `m68k_indirect_target_member` runtime non-member stop; and
  later-write invalidation of a proven index value through ADR-0009's existing
  merge / fixed-point. The T161 producer only supplies a new finite input to
  that unchanged machinery; it does not restate those guarantees.

The interior-overlap adversarial regression
(`general_startup_rejects_interior_overlap_of_the_immutable_offset_table_descriptor`)
was added in the PR #274 review correction: the first implementation of
`read_immutable_cartridge_bytes` proved its single-covering-claim obligation
only at the first and last descriptor byte, so a second structurally valid
claim overlapping only an interior subrange was wrongly accepted. The helper
now iterates every mapping claim once, collects every claim whose target
interval overlaps `[base, base+length)`, requires exactly one, and requires
that one to be structurally valid and to fully contain the interval.
After the change, execution evidence — not this ADR — chooses the next action:
the authorized generated-native Sonic route either walks through the newly
resolved indirect target, or reaches a new honestly recorded terminal.

### Observed boundary at the authorized Sonic Phase-B site (SEG-007-T161)

The producer engages fully at the runtime-selected Sonic Phase-B site: the
`MOVE.W` shape is recognised, the incoming selector is finite (it originates
from a defensive word `ANDI #mask` over a byte the handler prefix loads from
the 68k work-RAM region), the descriptor range resolves through exactly one
structurally valid immutable cartridge-image mapping claim,
`read_immutable_cartridge_bytes` succeeds, and a sorted deduplicated finite
entry set is folded into `M68kFiniteIndexValueSet`. Per-candidate EA evaluation
then derives at least one architecturally invalid (odd, word-misaligned)
target, and target admission correctly rejects the whole candidate set
(ADR-0009 requires the complete proven set; a partial set is unsound). The
site therefore retains `reached_unresolved_direct_edge` /
`unresolved_indirect_target` at the source control instruction — the
fail-closed outcome this ADR anticipates.

The proof obligation that genuinely cannot be met for this site's shape is
condition 2's premise that "the selector set *is* the enumerated entry set":
the selector bound here is a defensive hardware/RAM-status mask, not a proof
that every masked value indexes a real table entry. The true index is a
runtime work-RAM-resident routine selector, so the mask-derived submask
enumeration over-approximates the real table extent and drives the producer to
interpret bytes beyond the real jump table (adjacent code/data) as PC-relative
call offsets. Within this additive abstraction there is no sound way to prove
which enumerated selector positions are real entries; the producer's whole-set
fail-closed behaviour is correct. This is a genuine fail-closed terminal, not a
regression and not a too-strict check: resolving it needs either the concrete
runtime selector value or an independent proof of the real table's entry count,
neither of which this generation-time finite-value producer can supply. The
site stays an honest fail-closed terminal (NOT SUPPORTED) and remains the sole
`RUNTIME_SELECTED` frontier on the authorized route.

### Reuse caveat (over-approximation layering)

This producer inherits — and does not widen — ADR-0009's existing selector
over-approximation trust (`popcount(mask) <= 8` bounds a computed jump), and
stacks one further over-approximated layer: the finite *selector* set is used
as the table-*entry* set. At the Sonic site this is contained by the
odd/word-misaligned-target backstop plus ADR-0009's whole-set (all-or-nothing)
target admission, the 256-member cap, and the `m68k_indirect_target_member`
runtime non-member stop. If this producer is later reused at a site where every
over-approximated non-entry word happens to be even and lands on an admissible
in-image target, those safeguards would not fire. Any such future reuse must
first establish an independent bound on the real table's entry count rather
than relying on the defensive selector mask.
