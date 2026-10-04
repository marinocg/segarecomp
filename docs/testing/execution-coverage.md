# Execution-PC coverage and the reachability challenger (SEG-026-T001, ADR 0053)

Measurement-only tooling. Nothing here changes admission, generated C, generated execution authority or any
report/digest/checkpoint of an ordinary build.

## Complete execution-PC coverage

`GenesisRuntime.execution_coverage` is a host-owned observer pointer (NULL in every ordinary build). When a host
attaches a `GenesisExecutionCoverage`, the runtime marks, in a dense 1 MiB bitmap (one bit per even 24-bit bus
address), the PC of **every instruction that retires**:

- the PC about to execute is tracked from each dispatch (`genesis_runtime_step`) and, at every retirement that
  continues, from that retirement's selected successor (after interrupt admission);
- ordinary multi-instruction blocks therefore contribute every interior instruction (a Sonic attract run retires
  about 1.3 instructions per dispatch, so dispatch PCs alone are incomplete);
- immutable-ROM AOT entries and ADR 0049 RAM-copy aliases contribute their exact execution PC (aliases are
  work-RAM addresses, so they never conflate with their ROM source);
- an instruction that stops fail-closed before retiring is not counted.

The optional first-entry witness records, for the first retirement of each new PC, the previously retired PC and
the cause (initial, retirement successor, interrupt entry, dispatch, interrupt resumption). When interrupt
admission replaces a successor S, (S, predecessor) is kept on a bounded 16-entry stack; when S becomes current
again the witness names the pre-interrupt instruction as predecessor. Without this, an instruction first executed
right after a VBlank handler returned would be attributed to the handler's RTE.

No opcode is decoded, nothing is read back into guest state, and the observer is excluded from every report and
digest. `tests/genesis_execution_coverage_test.cpp` checks the exact set (including interior block PCs),
witnesses, interrupt resumption attribution, zero semantic effect (whole-runtime `memcmp` with and without the
observer) and determinism.

### Frame-bounded headless run

```sh
python3 tools/genesis_startup_bridge.py --rom games/<rom> --mode commercial --immutable-rom-aot \
  --execution-coverage 23200 --coverage-epoch-frames 100 --instruction-budget 1500000000 --out-dir build/cov
```

This builds the unmodified generated program with `-Dgenesis_runtime_run=genesis_execution_coverage_hook_run`
(`platforms/genesis/viewer/execution_coverage*.c`, same seam as `--capture-frames`). It runs exactly FRAMES
published virtual frames with no controller input (the dispatch allowance stays a hard cap) and prints one
`COVERAGE_SUMMARY` line on stderr:

- distinct PC count, retirement and dispatch counts;
- SHA-256 of the coverage bitmap, of the published-frame digest stream, and of the final CPU/RAM/device state;
- per-epoch checkpoints.

`--coverage-disabled` runs the identical loop with no observer, as the zero-effect and overhead baseline.
`--coverage-no-render` counts the genuine virtual frame boundaries without rendering; nothing is published. On
the measured workload it gives the identical coverage and guest state about 10x faster.

The two modes can differ in what "frame" means. In the rendered mode, a frame is a successfully published
frame. If an image's frames cannot be rendered, the rendered mode counts fewer frames than the no-render mode.
After the build, the executable can be re-run directly with the `SEGARECOMP_COVERAGE_*` environment variables
documented in `execution_coverage_main_hook.c`.

## Reachability challenger

```sh
segarecomp genesis-reachability-challenger --rom games/<rom> --reset-entry --rom-sha256 <sha> \
  --private-output build/challenger.json [--exception-model strict|normal-resumption] [--pea-continuations] [--universe]
```

- **Roots.** Only the architecture-owned roots: the reset entry and the handlers of the vectors the
  generated-native machine can raise (IRQ6 and the synchronous vectors).
- **Fixed flow.** From each root the challenger follows only the successors that `m68k_control_successors`
  (`libs/cpu/m68k`, a projection of `m68k_operation_effect`) states: fallthrough, BRA, both Bcc/DBcc outcomes,
  and BSR/JSR/JMP direct targets.
- **Returns.** Each discovered call contributes its continuation. The continuations are resumed once a
  reachable ordinary RTS exists; an ADR 0048 push-then-RTS is a computed jump and does not count.
- **Exception returns.** Under `strict`, RTE/RTR discover nothing. The `normal-resumption` hypothesis
  additionally resumes the stacked continuations of discovered TRAP/TRAPV instructions.
- **Unresolved sites.** Every other runtime-derived PC is recorded by family and not followed.
- **Never used:** linear sweep, broad-AOT identities, runtime coverage, hints or external disassemblers.
- **PC-indexed recovery (SEG-026-T002, ADR 0054, `--pc-index-recovery`, off by default).** A `JMP/JSR (d8,PC,Xn)`
  site is resolved only when its index register's exact finite domain is proven by a demand-driven backward
  evaluation over the challenger's own discovered graph:
  - constants, masks, CMP/TST + Bcc guards, add/sub/shift/EXT transforms;
  - entries read from uniquely owned immutable image bytes.
  A domain bounded only by the width of a byte loaded from mutable memory is `width_only_domain` and stays
  unresolved. `--pc-index-width-domains` admits it as a labelled measurement variant. Out-of-image entries or
  targets fail closed. The exact targets become ordinary discovery roots, iterated to a deterministic fixed point;
  a proof invalidated by a later edge restarts discovery with that site pinned unresolved. stdout gains a
  `pc_index_recovery` aggregate (per-site outcomes, proof mechanisms, graph-growth and accidental-decode
  indicators; never an address).
