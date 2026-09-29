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
