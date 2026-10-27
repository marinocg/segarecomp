# ADR 0205: SEG-078-T003 - reference-independent disc-extent discovery and the self-service Sega CD build

- Status: Pre-registration accepted; result and decision are recorded in the "Result" and "Decision" sections below when the evaluation is complete.
- Related: ADR 0142 (verified broad AOT, executable-image authority, build-time materialization), 0144/0147 (sub boot, backup RAM), 0176 (Word RAM
  extents), 0179 (static epoch reconstruction), 0180 (batched template admission), 0204 (interactive viewer and launcher).
- Privacy: no BIOS or disc byte, hash, address, extent coordinate or digest of commercial content appears here or in any test; the matrix is
  project-authored. The local commercial evaluation reports classes and counts only.

## Context

Playing a Sega CD game needs executable-image authority for every region the game loads from the disc into RAM (the boot IP and SP, later modules).
Today those regions are `--disc-image` extents given to the build driver, and for the checked commercial title they were found by a local,
content-aware analysis against a private reference emulator. A user who owns a BIOS and a disc has no such reference. SEG-078 delivered an import path
for verified prebuilt programs; T003 asks whether the product can find the extents itself, and expose a supported build.

## Constraints (from the operator and from ADR 0142)

- No external emulator, pre-recorded commercial trace, game-specific heuristic or commercial recipe in product code.
- A RAM snapshot or a byte-pattern match alone is NOT authority. Unknown executable bytes fail closed.
- No generic taint tracking, symbolic execution, interpreter or universal provenance framework unless a minimal experiment shows it is necessary.
- Existing run-time guards stay: every instruction is compared with the live bytes before it runs (ADR 0142 decision 5), so an extent can never make
  the program execute anything but those exact bytes; a wrong extent can only make it stop.

## The mechanism under test (fixed before the matrix was run)

1. **Frontier.** A build pass (`--materialize-dir`) already stops fail-closed at a RAM program counter with no compiled variant matching the live bytes
   (`entry_not_compiled`, or `code_bytes_mismatch` at an address inside an earlier extent). That stop is the question "where did these bytes come from?".
2. **Evidence the machine produces, not a person.** (a) the live bytes of the processor's executable RAM window around the program counter (main work
   RAM, the 2M Word RAM view; sub PRG RAM, the 2M Word RAM view), clipped to what the processor sees; (b) the *delivery ledger*: the disc blocks the CDC
   decoder has read into its buffer so far, as merged LBA ranges (capacity 256; an overflow makes the evidence unusable).
3. **Resolution (`segarecomp sega-cd resolve-source`).** Only delivered blocks of track 1 are searched. A 32-byte anchor at the program counter is looked
   up; each occurrence is extended both ways while the live window equals the delivered data. The longest run wins; a tie between distinct disc offsets is
   `ambiguous_source`; a run under 64 bytes is `too_short`; no occurrence is `no_source`; more than 64 occurrences is `ambiguous_source`. The extent is the
   run trimmed to an even base and length.
4. **Admission.** The extent becomes an ordered admission `extent:<pass-identity>:<cpu>:<offset>:<base>:<length>` in the existing provenance chain (the
   identity of the program whose pass reached the frontier), re-read from the disc and re-validated by the emitter exactly like a `--disc-image` extent.
5. **Termination.** A refusal ends the build at that frontier: the program is built and stops there with its typed stop; the refusal class is part
   of the build summary. Nothing is guessed. At most 16 extents (manual and discovered together), one round per extent.

What this does **not** establish, stated up front: it does not prove that the program copied those disc bytes (a causal copy path is not traced); it
proves that the bytes it is about to execute are an exact, sufficiently long and unique copy of blocks the machine itself read from the user's disc.
That is the same justification the existing `copy-image` rule accepts (a byte-checked copy of an immutable source), with the declaring party replaced by
device-owned evidence. A window assembled or transformed at run time (decompression, decryption, generated code, relocation) is not an exact copy and is
refused.

## Pre-registered synthetic matrix and decision criteria

Project-authored BIOS-like image and disc (`tools/sega_cd_bios_loader_fixture.py`); the sub reads modules through the CDD/CDC (decoder, DMA into 2M Word
RAM) and runs or alters them. Expected outcomes, written before the first run of the matrix:

