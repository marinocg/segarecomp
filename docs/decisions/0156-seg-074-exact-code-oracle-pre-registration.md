# ADR 0156: SEG-074-T001 exact-code oracle and compilation-cost floor - pre-registration

- Status: **Pre-registration, frozen.** Committed and pushed on the SEG-074-T001 branch before any oracle admission plan is compiled or
  measured on a corpus image. The measured results and the gate decision go in a later ADR (0157). This record is not edited afterwards,
  except to fix typos that don't change meaning.
- Scope: **offline research only.** The tooling is a report-only least-closed-superset producer (`--region-proposal-closure close`), an
  offline benchmark tool and synthetic tests. It changes nothing in production: production M3 + B35 (`gen2`), Compatibility (broad AOT) as the
  default and fallback, the structural prune, the admission validator, the emitter, the runtime, the launchers and Sega CD admission all stay as
  they are. The oracles are diagnostic. They do not show that the executable code of an arbitrary image can be completely identified, and they
  are not production classifiers.
- Related: ADR 0080 (hybrid admission plan), 0094 (region prune), 0103 (gen2 M3 + B35 route), 0111 (alias rule), 0151-0154 (SEG-072),
  0155 (SEG-073 H5). Nothing in 0151-0155 is reinterpreted, and the SEG-072 `sealed` command is not run.

## Question

If segarecomp knew exactly which MC68000 instruction starts are executable, how much generated C and compilation work could it eliminate?
Is today's inefficiency mostly:

- **A.** ML ranking error;
- **B.** coarse 128-byte admission granularity;
- **C.** structural closure and unnecessary candidate admission;
- **D.** the representation of the generated C and the compiler's behaviour on it;
- **E.** a combination of these?

## Strategies (five; each one is a hybrid admission plan, or broad)

| id | strategy | how the admitted set is produced |
|---|---|---|
| `BROAD` | broad immutable-ROM AOT (Compatibility) | no plan |
| `INC` | incumbent production M3 + B35, **as deployed** | the unchanged native route (`--immutable-rom-aot-ml-gen2-admission`, bridge `--ml-gen2-admission`). When its validator rejects, the deployed build is the broad fallback, and the cost counted is the broad build plus the cost of the rejected attempt |
| `M3RB50` | saved research pair M3-R + B50 | stored out-of-fold development regions (`results/table.M3_R.json`, policy B50: top-50% cells ∪ seed cells), then the unchanged prune + validator. On rejection the deployed build is broad plus the attempt (accounted as for `INC`) |
| `ORACLE_REGION` | oracle 128-byte region admission | every 128-byte cell that is **not** proven pure data (a cell holding any `I`, `c` or `U` byte), ∪ the production seed cells, then the unchanged downward prune + validator |
| `ORACLE_INSTR` | oracle instruction-start admission | the proposal is every even byte classed `I` (labelled start) or `U` (unknown or masked: never assumed non-executable), as 2-byte ranges. It is closed **upward** (`--region-proposal-closure close`) to the least superset that holds every machine root, every ADR 0049 materialized identity, and every fixed successor and call continuation that is itself a broad identity, and the unchanged validator must then accept it |

Notes:

- `ORACLE_REGION` and `ORACLE_INSTR` use source-backed truth, so they are optimistic floors, not producers. Neither treats a missing label as
  proof of data: unknown bytes count as possibly executable. Neither adds an address outside the CPU-owned broad universe `U`, and neither
  follows a dynamic transfer.
- The two oracles count separately:
  - the labelled starts and unknown positions inside `U` (`k0`);
  - the identities that the execution model demands on top of those (`added` and `roots_added`: structural closure).
- An oracle plan that the unchanged validator rejects is recorded as **rejected**, with its sanitized reason. The validator is never weakened.
  A rejected oracle still has a count-only floor, kept apart from the runnable result.
- `ORACLE_REGION` is pruned downward, exactly as production would treat any region. If the prune removes labelled starts, the C-outside-K count
  records it.
- `INC` keeps the actual deployed production model. On Sonic 1, Flicky, Shining Force, Phantasy Star II and Sonic & Knuckles it is in-sample:
  these titles trained the production M3. That bias favours the incumbent, so it shrinks the measured headroom and makes the G1 test
  conservative.

## Workloads

**Gate set (fixed): every development Genesis image whose broad AOT is accepted, weighted per lineage.** The images are:

| lineage | images |
|---|---|
| D01 | Sonic 1, Sonic 2, Sonic & Knuckles |
| D02 | Flicky |
| D03 | Shining Force, Shining Force II |
| D04 | Phantasy Star II |
| D05 | independently authored SGDK program |
| D06 | independently authored SGDK program |
| D07 | 240p Test Suite |
| D09 | independently authored SGDK program |

That is 11 images in 8 lineages. A lineage's value is the mean over its images, and every gate aggregate is the mean over the 8 lineages.

Outside the gate set:

- D10, D11 and D12 keep their typed broad status (`effective_address_not_24bit`). Nothing is measured for them.
- The full qualified development corpus (50 images, including the 36 verified Sonic CD application modules) gets **count-only** density
  statistics:
  - labelled starts;
  - candidate positions that decode as ordinary instructions;
  - positive, masked and negative cells;
  - the oracle region size;
  - the oracle instruction-start size.
- Sonic CD modules have no region-admission path (ADR 0131/0133), so they get no native compilation measurement. This is reported, not built.
- Sega CD authentic BIOS / materialized sub-BIOS images are a cost **inventory** from existing records only. No game-module oracle claim
  applies to them.

## Measurement protocol (identical for every strategy and image)

