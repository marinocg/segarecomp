# ADR 0024: Ghidra-Assisted Validated Code Inventory and Tier-2 Emitted-Set Dispatch

- Status: Accepted
- Date: 2026-09-05
- Amends: none (additive to ADR 0009 and ADR 0013; neither is weakened)
- Related: ADR 0009 (Tier 1, the existing statically-proven finite-target-set
  admission/emission machinery this ADR's Tier 2 sits beside, never inside),
  ADR 0013 (persistent Phase B expansion, demoted here to fallback
  discovery), ADR 0023 (the external-hints interchange file this ADR reuses
  verbatim, adding one more `kind`), SEG-007-T162 (the Ghidra MCP path this
  ADR's candidate inventory is produced through), SEG-007-T173 (the runtime/
  RAM-provenanced indirect-target frontier this ADR's Tier 2 exists to reach
  past without inventing a points-to architecture)

## Context

SEG-007-T173 proved the current Sonic Phase-B frontier is a genuine runtime/
RAM-provenanced indirect control target (a byte `MOVE` from a `-(An)`
predecrement source feeding an indirect call/branch index) that none of
ADR-0009's three recognized finite-value producers can soundly cover, and that
proving it finite at generation time would require a new memory-provenance/
points-to architecture this milestone has not adopted (see that task's own
Evidence for the full classification). The `backlog/seg-007-refine` fourth-pass
refinement considered, and rejected, inventing that provenance architecture,
standalone runtime-confirmed-target expansion beyond ADR-0013's existing
shape, and a milestone redirection. It selected instead a different
architecture direction: make trusted whole-ROM Ghidra analysis a *primary*,
independently validated code-discovery source, and add a second, explicit
dispatch tier that lets a runtime-derived computed control transfer reach an
already-validated *emitted* target without requiring ADR-0009's own
finite-target-set proof for that specific value.

SEG-007-T162 already proved a working, containerized, security-preserving
Ghidra MCP path for read-only whole-ROM analysis of the authorized pinned
Sonic ROM. SEG-007-T163/ADR-0023 already established the interchange-file
shape and trust discipline this ADR extends: an external tool may *propose* a
narrowly bounded fact, but every structural consequence of that proposal is
still independently re-derived by segarecomp's own proofs before it can
affect emission or runtime behavior. This ADR applies that exact same
discipline to a *code-entry* proposal instead of ADR-0023's *table-descriptor*
proposal.

## Decision

### Authority boundary

```
ROM
  -> Ghidra whole-ROM candidate function/entry-point inventory   (a PROPOSAL, no authority)
  -> segarecomp independent decode/mapping/static-safety walk    (the EXISTING per-seed discovery machinery, unchanged)
  -> successful representation as an ordinary discovered static block
  -> membership in EmittedCodeAddressSet                         (generation-time, one build)
  -> Tier-1 (ADR-0009, unchanged) or Tier-2 (this ADR) dispatch eligibility
```

A Ghidra candidate is **never** dispatchable merely because Ghidra labeled it
code. It is folded into the *seed set* `discover_m68k_general_startup` already
walks (ADR-0011 §4.1's per-seed independent walk, ADR-0013 §7's seed-set
union), and from that point on it receives **exactly** the same
decode/mapping/target-admission/static-safety treatment as the reset entry or
any Phase-B runtime-confirmed root -- no new validator, no relaxed check, no
separate code path. Only a candidate that survives that walk and is admitted
into the accepted static prefix ever becomes a member of
`EmittedCodeAddressSet`, the one generation-time set this ADR's Tier 2
dispatch checks membership against.

`EmittedCodeAddressSet` is exactly: the sorted, deduplicated set of every
validated, independently walked, reachable emitted static block's own entry
address, in one build. It carries no separate identity or lifecycle from the
existing accepted static prefix -- it is a direct, one-line derivation of the
already-final `emitted_block_entries` set C4 emission computes for its own
`genesis_dispatch` arms (`src/codegen/c11/frontend.cpp`), not a new discovery
pass or a second representation of "what is code."

### Candidate-inventory interchange contract

Reuses ADR-0023's existing `--external-hints <path>` JSON file/schema
verbatim, adding one more record `kind`: `"code_entry_candidate"`.

```json
{
  "rom_sha256": "<64 lowercase hex characters>",
  "kind": "code_entry_candidate",
  "address": "0x00001234",
  "provenance": {
    "tool": "ghidra",
    "tool_version": "11.x",
    "timestamp": "2026-09-05T00:00:00Z",
    "human_reviewed": false
  }
}
```

The two record kinds (`logical_table_descriptor` from ADR-0023,
`code_entry_candidate` from this ADR) may freely coexist in one interchange
file; each is parsed by its own reader
(`parse_genesis_external_hints`/`parse_genesis_external_code_entry_candidates`,
`src/machine/genesis/frontend.cpp`), sharing the same JSON parser, the same
`rom_sha256`-match precondition, and the same required, non-decorative
`provenance` object (`tool`/`tool_version`/`timestamp` as strings,
`human_reviewed` as a JSON boolean) ADR-0023 already established -- a record
missing any of these, or failing the ROM-hash check, or malformed JSON, is
silently dropped, never a hard build failure, exactly like ADR-0023's own
fail-closed contract.

Unlike ADR-0023's table-descriptor records, a code-entry candidate carries no
semantic content beyond "this address is worth an independent walk" -- two
candidates for the same address are never a conflict; they simply collapse to
one seed. The parser therefore deterministically sorts and deduplicates by
address rather than applying ADR-0023's stricter same-identity-must-agree
rule. A candidate is never dispatchable merely for surviving parsing: it must
still pass the full independent walk described above before its address can
ever appear in `EmittedCodeAddressSet`.

### Tier 1 (unchanged) vs Tier 2 (new)

**Tier 1** is exactly ADR-0009's existing statically-proven finite-target-set
mechanism: a computed indirect JMP/JSR whose control EA is the recognized
`pc_index8` form and whose index-register value flow is fully proven finite
gets an `M68kIndirectTargetEaSet` fact, a small per-site candidate array, and
the existing `m68k_indirect_target_member` runtime guard. Nothing about this
mechanism's proof obligations, representation, or emission changes.

**Tier 2** fires only when the decoded control EA is the same recognized
`pc_index8` form, its index is the word-size Dn kind ADR-0009's own EA
evaluator already handles (never An-indexed or long-size -- a genuinely
different SHAPE neither tier's C11 lowering can compute at all, since it
always sign-extends a word-size Dn index; that case keeps the exact
pre-existing hard `reached_unresolved_direct_edge` fail, unchanged), and the
value-flow proof for its target set *failed*
(`compute_indirect_target_set` returned false in
`src/cpu/m68k/static_discovery.cpp`'s `process_indirect_control`) -- an
unknown/unbounded index producer, or a candidate that failed evaluation/
canonicalization. Discovery's own outcome for this instruction is completely
UNCHANGED from ADR-0009: it still fails closed with the exact same
`reached_unresolved_direct_edge` issue it always has, so this instruction is
never part of a retained static block (the existing `exclude_frontier_
instruction` logic in `discover_m68k_general_startup`'s `build_analysis`
always excludes the failing frontier instruction itself), and every
downstream discovery/preflight/C4 consumer of that shape is unaffected.
Alongside that unchanged failure, discovery additionally records a weaker,
deliberately minimal fact for this exact case,
`M68kUnprovenIndirectControlEaSet` (source provenance, the decoded control
EA, and an `is_call` flag -- no value set, no candidate list), parallel to
but never combined with `M68kIndirectTargetEaSet`. A given source
instruction gets **at most one** of the two facts, chosen entirely by
whether the finite-value proof succeeded.

Tier 2 is realized entirely at C4 EMISSION time, not inside discovery's own
graph or ADR-0009's per-block terminal-lowering path (which Tier 2 never
reaches at all, for the structural reason above): the generic, unconditional
`genesis_frontier_stop_<addr>` function C4 already builds for every
`reached_unresolved_direct_edge` frontier (`build_genesis_frontier_stop_
function`) is replaced by a Tier-2-aware one, under the SAME function name
and the SAME single `genesis_dispatch` arm ADR-0013 Decision §6 already
builds for every frontier exit, whenever a matching `M68kUnprovenIndirectControlEaSet`
fact exists for that exact frontier's own source instruction. Every other
frontier (including one whose category is anything other than `reached_
unresolved_direct_edge`, or one with no matching fact) keeps the pre-existing
generic body, byte-for-byte. A JSR's own continuation is deliberately NOT
registered as a discovery-time block entry or walked at all (there is no
known callee to frame, so no `M68kStaticCall`/frame is ever created for it);
an eventual RTS back through an unframed Tier-2 call is therefore a distinct,
separately diagnosed `return_target_mismatch` frontier this minimal vertical
slice does not attempt to resolve -- a bounded, honestly fail-closed
limitation, not a soundness gap (the call still never dispatches anywhere
unvalidated; only its own eventual return is unresolved, and only once a
Tier-2 dispatch has already succeeded into a real, validated block).

When Tier 2 applies, its generated `genesis_frontier_stop_<addr>` function:

1. Emits the **exact same faithful EA computation** ADR-0009's Tier 1 already
   lowers (`pc_base_address + displacement + sign_extend_16(Dn)`), from the
   same decoded EA fields -- no new arithmetic, no relaxed canonicalization.
2. Checks that computed value for membership in the compiled-in
   `EmittedCodeAddressSet` array via a new shared binary-search helper,
   `m68k_emitted_code_address_member` (`runtime/genesis/runtime.c`) --
   distinct from Tier 1's own `m68k_indirect_target_member` linear scan only
   because the shared set may be far larger than any one site's own small
   proven candidate array; both are the same *kind* of generic array+count
   membership primitive, and neither ever fetches or decodes ROM bytes.
3. On membership, dispatches through the **exact same** existing
   `genesis_dispatch` (`GENESIS_CONTINUE_AT_PC`) any other resolved control
   transfer already uses -- no second dispatch loop, no new dispatcher.
4. On non-membership, fails closed via `genesis_static_stop` with the
   existing `GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET` stop class (unchanged)
   and a new diagnostic, `GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED`
   (`runtime/genesis/runtime.h`), distinct from Tier 1's
   `GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE` so a sanitized report can
   always attribute which tier's guard actually fired, with PC/A7 left
   completely unchanged (mirroring ADR-0009's own non-member contract).

Tier 2 never widens Tier-1's own per-site semantics, never broadens which
computed-EA *forms* are representable (still exactly `pc_index8`, still
requiring `!index_is_address && !index_is_long`), and is mutually exclusive
per site with Tier 1 -- it is additive at the granularity of "one more site
shape becomes representable," not a relaxation of an existing one.

### Phase B demotion to fallback

`discover_m68k_general_startup`'s seed set (`src/machine/genesis/frontend.cpp`)
becomes:

```
S = {reset entry} ∪ external_code_entry_candidates (Ghidra, or an equivalent tool/human-authored source) ∪ runtime_confirmed_seeds (Phase B)
```

`external_code_entry_candidates` is folded in ahead of `runtime_confirmed_seeds`,
matching the ordering already used for the reset entry ahead of Phase B's own
roots. ADR-0013's own mechanism (persistence, checkpoint/resume, the
per-invocation newly-promoted-root budget, the round cap) is **completely
unchanged** -- this ADR only adds one more source of seeds ahead of it in the
union, demoting Phase B to genuine fallback discovery: code Ghidra's analysis
missed, a runtime-confirmed target absent from the candidate inventory, or
genuine code/data ambiguity an external tool could not resolve. Every Phase-B
mechanism SEG-007-T172 finalized continues to operate exactly as before for
any seed not already covered by the candidate inventory.

`external_code_entry_candidates` is empty by default (`FrontendProgram`'s own
default-constructed field). Ordinary raw-ROM recompilation that never opts
into `--external-hints` therefore reduces this union to exactly
`{reset entry} ∪ runtime_confirmed_seeds` -- byte-for-byte identical to the
seed set before this ADR. This is a structural argument (an empty set unions
in nothing), not a claim requiring re-derivation per ROM: the raw/unannotated
route is unaffected by construction, and this ADR's own Scope item 8
re-confirms it empirically for the authorized pinned Sonic route.

This same empty-by-default argument extends to Tier 2's own emission gate:
Tier-2 lowering additionally requires at least one validated candidate root
(`FrontendAnalysis::validated_code_entry_candidate_roots`, populated only
from `external_code_entry_candidates`) to exist ANYWHERE in the accepted
prefix before it engages for ANY frontier, in ANY part of the program --
never merely "this specific site's own candidate is present." Without this
gate, an unprovable computed indirect site reached by a program that never
opted into `--external-hints` at all would still observe a DIFFERENT
generated diagnostic (`GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED`
instead of the pre-existing `GENESIS_DIAG_REACHED_UNRESOLVED_DIRECT_EDGE`)
merely because this ADR's mechanism exists, which would violate "byte-for-
byte unaffected" for the raw route even though it would still be sound. With
the gate, `M68kUnprovenIndirectControlEaSet` facts are still always recorded
by discovery (harmlessly, whether or not any candidate exists), but C4
emission's generic, pre-existing frontier-stop body is chosen for every site
until at least one candidate has actually been validated somewhere in the
same build.

## Consequences

SEG-007-T174 implements and tests exactly this mechanism:

- `include/segarecomp/machine/genesis/frontend.hpp` /
  `src/machine/genesis/frontend.cpp`: `GenesisCodeEntryCandidateHint`,
  `FrontendProgram::external_code_entry_candidates` (empty by default),
  `parse_genesis_external_code_entry_candidates` (sort+dedup by address, same
  fail-closed contract as ADR-0023's reader), and the seed-set fold-in inside
  `discover_m68k_general_startup`.
- `include/segarecomp/cpu/m68k/static_program.hpp` /
  `include/segarecomp/cpu/m68k/static_discovery.hpp` /
  `src/cpu/m68k/static_discovery.cpp`: the new
  `M68kUnprovenIndirectControlEaSet` fact (source provenance, decoded control
  EA, and an `is_call` flag), constructed ADDITIVELY alongside discovery's
  existing, completely UNCHANGED `reached_unresolved_direct_edge` fail-closed
  path in `process_indirect_control` -- discovery's own success/failure
  outcome, and therefore every existing ADR-0009/0022/0023 test's own
  "still unresolved" assertion, is untouched by this ADR.
- `src/codegen/c11/frontend.cpp`: `build_genesis_frontier_stop_function`
  gains an optional `emitted_code_addresses` parameter. A `reached_
  unresolved_direct_edge` frontier whose exact source instruction matches a
  retained `M68kUnprovenIndirectControlEaSet` fact is emitted as the Tier-2
  faithful EA computation plus a binary-search membership guard, IN PLACE OF
  the generic unconditional-stop body every other frontier still gets --
  same function name, same single `genesis_dispatch` arm ADR-0013 Decision
  §6 already builds for every frontier exit, so no new dispatch mechanism is
  added anywhere. Because a computed indirect site's own instruction is, by
  construction, always the failing frontier instruction itself (`build_
  analysis`'s existing `exclude_frontier_instruction` logic in `src/machine/
  genesis/frontend.cpp` always excludes it from ever becoming part of a
  retained static block), Tier 2 never reaches ADR-0009's own per-block
  terminal-lowering path at all -- that path, and Tier 1's own emission,
  stay completely unchanged. `emitted_code_address_set` itself is one
  sorted/deduplicated copy of the final `emitted_block_entries` set (the
  same set `genesis_dispatch`'s own per-block arms are already built from);
  because it is not known until after every accepted block's own
  reachability is resolved, but a frontier stop function's NAME must be
  known before blocks are emitted (they call it by name), this function's
  own real body text is built and appended in a second pass once
  `emitted_code_address_set` is known, after a first pass emits only a
  forward declaration for each name -- ordinary, always-legal C, no runtime
  behavior change.
- `include/segarecomp/machine/genesis/frontend.hpp` /
  `src/machine/genesis/frontend.cpp`: `FrontendAnalysis::validated_code_
  entry_candidate_roots` -- every external code-entry candidate discovery
  actually admitted as a block entry -- seeded into every existing
  reachability walk that already seeds itself from the IRQ6 handler root
  (`runtime_frontier_eligible` in both its `machine/genesis/frontend.cpp` and
  `codegen/c11/frontend.cpp` copies, and the C4 emission reachability walk),
  so a validated, entry-disconnected candidate's own block is retained/
  emitted rather than pruned as unreachable, exactly like the pre-existing
  IRQ6 handler root.
- `runtime/genesis/runtime.h` / `runtime/genesis/runtime.c`:
  `GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED` (paired with the existing
  `GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET` stop class, no new stop class),
  and `m68k_emitted_code_address_member`, a sorted-array binary search beside
  the existing linear-scan `m68k_indirect_target_member`.
- `src/main.cpp`: parses `code_entry_candidate` records from the same
  `--external-hints` file already opted into for ADR-0023's records, logging
  each accepted candidate's address/provenance to stderr (never stdout).
- `tools/genesis_startup_bridge.py`: the diagnostic-pairing table gains the
  new diagnostic under the existing stop class.

SEG-007-T174's own Evidence records the synthetic coverage proving: a valid
candidate inventory validates and emits; a false-positive candidate (pointing
at data or an invalid decode) is excluded/fails closed without ever becoming
runtime authority (via the existing per-root independent-walk/merge
machinery -- a bad seed's own primary issue becomes an ordinary best-effort
secondary, never poisoning another root's successful result); a duplicate/
out-of-order inventory normalizes deterministically; Tier-1 behavior is
completely unchanged with Tier-2 code compiled in but untriggered; a Tier-2
computed target inside `EmittedCodeAddressSet` dispatches successfully; a
Tier-2 computed target outside it fails closed with the precise new
diagnostic and unchanged PC/A7; no runtime opcode fetch/decode exists
anywhere in the Tier-2 path; the raw/unannotated route is byte-for-byte
unaffected; and Phase-B fallback still promotes a runtime-confirmed target
absent from the initial candidate inventory, including a checkpoint/hints-
identity-mismatch regression. See that task's Evidence for the normalized
re-execution results of the authorized pinned Sonic route (no raw address,
opcode, disassembly, or byte content from that ROM, per the project charter's
commercial-derived-evidence reduction rule).
