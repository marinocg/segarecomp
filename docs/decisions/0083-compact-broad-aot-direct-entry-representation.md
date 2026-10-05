# ADR 0083: Compact Broad-AOT Entry Representation (Direct Helper-Backed Entries)

- Status: Accepted (decision: **ADOPT**, for sharded Genesis output with factored bodies. The previous owner/wrapper form remains
  the representation of every entry that is not helper-backed, of single-file output and of `--legacy-aot-entries`.)
- Date: 2026-10-05
- Task: SEG-036-T001..T007 (one combined delivery).
- Extends: ADR 0045 (exact statically selected body helpers) and ADR 0052 (own-PC helpers). Unchanged: admission, the compiled-address
  set, every lowered statement and helper body, dispatch authority (the sorted compiled-entry table), timing and provenance semantics.
- Related, unchanged and not bypassed: SEG-028 ExecutableImage, SEG-029 analysis core, SEG-030 M68K analysis, SEG-031 hybrid admission,
  SEG-034 / SEG-035 diagnostics (ADR 0080, 0081, 0082). Broad immutable-ROM AOT remains the production completeness mechanism.

## Question

Broad AOT admits about 246k M68K identities for the 512 KiB reference title and emits about 220 MB of C. Can it stay the completeness
mechanism while becoming materially cheaper to generate, compile, store and execute, by removing per-entry generated scaffolding for
entries whose exact semantic implementation is already statically selected? This is a representation question. The compiled-PC set must
not change, and nothing may become an interpreter.

## Current-main census (T001, `generated_code_scalability_report.py`, exact partition)

Sonic 1 (512 KiB), `-O0`, no-hints broad AOT, baseline main `e73a78d`:

| AOT entries (245,694) | entries | bytes |
| --- | ---: | ---: |
| exact-shared-helper | 108,930 | 15.6 MB |
| own-PC-helper | 72,563 | 17.5 MB |
| still-inline (unique body, all helper-eligible) | 64,201 | 93.9 MB |
| other / helper-ineligible (`case`, `static`, `switch`) | 0 | 0 |

Entry-local C that only routes a known PC to an already selected helper (owner `case` row, label, call syntax, own-PC argument):
**30.3 MB = 13.7%** of 220.2 MB. The per-entry provenance compound literal passed to the helper is a further 18.0 MB (8.2%). Together
**48.3 MB = 21.9%**. OutRun (1 MiB): 64.8 MB routing-only = 20.7%, 100.2 MB with provenance = 32.1% of 312 MB. Pre-registered rule: activate
the prototype if the ceiling is at least 15% of generated C or comparably strong executable evidence. Strict routing alone was 13.7% on
Sonic 1 and 20.7% on OutRun; with the provenance argument both were above 15%, and stripping the helper-backed entries from the AOT
translation units cut object text 40% at `-O2` (a throw-away measurement, not shipped). The rule was met on the combined ceiling and on
executable evidence, and the prototype was built.

## Decision

1. A helper-backed AOT entry (one whose generation-time exact or own-PC body identity selected a shared helper) has **no owner `case`,
   no label and no wrapper**. It is one row of static tables in the `entries` translation unit:
   - `genesis_compiled_entry_addresses[]` (sorted compiled PCs, unchanged authority),
   - `genesis_compiled_entry_owner_ids[]`: an id below the legacy-owner count names an owner as before; an id at or above it names
     `id - owner_count` in `genesis_aot_direct_helpers[]`, a table of the statically selected generated helper functions,
   - `genesis_aot_direct_meta[]`: one packed 64-bit word per compiled entry (length bits 0-7, the two primary bytes bits 8-23, image offset
     bits 24-63; zero for an owner-backed row).
