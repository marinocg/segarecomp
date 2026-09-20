# ADR 0013: Discovery-Prefix Boundary Construction and Runtime-Confirmed Expansion

- Status: Accepted
- Date: 2026-08-31
- Amended by: SEG-007-T167 (2026-09-04) — Decision §7 seed/round cap raised `4U -> 5U` from
  re-confirmed executed evidence; see *Decision §7a*.
- Decided by: SEG-007-T133, from independent inspection of the current static-discovery,
  partial-program, C11-emission, runtime-dispatch, and startup-driver seams at this repository head,
  plus one authorized generated-route diagnosis of the pinned Sonic image at this same head.
- Amends: ADR 0010 (§§3-5) and ADR 0011 (§§1, 4), in each case only where noted below.
  Decision §6 adds one arm to `genesis_dispatch`, which ADR 0010 §5's must-not-touch list names
  explicitly and which ADR 0011 §1 twice calls "unchanged". Those literal qualifiers are amended; the
  substance both ADRs protect — that `genesis_dispatch` selects only already-emitted block identities
  and never fetches or decodes a target opcode — is preserved exactly (Decision §8).
  **Supersedes** ADR 0010 §3's factual premise that the instruction ceiling stops "with
  `discovery_budget_exhausted` at the exact instruction address whose decode would have crossed it,
  retaining full `InstructionProvenance` ... exactly as today", ADR 0010 §3's proposal to promote that
  stop by relaxing `runtime_frontier_eligible`'s `unresolved_reason` disqualifier or by granting
  `known_but_unemitted_target`-style exemptions, and ADR 0010 §4's claim that "the frontier PC is
  registered in the emitted block list bound to its `genesis_frontier_stop_<addr>` function [so that]
  when `genesis_dispatch` is next asked to continue at that PC it selects that function". Direct
  inspection (Context §§A-C below) falsifies all three against the current code.
  **Leaves intact**: ADR 0010 §1's fixpoint completeness definition; ADR 0010 §2's retirement of
  `m68k_discovery_max_blocks` as an independent gate and its bounded-resource-ceiling justification
  method for `m68k_discovery_max_instructions`; ADR 0010 §3's core choice that ceiling exhaustion is
  ADR 0002's *fail-closed trap* arm rather than its *whole-program reject* arm; ADR 0010 §4's IR
  no-change rule and its "no path past the boundary without a new build" argument; and **ADR 0011
  Decisions §§1-3 in full** (address-only iterative discovery, runtime-stack-owned return membership
  over a whole-program continuation set, and the finite-state termination model), which this ADR
  neither weakens nor reopens. ADR 0011 §5's reachability-provenance tagging discipline
  (`runtime_confirmed` vs `static_only`) is preserved verbatim and is load-bearing in Decision §7.
- Related: ADR 0002, ADR 0006, ADR 0007, ADR 0009, ADR 0012.
- Implemented by: SEG-007-T134, not this ADR. This ADR changes no `src/`, `include/`, `runtime/`,
  `tools/`, or `tests/` file, no `docs/architecture/*` file, and no capability-map row.

## Context

### A. Executed evidence at this exact head

One authorized generated-route diagnosis of the pinned commercial image (SHA-verified, commercial
mode, `--diagnose-frontier`) was run at this head. It deterministically produced
`result = rejected`, `category = discovery_budget_exhausted`, at the `static_discovery` pipeline
stage — **before** C emission, compilation, or any generated-native execution. Its normalized shape is
decisive and is recorded here without any address, offset, or byte value: the report carried
**instruction provenance and an instruction length**, but **null mapping claims and zero bus-access
records**, and the classified stop was a *call-admission* refusal, not the instruction ceiling.

Two conclusions follow, both independent of the image:

1. **ADR 0011 Decisions §§1-3 are unimplemented at this head.** SEG-007-T128's implementation
   candidate was reverted; `M68kStaticGraphWalker::walk` is still a single host-recursive member
   function keyed on `(addr, stack_signature(frame_stack))`
   (`src/cpu/m68k/static_discovery.cpp:633-651`), and the `JSR`/`BSR`/indirect-`JSR` call-admission
   refusals at `frame_stack.size() == limits_.max_call_frame_depth` are all still present
   (lines 752-758, 789-795, 953-958), with `m68k_discovery_max_call_frame_depth = 2U`
   (`include/segarecomp/machine/genesis/frontend.hpp:87`). The instruction ceiling is therefore **not**
   the stop the authorized route currently reaches; the call-depth gate is reached first. SEG-007-T128's
   own recorded evidence is the complementary half of this fact: with ADR 0011 §§1-2 mechanically
   applied in its (reverted) candidate, the very same route advanced past the call-depth signature and
   deterministically reached `m68k_discovery_max_instructions` instead.
2. **No budget-exhaustion stop of any shape can currently become a frontier exit**, because none of
   them carries the mapping-claim and bus-access structure `runtime_frontier_eligible` demands
   (Context §B). The observed null mapping claims and empty access list are the direct wire evidence
   of that.

For completeness, **ADR 0010 §2's retirement of `m68k_discovery_max_blocks` as an independent gate is
also unimplemented at this head**: SEG-007-T126 was delivered BLOCKED and its spike reverted, so the
hard rejection at `src/cpu/m68k/static_discovery.cpp:338-343` is live with
`m68k_discovery_max_blocks = 192U`. Where Decision §4 says that disposition "stands unchanged", it
means this ADR does not revise ADR 0010 §2's *decision*, not that the gate is already gone. Carrying
that accepted decision into production is an in-scope obligation of the implementing successor
(Consequences and Next-Task Shape, binding scope item 2), not a further decision and not a separate
task. This does not disturb Decision §1's ordering: SEG-007-T128's evidence shows the authorized route reached the
instruction ceiling with ADR 0011 §§1-2 applied **while the block gate was still live at 192U**.

### B. Why a budget stop cannot currently be promoted (direct inspection)

`runtime_frontier_eligible` exists as **two independent translation-unit-local mirrors**, exactly like
`c4_stop_class`: `src/machine/genesis/frontend.cpp:257-492` (anonymous namespace opened at line 18),
called by `partial_or_rejection` at lines 1493, 1508, and 1513; and
`src/codegen/c11/frontend.cpp:262-498` (anonymous namespace opened at line 12), called by
`build_genesis_frontier_stop_function` at line 878. C11 emission calls its **own** copy, not the
`machine/genesis` one. The two are structurally identical and must be kept in step; line numbers below
cite the `machine/genesis` mirror. Its top-level structural gate
(lines 261-275) rejects a diagnostic unless **all** of the following hold: `diagnostic.source_address`,
`diagnostic.image_offset`, `diagnostic.provenance`, `diagnostic.instruction_length`, and
`diagnostic.direct.has_provenance` are all present; `diagnostic.direct.unresolved_reason` is **empty**;
`diagnostic.direct.block.entry` equals the source address; and the provenance's own source address,
image offset, and length agree exactly with those fields. For every class except
`unresolved_indirect_target` it additionally requires (lines 311-330) **exactly one** mapping claim,
mirrored identically in `direct.mapping_claims`, selected as the unique affine claim covering the
provenance, and **exactly one** bus-access record that is an `instruction_read` from
`raw_cartridge_rom` at the provenance's own address whose byte count equals the provenance length and
whose first two bytes equal the provenance's primary bytes. Line 349 additionally rejects any
non-`known_but_unemitted_target` class whose `diagnostic.direct.has_target` is set.

`translate_m68k_discovery_issue` (`src/machine/genesis/frontend.cpp:784-860`) shows exactly where that
structure comes from: the mapping claim and the instruction-read bus record are reconstructed **only**
when the originating `M68kDiscoveryIssue` sets `reconstruct_single_mapping_claim` and
`reconstruct_instruction_read_access` (lines 837-857; a claim supplied directly on the issue is honored at lines 835-836), and the image offset only when
`set_pc_based_image_offset` or a provenance is present (lines 812-819). Discovery sets those flags on
its decode-issue path (`static_discovery.cpp:466-478`) and its resolved-operand rejection path
(lines 416-419) — and on **no** budget path.