| Case | Expected discovery | Expected program |
| --- | --- | --- |
| `direct` (a module DMA'd to Word RAM and run) | one extent equal to the module's span | runs to the end, marker written |
| `siblings` (P and adjacent Q share a 1,500-byte prefix; Q is run) | one extent explaining the window (P+Q), not the shorter sibling | runs, Q's marker |
| `duplicates` (two identical disc copies, both delivered, the first run) | refusal `ambiguous_source` | stops `entry_not_compiled` at the module |
| `overlay` (M1 run, then M2 loaded over it and run) | two extents at one base | runs, both markers |
| `late_flip` (one byte far inside the code flipped before the run) | the exact-copy prefix as an extent, then refusal `no_source` at the flipped instruction | stops, never executes the flipped byte as compiled |
| `generated` (code written by immediate moves) | refusal `no_source` | stops |
| `transformed` (XOR-encoded module decoded in place) | refusal `no_source` | stops |
| `undelivered` (the code is also on the disc but the disc blocks were never read) | refusal (no delivered source) | stops |

Plus: determinism (the same case built twice gives byte-identical summaries and program identity); the resolver refuses incomplete evidence (ledger
overflow, empty ledger, window smaller than an anchor, a run shorter than 64 bytes); a hand-edited `extent:` admission with the wrong pass identity is
`provenance_mismatch`, with bytes that are not the disc's or outside the processor's RAM is refused.

- **GO**: every row behaves as expected AND the mechanism reaches a useful bounded build on an independently fixed route of authorized local commercial
  inputs without any reference (the reached region and the first refusal reported as classes/counts).
- **PARTIAL**: every synthetic row behaves as expected, but the commercial route stops early at a refusal class the mechanism cannot resolve: implement
  and ship exactly the synthetic-proven subset and say so; no general-compatibility claim.
- **BLOCKED**: a synthetic row fails soundness (a guessed or wrong extent is admitted), or the subset is not useful: keep the prebuilt workflow and
  publish the blocker.

(Chronology, for the record: the resolver, the loader fixture and the driver hook were written together; the cases and their expected outcomes above
were fixed in the fixture's description before its first run, and the first run matched every row. The local commercial evaluation had not been
run when this section was committed.)

## Result

**Synthetic matrix** (`tests/sega_cd_extent_discovery_test.py`, 26 checks, run in CI): every row behaved as pre-registered. `direct`, `siblings` and `overlay`
discovered extents equal to the disc spans actually loaded (the sibling case admitted the longer explanation, the overlay case two extents at one base)
and the programs ran to their end writing every marker; `duplicates` was refused `ambiguous_source` and the module never ran; `late_flip` admitted only
the exact-copy prefix and stopped at or before the flipped instruction (`no_source`); `generated`, `transformed` and `undelivered` admitted nothing and
stopped (`no_source`). Two builds of one case gave identical extents, admissions, rounds and program identity. The resolver refused a ledger overflow, an
empty ledger, a window shorter than an anchor, a PC outside the window, an explanation shorter than 64 bytes, and bytes whose only matches were *not*
delivered blocks; the emitter refused an `extent:` admission with a wrong pass identity (`provenance_mismatch`), one outside the processor's RAM and one
beyond the track. No guessed or wrong extent was ever admitted. The product CLI (`segarecomp sega-cd build`, ported from the driver) matches the driver
(extents, frontier, program identity) on the same fixtures (`tests/sega_cd_native_build_test.py`).

**Authorized local commercial title (one, classes and counts only; nothing derived from it is committed).** Neither route used a manual extent, an epoch
recipe, a trace or any reference; both consumed only the user's BIOS image and disc (and a formatted empty backup RAM). Chronology: route B was run first
and route A afterwards because the launcher supplies no controller input; neither route was in the pre-registration above.

| Route | Rounds | Extents | Other admissions | Resolver refusals | End of the program |
| --- | --- | --- | --- | --- | --- |
| A - the launcher route (no input, no deadline) | 17 | 12 | 1 epoch image, 3 jump-slot templates | none | the documented audio boundary (`pcm_unsupported`) after 3,371 vertical-blank onsets, i.e. past the 2,802 onsets of the earlier developer-procedure route |
| B - the timed input schedule and deadline of the earlier developer procedure (machine inputs, not recipes) | 14 | 10 | 1 epoch image, 2 templates | none | an uncompiled *computed destination* inside the span of an admitted extent (`entry_not_compiled`), after 2,156 onsets (77% of the route's deadline) |

End to end on route A through the launcher's own code (`Segarecomp --sega-cd --bios ... --cue ... --build`, bundled-compiler stand-in, Release product CLI): added
with no program, built (17 rounds), the built viewer program verified against the BIOS and the disc files, Ready to play, and run in the SDL3 viewer (dummy
video driver, scripted close after 900 frames): 899 frames presented, clean close, no input events.

Every round of both routes was resolved by the mechanism (the disc spans were found by byte-exact match against delivered blocks, never ambiguous, never
refused); the two stops are not extent refusals.

## Decision: GO, for a stated scope

`GO` by the criteria above: every synthetic row behaved as expected and the mechanism built a useful bounded program for a commercial title without any
reference, on the route the launcher actually runs. The scope of what is claimed is exactly that: **the build discovers by itself the disc spans that the
console loads into RAM and executes, and compiles the path the console takes from reset without controller input, up to the first code that is not
provably on the disc or the first documented non-goal.** It is not a general CUE-to-play claim. Implemented and shipped with this decision:

- the runtime delivery ledger and region capture (build pass only), `segarecomp sega-cd resolve-source`, ordered `extent:` admissions (the same authority and
  limits as `--disc-image`, sharing its 16-extent bound), the Python driver's `--discover-extents`;
- the product `segarecomp sega-cd build` (bounded rounds, content-addressed object cache, parallel compile, `@stage`/`@round`/`@result` progress,
  `status.json` with a typed frontier refusal, `--viewer` variant) and its packaging (runtime *sources* ship; no BIOS, disc or program ever does);
- the launcher's Build / Cancel / progress and the headless `--build`, verifying the built program exactly like an imported one; the prebuilt import path
  stays.

## What is still not available (the next obstacles)

1. **Computed destinations inside proven extents** (route B's stop): an indirect jump/return to an address inside an admitted extent that no static entry
   reaches stops with an ordinary `entry_not_compiled`. The smallest next step is to capture such a destination in the build pass and admit it as an entry of
   the extent that already proves its bytes (the extent supplies the authority; the destination is only a new entry).
2. **Input-driven coverage**: the build presses nothing, so code behind a START press is covered only if a developer supplies a pad schedule
   (`--input-script`, a machine input of the developer driver, not exposed in the launcher).
3. **Audio** (PCM, CD-DA, the Z80): route A ends there. Out of scope for this task.
4. Code that is generated, decompressed or transformed at run time is refused (`no_source`) by design; a generic answer would need the immutable
   generated-data ownership evaluated in `AGENTS.md`, not byte matching.