- **Store provenance of width-only indices (SEG-026-T003, ADR 0055).** This was measured and STOPPED. The experiment
  implementation is not retained, so there is no flag for it.
- **Output.** stdout carries aggregate counts only; `--universe` adds the unchanged broad AOT count `U`.
- **Private output.** `--private-output` holds exact PCs.

## Comparison

```sh
python3 tools/reachability_coverage_compare.py --coverage-dir build/cov/coverage-private \
  --challenger build/challenger.json --coverage-summary <file with the COVERAGE_SUMMARY line>
```

`--classification` takes a private classification of the observed PCs, produced by
`segarecomp genesis-reachability-challenger ... --classify-pcs <pcs> --classify-output <file>`. The same decoder
labels each PC; nothing enters `D`. With it, the tool adds structural attribution: returns are traced to their
observed callers, and each missing PC gets a first gate and a nearest mechanism.

The tool prints aggregates only:

- the set sizes `O`, `D`, `U`, `O ∩ D`, `O - D` and `D - O`, and the recall `|O ∩ D| / |O|`;
- per-checkpoint recall;
- unresolved sites per family, and how many of them executed;
- first-miss attribution: every missing PC is traced through its witness chain to the first transition out
  of `D`, and classified by the control family of that transition's source.

With `--pc-index-recovery`:
- `pc_index_recovery_check` counts observed first entries from a resolved site that fall outside its proven target
  set (escapes, expected 0; later transfers to an already-observed PC are not witnessed), and how many proven
  targets were observed;
- a proven edge is structural in the attribution, and an escape is labelled `pc_index_recovery_escape`;
- with `--classification`, observed PC-indexed sites carry the strict local domain label of the same proof, over a
  predecessor graph built only from the classified instructions (never D). The tool reports those sites, and the
  missing PCs behind them, by label.

Runtime coverage only falsifies the challenger. It never expands or authorizes it, and a PC that was not observed
is not unreachable.

## Privacy

Several outputs are commercial-derived local diagnostics:

- `coverage-private/` (the bitmap and witnesses);
- the challenger's `--private-output`;
- any captured frames.

Keep them under ignored build directories and never commit or persist them. Durable evidence may contain only
the aggregate counts, ratios, digests, frame and dispatch counts, and generic family names printed by the
tools above.

## Z80 execution-PC observation (SEG-031, ADR 0080)

SEG-027-T005 planned this observer for SEG-031's multi-platform comparison; SEG-031-T006 delivers it for the Master System Z80 and the
Genesis materialized Z80 images. It is a falsifier and regression oracle only: Z80 production stays broad AOT, and no Z80 admission
claim depends on it.

- **Hook.** `Z80Runtime` (`libs/codegen/c11/include/segarecomp/codegen/c11/runtime/z80_runtime.h`) carries a non-architectural
  `observe_pc` / `observer` pair, NULL in every program. The call `Z80_OBSERVE_PC` is compiled only with
  `SEGARECOMP_Z80_EXECUTION_COVERAGE`; it fires with the logical PC of every instruction that begins executing: after the boundary
  check (`z80_owner_prologue`) and, for a RAM-backed image, after the live guard accepted it (`z80_live_guard`). An instruction that
  begins always completes, so this is the retired set. Generated Z80 C is unchanged (the header is included, not emitted), and an
  ordinary program never calls the observer.
- **Collector.** `z80_execution_coverage.h` (header-only, machine-neutral) keys every observed PC by `(code-image identity, PC)`, the
  identity being the host's own `code_image` answer at that PC, so banked ROM images and materialized RAM images never conflate. At
  most 256 identities; overflow and identity failures are counted.
- **Master System.** A measurement build (`segarecomp build ... --cc-arg -DSEGARECOMP_Z80_EXECUTION_COVERAGE`) adds
  `--execution-coverage <dir>` to the headless program: `<dir>/z80-coverage.txt` is the private canonical list (identity hex8, PC hex4,
  ascending) and stderr carries one sanitized `Z80_COVERAGE_SUMMARY` (identities, distinct PCs, retirements, unknown identities, a digest
  of the list). The default build rejects the option.
- **Genesis.** The materialization pass of a measurement build (`--keep-work 1`, the same macro) honours
  `SEGARECOMP_Z80_COVERAGE_DIR` beside `SEGARECOMP_MATERIALIZE_DIR`/`_FRAMES`: the full game runs headless with the final image
  registry, keyed by materialized image identity. `pass.report` is unchanged.
- **Tooling.** `tools/z80_execution_coverage.py --platform master-system|genesis` builds, runs twice (deterministic list) and, for the
  Master System, once without the observer (identical machine state digest), and prints sanitized JSON. Broad Z80 AOT is falsified by
  a Z80 error or an unknown identity during the run (every observed PC executed through an exact generated entry otherwise).
- **Tests.** `z80_execution_coverage_test` (hermetic SMS fixture): zero semantic effect, determinism, canonical private list, and the
  default program's rejection of the option.

No first-entry witness is recorded for Z80 (no Z80 admission claim needs attribution). The M68K and Z80 observers share no code: the
generic interface stays unextracted until a second consumer needs the same shape.