1. **Plan.** Run the Release CLI built from this branch and time it: wall time, child CPU and peak RSS from `wait4`. This step does the ML
   inference (`INC`) or the plan production (prune or close).
2. **Emission.** `emit-general-startup-bridge-c --immutable-rom-aot [plan]`, written to a sharded TU directory, timed the same way.
   Recorded:
   - generated C bytes;
   - TU count;
   - admitted AOT identities and the emitter's enumeration counts (from its stderr accounting);
   - from the generated C:
     - per-identity entry segments (`genesis_aot_entry_*:` labels);
     - AOT owner functions;
     - `case` dispatch entries;
     - ordinary block functions.
3. **Cold compile.** Every TU plus the unchanged runtime, compiled with the bridge's flags and the `optimized` profile:
   `-std=c11 -Wall -Wextra -Werror -pedantic -O2`.
   - Host compiler: clang 18.1.3.
   - 4 concurrent jobs (the bridge default), object cache disabled, and one image at a time on an otherwise idle host.
   - Recorded:
     - per-process user + system CPU and peak RSS;
     - the sum of compile CPU;
     - the maximum per-process RSS (peak compiler memory);
     - compile wall time;
     - object bytes.
   - The compiled TUs are linked; link CPU/wall and executable bytes are recorded.
4. **Cache-assisted rebuild.** A second build of the same strategy in the same directory, with the bridge's production content-addressed
   object cache (`compile_objects`), first cold-populated and then warm. Recorded: wall time, hits and misses. Cross-strategy reuse is reported
   as a property of the cache key: the key includes the TU path and the preprocessed content.
5. **Bounded execution.** `tools/genesis_startup_bridge.py` (explicit instruction budget = max(10,000,000, frames x 100,000); 120 frames;
   `quick` profile; wall timeout). The outcome, frames published, final state, coverage and frame-stream digests, and the executed PCs that lie
   outside broad's executed PCs are compared with `BROAD`. Short-trace equivalence is reported as such and never as static recall.

**Primary cost:** total cold build CPU seconds = plan CPU + emission CPU + compile CPU + link CPU. For a deployed fallback, the attempt's
plan CPU is added to the broad build.

**Secondary cost:** generated C bytes.

Also reported, but not gating:

- wall times;
- object and executable bytes;
- peak compiler RSS;
- cache-assisted wall;
- C outside K (labelled starts that are not admitted).

## Gate (fixed before measurement)

For each lineage L, the ratio `X/Y` is the lineage mean, over the lineage's images, of the image ratio of strategy X's primary cost to
strategy Y's. The headline figures are means over the 8 lineages:

- `g = 1 - mean_L(ORACLE_INSTR/ORACLE_REGION)` (primary cost);
- `g_c = 1 - mean_L(ORACLE_INSTR/ORACLE_REGION)` (generated C bytes);
- `h = 1 - mean_L(ORACLE_INSTR/INC)` (primary cost; INC as deployed, including fallback).

**G1 - instruction-level admission has meaningful potential.** All of the following must hold:

1. `g >= 0.20` and `g_c >= 0.20`;
2. `h >= 0.30`;
3. the per-lineage instruction-vs-region gain is at least 0.10 on at least 5 of the 8 lineages (no single favourable lineage carries the
   result);
4. the unchanged validator accepts `ORACLE_INSTR` on at least 6 of the 8 lineages (all images of the lineage).

**Correctness blocker (overrides G1).** Suppose an accepted `ORACLE_INSTR` (or `ORACLE_REGION`) build differs from `BROAD` in its bounded
execution outcome, frames, final state, coverage or frame-stream digest, or executes a PC outside broad's executed PCs. Then the difference is
investigated and reported. G1 cannot be selected on a build that is not runtime-equivalent, unless the difference is shown to come from
non-admission of code that executes at runtime but has no source-backed label, which is a label finding. A fail-closed typed stop on an
unadmitted dynamic PC is such a difference.

**G2 - region admission is already close to instruction-level cost.** Not G1, and the measured **representation headroom** `rho >= 0.20`:

- `rho` is measured on the `ORACLE_REGION` build: lineage mean, otherwise the `INC` build.
- It is the share of the build's total generated C bytes that lie in per-identity entry segments whose text exactly duplicates an earlier
  segment of the same build. A segment runs from one `genesis_aot_entry_*:` label to the next label or the end of its owner function. Before
  comparing, its own label, every hexadecimal literal and every decimal literal of 4 or more digits are normalized. The emitter has no
  per-identity functions: identities are labelled segments inside owner functions with `case` dispatch.
- That share is a direct estimate of what PC-parameterized function sharing or factoring could remove.

T002 is then refined toward generated-C factoring, function sharing or compilation-pipeline efficiency, not a new admission layer.

**G3 - neither has sufficient headroom.** Neither G1 nor G2 holds. A supported negative result is recorded, and T002-T005 are deferred.
No more complex model is invented.

The thresholds are absolute and are applied unchanged. If a workload cannot be measured, its typed reason is recorded and its lineage
drops out of the denominator only when every image of that lineage is unmeasurable. The rest of the protocol is unchanged.

## Fairness and privacy

- Every strategy for an image uses the same Release CLI binary, compiler, flags, job count and host, built back to back. Wall time is
  descriptive. CPU time is the gate metric, because it is robust to scheduling.
- Durable results hold only aggregates, identities/digests and classes: no address, cell index, per-start truth, generated commercial C or
  trace. The work directory must be an ignored path, and the tool refuses any other.
- Synthetic fixtures with exact instruction maps (project-authored bytes) are the only committed executable inputs.
