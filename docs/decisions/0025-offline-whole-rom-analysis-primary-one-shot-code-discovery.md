# ADR-0025: Offline whole-ROM analysis as the primary one-shot assisted code-discovery mechanism

- Status: Accepted
- Date: 2026-09-06
- Deciders: SEG-007-T179 (architecture-class vertical slice), consuming the
  `backlog/seg-007-refine` decision (PR #299) that itself consumed SEG-007-T178's
  `needs_full_refinement - the discovery-architecture transition` handoff.
- Amends: ADR-0024 (Tier-2 emitted-set dispatch form set, and its cross-root
  cross-tier fail-closed rule). Demotes ADR-0013 Phase B on the canonical
  assisted route only. Weakens neither ADR-0009 Tier-1 nor the raw/unannotated
  recompilation route.

## Context

ADR-0024 introduced a Ghidra-assisted, validated code-entry inventory
(`code_entry_candidate` records in the heterogeneous `--external-hints` format)
as an *additional* seed source, and a `pc_index8`-only Tier-2 dispatch that
lowers an unprovable computed-control site to a compiled-in `EmittedCodeAddressSet`
membership test. ADR-0013 Phase B remained the primary code-discovery engine: a
multi-round build / execute / expand loop that promotes runtime-confirmed
`discovery_prefix_boundary` addresses into the seed set and regenerates.

SEG-007-T178 landed the ADR-0009 finite-`An` Tier-1 producer for `A0`-`A6` and
handed off that continuing to specialise frontier-by-frontier target recovery in
a runtime loop is the wrong investment: each round is one commercial-derived
address promoted into a persisted checkpoint, the loop is non-deterministic
across environments, and it structurally cannot converge offline.

## Decision

### 1. Deterministic offline whole-ROM analysis is the primary discovery mechanism

`python3 tools/ghidra.py analyze --rom <rom> --output <artifact>` runs a single
deterministic headless Ghidra pass in the existing pinned, containerised
environment (SEG-007-T162 Docker stack). No LLM, MCP bridge, or agent
participates. It emits a high-recall union of code-entry candidates:

- `FunctionManager.getFunctions(true)` entry addresses;
- `BasicBlockModel` / `SimpleBlockModel` code-block start addresses and
  `Listing.getInstructions(true)` instruction-run starts;
- `ReferenceManager` non-computed call / jump / flow reference `getToAddress()`;
- code / flow reference destinations landing in the executable range;
- "code islands": contiguous disassembled instruction runs outside any
  `Function` body (run-start addresses only).

The union is filtered to the executable range, normalised to the m68k program
space (image base 0), odd addresses dropped, sorted ascending and de-duplicated.
False positives are acceptable; false negatives are the concern. Every candidate
is a *proposal*, never a pre-validated seed.

### 2. Interchange artifact: reuse the ADR-0024 `code_entry_candidate` record

No new record kind, parser, or authority path. The producer emits
`kind = "code_entry_candidate"` records (each carrying `address` and
`provenance`) in the existing heterogeneous `--external-hints` array, with a
top-level / per-record `rom_sha256` binding, consumed unchanged by
`parse_genesis_external_code_entry_candidates`. On ROM-hash mismatch the producer
emits an empty candidate list. Records carry addresses only — never Ghidra ids,
names, timestamps beyond the existing `provenance.timestamp`, or disassembly.

Ghidra is the primary offline **code-entry candidate** producer only. It is not
authoritative for trusted table semantics: `tools/ghidra.py analyze` emits a
candidate-only artifact and MUST NOT be consumed directly as the canonical
`--external-hints` input. A future analyzer MAY propose richer structured
information, but T179 does NOT automatically trust Ghidra switch/table inference
as an ADR-0023 `logical_table_descriptor`; that requires a separate generic
validation contract (see §10).

### 2a. The canonical offline analysis input is the composed heterogeneous artifact

Offline Ghidra analysis AUGMENTS, never REPLACES, existing structured hints.
`tools/ghidra.py compose-hints --candidates <analyze output> [--base-hints <path>]
--rom <rom>|--rom-sha256 <sha> --output <artifact>` deterministically produces the
canonical one-shot artifact as the UNION of:

- the pre-existing ROM-bound heterogeneous structured hints for that ROM
  (resolved by default via `tools/analysis_hints_path.py`), with **every**
  recognized structured record kind — every `logical_table_descriptor` in
  particular — preserved byte-for-byte per its existing parser/validation
  contract, never dropped and never reinterpreted as a code candidate;
- the freshly generated Ghidra `code_entry_candidate` records.

Composition verifies a single matching `rom_sha256` across all inputs;
canonically re-serialises with the same
`json.dumps(sort_keys=True, separators=(",", ":"))` + trailing-newline form the
candidate producer already uses; de-duplicates `code_entry_candidate` records by
address; fails closed on incompatible ROM identity or genuinely conflicting
structured records; and never writes the composed artifact over the canonical
private per-ROM structured-hints source file. Two clean runs on identical inputs
are byte-identical. SEG-007-T174 already proved a candidate-only hints file
regresses the real assisted route by silently discarding the trusted
`VBlank_Index` table descriptor; the composed artifact is the corrected input.

The existing `provenance` object (`tool`, `tool_version`, `timestamp`,
`human_reviewed`) plus the record `rom_sha256` is sufficient for
authority / identity. Optional, parser-tolerant, non-authoritative
`provenance.tool_build` (image digest / Ghidra revision) and
`provenance.analysis_recipe` (recipe hash) MAY be carried for audit binding
only; a missing field is never an admission failure.

### 3. Candidates are validated only by the existing per-root discovery walk

External candidates are folded into the seed set
(`{reset entry} ∪ external_code_entry_candidates`) and validated by the
*existing* independent per-root discovery walk (mapping, alignment/address,
MC68000 decode, static safety/discovery, representation/emission). No second
validator is added and no candidate is pre-validated. Only a candidate that
survives that walk and is admitted into the accepted static prefix becomes an
emitted block / `EmittedCodeAddressSet` member; a failing candidate is excluded
with a precise normalised rejection reason and never becomes runtime authority.

### 4. Tier-2 dispatch generalised to pure register-indirect control EA

ADR-0024's `pc_index8`-only Tier-2 emitted-set dispatch is generalised to the
supported pure register-indirect control EA forms: `JMP (An)` and `JSR (An)`
(EA mode 2, no displacement, no index, no extension word), `n` in `0..6`.

- Producer (`src/cpu/m68k/static_discovery.cpp`
  `process_indirect_control_an`): when the finite-`An` Tier-1 proof is genuinely
  unavailable for a pure `(An)` site with `n < 7`, additionally record
  `M68kUnprovenIndirectControlEaSet` (source provenance + decoded control EA
  only, no candidate list) — the same additive shape `process_indirect_control`
  records for `pc_index8`. The primary `reached_unresolved_direct_edge` failure
  is unchanged. For `(A7)`: record **nothing** (the SEG-007-T178 A7/SP exclusion
  is preserved at the producer). Tier-1 (`compute_indirect_target_set_an`,
  callee-write footprint proof, `m68k_an_finite_proof_eligible`) is untouched.
- Emission (`src/codegen/c11/frontend.cpp`
  `build_genesis_frontier_stop_function`): the Tier-2 form gate accepts pure
  `address_indirect` (`reg < 7`) in addition to `pc_index8`. For the register-
  indirect form the computed EA is emitted as `runtime->a[<reg>]` with **no
  arithmetic**; every downstream gate is shared unchanged with `pc_index8` —
  the non-empty `validated_code_entry_candidate_roots` engagement gate, the
  `m68k_emitted_code_address_member` binary-search membership test, the
  fail-closed non-member path (`GENESIS_STOP_UNRESOLVED_INDIRECT_TARGET` /
  `GENESIS_DIAG_TIER2_COMPUTED_TARGET_NOT_EMITTED`, PC and A7 untouched), and
  the `JSR` return-frame push (identical alignment/range checks; the push is
  form-agnostic, so `JSR (An)` is shipped with the same minimal soundness
  ADR-0024 already requires for `JSR (d8,PC,Xn)`). `reg == 7` is refused
  outright. `runtime/genesis/runtime.c/.h` is unchanged
  (`m68k_emitted_code_address_member` and the diagnostic are already
  form-agnostic).

No runtime opcode fetch or decode exists in the Tier-2 `(An)` path: it compares
one 32-bit integer against a compiled-in address table.

### 5. Tier-1 strictly supersedes Tier-2 across roots (ADR-0024 amendment)

ADR-0024's cross-root cross-tier rule failed the whole aggregation closed
(`startup_graph_mismatch`) when one root proved a finite Tier-1
`M68kIndirectTargetEaSet` for a source address while another root recorded only
the weaker Tier-2 `M68kUnprovenIndirectControlEaSet` for the same address. That
fragile fail-closed is replaced: **Tier-1 strictly supersedes Tier-2 for one
source address.** `merge_root_result` drops the weaker unproven fact and keeps
the proven set. The proven set is a decode-validated finite enumeration;
preferring it over fail-closed can never introduce an unsound dispatch, and the
prior rule made the one-shot route brittle whenever the offline inventory seeded
a root that reached an `(An)` site without its `An` producer. A genuine
Tier-1/Tier-1 candidate-set disagreement, and a Tier-2/Tier-2 control-EA
disagreement, still fail closed unchanged.

### 6. Canonical one-shot assisted route

`tools/genesis_startup_bridge.py --one-shot` (commercial mode,
`--diagnose-frontier`, `--external-hints <artifact>` required): exactly one
generation, one compile, one run. `<artifact>` is the composed heterogeneous
artifact from §2a (structured hints ∪ Ghidra candidates), never the raw
candidate-only `analyze` export. Hard-rejects `--checkpoint` /
`--compare-runs` / `--full-report-path` / expansion input. Seed set is
`{reset entry} ∪ external_code_entry_candidates`; `runtime_confirmed_seed_count`
is `0`; no `--analysis-seed` is ever appended. Reaching unemitted code on a
generated-native stop at an unresolved computed-control / discovery-prefix
boundary is reported once as `driver_result = offline_inventory_incomplete` with
the normalised frontier class — never another round.

### 7. ADR-0013 Phase B demoted to fallback + non-canonical oracle only

The raw/unannotated (`--external-hints`-free) recompilation route is
byte-for-byte unchanged; Phase B still works there. The multi-round expansion
loop / checkpoint / `PHASE_B_BATCH_COMPLETE` machinery is retained only as (a)
that unchanged raw-route fallback and (b) a non-canonical, non-production
diagnostic coverage oracle for finding legitimately missed roots. Oracle
findings improve the Ghidra recipe or add structured `code_entry_candidate` /
`logical_table_descriptor` records; they are never re-normalised as production
seeds and never persisted to a canonical checkpoint.

### 8. Determinism

The `analyze` wrapper pins a deterministic analyzer recipe (enable disassembly /
aggressive-instruction-finder / function-start / basic-block / reference;
disable decompiler parameter-id and variance-prone analyzers), imports with
processor `68000:BE:32:default` and `BinaryLoader` base `0x0`, runs with
`-max-cpu 1`, a fixed `-analysisTimeoutPerFile`, `-deleteProject`, then
canonically re-serialises the script output
(`json.dumps(sort_keys=True, separators=(",", ":"))`, candidate records sorted
by address, trailing newline). Two clean runs on the same ROM and pinned image
must be byte-identical.

### 9. Composition preserves, never trusts, structured records

`compose-hints` copies each recognized non-candidate structured record through
unchanged and applies no new semantic authority. A `logical_table_descriptor` in
the composed artifact carries exactly the trust its ADR-0023 producer +
`parse_genesis_external_hints` validation already granted it — no more because it
now sits beside Ghidra candidates, no less because Ghidra did not originate it.

### 10. No broadening into general jump-table recovery

T179's scope is bounded: the canonical assisted route must not regress by
dropping trusted structured hints it already consumes. It does NOT add automatic
general jump-table / switch-table recovery, and it does NOT let Ghidra's own
table inference become an ADR-0023 logical descriptor without a separate generic
validation contract. Such an analyzer is future work.

## Normalized metrics (record per canonical re-execution, in Evidence, verbatim)

offline candidate code-entry count; accepted count; rejected count with
normalised rejection-reason classes; emitted block / code-address count;
`runtime_confirmed_seed_count` (must be `0`); generation-round count (`1`) and
compile count (`1`); whether the Phase-B diagnostic oracle was run and, if so,
missed-root count with per-miss classifications (exporter omission / block
granularity / opaque-or-table dispatch expressible via structured hints /
Ghidra-undefined island / other); final semantic frontier (normalised stop
class / diagnostic category); a generated-source / build-cost proxy;
VRAM/CRAM/VSRAM nonzero-byte counts; DMA phase/kind/target/progress count;
relevant VDP display/register configuration; whether the SEG-007-T178
`JMP (An)` frontier passed through sound Tier-2 dispatch. No raw address,
opcode, byte, or disassembly.

## Consequences

- Assisted discovery becomes deterministic and offline; no persisted
  commercial-derived seed checkpoint on the canonical route.
- Tier-2 covers both supported pure computed-control EA forms; ADR-0009 Tier-1
  and the raw route are unchanged.
- A historical fallback root that cannot be subsumed by a generic offline
  candidate / structured hint without a hardcoded Sonic address, weakened
  segarecomp validation, blind full-ROM linear decode, or a silently restored
  production Phase-B round stops T179 with `needs_full_refinement` and the
  normalised missing-root class rather than forcing a fit.