The three budget producers are correspondingly ineligible in three different ways:

- **Instruction ceiling** (`static_discovery.cpp:437-444`): the check
  `instructions_used_ == limits_.max_instructions` is evaluated **before**
  `environment_.instruction_source(pc)` (line 445) and before `decode_cache_.decode_or_get` (line 457).
  The issue it builds has only `category`, `address`, and
  `unresolved_reason = "m68k_discovery_max_instructions"` — **no provenance, no image offset, no
  instruction length, no mapping claim, no bus record**. ADR 0010 §3's premise that this path retains
  full `InstructionProvenance` is therefore factually wrong, and no relaxation of the
  `unresolved_reason` clause alone would make it promotable: six further structural fields are also
  missing.
- **Block-entry cap** (`note_block_entry`, lines 336-347): carries the *referring* instruction's
  provenance plus a `target`, so it sets `direct.has_target` and is rejected by line 349 in addition to
  its non-empty `unresolved_reason`.
- **Call-admission cap** (lines 752-758, 789-795, 953-958): carries provenance but no mapping claim and
  no bus record, exactly as Context §A observed on the wire.

### C. Where a frontier stop is actually bound at runtime (direct inspection)

`genesis_frontier_stop_<8-hex-address>` functions are emitted per retained exit by
`build_genesis_frontier_stop_function` (`src/codegen/c11/frontend.cpp:862-935`). They are bound to a PC
in exactly one place: the **tail of a retained block's own body**
(`src/codegen/c11/frontend.cpp:2358-2360`), over `reached_frontier_addresses`, which is populated
per-block from that block's terminal instruction's outgoing static edges whose target is a represented
frontier address (lines 1872, 1913).

`genesis_dispatch` itself (lines 2365-2371) maps **only** `partial.accepted_prefix.static_blocks`
entries to `genesis_block_<addr>`, and falls through to
`genesis_internal_dispatch_inconsistency_stop`. It has **no frontier arm**. A dispatcher re-entry at a
frontier PC therefore yields `GENESIS_STOP_INTERNAL_DISPATCH_INCONSISTENCY`, **not** the frontier's own
stop class. ADR 0010 §4's re-entry-refusal argument is still *safe* (execution never proceeds past the
boundary), but its *identification* claim is wrong, and identification is precisely what a
runtime-confirmed expansion loop depends on.

Note what this does **not** say. Today's ADR 0009 indirect dispatch does *not* bypass the block tail:
the emitted indirect `jump_general` / `call_general` lowerings assign the PC and `break`
(`src/codegen/c11/m68k.cpp:1799-1801, 1822-1826`), as does the `runtime_routing` `RTS` lowering
(lines 598-599), so control still reaches the tail, and `reached_frontier_addresses` is populated at
`src/codegen/c11/frontend.cpp:1913` inside the loop that handles `indirect_call`, `indirect_branch`,
and `return_to_continuation` alike. An ADR 0009 indirect candidate that is a represented frontier is
therefore already bound correctly today.

**At this head the per-block tail is in fact sufficient, and Decision §6's arm is not yet needed.** Both
sets are built in the same function from the same `edges_by_source` map keyed on the same address: the
tail's `reached_frontier_addresses` at `src/codegen/c11/frontend.cpp:1913` iterates
`edges_by_source[terminal.source.address.value]` over every accepted edge kind including
`return_to_continuation`, and today's `runtime_return_targets` is built at lines 2345-2347 from
`edges_by_source[provenance.source.address.value]` filtered to `return_to_continuation`. For a terminal
`RTS` those two addresses are equal, so every legal return target that is a represented frontier is
already in the tail set. Reaching `genesis_dispatch` with a frontier PC is currently unreachable, which
is precisely why `internal_dispatch_inconsistency` is the correct answer there today.

**The gap opens with ADR 0011 §1, which SEG-007-T134 lands in the same task** (Decision §1). ADR 0011
§1 replaces the per-`RTS` list with "the whole-program set of every `M68kStaticCall.continuation`
address discovery has proven", tested by "every `RTS` site's generated membership check ... against
this one shared set" (ADR 0011 line 143), and its §6 table explicitly retargets the lowering "to read
`runtime_return_targets` from the new shared whole-program call-continuation set ... instead of a
per-walk frame-stack-derived list" (ADR 0011 line 339). Once that shared set is wider than any single
`RTS`'s own outgoing edges, an observed return value may be a legal member without being an edge of
*this* `RTS`'s block — and `reached_frontier_addresses`, which is derived from that block's own edges,
structurally cannot contain it. That, and only that, is what Decision §6's dispatcher arm exists for.
It is a prerequisite of ADR 0011 §1's correctness, not an independent improvement, and it is why
Decision §6 must land in the same task as ADR 0011 §§1-3 rather than later.

### D. What the partial-program seam already guarantees (direct inspection)

`build_analysis(completed_blocks_only = true, exclude_frontier_instruction = true)`
(`src/machine/genesis/frontend.cpp:1067-1160`) — the exact call `partial_or_rejection` makes at
line 1484 — already implements a strict retained-prefix construction that this ADR reuses rather than
replaces:

- it walks each registered block entry in `block_entry_order`, requiring every instruction to be
  present in `decoded_by_address`, and **discards** any block that cannot be completed (lines 1076-1129);
- it **excludes** every candidate frontier instruction from every block (lines 1084-1089);
- a straight-line block whose very next address **is** a candidate frontier address is closed cleanly
  with a synthesized `fallthrough` edge to that frontier (the SEG-007-T040 C3 `reaches_frontier` arm,
  lines 1115-1121) — this is exactly the "ceiling reached mid-block" case;
- it then **iteratively erases** any retained block having an outgoing edge whose target is neither a
  retained block entry nor a candidate frontier address, repeating until fixpoint (lines 1140-1159);
- it rebuilds `decoded`/`ir` in `decode_order` restricted to retained instruction addresses
  (lines 1161-1171), so no partially-validated instruction can survive.

`partial_or_rejection` (lines 1467-1538) then requires that the final represented frontier set's
address set be **exactly equal** to the candidate set `build_analysis` treated as safe edge targets
(`if (retained_addresses != sibling_addresses) return reject();`, lines 1533-1536). Silent truncation
by `recompiler_sort_dedup_and_bound_frontiers(..., m68k_discovery_max_frontier_exits, ...)` (line 1518)
therefore cannot produce an emitted artifact with an unlowerable edge: it fails the whole promotion
closed instead. `classify_frontier` (lines 1440-1455) currently maps `discovery_budget_exhausted` to
`std::nullopt` via its `default` arm, which is the single line that routes every budget stop to whole-
program rejection.

### E. Runtime and driver surfaces (direct inspection)

`GenesisStopClass` (`runtime/genesis/runtime.h:401-408`) currently ends at
`GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED = 8` — a *runtime* ADR 0007 watchdog class, unrelated to
static discovery. `GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED = 34` already exists
(`runtime.h:496`, named in `runtime.c:1133`), and `c4_diagnostic` already resolves it
(`src/codegen/c11/frontend.cpp:839`). `genesis_valid_stop_pair` (`runtime.c:1143-1174`) enumerates the
legal (stop class, diagnostic) pairs exhaustively. `tools/genesis_startup_bridge.py`'s
`STOP_DIAGNOSTIC_PAIRS` (lines 21-40) mirrors that table on the driver side, and the driver is a
single-shot generate/compile/run tool with no expansion loop.

## Decision

### 1. Delivery order, and why it is not circular

SEG-007-T128 reported a circularity: ADR 0011 §4's expansion trigger requires generated-native
execution to stop at `discovery_prefix_boundary`, but no such stop can exist until the ceiling first
produces a runnable partial program. The resolution is that the two required constructions are both
**static-only** and neither consumes the other's runtime output:

1. **ADR 0011 §§1-3** (address-only iterative walker; call-admission gate retired). Static-only.
   Required because, at this head, the authorized route stops at the call-admission gate and never
   reaches the instruction ceiling at all (Context §A). Without it the boundary mechanism is correct
   but unreached on the pinned route.
