# SEG-014-T003 discovery and recompiler seam

## Status

T003 is delivered. This document originally recorded the pre-move
characterization and the planned extraction shape (production commits
`c23328d`, `f7443c1`; see the SEG-014-T003 backlog record for full
evidence). It now records the delivered ownership state, not a plan.

The one production walk that needed to move -- the established
`discover_m68k_general_startup` recursive DFS -- has moved. `cpu/m68k` now
owns the real graph traversal; `m68k_pipeline_frontend.cpp` supplies only
external machine/scenario facts through a narrow environment interface and no
longer contains a second copy of the traversal switch.

## Delivered ownership table

| Responsibility | Owner | Notes |
| --- | --- | --- |
| Canonical decoded instruction keyed by source address; decode profile/order; `RejectedM68kDecode` interpretation; decode insertion order; decoded/lifted source provenance | `cpu/m68k` (`M68kStaticDecodeCache`, `libs/cpu/m68k/include/segarecomp/cpu/m68k/static_discovery.hpp`) | The CPU walk requests a verified instruction source from `M68kStaticDiscoveryEnvironment::instruction_source`, calls `decode_m68k_instruction` itself, and owns the one-address decode cache and all reuse. |
| Mapped image-byte availability, claim-bounded source span, source image offset | Genesis frontend (`M68kGeneralStartupEnvironment`, `src/m68k_pipeline_frontend.cpp`) | Implements `instruction_source`; never calls the MC68000 decoder itself; the only owner of `FrontendProgram`/`MappingClaim` lookups for this purpose. |
| Visitation key `(address, active call-frame stack)`, recursive work order, primary versus sibling (SEG-007-T062/T064) failure preservation | `cpu/m68k` (`discover_m68k_static_graph`, `libs/cpu/m68k/src/static_discovery.cpp`) | Owned entirely by the moved recursive walk; the environment never decides whether a state was visited. |
| Instruction, block-entry, and call-frame-depth discovery budgets | `cpu/m68k` | Consumed as `M68kStaticDiscoveryLimits`, supplied by the frontend from the existing `m68k_discovery_max_instructions`/`m68k_discovery_max_blocks`/`m68k_discovery_max_call_frame_depth` constants (unmoved, still Genesis-owned tuning values, not hardware facts). |
| Block-entry registration/order, mid-instruction direct-target consistency over canonical decoded spans | `cpu/m68k` | Uses the CPU's own decode cache and `accepted_branch_targets` bookkeeping; unchanged from the pre-move algorithm. |
| Bcc/BRA/BNE/DBcc target arithmetic, branch edge construction/order, JMP/JSR/BSR/RTS control flow, frame-stack changes, `switch (decoded.kind)` successor selection | `cpu/m68k` | The entire successor-selection switch now lives only in `discover_m68k_static_graph`; confirmed absent from `m68k_pipeline_frontend.cpp` by `tests/cpu_m68k_static_discovery_boundary_test.py`. |
| Static call identity, direct-call/return edge identity, contextual RTS return-edge deduplication, static frames | `cpu/m68k` (`libs/cpu/m68k/include/segarecomp/cpu/m68k/static_program.hpp`, unchanged by this task) | `m68k_make_static_call`, `m68k_make_static_call_edge`, `m68k_make_static_return_edge` remain the sole constructors. |
| Control-EA resolvability, target arithmetic, canonical CPU address mechanics, 24-bit/odd target validity | `cpu/m68k` | Validated before external target admission, via `M68kDiscoveryTargetRole::direct_branch`/`direct_call`. |
| Target mapping/claim availability, conflicting mapping, call-target-specific admission asymmetry | Genesis frontend (`M68kGeneralStartupEnvironment::admit_target`) | Implements the exact pre-move asymmetry: branch-target admission returns a populated `matched_claims` list on conflict; call-target admission (`validate_m68k_static_call_target`) collapses "unmapped" and "conflicting" into one category with an always-empty claims list. |
| Static memory operand routing, RAM/ROM/controller/device classification | Genesis frontend (`M68kGeneralStartupEnvironment::classify_memory_access`, delegating to the trimmed `m68k_classify_general_memory_access`) | The CPU supplies an already-foldable, already-canonical, already-aligned `M68kCpuMemoryAccessRequest`; the adapter owns Genesis ROM/RAM/device-routing policy. |
| Synthetic completion declaration and sentinel/stack-slot policy | Genesis frontend (`M68kGeneralStartupEnvironment::is_completion_rts`) | The CPU asks whether a decoded empty-stack RTS is this scenario's declared terminal; the frontend answers from `FrontendProgram::synthetic_completion`. |
| `M68kDiscoveryIssue` -> `FrontendRejected` legacy diagnostic/report projection, `frontier_access` reconstruction, raw-instruction-read access-record reconstruction | Genesis frontend (`translate_m68k_discovery_issue`) | Consolidates every pre-move inline `FrontendRejected` construction site into one translation function; a temporary, explicitly-acceptable adapter per the extraction discipline this task followed. |
| Accepted-prefix reconstruction/pruning (`build_analysis`), C4 static-memory/movem-adjacent-LEA/owned-cartridge-region fact enrichment, Genesis frontier class projection (`classify_frontier`), frontier eligibility (`runtime_frontier_eligible`) | Genesis frontend, still awaiting T004 placement | Per `docs/architecture/seg-014-t001-symbol-migration-map.md` ("T003 begins the split, T004 completes it"), physical relocation of this Genesis-specific logic into `machine/genesis/` is SEG-014-T004's scope, not T003's. Not moved by this task; RULE 10 (avoid speculative genericity with only one real consumer) applies. |
| Deterministic multi-frontier `(source, target-or-zero, class)` sort/dedup/budget-bound | `recompiler` (`recompiler_sort_dedup_and_bound_frontiers`, `libs/recompiler/include/segarecomp/recompiler/frontend.hpp`) | A header-only template taking a caller-supplied key function; contains no Genesis or MC68000 type. Genesis's own `classify_frontier`/`frontier_sort_key`/`runtime_frontier_eligible` still supply the classification and key, unmoved. |
| Generic partial-program contract (`RecompilerPartialProgram<AcceptedProgram, Frontier>`) and C4 compilation-plan-readiness types | `recompiler` (established by earlier T003 commits, unchanged by the final checkpoint) | `machine/genesis/frontend_compat.hpp` instantiates `FrontendPartialProgram = RecompilerPartialProgram<FrontendAnalysis, UnresolvedFrontier>`. |

