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