2. **This ADR's Decisions §§2-6** (ceiling to source- and mapping-provenanced runnable partial program,
   and its emitted fail-closed dispatch). Static-only, and validated entirely on project-authored
   synthetic inputs. Required because, with (1) applied, the ceiling becomes the reached stop and is
   still a whole-program `FrontendRejected`.
3. Only **after** (1) and (2) both exist does a generated program that can be built and run past the
   ceiling exist. Its execution may then confirm a boundary, and only that confirmation may promote a
   seed (Decision §7).

The non-circular sequence is therefore: *static ceiling -> static-only runnable partial generated
program -> build -> generated-native execution -> execution-confirmed boundary -> promoted next seed.*
**Generated-native execution never authorizes the mechanism needed to emit the first partial program.**
Steps (1) and (2) belong to SEG-007-T134 and are provable without any commercial input; step (3) is
conditional and is claimed only if it actually happens.

### 2. The instruction ceiling becomes an *admission* ceiling with a bounded, side-effect-free boundary probe

`m68k_discovery_max_instructions` is redefined as a ceiling on **admitted** instructions — the working
set discovery decodes, validates, caches, lifts, and emits — not on instruction *reads*. Its value
(`256U`) is unchanged, is not retuned, and is not searched.

On a ceiling trip at address `pc`, discovery performs exactly one **boundary provenance probe**:

- resolve `environment_.instruction_source(pc)`, then decode through the pure
  `decode_m68k_instruction` entry point (`include/segarecomp/cpu/m68k/decode.hpp:59-60`), which takes an
  explicit `M68kDecodeProfile`. That profile **must be `M68kDecodeProfile::general_startup`**, exactly
  matching the canonical discovery path whose admission ceiling triggered the probe
  (`decode_cache_.decode_or_get(source, M68kDecodeProfile::general_startup)`,
  `src/cpu/m68k/static_discovery.cpp:457`). The probe must **not** use `genesis_startup`, `direct_flow`,
  `moveq`, or any new probe-specific profile
  (`include/segarecomp/cpu/m68k/instruction.hpp:47` enumerates all four). A probe failure is legitimate
  **only** when the same `general_startup` decode semantics would fail at that address; a profile
  mismatch must never manufacture a terminal probe failure, because Decision §7's terminal-outcome rules
  make such a failure an accepted whole-program rejection and a narrower profile would silently convert
  representable boundaries into premature stops;
- the probe **must not** mutate `decode_cache_`, `decode_order_`, `block_entries_`,
  `block_entry_order_`, `instructions_used_`, `visited_states_`, `pending_access_`, or
  `accepted_branch_targets_`, and **must not** run the per-`decoded.kind` operand read/write
  validation switch;
- the probe **must** restore the image-global provenance offset the raw decode does not produce.
  `decode_m68k_instruction` returns a span-relative `provenance.source.image_offset`; `decode_or_get`
  repairs it with `accepted.provenance.source.image_offset = source.provenance_image_offset;`
  (`src/cpu/m68k/static_discovery.cpp:53`). The probe replicates exactly that one assignment. Omitting
  it fails `runtime_frontier_eligible`'s image-offset agreement check (line 274) — fail-closed, not
  unsound, but it would silently prevent every boundary from ever promoting;
- on success, the boundary issue carries the probe's full `InstructionProvenance` and sets
  `set_pc_based_image_offset`, `reconstruct_single_mapping_claim`, and
  `reconstruct_instruction_read_access`, so `translate_m68k_discovery_issue` reconstructs exactly the
  one mapping claim and the one `raw_cartridge_rom` `instruction_read` record
  `runtime_frontier_eligible` requires (Context §B). The existing `provenance_issue` helper
  (`src/cpu/m68k/static_discovery.cpp:318-328`) already sets both `provenance` and `instruction_length`
  while leaving `target` unset, so no additional issue member needs a new mutation path;
- the boundary issue sets **no** `target` (so `direct.has_target` stays false) and **no**
  `unresolved_reason` (Decision §3);
- on **any** probe failure — unmapped or conflicting instruction source, truncated, illegal,
  unsupported form, or a decoded length exceeding the covering claim's remaining bytes — **no boundary
  is constructed**: discovery keeps today's whole-program `FrontendRejected` for the ceiling. Provenance
  is never fabricated, and an address that is not known-decodable is never described as
  "known target, not yet emitted".

The probe costs exactly one instruction source read and one decode per ceiling trip. It cannot enlarge
the emitted prefix: `instructions_used_` still counts only instructions that reached the admission
increment at `static_discovery.cpp:611`, so the retained prefix is byte-identical to the prefix the
ceiling truncates today. This is the **only** amendment this ADR makes to ADR 0010 §3's mechanism, and
it is what makes the rest of ADR 0010 §§3-4 implementable at all.

### 3. `unresolved_reason` is left empty, and is the boundary's discriminator

`runtime_frontier_eligible`'s non-empty-`unresolved_reason` disqualifier
(`src/machine/genesis/frontend.cpp:266`) is **not** relaxed, and no
`known_but_unemitted_target`-style exemption is granted to the boundary class. ADR 0010 §3's
alternative is amended away because Decision §2 supplies real provenance, so no exemption is needed,
and relaxing a shape invariant shared by every frontier class — in **both** mirrors — to accommodate
one class would weaken the C4 frontier-shape gate for every other class too.

Instead the boundary issue simply does not set `unresolved_reason`. The constant's identity
(`m68k_discovery_max_instructions`) is host-side bookkeeping that this ADR deliberately does **not**
route anywhere: the only existing channel for that field is `translate_m68k_discovery_issue` ->
`direct.unresolved_reason` -> the direct-flow report, which an empty value simply leaves blank. No new
reporting mechanism is introduced and none is in the successor's scope. The empty reason is then the
**mechanically checkable discriminator**: a `discovery_budget_exhausted` diagnostic classifies as
`discovery_prefix_boundary` **if and only if** its `direct.unresolved_reason` is empty and its
`direct.has_target` is false. Every other budget producer keeps its non-empty reason and remains a
whole-program rejection, so the classification cannot be reached by accident.

### 4. Exactly one producer of `discovery_prefix_boundary`

`GenesisFrontierClass::discovery_prefix_boundary` has exactly one static producer: the instruction
admission ceiling of Decision §2. In particular:

- `m68k_discovery_max_blocks` does **not** become a boundary; ADR 0010 §2's disposition — retirement as
  an independent gate — stands unchanged.
- `m68k_discovery_max_call_frame_depth` does **not** become a boundary; ADR 0011 §1's disposition —
  retirement as a walker admission gate — stands unchanged.
- `known_but_unemitted_target` keeps its existing narrow, secondary, best-effort role exactly as
  SEG-007-T064 left it and is not reused, widened, or re-triggered (ADR 0011 §4.3, preserved).

A single producer keeps the trigger condition mechanically checkable and keeps `classify_frontier`'s
new arm total and side-effect-free.

### 5. Retained-prefix invariant, mid-block ceiling, and already-decoded addresses

**Reuse, do not reinvent.** Context §D establishes that
`build_analysis(true, true)` plus `partial_or_rejection`'s
`retained_addresses == sibling_addresses` equality check already construct and validate the retained
prefix. This ADR adds no new pruning pass. It fixes the following as the **mechanically checkable
retained-prefix invariant** the implementing task must assert directly in project-authored tests, on
every promoted partial program:

- **P1 (admission disjointness).** Every retained instruction address is a member of `decode_order_`,
  and no retained instruction address is a represented boundary address. The two sets are disjoint.
- **P2 (no incomplete semantic state).** `prefix.decoded.size() == prefix.ir.size()`, each pair
  satisfies `independently_decoded_and_lifted`, and every retained instruction was admitted through the
  full per-kind operand read/write validation switch. A probed-but-unadmitted boundary instruction is
  never decoded into `prefix.decoded`, never lifted into `prefix.ir`, and never emitted.
- **P3 (no dangling edge).** Every outgoing edge of every retained block targets either a retained
  block entry or a represented frontier/boundary address.
- **P4 (clean termination).** Every retained block either ends in a transfer instruction or carries a
  `fallthrough` edge to a retained block entry or to a represented boundary address.