## The delivered typed environment

`M68kStaticDiscoveryEnvironment` (`libs/cpu/m68k/include/segarecomp/cpu/m68k/static_discovery.hpp`)
is one abstract interface, not a callback framework. It has exactly four
operations, each answering a question about one already-identified address or
request; none receives or returns a walk/recurse/successor-set/frame-stack
decision:

1. `instruction_source(pc)` -- the bounded, already-validated instruction byte
   span and independent target provenance for `pc`, or a typed
   `M68kInstructionSourceIssue` (unmapped/conflicting, with the matched
   `MappingClaim` list);
2. `admit_target(target, role)` -- mapped-address admission for one
   already odd/mid-instruction-checked (`direct_branch`) or not-yet-checked
   (`direct_call`, which owns its own odd/24-bit/mapped rule) candidate
   target, returning `std::nullopt` to accept or a typed `M68kMappingIssue` to
   reject;
3. `classify_memory_access(request)` -- region/ROM-write/device-routing
   classification for an already-foldable, already-canonical, already-aligned
   memory operand, returning `std::nullopt` to accept or a `DirectFlowDiagnostic`
   to reject;
4. `is_completion_rts(rts_provenance)` -- whether this decoded empty-stack RTS
   is the scenario's own opt-in synthetic-completion terminal.

`M68kDiscoveryIssue` is the CPU-owned result type crossing this boundary. It
reuses the existing CPU-owned `DirectFlowDiagnostic` enum (no second,
parallel diagnostic taxonomy) and carries every field a hosting scenario's own
rejection report needs to reconstruct byte-for-byte
(`translate_m68k_discovery_issue`): optional provenance/target/instruction
length/available bytes/requested length/mapping claims/unresolved reason, an
optional `M68kCpuMemoryAccessRequest`, and three narrow boolean
reconstruction-shape discriminators
(`reconstruct_single_mapping_claim`/`reconstruct_instruction_read_access`/
`set_pc_based_image_offset`) needed only because several distinct pre-move
rejection-construction shapes share the same `DirectFlowDiagnostic` category.

`M68kStaticDiscoveryResult` is the CPU's complete discovery-pass result:
`decode_cache`/`decode_order`, `block_entries` in first-recognition order,
`edges`/`frames` in emission order, an optional `completion_rts`, an optional
`primary_issue`, and ordered `secondary_issues` (the SEG-007-T062/T064
best-effort sibling exits). It deliberately does not contain
`FrontendAnalysis`, a C4 fact, `GenesisFrontierClass`, or a partial program --
the CPU owns exactly one walk, and recompiler/Genesis promotion cannot start a
second one.

## What moved, verbatim

`discover_m68k_static_graph` (`libs/cpu/m68k/src/static_discovery.cpp`) is the real
production traversal, moved with its control structure retained: the decode
cache, decode/block-entry order, visited-state and graph-fact-deduplication
identity, `accepted_branch_targets`, the recursive walk, and every existing
budget/failure observation point. Only source acquisition, external
mapping/operand policy, and scenario-completion recognition were replaced by
the environment operations above. Independent adversarial review of the
complete diff confirmed byte-for-byte behavioral equivalence with the
pre-move inline implementation (see the T003 backlog record's Evidence),
and a controlled real-Sonic A/B comparison between the PR's base and head
commits confirmed both authorized production routes reach the identical
normalized frontier before and after the move.

## What T004 still owns

`build_analysis`'s block reconstruction and its C4/movem-adjacent-LEA/
owned-cartridge-region enrichment passes, `classify_frontier`,
`runtime_frontier_eligible`, and the rest of `partial_or_rejection` remain
inside `m68k_pipeline_frontend.cpp`. Per
`docs/architecture/seg-014-t001-symbol-migration-map.md`, their physical
relocation into `machine/genesis/` is SEG-014-T004's scope. `build_analysis`'s
own block-boundary decision is driven directly by `M68kInstructionKind`
classification, not machine-agnostic composition, and the remaining
Genesis-typed logic has exactly one real consumer today (MC68000/Genesis);
neither is a safe or justified generalization target without T004's own
device/machine-ownership work, per architecture-contract RULE 10.

## Non-goals

This task is not a plugin system, a universal CPU IR, a Genesis device move,
a codegen move, or CLI cleanup. The extracted environment interface preserves
the existing behavioral owner (MC68000 discovery semantics) and puts only
external mapping/scenario policy outside it, exactly as planned before
implementation.
