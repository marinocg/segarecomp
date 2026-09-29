# ADR 0052: Own-PC Parameterized Immutable-ROM AOT Helpers (Factoring Group C)

- Status: Accepted
- Date: 2026-09-29
- Task: SEG-025-T001
- Extends: ADR 0045 (exact statically selected AOT body helpers, groups A and B). Unchanged: admission, the
  compiled-address set, dispatch, lowering (`emit_immutable_rom_aot_body` -> `emit_m68k_operation_c`), timing.

## Context

After ADR 0045 shares byte-identical AOT bodies, the SEG-025 attribution of a current 512 KiB no-hints title
(245,694 AOT entries) found 150,587 exact distinct bodies (177 MB) but only 38,173 literal-normalized shapes.
Normalizing one literal class at a time, the entry's **own instruction address** dominated: it alone cut distinct
bodies to 78,117 and distinct body bytes by 71.5 MB (40%); every other class was worth at most 9 MB. About 72,000
of those bodies are instruction-exception raises (data words that decode as ILLEGAL/line A/line F), which differ
only in the stacked PC they pass to `genesis_raise_software_exception`.

## Decision

1. After exact grouping (ADR 0045 group B), a body that is **single-use under exact identity** is re-keyed by its
   own-PC-normalized text: every token spelled exactly `UINT32_C(<own address as 0x%08X>)` is replaced by the
   identifier `genesis_aot_pc`. Bodies containing `case `, `static ` or `switch (` are never rewritten.
2. Normalized bodies used by two or more entries become one generated helper
   `genesis_aot_shared_N(GenesisRuntime *runtime[, const GenesisInstructionProvenance *genesis_aot_source],
   uint32_t genesis_aot_pc)`. Each entry calls it with its own PC literal. A normalized body used once is emitted
   inline with the literal restored, byte-identical to the pre-group-C form.
3. Exact-shared bodies are never re-keyed, so group C only adds sharing. (A first prototype re-keyed every body;
   an RTS whose own address is in the return-target set then lost its exact helper and was inlined with the whole
   membership chain, which doubled owner-TU compile time on a 1 MiB title.)

Selection stays a generation-time exact text identity. The helper identity is chosen by code generation. There is
no runtime opcode decode, IR dispatch, family switch, descriptor or instruction engine. No opcode family, width,
EA mode, privilege behavior, exception class or timing family is parameterized.

## Soundness

Every replaced position held exactly that entry's own address, and all members of a group have identical text
outside the replaced positions. The caller passes that same value. A `UINT32_C` constant has the promoted type of `uint_least32_t`, which
is `uint32_t` on every supported target, so each use sees the same value and type. Bodies with constant-expression contexts are excluded.

## Evidence (SEG-025-T001)

Baseline main `e559d8a` vs this change. Apple clang 21, arm64, 8 jobs. Sizes and counts are deterministic;
times and RSS are machine-variable.

| input | generated C | compile -O0 | compile -O2 | executable -O0 / -O2 | runtime -O2 (20M instr) |
| --- | --- | --- | --- | --- | --- |
| 512 KiB title | 296 -> 218 MB (-26%) | 7.6 -> 6.1 s | 22.7 -> 17.4 s | 82.0 -> 64.9 / 39.5 -> 33.7 MB | 2.182 -> 2.189 s |
| 1 MiB title | 546 -> 390 MB (-29%) | 17.1 -> 12.2 s | 42.7 -> 33.3 s | 149.1 -> 114.1 / 72.8 -> 60.8 MB | 2.200 -> 2.194 s |
| synthetic 2 MiB | 1.22 -> 0.88 GB (-28%) | 33.0 -> 20.2 s | - | 341 -> 266 MB | - |
| synthetic 4 MiB | 2.40 -> 1.72 GB (-28%) | 62.5 -> 43.0 s | - | 667 -> 519 MB | - |

- Admitted AOT, final compiled-entry, AOT-owned and block-owned address sets are identical (count + SHA-256) on
  every input. Budgeted title runs produce byte-identical reports at -O0 and -O2.
- Only 31-53 helpers are added. AOT body bytes fall 38-42%. Generation wall rises 0-9%, and generation peak RSS
  rises 1-30% (normalized texts are held until helpers are emitted).
- The differential fixture (`genesis_immutable_rom_aot_body_factoring_differential_test`) adds ILLEGAL, line A,
  line F and TRAP, and installs software-exception handlers in one scenario. For every compiled entry, the factored
  and unfactored forms match (sharded and single-file, 5 scenarios, single step and 12-step runs). Registers, SR,
  USP, PC/transfer, work RAM, stop class and full provenance, checkpoint and whole-runtime digests all agree. Each
  call must pass its own PC.

## Consequences

The remaining literal classes (immediates, direct successors, absolute operand addresses, masks) are each worth
at most about 5% of distinct AOT body bytes on the measured title. Parameterizing several of them together would
multiply helper parameters and risk for a modest gain, so no further group is adopted.