- **P5 (exit representation).** The set of represented exit source addresses equals the set of
  candidate frontier addresses `build_analysis` treated as safe edge targets.

**Ceiling reached mid-block.** No new mechanism. The block whose straight-line continuation is the
boundary address is closed by the existing `reaches_frontier` arm with a synthesized `fallthrough` edge
to the boundary, satisfying P3/P4. If the boundary is instead a branch, call, or indirect target, the
referring block's own `direct_branch`/`direct_call`/`indirect_*` edge already targets it, and the
existing iterative pruning retains that block for the same reason. If neither holds, the referring block
is pruned. All three outcomes are deterministic and already implemented.

**Already-canonically-decoded address.** `walk` consults `decode_cache_.entries()` *before* calling
`decode_instruction` (`static_discovery.cpp:638-643`), so an address that was already canonically
decoded can never reach the ceiling check, and a boundary address can never later be admitted within
the same round (the ceiling is still tripped on every subsequent attempt, including from the T062/T064
best-effort sibling exploration that discards `walk`'s boolean result). P1 is therefore a **structural**
property of the existing traversal, not a new runtime guard, and the implementing task asserts it rather
than enforcing it. The boundary must additionally be the walk's **primary** issue; a best-effort
secondary must never displace it.

### 6. Emitted representation: classification, dispatch, C11, runtime, and driver

- `include/segarecomp/machine/genesis/frontend.hpp:53` gains
  `GenesisFrontierClass::discovery_prefix_boundary`, appended last so no existing ordinal moves.
- `classify_frontier` (`src/machine/genesis/frontend.cpp:1440-1455`) gains one arm:
  `discovery_budget_exhausted` maps to `discovery_prefix_boundary` under Decision §3's discriminator,
  and to `std::nullopt` otherwise.
- `runtime_frontier_eligible`'s `exact_category` switch gains
  `discovery_prefix_boundary -> category == discovery_budget_exhausted` in **both mirrors**
  (`src/machine/genesis/frontend.cpp:281-304` and `src/codegen/c11/frontend.cpp:286-309`), exactly as
  `c4_stop_class` requires both of its own. **No other clause changes.** Both switches are exhaustive
  over an `enum class`, so a missed mirror fails loudly under `-Wswitch`; were one nevertheless missed,
  `build_genesis_frontier_stop_function` would return `std::nullopt` and emission would reject the
  program closed rather than emit an unsound artifact.
  The class needs no `access` (so it takes the `needs_access == false` path), must not carry
  `direct.has_target`, and must satisfy the one-mapping-claim and one-`instruction_read`-record shape
  unchanged.
- `c4_stop_class` — both mirrors, `src/machine/genesis/frontend.cpp:19-28` and
  `src/codegen/c11/frontend.cpp:809-820` — gain
  `discovery_prefix_boundary -> "GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY"`. Unlike
  `known_but_unemitted_target`, the boundary lowers its **real** diagnostic category through the
  already-resolvable `GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED` (`frontend.cpp:839`); no literal
  override is introduced.
- **`genesis_dispatch` gains a frontier arm** (correcting ADR 0010 §4, Context §C). Immediately before
  its `genesis_internal_dispatch_inconsistency_stop` fallback, `genesis_dispatch`
  (`src/codegen/c11/frontend.cpp:2365-2371`) emits one `runtime->pc` comparison per **represented
  frontier exit**, dispatching to that exit's already-emitted `genesis_frontier_stop_<addr>` function.
  The existing per-block tail arms (lines 2358-2360) are unchanged and still short-circuit first.
  This is uniform across every frontier class, not special-cased for the boundary: the soundness
  argument is identical for all of them, and a class-specific arm would be an unmotivated exception.
  It is required for correctness of **ADR 0011 §1**, not of Decision §7: once the `RTS` membership set
  becomes whole-program, a legal return target may not be an edge of the returning block, so the
  per-block tail structurally cannot bind it and the boundary would be misreported as an internal
  invariant violation (Context §C). At this head, before ADR 0011 §1 lands, the tail is already
  sufficient and this arm is defensive only — which is also why ADR 0010 §4's identification claim,
  though genuinely false about the mechanism, has not yet caused an observable defect.
  The arm can never shadow a real block: `runtime_frontier_eligible` already refuses any frontier whose
  address is a retained instruction (`src/machine/genesis/frontend.cpp:383-384`), so a represented
  frontier address is never an accepted block entry, and the arm fires only where
  `internal_dispatch_inconsistency` would have.
- `runtime/genesis/runtime.h` gains `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY = 9` (the next free value
  after `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED = 8`, which is ADR 0007's unrelated *runtime*
  watchdog class and must not be conflated with it). `runtime.c` gains the matching
  `genesis_stop_class_name` arm returning `"discovery_prefix_boundary"` and the matching
  `genesis_valid_stop_pair` arm admitting **only** `GENESIS_DIAG_DISCOVERY_BUDGET_EXHAUSTED`. No ABI
  struct field is added and `genesis_runtime_drive` is unchanged.
- `tools/genesis_startup_bridge.py`'s `STOP_DIAGNOSTIC_PAIRS` gains
  `"discovery_prefix_boundary": {"discovery_budget_exhausted"}`.
- **IR: no change.** The boundary is a frontier exit, not an operation. No `M68kIrOperation`,
  `M68kIrKind`, or `DirectFlowIrKind` value is added or changed (ADR 0010 §4, preserved).

### 7. Static-only boundary construction versus runtime-confirmed seed promotion

These are two distinct phases with a one-way dependency, and the separation is the substance of this
ADR's answer to SEG-007-T128.

**Phase A — static-only, unconditional, no execution required.** Decisions §§2-6. Single seed: the
fixed reset entry. Produces a runnable, source- and mapping-provenanced partial generated program whose
boundary is an emitted fail-closed stop. Validated entirely on project-authored synthetic inputs:
a positive case (ceiling trips, boundary emitted, prefix satisfies P1-P5), a deterministic-repeat case
(byte-identical generated C across two runs), a strict-C11 compilation case, and adversarial-negative
cases (probe decode failure falls back to whole-program rejection; a non-empty-`unresolved_reason`
budget stop is not classified as a boundary; a hand-forged boundary carrying `direct.has_target` or a
mismatched bus record is refused by `runtime_frontier_eligible`). **Phase A requires no commercial input
and no generated-native execution.**

**Phase B — runtime-confirmed expansion, conditional.** Permitted only after Phase A exists and only
when a real generated-native run actually stops at a boundary.

- Round 1's seed set is exactly `{reset entry}`. A `static_only` address (ADR 0011 §5) may **never**
  become a seed, in any round, by any path.
- After building round *n*, the generated program is run deterministically from the fixed reset state.
  If its `GenesisControlTransfer` stops with `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY` at address *a*,
  then *a* is `runtime_confirmed` and round *n+1*'s seed set is `S_n ∪ {a}`.
- Any **other** stop class ends the loop and is the reported frontier. An
  `unsupported_cpu_form` / `unsupported_device_access` / `unsupported_memory_region` /
  `unresolved_indirect_target` / `unsupported_interrupt_or_scheduling_event` /
  `internal_dispatch_inconsistency` / `instruction_budget_exhausted` stop is a real capability gap or a
  real invariant violation, never an address to auto-expand past (ADR 0011 §4.4, preserved).
- **Duplicate/repeated boundary.** If *a* is already a member of `S_n`, the loop halts immediately with
  a deterministic `expansion_no_progress` driver result. It is never retried, never counted as
  progress, and never silently looped on. Because seeds are added one at a time and never removed,
  `S_n` is monotonically non-decreasing and this rule makes non-termination impossible.

**Finite deterministic limits.** Two new fixed constants, neither derived from any ROM, neither
searched, and neither a retuning of `m68k_discovery_max_instructions`:

- `m68k_discovery_max_seed_entries = 5U` — the maximum size of any seed set. **Amended from `4U` to
  `5U` by SEG-007-T167** (see *Amendment (SEG-007-T167)* below); originally chosen to match
  `m68k_discovery_max_frontier_exits` order of magnitude, which it still does. This does **not** make
  exit-cap overflow
  unreachable, and this ADR does not claim it does: one round's exits include a primary per seed plus
  every retained T062/T064 best-effort secondary and every non-boundary class, so four seeds can
  legitimately produce more than four exits. The operative safety property is the weaker one Context §D
  already establishes and which needs no new constant: `partial_or_rejection`'s
  `retained_addresses != sibling_addresses` check (`src/machine/genesis/frontend.cpp:1536`) fails the
  **whole promotion closed** rather than silently dropping any exit. Exit-cap pressure is therefore a
  deterministic build-time refusal, never an unsound emitted artifact.
- `m68k_expansion_max_rounds = 5U` — the maximum number of build/execute/expand rounds per driver
  invocation, an independent explicit outer bound that forces termination regardless of seed-set state.
  **Amended from `4U` to `5U` by SEG-007-T167**, synchronized with `m68k_discovery_max_seed_entries`.
- **Seed-set saturation.** If a run reports a new, distinct, `runtime_confirmed` boundary while
  `|S_n| == m68k_discovery_max_seed_entries`, the loop halts with a deterministic
  `expansion_seed_limit_reached` driver result. Like `expansion_no_progress` it is an honest stop —
  neither a failure nor progress. Raising either constant is a separate decision requiring its own
  executed evidence.

Per ADR 0011 §4.1, each seed is walked to fixpoint or ceiling **independently**, so the **per-walk**
resource ceiling remains exactly `m68k_discovery_max_instructions` (`256U`), unchanged and unsearched.
The aggregate per-round admitted-instruction bound is the derived product
`m68k_discovery_max_seed_entries * m68k_discovery_max_instructions` = `1280` (SEG-007-T167 amendment;
was `1024` at `4U`). State this plainly rather
than by implication: a saturated Phase B round may emit a prefix up to **five times** today's, so
"unchanged" is a claim about the per-walk constant only, not about a round's total emitted size. The
aggregate is nevertheless a product of two fixed constants — deterministic, image-independent, never
searched or tuned against any ROM — and it is reached only after that many distinct boundaries were
each separately confirmed by an actual generated-native run.

**Loop ownership.** Phase B's build/execute/expand loop is owned by build-time driver tooling (an
extension of the existing `tools/genesis_startup_bridge.py`-class driver), never by discovery or
emission. Every decode in every round happens at build time.

