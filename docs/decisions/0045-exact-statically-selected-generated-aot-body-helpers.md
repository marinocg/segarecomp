# ADR 0045: Exact Statically Selected Generated Immutable-ROM AOT Body Helpers

- Status: Accepted
- Date: 2026-09-26
- Task: SEG-022-T011
- Extends, without editing: ADR 0039 (monotonic immutable-ROM AOT representation), ADR 0044 (deterministic
  translation-unit sharding) and SEG-022-T008's bounded AOT owners. Admission, the admitted immutable-ROM
  AOT address set, the final compiled-address set and generated guest semantics are untouched.

## Context

After SEG-022-T002..T009 the dominant remaining generated-source category was repeated per-instruction
semantic lowering: about 69% of the Sonic hinted and no-hints sources were immutable-ROM AOT and
ordinary-block instruction bodies (SEG-022-T010). Measurement of the AOT bodies showed two concrete,
generic duplication shapes:

1. every routed memory access (and stack push/pop guard) inlined the complete failure tail -- six
   provenance field stores, the route-provenance helper call and the stop transfer -- with the
   instruction's provenance compound literal spelled twice (about 38% of AOT body bytes);
2. once that provenance literal is set aside, most AOT entry bodies are byte-identical to bodies of other
   entries: whole-ROM aligned admission lowers the same instruction shape at many addresses, and a lowered
   body refers to its own address only through that provenance (register, immediate, branch-target and
   continuation literals still make bodies distinct where they differ).

## Decision

1. **Routed-failure tail (group A).** The generated `meta` unit defines one external function
   `genesis_routed_failure_stop(stop, source, address, width, direction)` beside `genesis_static_stop`.
   Its statements are exactly the inline failure tail it replaces (same fields, same order, same
   `genesis_attach_route_provenance` call, same returned transfer). A factored AOT routed access is
   `if (genesis_route_access(...) != GENESIS_ACCESS_OK) return genesis_routed_failure_stop(...);`. The
   platform emitter (`M68kRuntimeCEmitter`) owns the spelling; the M68K lowering owns the semantics and
   only opts in through `M68kMemoryEmissionContext::factored_route_failure`.
2. **Provenance by pointer.** With `M68kMemoryEmissionContext::runtime_source_symbol` set, the lowering
   spells the entry's own instruction provenance as the `const GenesisInstructionProvenance *` identifier
   `genesis_aot_source` instead of the inline compound literal. The pointee is the exact literal
   `instruction_source(operation)` would have spelled; the lowered statements are unchanged.
3. **Exact shared bodies (group B).** Every AOT entry body is lowered once, unchanged, by the existing
   owner (`emit_immutable_rom_aot_body` -> `emit_m68k_operation_c`) and interned by its complete generated
   text. A text used by two or more entries is emitted once as `genesis_aot_shared_NNNNN(runtime[,
   genesis_aot_source])`, a helper whose body *is* that text; each such entry becomes
   `{ return genesis_aot_shared_NNNNN(runtime, <its own provenance literal>); }`. A text used once stays
   inline, binding `genesis_aot_source` locally. Identity is exact byte equality of generated C: nothing
   is parameterized, re-derived or re-implemented, and there is no second M68K implementation.
4. **Static selection only.** Which helper an entry calls is decided at generation time. The helper is a
   straight-line body with no switch over guest code: no interpreter, no runtime opcode decode, no runtime
   IR dispatch, no generic instruction engine, and no runtime state selects a helper.
5. **Determinism and layout.** Helper numbering is first-use order over ascending AOT addresses. Helpers
   are units of a new `shared` family (16 shards, key = helper ordinal, page shift 6) declared in the
   shared header; the Genesis layout is now main + meta + entries + stop + 8 block + 32 AOT + 16 shared =
   **60** TUs, still bounded independent of ROM size. `genesis_bridge_translation_unit_families()` is the
   single definition of that layout. In the single-file form helpers are `static` functions emitted before
   their first caller.
6. **Scope.** Only the bridge route that emits immutable-ROM AOT owners factors. Ordinary CFG blocks and
   the legacy C3 profile are unchanged (ordinary-block failure tails measured about 10% of block bytes
   with no compile-time effect for group A, so they are not factored).
7. **Reference form.** `ImmutableRomAotBodyFactoringScope(false)` reproduces the unfactored bodies on the
   calling thread. It is generation-time only and exists for the factored-vs-unfactored differential test
   and for bisection.

## Consequences

- Generated source and compile time fall on both Sonic routes (SEG-022-T011 evidence), most on the
  no-external-hints route where AOT bodies dominate.
- Generation holds the distinct AOT body texts until the helpers are emitted, so peak generation memory
  rises (still far below the SEG-022-T001 baseline).
- A test that locates an entry's lowering must follow a `genesis_aot_shared_*` call to the helper
  definition; textual identity of generated C is not a contract.
- Correctness evidence: `genesis_immutable_rom_aot_body_factoring_differential_test` runs every compiled
  entry of a project-authored multi-family fixture under several architectural scenarios in the factored
  and unfactored forms (sharded and single-file) and requires identical transfers, complete stop
  provenance, registers, memory, checkpoint digest and whole-runtime digest.
