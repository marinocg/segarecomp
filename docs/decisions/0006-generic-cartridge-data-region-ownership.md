# ADR 0006: Generic Immutable/Generated Cartridge-Data Region Ownership

- Status: Accepted
- Date: 2026-08-25
- Amends: The T028 generalized-startup partial-program/runtime-bridge architecture contract's §6
  ("Shared runtime memory/device routing boundary"), §10 ("ROM/data ownership and lifetime", including
  its "Invariant binding SEG-007-T029" paragraph), and §11 ("Static folding versus runtime routing
  precedence") (`docs/architecture/genesis-generalized-startup-runtime-bridge-contract.md`), consumed by
  SEG-007-T077. Does not amend, and does not relitigate, §14's privacy boundary (ADR 0004/ADR 0005),
  §7's build-time-rejection precedence, §4's dispatcher contract, or any C4 lowering call site.

## Context

SEG-007-T074 diagnosed a recurring family: the generated Sonic startup bridge reaches `genesis_
route_access`'s existing fail-closed guard for a runtime-routed cartridge **read** whose effective
address is not a translation-time constant. SEG-007-T075 resolved one bounded instance with a narrow
scalar constant-propagation fold (an adjacent `LEA`→`MOVEM` producer/consumer pair whose base address
and per-slot values are all statically known). SEG-007-T076 found a second, non-identical recurrence of
the same generic guard for a different (`long`-width) runtime-routed read. Per this repository's own
the project charter ("Advancement Discipline") and the `sonic-startup-capability-inventory` skill's cartridge-
data escalation rule, a second non-identical recurrence after one bounded scalar fold requires evaluating
a **generic** immutable/generated cartridge-data ownership mechanism rather than a third scalar-fold
exception. SEG-007-T077 is that mechanism's checkpoint-and-implement task; this ADR records its design.

### The actual pre-existing gap this ADR resolves is broader than one instruction shape

Direct source inspection performed for this task found that the T028 contract's own §6 text already
overstated what the shipped C4 lowering actually does. §6 (pre-ADR-0006) stated: "under §10's chosen
ROM-ownership option (a), no accepted generated operation may ever call `genesis_route_access` for a ROM
**read**: every valid ROM read is either folded to a translation-time constant (§11) or is not accepted
at all (§7)." That was true of T029's original narrow scope, but SEG-007-T067 through SEG-007-T072
(generalizing `write_move`/`write_movea`/`test_operand`/`write_clr`/`logical_and_immediate`/
`movem_transfer` onto register-indirect, postincrement, predecrement, and `d16(An)` effective-address
families) already made it false in practice: every one of those representable, non-constant-foldable
effective-address forms is unconditionally routed through `genesis_route_access` at runtime **regardless
of what address it resolves to at execution time**, exactly matching §11's own "every access whose
target address is not proven statically constant... must route through §6's boundary at runtime instead
of being folded" rule. A runtime-computed address that happens to resolve into ROM (`< 0x00400000`)
therefore always reached this boundary once those tasks shipped -- SEG-007-T074/T075/T076's own evidence
is the empirical proof -- and it always failed unconditionally on arrival, because §6's ROM branch never
implemented anything but the defensive fail-closed case. This ADR does not create a new call site, a new
class of accepted C4 operation, or a new reachable frontier; it gives an **already-reachable** case a
sound, bounds-checked success outcome, closing a documentation/implementation drift the T028 contract's
own text never caught up to.

### The proof obligation this mechanism requires

The pipeline already carries exactly the proof this mechanism needs: `MappingClaim` (`include/segarecomp/
m68k_pipeline.hpp`), the region-mapping/bounds record `genesis_valid_provenance` already trusts for every
other purpose, threaded onto `FrontendAnalysis`/`FrontendPartialProgram.accepted_prefix.mapping_claims`
and already used by SEG-007-T075's own `claims(program.mapping_claims, base)` containment check. Every
`MappingClaim` reaching general-startup discovery is `raw_cartridge_rom`-named by construction at every
production call site (`src/main.cpp`'s `emit-general-startup-bridge-c` and `genesis-general-startup`/
`genesis-rom-startup` commands), so ownership and immutability are implicit; only mapping and bounds need
independent re-verification, matching the exact arithmetic `genesis_valid_provenance` already performs
(`target_end > target_begin`, `image_end - image_begin == target_end - target_begin`).

## Decision

`genesis_route_access`'s ROM branch gains one new, narrowly bounded, read-only success path: for
`direction == GENESIS_ACCESS_READ` only, before falling through to the existing fail-closed default, it
linearly scans a small, generated, build-time-constant table of `GenesisOwnedCartridgeRegion` descriptors
(`begin`, `end`, a pointer to a compiled `static const uint8_t[]` backing array, and a redundant `length`
that is re-checked, matching `GenesisMappingClaim`'s own style); on a full-width in-bounds match, it reads
big-endian bytes from that array and returns `GENESIS_ACCESS_OK`. No match falls through to the existing,
unchanged `GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY` fail. Writes remain completely unaffected --
still unconditionally `GENESIS_DIAG_ROM_WRITE_PROHIBITED` -- and this mechanism is never consulted for
them.

The static proof obligation is:

1. **Region mapping and bounds.** Reuse `MappingClaim` exactly as-is: `target_begin < target_end`,
   `image_begin < image_end`, `image_end - image_begin == target_end - target_begin` (the `valid_claim`
   invariant, identical to `genesis_valid_provenance`'s own arithmetic), and `image_end <= program.image.
   bytes.size()` (in-bounds against the actual generation-time image).
2. **Ownership/immutability.** Implicit: every claim discovery produces for the `general_startup`
   profile is `raw_cartridge_rom`-backed by construction, matching the same implicit trust `retain_fact`'s
   own `raw_cartridge_rom` classification already relies on for the unrelated scalar-fold path.
3. **Hardware-window containment.** `target_end <= 0x00400000` -- the exact fixed cartridge-ROM window
   `genesis_route_access` already hardcodes, never overlapping the work-RAM (`0x00FF0000-0x01000000`) or
   device (`0x00A10000-0x00A10020`) windows.
4. **Generated backing data.** The region's bytes are resolved once, at discovery time (when
   `program.image.bytes` is available), into a new `M68kOwnedCartridgeRegionFact` (`claim`,
   `resolved_bytes`) retained on `FrontendAnalysis`/`FrontendPartialProgram.accepted_prefix.owned_
   cartridge_region_facts`, structurally parallel to `M68kStaticMemoryFact`/`M68kMovemAdjacentLeaFact`.
   Neither the independent C4 re-verification (`valid_c4_owned_cartridge_region_fact`, file-local to
   `src/m68k_pipeline_frontend.cpp`) nor the bridge emitter is ever given an image handle; both re-derive
   claim membership and every bound above from `mapping_claims` alone, and only check `resolved_bytes`'
   own **length** (never its content) against `image_end - image_begin`, the same "never re-read from a
   source image at C4 time" discipline `M68kMovemAdjacentLeaFact.transfer_values` already established.
   A fact that fails this re-verification, or duplicates an already-seen claim identity, rejects the
   whole translation -- never a silent skip -- exactly matching the existing forged-fact precedent.

This is address- and value-independent by construction: the proof covers the **region** (mapping,
ownership, immutability, bounds) and the **generated backing data**, never the exact effective address or
the exact value a given execution happens to read. A runtime-computed, non-constant-foldable address is
therefore just as eligible as a constant one, as long as it resolves in-bounds against a proven region --
this is what distinguishes the mechanism from SEG-007-T075's narrower scalar-constant-fold precedent,
which required the exact address and value pair to be known at translation time.

**No C4 lowering call site changes.** `m68k_emit_ea_read`, `m68k_emit_routed_read`, `m68k_emit_routed_
write`, and every `write_move`/`write_movea`/`test_operand`/`write_clr`/`logical_and_immediate`/`movem_
transfer` switch case in `emit_m68k_general_startup_runtime_c` are unchanged: they already route a
non-foldable effective address through `genesis_route_access` unconditionally, as established above. The
entire fix is new generated-data emission (`emit_m68k_general_startup_bridge_c`, the `FrontendPartial
Program` overload only, since that is the sole production emitter whose block bodies actually reach a
non-foldable ROM-eligible read at runtime) plus one new success branch inside `genesis_route_access`.
`GenesisRuntime` gains two new trailing fields (`owned_regions`, `owned_region_count`), zero-valued by
every existing `GenesisRuntime runtime = {0};` construction (verified directly: the two other
construction sites, in the synthetic-completion-only emitter overloads, never reach a runtime-routed ROM
read at all, so they are correctly left unwired), so every existing fixture that does not populate them
is provably unaffected.

## Resolving the whole-ROM-claim tension with this task's own "no general-purpose read path" non-goal

Today's sole production ROM-mapping call site (`emit-general-startup-bridge-c`, `src/main.cpp`)
constructs exactly **one** `MappingClaim` spanning the **entire** mapped ROM range (`[entry, entry +
image.size())` mapped to `[0, image.size())`). Under this mechanism, once any runtime-computed address
lands anywhere inside that one claim, the whole mapped ROM range becomes readable at runtime. This is
architecturally sound, and not a violation of this task's own "must not become a second general-purpose
runtime read path for arbitrary addresses" non-goal, for four reasons:

1. Every address outside every proven `MappingClaim`, and every address outside the fixed
   `[0, 0x00400000)` hardware ROM window, still fails closed exactly as before this ADR -- the bounds
   check is real and exact (proven by this task's own paired negative fixture, which fails one byte past
   a claim's own boundary even while a nearby in-region address in the same execution succeeds), never
   vacuous.
2. This mechanism never adds instruction fetch/decode capability. `genesis_route_access` is reached only
   from the existing data-access lowering paths (`m68k_emit_routed_read`/`m68k_emit_routed_write`), never
   from `genesis_dispatch`'s PC-driven control transfer. §10's binding invariant on SEG-007-T029 (never
   fetch/decode ROM instruction bytes at runtime) is structurally unweakened -- restated explicitly:
   unchanged by this ADR, under any revision.
3. A `MappingClaim` spanning the entire mapped ROM is not an arbitrary or unbounded claim -- it is itself
   a statically proven, discovery-time-verified region, the same claim `genesis_valid_provenance` already
   trusts today for every other purpose (retained-provenance emission, the scalar-fold path, static block
   discovery). The "generic" property this task requires is that the **address** be runtime-variable, not
   that the **region** be artificially narrower than what discovery has already legitimately proven; a
   region this task's own existing, unmodified discovery pass already establishes is not a new attack
   surface this ADR introduces.
4. Writes remain completely unaffected (still unconditionally `GENESIS_DIAG_ROM_WRITE_PROHIBITED`) and
   instruction dispatch remains completely unaffected -- only the **read** success/fail outcome for an
   already-recognized, already-reachable ROM region changes, from unconditional fail to bounds-checked
   success.

## Consequences

- The generated executable still never reads the original source ROM file/image at runtime (only its own
  compiled, build-time-embedded copy of a proven region) and never fetches or decodes an instruction at
  runtime, under any revision of this option -- unchanged from before this ADR.
- Fail-closed behavior for any read this mechanism cannot bounds-prove against a statically owned,
  immutable, generated region is unchanged: `GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY`/`GENESIS_
  DIAG_INTERNAL_DISPATCH_INCONSISTENCY`, exactly as before.
- No second control-transfer mechanism, no new frontier representation kind, and no restructuring of
  `genesis_dispatch` is introduced.
- A later, separately evidenced task that wants dynamic-indexing/table/checksum/decompression/asset/
  sound-data/DMA-visible cartridge-read coverage beyond what a single discovery-time `MappingClaim`
  already proves should cite this ADR as its region-ownership precedent rather than inventing a second
  mechanism; it remains bounded by the same non-goal (never a general-purpose runtime read path for a
  region discovery has not itself proven).