### 7a. Amendment (SEG-007-T167): seed/round cap `4U -> 5U`

Decision §7 requires that "raising either constant is a separate decision requiring its own executed
evidence." SEG-007-T167 supplies that evidence and makes that decision, synchronized across both
constants and both driver mirrors.

**Executed evidence (re-confirmed independently by SEG-007-T167 in its own worktree).** With
`m68k_discovery_max_seed_entries` / `m68k_expansion_max_rounds` and the
`tools/genesis_startup_bridge.py` driver mirrors raised together to `5U` and `segarecomp` rebuilt, the
authorized assisted pinned Phase-B route (canonical private-hints path resolver + explicit
`--external-hints` opt-in) runs round 5 **cleanly with no `aggregation_conflict` of any kind** (none of
`merge_root_result`'s decoded-fact / indirect-target-candidate-set / completion-RTS arms fire) and
reaches a new, later, deterministic terminal: `stop_class = c4_lowering_gap`, dimension family
`logical_or_missing_dispatcher`. The generated C is byte-identical across two independent runs. Raising
both constants to `6U` instead reproduces the identical round-5 terminal (the `c4_lowering_gap` stop
ends the Phase-B loop before a sixth round is attempted), so `5U` is already the smallest cap that
exposes this next real frontier and a larger cap adds nothing.

The earlier `backlog/seg-007-refine` observation of a `build_time_translation_rejected` /
`startup_graph_mismatch` at round 5 was a **caller/compiled-constant mismatch** (the Python driver
mirror raised without the compiled `frontend.hpp` constant the static discovery implementation
enforces), not a genuine cross-seed retained-fact conflict. Raising the two together removes it.

**Scope of this amendment.** Only the two named constants change value (`4U -> 5U`), synchronized, in
`include/segarecomp/machine/genesis/frontend.hpp` and `tools/genesis_startup_bridge.py`. The per-walk
resource ceiling `m68k_discovery_max_instructions` (`256U`) is unchanged and unsearched; the derived
aggregate per-round admitted-instruction bound becomes `5 * 256 = 1280`. No mechanism, discriminator,
promotion rule, invariant, or `GenesisFrontierClass` value changes. `expansion_no_progress` /
`expansion_seed_limit_reached` remain terminal honest stops at the new cap. Every seed after round 1
is still discovered only by the target program's own faithfully executed behavior.

### 7b. Amendment (SEG-007-T170): seed/round cap `5U -> 8U`

Decision §7 requires that "raising either constant is a separate decision requiring its own executed
evidence." Decision §7a's `5U` cap resolved one round-5 `c4_lowering_gap` terminal
(`logical_or_missing_dispatcher`), but after SEG-007-T167's logical-family batch and SEG-007-T169's
CRAM/VSRAM DMA-target-selection batch each closed their own real capability gap, Phase B saturated the
identical `5U` cap again at round 5 with the identical `discovery_prefix_boundary` /
`discovery_budget_exhausted` / `expansion_seed_limit_reached` resource-ceiling stop class — the second
consecutive occurrence of that same ceiling class. Per the project charter's anti-churn rule, a third one-step
numeric cap bump is forbidden unless evidence shows batching a durable headroom policy is unsound.
SEG-007-T169's own bounded, fully ephemeral, fully reverted diagnostic exploration supplies the
opposite evidence: a fixed, modest, evidenced headroom resolves the recurrence.

**Executed evidence (from SEG-007-T169's own merged Evidence, reconfirmed by the `backlog/seg-007-refine`
full refinement that decided this amendment, and reconfirmed once more independently by SEG-007-T170 in
its own worktree — see this task's own Evidence section for the round-7 reconfirmation run).**
Temporarily raising both constants together to `6U` reproduces the identical
`discovery_prefix_boundary` / `discovery_budget_exhausted` / `expansion_seed_limit_reached`
resource-ceiling terminal, deterministically, at `rounds = 6` / `seed_count = 6` — one more round of the
same ceiling, not a real frontier. Raising instead to `8U` reaches a materially different, deterministic,
byte-identical-across-two-runs, inexpensive real terminal at round 7: `stop_class = c4_lowering_gap`,
`c4_lowering_dimensions = {"family": "subtract_immediate_missing_dispatcher"}`, approximately 10 MB of
generated C, roughly 11–16 seconds of wall time. Raising further to `12U` reaches the identical round-7
terminal, confirming `8U` already supplies sufficient headroom with no additional benefit from a larger
cap. No VDP/CRAM/VRAM/VSRAM device register state changed at any tested cap value (`6U`, `8U`, `12U`)
relative to the `5U` baseline; only the discovery/expansion loop's own round/seed progression changed.

**Decision.** Raise `m68k_discovery_max_seed_entries` and `m68k_expansion_max_rounds` together,
synchronized, from `5U` to `8U`. This is a **headroom choice** among the evaluated candidates, not a
claim that `8U` is the provably minimal sufficient cap: only `6U`, `8U`, and `12U` were evaluated, not
every intermediate value (for example `7U` was never tried), so a smaller cap than `8U` might also
expose the same round-7 terminal. `8U` is adopted because it is the smallest of the three *evaluated*
candidates that exposes a real, non-resource-ceiling terminal (`6U` still reproduces the identical
resource-ceiling class, so it is rejected outright) and `12U` demonstrates no further benefit accrues
from evaluating larger values, so `8U` is a reasonable, bounded, evidence-anchored headroom pick rather
than an unsearched or arbitrary one. Unlike Decision §7a's single-step `4U -> 5U` bump, this is a
multi-step jump, so "smallest cap that resolves the stop" is not established here to the same precision
Decision §7a's consecutive-value evidence gave it.

**Scope of this amendment.** Only the two named constants change value (`5U -> 8U`), synchronized, in
`include/segarecomp/machine/genesis/frontend.hpp` and `tools/genesis_startup_bridge.py`. The per-walk
resource ceiling `m68k_discovery_max_instructions` (`256U`) is unchanged, unretuned, and unsearched; the
derived aggregate per-round admitted-instruction bound becomes `8 * 256 = 2048`. No mechanism,
discriminator, promotion rule, invariant, or `GenesisFrontierClass` value changes. No speculative seed
list, ROM-derived cap, or arbitrary full-ROM code admission is introduced. `expansion_no_progress` /
`expansion_seed_limit_reached` remain terminal, honest, acceptable stops at the new cap. Every seed
after round 1 is still discovered only by the target program's own faithfully executed behavior. Raising
either constant again beyond `8U` remains a separate decision requiring its own executed evidence,
exactly as Decision §7 and §7a already require.

### 7c. Amendment (SEG-007-T172): persistent runtime-confirmed roots, per-invocation batch budget

**Executed evidence.** SEG-007-T172's own bounded, fully ephemeral, fully reverted diagnostic
exploration (preserved verbatim in SEG-007's own Notes, "Correction" entry) raised the two Decision §7
constants across `9U`, `10U`, `11U`, and `12U`: `9U` reproduces the identical
`discovery_prefix_boundary` / `discovery_budget_exhausted` / `expansion_seed_limit_reached`
resource-ceiling terminal one round later (the third consecutive occurrence of that same ceiling
class, after Decision §7a's `5U` and Decision §7b's `8U`); `10U`, `11U`, and `12U` all reach an
identical, deterministic, cap-independent terminal instead (`driver_result =
build_time_translation_rejected`, a C4-emission-time "invalid C4 indirect target set" rejection,
reached only once the ADR-0023 external-hints route's own multi-entry `logical_table_descriptor`
annotation is opted in). This is the third consecutive lifetime-cap bump exposing nothing but the
artificial ceiling itself, which is itself the evidence: a small fixed **total lifetime** seed/round
cardinality is the wrong durable abstraction for commercial-game static discovery.

**Decision.** Decision §7's runtime-confirmed root set (`S_n` there, `R` here) is redefined as
**persistent, monotonically growing program knowledge across driver invocations**, never reset merely
because one invocation reaches its own operational work budget. `m68k_discovery_max_seed_entries` and
`m68k_expansion_max_rounds` are redefined from a lifetime ceiling on `|R|` (or on the number of
build/execute/expand rounds ever performed) to a **per-invocation batch budget**:

- `m68k_discovery_max_seed_entries` now bounds the number of **newly promoted** runtime-confirmed roots
  a single Phase-B invocation may add to `R`, not the total size `R` may ever reach.
- `m68k_expansion_max_rounds` now bounds the number of build/execute/expand rounds a single invocation
  may perform, not the lifetime round count.
- The evidenced numeric value (`8U` for both, from Decision §7b) is **kept unchanged** as the initial
  per-invocation batch budget. This amendment removes the lifetime-cardinality abstraction; it does not
  relocate the cap to a new number, and no new lifetime cardinality is introduced (the project charter's
  anti-churn rule: a third one-step numeric lifetime bump is exactly the pattern this amendment retires).
- **New terminal outcome: `phase_b_batch_complete`.** When a single invocation's batch budget (either
  constant) is consumed while valid pending expansion work remains — i.e. the just-completed round still
  confirmed a new, distinct `discovery_prefix_boundary` worth continuing from — the invocation halts with
  `phase_b_batch_complete`. This is **resumable driver state**, never a target-program frontier and never
  grounds for another lifetime-cap amendment. It is mechanically distinct from `expansion_no_progress`
  (a duplicate boundary — a genuine dead end, never resumed into more progress) and from a genuine
  capability-gap stop (`c4_lowering_gap`, `unsupported_cpu_form`, etc. — a real frontier, not resumable).
  `tools/genesis_startup_bridge.py` implements this as the `PHASE_B_BATCH_COMPLETE` driver result,
  replacing the prior `expansion_seed_limit_reached` / `expansion_round_limit_reached` names for both
  budget dimensions (both are the same "batch exhausted, resume me" outcome now).
- A later invocation, given the persisted confirmed-root set `R`, resumes round 1 of its own batch with
  seed set `{reset entry} ∪ R` and continues normal Decision §7 expansion. Full rebuild-from-persisted-
  roots is explicitly acceptable; no incremental object-level compilation or persistent generated-C block
  cache is required.

**Checkpoint/resume mechanism (generic, driver-owned, not Sonic-specific).** Per Decision §7's own
"Loop ownership" clause, Phase B's build/execute/expand loop already belongs to build-time driver
tooling, never to discovery or emission; checkpoint/resume is therefore implemented entirely in
`tools/genesis_startup_bridge.py`, with no change to the discovery/emission frontend's own contract:

- An optional `--checkpoint <path>` argument persists one JSON file per generation-input identity: the
  ordered, deduplicated `runtime_confirmed` root list `R` accumulated so far (exactly the seed-set shape
  Phase B already builds each round — no new fact type), and a binding header of `schema_version`,
  `rom_sha256` (the same identity field the ADR-0023 hints mechanism already uses), and the exact
  `--external-hints` path plus its own content hash (`hints_path` / `hints_sha256`) so an annotated and
  an unannotated run can never cross-contaminate a resume.
- **Fail-closed binding.** On load, the current invocation's actual `rom_sha256` and hints-file content
  hash are recomputed and compared exactly against the persisted header. Any mismatch, missing field,
  wrong type, unparseable JSON, or missing file drops the persisted state and starts a fresh round 1 —
  never silently applying persisted roots to a different ROM/config.
- **No new trust path.** A persisted root is, by construction, exactly the same `runtime_confirmed`
  boundary address Decision §7's existing promotion rule already produces; the checkpoint file carries no
  additional authority and no alternate admission route.
- The checkpoint is always rewritten after a run, regardless of outcome (including a later round's
  build-time translation rejection), so the roots actually confirmed before a mid-batch failure are not
  lost.

**Scope of this amendment.** Only the *meaning* of the two named constants changes (lifetime ceiling ->
per-invocation batch budget); their numeric value (`8U`) is unchanged. No mechanism, discriminator,
promotion rule, `GenesisFrontierClass` value, or C4/runtime contract changes. The per-walk resource
ceiling `m68k_discovery_max_instructions` (`256U`) and every other existing call-depth/mapping/alias/
fact-validation/loop-proof/fail-closed limit are unchanged and unweakened. `expansion_no_progress`
remains a terminal, honest, non-resumable stop. `static_only` addresses (ADR 0011 §5) still may never
become a seed, in any round, by any path, regardless of persistence. Build-time/static instruction
decoding only — no interpreter, no runtime opcode fetch/decode, no JIT. No speculative full-ROM
admission and no Sonic-specific seed list are introduced by persistence; only already-runtime-confirmed
boundaries from the target program's own faithfully executed behavior are ever persisted. Runtime still
only ever discovers *importance*; static analysis still discovers *code* (Decision §7's own invariant,
restated here unweakened).

**Correction (SEG-007-T172, same task): a stale total-seed guard in `discover_m68k_general_startup`.**
The initial §7c landing left one pre-existing caller-contract guard in
`src/machine/genesis/frontend.cpp` unfixed:
`if (seeds.size() > m68k_discovery_max_seed_entries) { ...reject with startup_graph_mismatch... }`, where
`seeds` is `{reset entry} ∪ program.runtime_confirmed_seeds` — the **total accumulated** seed count, not
"newly promoted this invocation". Once checkpoint/resume persisted more than `8U` confirmed roots and a
resumed invocation supplied all of them as `--analysis-seed`, this guard tripped on the total count and
incorrectly rejected the build with `startup_graph_mismatch` — the exact same diagnostic category the
genuine `aggregation_conflict` cross-root fact-disagreement path also produces, making the two
indistinguishable from the rejection alone. **Fix:** the guard is removed outright (not retuned, not
replaced with a differently-named total-cardinality bound). `program.runtime_confirmed_seeds` is a flat
list that carries no distinction between "persisted from an earlier invocation" and "newly promoted this
invocation", so `discover_m68k_general_startup` has no way to re-derive that per-invocation distinction
from the list alone and must not attempt to re-enforce it as a total-count bound; every seed still
receives its own fully independent, per-root-bounded walk (`m68k_discovery_max_instructions`) regardless
of the total seed count, so no safety invariant depended on that guard. Every other existing
resource/safety ceiling (`m68k_discovery_max_instructions`, `m68k_discovery_max_blocks`, call-depth,
dedup, `aggregation_conflict`) is unaffected. Project-authored regression coverage
(`general_startup_admits_more_than_the_per_invocation_seed_budget_in_total`,
`tests/m68k_pipeline_test.cpp`) proves a ten-seed accumulated set (`|S| == 10 > 8U`) is discovered and
aggregated successfully, confirmed to fail with the stale pre-fix guard and pass post-fix.

**Automatic continuation past `phase_b_batch_complete` (SEG-007-T172, same task).** An operational
Phase-B batch boundary is resumable driver state, not a stopping point requiring a human/task/refinement
intervention. `tools/genesis_startup_bridge.py`'s `main`, when `--checkpoint <path>` is supplied, now owns
an outer resume loop: it re-invokes `run_expansion_loop` again with the freshly-saved checkpoint's seeds
as the next `initial_seeds` whenever a batch ends in `PHASE_B_BATCH_COMPLETE`, repeating until either a
genuine terminal (anything other than `PHASE_B_BATCH_COMPLETE`) is reached or a defensive
`MAX_OUTER_RESUME_BATCHES` safety net is hit. That safety net is a genuine-infinite-loop guard only — not
a new lifetime cardinality on `R` or on the number of Phase-B rounds a discovery session may ever
perform — and is set generously high so it is never the actual limiting factor for any currently
realistic route. Without `--checkpoint`, this loop runs exactly once, byte-identical to the prior
single-batch behavior every existing caller/fixture relies on. Project-authored coverage
(`tests/genesis_startup_bridge_checkpoint_test.py`,
`test_automatic_continuation_across_multiple_batches`) proves a scenario requiring more total rounds than
one batch's own budget resolves to the genuine final terminal in ONE CLI invocation.

**Checkpoint trust-boundary reconciliation with `--analysis-seed` (SEG-007-T172, same task).** The
checkpoint file is driver-owned generated build/session state — the SAME trust class the existing
in-memory `--analysis-seed` transport surface already has — never an externally-audited input format like
the ROM image or the `--external-hints` file (which do receive real fail-closed schema/hash verification
against externally-supplied claims in `parse_genesis_external_hints`). Persisting the confirmed-root set
across invocations creates **no new authority path**:

- A checkpoint's `seeds` entries are subject to exactly the same (lack of) frontend-side verification an
  ordinary `--analysis-seed` value always has had, and are only ever legitimately produced by
  `save_checkpoint` immediately after a genuine `run_expansion_loop` extraction from that round's own
  actual generated-execution output — never hand-authored or independently derived.
- A hand-edited checkpoint carrying a fabricated seed address receives no special weaker or stronger
  admission: it is walked and merged through the exact same `discover_m68k_static_graph`/
  `merge_root_result` path as any other seed, and any fact disagreement with an already-confirmed root
  still fails the whole build closed via the existing `aggregation_conflict` path. This is not a new
  protection introduced by checkpoint/resume; it is the same protection `--analysis-seed` already had,
  now reachable via a persisted file instead of only an in-process value.
- `src/main.cpp`'s existing doc comment already states the CLI/frontend perform no additional
  cryptographic or provenance verification of a supplied `--analysis-seed` value; this is unchanged by
  §7c's persistence mechanism.
- The checkpoint file itself is build/session-generated state, not a repository artifact; every path this
  task's own tests/tooling write a checkpoint to lives under an already fully-ignored directory (e.g.
  `build/...`), never committed.

Project-authored adversarial coverage
(`test_checkpoint_seed_is_no_more_trusted_than_an_ordinary_analysis_seed`,
`tests/genesis_startup_bridge_checkpoint_test.py`) hand-constructs a checkpoint carrying a seed address
never produced by a genuine `run_expansion_loop` extraction and proves it is treated with exactly the
same authority/outcome as supplying that identical address as an ordinary in-process `--analysis-seed` in
a single, non-checkpoint invocation.

### 8. Invariant preservation

- **No interpreter, JIT, or runtime opcode decode.** The boundary probe (Decision §2) is a build-time
  static decode. At runtime, both the existing block-tail arms and Decision §6's new `genesis_dispatch`
  arm select **only among already-emitted function identities**; the boundary's body is an
  unconditional fail-closed stop that touches no target byte. Decision §6's new arm strictly *narrows*
  a stop that was previously `internal_dispatch_inconsistency` into a more precise class; it never
  converts a stop into continuation, and it adds no fetch, no decode, and no target-address
  computation. Phase B performs every decode at build time only.
- **Deterministic output.** The probe is a pure function of the image and the ceiling address; the
  admitted prefix is byte-identical to today's ceiling-truncated prefix; exit ordering continues to use
  the existing `frontier_sort_key` tuple; the new `genesis_dispatch` arms are emitted in that same
  deterministic exit order; seed sets are ordered and deduplicated. Two runs on identical input and
  options produce byte-identical generated C.
- **Fail-closed on genuinely unsupported behavior.** Strengthened, not weakened: a probe failure falls
  back to whole-program rejection; a non-empty-`unresolved_reason` budget stop remains a rejection; a
  represented-exit-set mismatch still fails the whole promotion closed; and a previously
  `internal_dispatch_inconsistency` re-entry now reports its precise class.
- **Source-address provenance through every stage.** The boundary carries the probed instruction's full
  `InstructionProvenance`, its unique mapping claim, and its single instruction-read bus record — this
  is the specific gap Decision §2 closes, and it is what makes the emitted partial program "source- and
  mapping-provenanced" rather than merely truncated.
- **No Sonic-specific heuristics.** No address, offset, seed list, code map, or per-title branch is
  introduced. The ceiling, the probe, the discriminator, the promotion rule, and both new constants are
  generic and input-independent. Every seed after round 1 is discovered by the target program's own
  faithfully executed behavior, never supplied.
- **Commercial-input policy.** Boundary addresses and probed bytes are commercial-derived when the input
  is a commercial image. They remain ephemeral session diagnosis under ADR 0004/ADR 0005; durable
  evidence records only normalized classes, stop classes, diagnostic categories, and advancement
  results.

### 9. Relationship to existing ADRs

- **ADR 0002.** Unchanged. This ADR completes ADR 0010 §3's routing of discovery *incompleteness* to
  ADR 0002's fail-closed-trap arm by supplying the provenance that arm structurally requires. A partial
  program with a boundary exit remains a non-success exit making no completeness claim past the
  boundary.
- **ADR 0006.** No interaction. The boundary is control-side; cartridge *data* reads are unaffected.
- **ADR 0007.** Complementary and non-overlapping. `GENESIS_STOP_INSTRUCTION_BUDGET_EXHAUSTED` is ADR
  0007's runtime loop watchdog; `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY` is a static-discovery-stage
  boundary. Decision §6 keeps them distinct enumerators with distinct diagnostic pairings precisely so
  they can never be conflated.
- **ADR 0009.** Fully reused, not altered. Its indirect-target mechanism is one of the two dispatch
  paths that motivates Decision §6's `genesis_dispatch` arm.
- **ADR 0010.** §1 and §2 preserved; §3's fail-closed-trap choice preserved, its provenance premise and
  its exemption proposal superseded (Decisions §§2-3); §4's IR rule and no-path-past-the-boundary
  argument preserved, its dispatch-binding claim corrected (Decision §6).
- **ADR 0011.** Decisions §§1-3 preserved in full and made a hard prerequisite (Decision §1). §4's
  representation reuse and its "other stop class is a real capability gap" rule are preserved; §4's
  trigger is made explicitly conditional and given the finite limits and duplicate-boundary rule it
  left open (Decision §7). §5's `runtime_confirmed` / `static_only` tagging is preserved verbatim and
  is what forbids a static-only seed.
- **ADR 0012.** No interaction. Checkpoint-evidence runtime and independent-oracle ownership are
  orthogonal to discovery-prefix boundary construction.

## Consequences and Next-Task Shape

SEG-007-T134 is the implementing successor. Its own record already defers its concrete seam to
"the accepted T133 ADR"; this section **is** that seam, and SEG-007-T134 consumes it verbatim.

**Binding scope for SEG-007-T134.**

1. Implement **ADR 0011 Decisions §§1-3** (address-only iterative worklist; runtime-stack-owned return
   membership over the whole-program call-continuation set; the corrected finite-state termination
   model), against ADR 0011 §6's ownership table. Decision §1 above makes this an in-scope hard
   prerequisite, not a separate task: it is unimplemented at this head (Context §A) and without it the
   instruction ceiling is never reached on the authorized route.
2. Implement **ADR 0010 §2's already-accepted retirement of `m68k_discovery_max_blocks` as an
   independent discovery admission gate**. Context §A records that this retirement is accepted but, like
   ADR 0011 §§1-3, still unimplemented at this head: the hard rejection at
   `src/cpu/m68k/static_discovery.cpp:338-343` is live. Remove that rejection's **semantic
   admission role** — a block-entry count must no longer be able to refuse a block entry or raise a
   `discovery_budget_exhausted` failure — leaving `m68k_discovery_max_instructions` as the single
   resource ceiling, exactly as ADR 0010 §2 already decided and as Decision §4 above already assumes.
   This is a removal of gating behavior, **not** a change of value: the constant is not widened,
   retuned, searched, or replaced, and it does not become a `discovery_prefix_boundary` producer
   (Decision §4, which keeps that class to a single producer). Whether the constant is deleted outright
   or retained for some non-gating purpose is the implementing task's choice, provided no
   admission-refusal behavior for it survives anywhere.
3. Implement this ADR's **Decisions §§2-6**: the admission ceiling and its bounded side-effect-free
   boundary provenance probe; the empty-`unresolved_reason` discriminator; the single boundary producer;
   the P1-P5 retained-prefix invariants; and the `GenesisFrontierClass` value, `classify_frontier` arm,
   `runtime_frontier_eligible` `exact_category` arm, both `c4_stop_class` mirrors, the
   `genesis_dispatch` frontier arm, the `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY = 9` runtime enumerator
   with its name and stop-pair arms, and the driver `STOP_DIAGNOSTIC_PAIRS` entry.
4. Add **Decision §7 Phase A's test matrix** on project-authored, legally redistributable synthetic
   inputs: a positive boundary case asserting P1-P5 directly; a deterministic byte-identical repeat; a
   strict-C11 compilation of representative generated output; and the adversarial-negative cases (probe
   decode failure falls back to whole-program rejection; a non-empty-`unresolved_reason` budget stop is
   not classified as a boundary; a forged boundary carrying `direct.has_target` or a mismatched
   mapping-claim/bus record is refused by `runtime_frontier_eligible`).
5. Re-execute the authorized pinned route **only after** Phase A exists, and implement **Decision §7
   Phase B** in that same task **only if** the run actually stops with
   `GENESIS_STOP_DISCOVERY_PREFIX_BOUNDARY`. Otherwise record the run's real stop class truthfully as
   the next execution-selected frontier and do not claim expansion. See *Binding terminal outcomes and
   acceptance* below for which end states are already complete and must not be "fixed".

**Binding non-goals for SEG-007-T134.** Amending this ADR or reopening ADR 0011 Decisions §§1-3;
raising, searching, retuning, or replacing the value of `m68k_discovery_max_instructions` or of
`m68k_discovery_max_blocks` (item 2 retires the latter's gate role, never its value); promoting a
`static_only` address to a seed; an interpreter, JIT, or runtime opcode fetch/decode; any Sonic-specific address, seed, code map,
or heuristic; and every seam ADR 0011 §6 already places out of bounds (the non-`runtime_routing` C11
lowering profile, the ADR 0009 indirect mechanism itself, the ADR 0007 watchdog, the ADR 0006
cartridge-data path, device/VDP/Z80/PSG/controller-I/O routing, rendering, and timing).

**Binding terminal outcomes and acceptance for SEG-007-T134.** Each end state below is a *complete,
bounded, acceptable* result. None is a task failure, and none may be "resolved" by weakening a
fail-closed rule, fabricating evidence, or raising a bounded limit.

1. **A Phase A probe-failure rejection is terminal and acceptable.** If the authorized route reaches the
   instruction admission ceiling but Decision §2's boundary provenance probe legitimately fails —
   unmapped or conflicting instruction source, truncated, illegal, unsupported form, or otherwise
   non-representable — then the whole-program static `FrontendRejected` Decision §2 mandates **is** the
   architecturally correct outcome. T134 records that normalized result and makes the appropriate
   successor or `needs_full_refinement` handoff. **T134 is not required to produce generated-native
   execution when the accepted fail-closed architecture itself makes emission impossible.** The probe
   must not be weakened, broadened, or given fabricated or partial provenance in order to satisfy a
   generated-native acceptance criterion; a probe relaxed for that purpose is a contract violation, not
   a pass. Symmetrically, the probe must decode with `M68kDecodeProfile::general_startup` exactly as
   Decision §2 requires: a terminal probe failure is valid only when that same profile's semantics
   genuinely fail at the address, never when a mismatched decode profile produced it.
2. **Phase B build-time seed plumbing is explicitly authorized.** If a real generated-native run confirms
   a `discovery_prefix_boundary`, T134 **may** add the minimum generic build-time plumbing needed to
   transport the ordered, bounded seed set between `tools/genesis_startup_bridge.py`, the existing
   `emit-general-startup-bridge-c` command and its frontend composition, and static discovery —
   explicitly including `src/main.cpp` and `FrontendProgram`-level seed transport where required. That
   plumbing may carry only statically proven or `runtime_confirmed` addresses (Decision §7 and ADR 0011
   §5); it performs no runtime opcode fetch or decode; and it introduces no Sonic-specific address,
   seed, code map, or table. Deterministic aggregation and deduplication of independently discovered
   seed results is part of Phase B implementation scope: two discovered facts that conflict for the same
   address must **fail closed**, never be silently resolved by picking one.
3. **Phase B `expansion_no_progress` and `expansion_seed_limit_reached` are terminal and acceptable.**
   Either is a bounded, honest end state under Decision §7. Neither justifies raising, retuning,
   searching, or replacing any limit merely to satisfy Acceptance. T134 records the normalized result
   and creates the appropriate bounded successor or `needs_full_refinement` handoff under the normal
   workflow rules.

None of these outcomes authorizes a capability-status change beyond the truthful executed result, and
none of them creates a new task or a new architecture decision by itself.

`docs/development/sonic-title-critical-capabilities.md` is reconciled by that task's pull request, not
by this ADR.

**Record-level reconciliation is deferred to a SEG-007 full-refinement pull request.** SEG-007-T134 is
an existing sibling record, and the task pull-request gate correctly forbids a task pull request from
modifying one. That refinement must apply exactly these **three** edits and nothing else:

1. Transition SEG-007-T134 `draft -> ready`. Its stated reason for being a draft — "pending
   SEG-007-T133's accepted ADR" — is now satisfied.
2. Fold this section's **complete** binding scope, acceptance, and non-goals into SEG-007-T134's record,
   **including binding scope item 2's ADR 0010 §2 retirement of `m68k_discovery_max_blocks` as an
   independent discovery admission gate**. That is an in-scope obligation of SEG-007-T134, not a
   separate task, and it removes the admission-refusal role only — the constant's value is not widened,
   retuned, searched, or replaced.
3. Replace **only** the stale sentence in `docs/development/sonic-title-critical-capabilities.md`'s
   static-discovery boundary row that reads "This is the sole current `RUNTIME_SELECTED` blocker; full
   refinement must decide that non-circular boundary ownership/delivery contract." That contract is now
   decided by this accepted ADR, so the sentence must instead point at ADR 0013 and its implementing
   task SEG-007-T134. **That row's `RUNTIME_SELECTED` status and its `observable_state` remains
   `not_ready` statement are preserved unchanged**, because this ADR implements no capability and
   changes no capability status.

No new task is created, no capability status changes, no architecture contract changes, and no scope
beyond these three edits is authorized.