2. `genesis_compiled_entry_find` (binary search over the unchanged address table) and `genesis_compiled_entry_invoke` are `static inline`
   functions in the shared header. `invoke` calls either the legacy owner, or the table-selected helper with exactly the arguments the
   legacy wrapper passed: a `GenesisInstructionProvenance` value rebuilt from the packed word (CPU variant constant, source address = the
   entry's own PC, image offset, primary bytes, length) and the entry's own PC. `genesis_compiled_entry_lookup` (membership,
   `== NULL` checks at indirect transfers) keeps its signature and semantics: a direct entry returns a real function that resolves the
   same table row.
3. All helpers of a direct build share one signature `(runtime, provenance, own_pc)`. A parameter the exact body does not use is discarded
   with `(void)`; the body text is unchanged. Inline (unique-body) entries, ordinary blocks, frontier stops and every
   helper-ineligible form keep the owner `switch (runtime->pc)` representation (a hybrid; there were no ineligible entries on the measured titles).
4. The generator proves each packed word: it rebuilds the legacy spelling from the unpacked fields and rejects the whole translation
   unless it equals `instruction_source(operation)`. It also rejects a length over 255, an offset of 2^40 or more, or a source address
   that is not the entry's own address. There is no fallback guess.
5. The table is written with bare suffixed literals (`0x0000D92u`, decimal ids, `0x...ULL`) instead of `UINT*_C` macros: the macro
   form of the table measured 1.75 GB compiler RSS for the 4 MiB entries unit (1.22 GB before the change); bare literals measure
   209 MiB vs 187 MiB (512 KiB title) and 843 MiB vs 872 MiB (4 MiB synthetic) for the same unit.
6. On by default for sharded output with factored bodies. `--legacy-aot-entries` (CLI) / `ImmutableRomAotDirectEntriesScope(false)`
   (API) select the previous form. Single-file output is unchanged.

**What this is not.** Nothing is classified or decoded at run time. A row maps a compiled PC to a helper the generator already selected
from exact generated-text identity; the runtime performs a table lookup and a call. There is no opcode, EA or family switch, no descriptor
interpreter, no JIT, no runtime discovery. The packed word carries only provenance metadata; it is unpacked arithmetically into the
same struct a literal used to spell. The differential tests assert the absence of any `switch` in the invoke path.

## Soundness

- **PC set.** The table authority is unchanged and produced by the same code. Admitted, final compiled, AOT-owned and block-owned
  address-set digests (count + SHA-256) are identical to baseline on all six inputs (below). The AOT-owned set is recomputed from the
  new id space (id at or above the owner count is AOT-owned) and equals the baseline set.
- **Right helper for the right PC.** The helper id is the id of the same body identity the wrapper called. Mutants (a rotation of helper
  ids across direct entries, a shifted packed word, swapped packed words, a wrong own-PC argument) are all detected by the differential.
- **Behaviour.** Every compiled entry of the project fixture is entered under five architectural scenarios (mapped, unmapped, odd address
  registers, supervisor/user, handlers present/absent), single step and 12-step runs, for the factored, unfactored and direct forms. Full
  observable output (transfer, stop class and diagnostic, full stop provenance, access, mapping claims, bus accesses, registers, SR, USP,
  work-RAM digest, checkpoint digest, whole-runtime digest) is byte-identical. Real Sonic 1: the budgeted production route report is
  byte-identical, and a 120-frame execution-coverage run has identical dispatch / retirement counts, coverage digest, frame-stream digest
  and final-state digest.
- **Provenance lifetime.** The legacy pointer referred to a compound literal alive for the call; the new pointer refers to a local alive
  for the call. Nothing retains it.

## Evidence (T004; Apple clang 21, arm64, 12 jobs, per-TU `-O0` compile unless stated; sizes/counts exact, times machine-variable)

Sonic 1 production route, `-O2`, `/usr/bin/time` around the unchanged `genesis_startup_bridge.py` (generation + compile + link + 20 M-dispatch run):

| | before | after |
| --- | ---: | ---: |
| admitted / final compiled / AOT-owned / block-owned identities | 246,293 / 246,293 / 245,694 / 599 | identical (digests equal) |
| generated C | 220.2 MB | 177.9 MB (-19.2%) |
| compile CPU, sum of TUs, `-O0` | 70.1 s | 57.3 s (-18.3%) |
| compile wall, 12 jobs, `-O0` | 5.97 s | 4.94 s (-17.3%) |
| object bytes, `-O0` | 72.9 MB | 59.5 MB (-18.4%) |
| executable, `-O0` | 64.9 MB | 54.9 MB (-15.5%) |
| executable, `-O2` | 33.7 MB | 25.1 MB (-25.3%) |
| full route wall / CPU, `-O2` | 41.5 s / 134 s | 35.2 s / 111 s (-15% / -17%) |
| generation wall / peak RSS | 4.2 s / 519 MiB | 3.5 s / 619 MiB (+19% RSS) |
| largest compiler RSS | 187 MiB | 211 MiB (+13%) |
| dispatch-bound benchmark, 40 M instructions, `-O2` (min / median of 9) | 4.349 / 4.368 s | 4.579 / 4.616 s (+5.3% / +5.7%) |
| 120-frame coverage run, `-O2` | 0.95-0.96 s | 0.87-0.90 s |

Other inputs (`-O0`):

| input | generated C | compile CPU | object | executable | compiler RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| OutRun (1 MiB) | 312 -> 217 MB (-30.4%) | 85.0 -> 64.6 s (-24.0%) | -28.3% | 87.6 -> 65.6 MB (-25.1%) | 324 -> 301 MiB |
| Cool Spot (1 MiB) | 362 -> 273 MB (-24.5%) | 91.6 -> 79.0 s (-13.8%) | -22.8% | 103.4 -> 83.6 MB (-19.2%) | 330 -> 307 MiB |
| Sonic 2 (1 MiB) | 393 -> 303 MB (-22.9%) | 103.8 -> 89.7 s (-13.6%) | -22.1% | 114.2 -> 93.1 MB (-18.5%) | 331 -> 369 MiB |
| synthetic 2 MiB | 647 -> 461 MB (-28.9%) | 182 -> 137 s (-24.9%) | -26.9% | 183 -> 140 MB (-23.8%) | 605 -> 556 MiB |
| synthetic 4 MiB | 1.35 -> 0.97 GB (-27.9%) | 365 -> 264 s (-27.5%) | n/a | n/a | 802 -> 889 MiB |

The 4 MiB synthetic image does not link with this Apple clang for either form (an existing compiler crash on a few translation units,
identical before and after); only its generated-C and per-TU compile numbers are comparable. The set digests are equal on every input.
Generation peak RSS changes by -5% to +20% (the binding rows are held until the table is written).

Pre-registered decision criteria were generated C -20..25% **or** executable -15% (preferably both), runtime regression at most 10%
(preferably 5%). Executable: -15.5% to -25.3% on every linkable input (met). Generated C: -19.2% to -30.4% (met on five of six inputs, 19.2%
on Sonic 1). Runtime: +5.3% / +5.7% on the worst case measured (a pure dispatch loop), with a real frame workload faster; the
preference of 5% is narrowly missed, the 10% limit holds.

## Alternatives measured or rejected

- A first table shape stored 24-byte `GenesisInstructionProvenance` rows (macro initializers). Source -16.3%, executable -15.5%, but the
  entries unit needed 475 MB of compiler RSS (+154%). The packed word and bare literals fixed that and are the shipped shape.
- `find` / `invoke` as ordinary functions in the entries unit instead of `static inline` in the header: runtime +5.7% before, +5.3%
  after; kept the inline form (also removes an indirection from the dispatcher).
- Parameterizing further literal classes was not attempted (SEG-025 showed each is small).

## T005 (second representation refinement): cancelled

After T002 the largest representation-only remainders on Sonic 1 are the provenance setup of inline entries (8.6 MB = 4.8%), the compiled
address and id tables (5.6 MB = 3.1%, already compact), owner dispatch (4.0 MB = 2.3%) and mapping metadata (4.1 MB = 2.3%). The 92 MB
of bodies and the 23 MB of per-body retirement tail (13%, a source-only macro candidate that would not shrink object code) are not entry
representation. Pre-registered rule: one experiment only if a single remaining representation category is clearly dominant (8% or
more removable without semantic parameterization). None is, so T005 was cancelled and no second pass was made.

## Consequences

- Broad AOT remains the completeness mechanism and is now materially cheaper to build and ship; the 4 MiB class is still about 1 GB of C.
- The remaining cost is dominated by the lowered instruction bodies themselves (about half of the C). Further reduction needs fewer
  admitted identities, which is the SEG-031 / SEG-037 direction, not representation.
- Follow-up candidates, deliberately not done: converting unique-body inline entries to helpers (about 4% of C), a header-level
  retirement-tail function (about 8% of C, source only), and a generated-text-free route for provenance.
