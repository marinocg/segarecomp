# ADR 0228: SEG-101-T002 - compiled / bound / executed / previously-unvisited accounting

- Status: Accepted. Milestone SEG-101 (M5), task T002. Related: ADR 0223 (binder, `dynamic` report), 0227 (definitions).
- Privacy: image ids are file indexes of the build's inventory and appear only in the run report, which is a private artifact. Committed material carries counts.

## Decision

M4's run report already separated *compiled* (`images`, `compiled_offsets`), *identified* (`distinct_images_bound`), *executed* (`distinct_images_exercised`, `retired`), refusals, replacements and mapping invalidations. It could not say **which** images, nor how many distinct source offsets of them ran, so "an image the build never executed" was not decidable from a report. The minimum added, in the existing `dynamic` section (so every machine-state comparison that ignores `dynamic` is unchanged):

- `exercised`: per image that retired at least one instruction through its precompiled bodies: `{image, retired, offsets}` - instructions retired and distinct even source offsets among them;
- `bound_images`: ids of every image the binder identified at least once.

The count is made **after** a body returned success (`scd_dynamic_dispatch`), read only by the report, never by dispatch, binding or timing. A refused, ambiguous, mismatching or merely identified image is therefore never "executed"; a delivered or loaded file is never touched by it. Source offsets are bit-set per image (allocated on an image's first retired instruction, released at exit). No tracing framework, no per-instruction log.

Definitions that use it (ADR 0227 decisions 4-5): *build-observed* = executed by the input-free route; *previously unvisited* = executed image not in that set; the same image mapped at another base, or re-executed, is one identity.

## Control tests (project-authored discs, `tests/sega_cd_file_first_test.py`)

Module B is compiled before the build route executes module A (four images, one executed by the build route); the build route never loads B; a later route loads it and B retires instructions without a rebuild and is counted as previously unvisited; module C mapped at two bases is one previously unvisited image with two bindings; re-executing the build route's own module is not new; an overwritten module counts only what retired; a program without dynamic images reports no account and equals the static reference's state. The per-image retired counts sum to the dynamic total on every route.

## Gate

`COVERAGE_ACCOUNTING_GO`: the one missing fact (which images) is reported; everything else reused M4's structures.
