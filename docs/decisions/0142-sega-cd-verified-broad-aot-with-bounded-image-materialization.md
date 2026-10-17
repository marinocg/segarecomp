# ADR 0142: Sega CD verified broad AOT with bounded executable-image materialization

- Status: Accepted (operator-approved prospective amendment of ADR 0130, SEG-064-T002). Mechanism-gate outcome: see "Result" below.
- Later (SEG-064-T003, ADR 0143): BIOS-scale emission (translation units, object reuse) and the main-side boot hardware; first G-A
  evidence - the generated-native authentic main BIOS reproduces the reference's sub epoch image. Decisions 1-8 here are unchanged;
  ADR 0143 decision 9 adds exception entry as a source of the decision-7 RAM frontier.
- Related: ADR 0130 (Amendment 1), ADR 0141 (the historical `NO_GO`, unchanged), ADR 0076 (image authorities), ADR 0073 (bounded
  build-time materialization), ADR 0097 #10 (materialized RAM `JMP` thunks), ADR 0049 (byte-identity guards), ADR 0126-0132
  (Sega CD images, dispatch, scheduling, overlays and module lists).

## Context

ADR 0141 showed that root-driven discovery cannot admit the authentic BIOS (at most 59.5 % of the independently observed
executed BIOS code), while every observed instruction start lowers with the existing MC68000 machinery. The obstacles are not
instruction families but executable-image authority, computed control, transformed RAM code and mutable executable structures:
the main BIOS builds the sub BIOS in PRG RAM before the sub reset release; the sub BIOS has resident and transient regions (the disc
SP later overwrites some of its code) next to writable data; both processors dispatch through RAM `JMP (xxx).L` tables whose operands
change, and the sub table is built after the release.

## Invariants (all mandatory)

1. Every native instruction function is generated before the generated program starts.
2. Every executable image has a defined build-time authority (ADR 0076: `immutable_input`, `static_proof`,
   `bounded_build_time_materialization`).
3. Materialized RAM images come from a bounded, deterministic build-time process with an independently justified capture epoch.
4. Run-time execution verifies the identity of the executable code it uses.
5. Run-time control flow may select among precompiled entries; it never decodes, interprets, compiles or synthesizes guest
   instructions.
6. Mutable operands or jump targets have a statically established instruction shape and an explicit, checked run-time value.
7. Unknown executable bytes, invalid instruction shapes, uncompiled destinations and unverified image transitions fail closed.
8. Main and sub processors keep independent execution state and platform-defined memory views.
9. The synthetic post-BIOS profile (SEG-062/SEG-063) is unaffected: identical generated output for identical inputs and options.
10. Authentic boot and application handoff still require independent execution and hardware evidence (gate G-A).

Broad compilation of every aligned start is a deliberate precompilation strategy for an established image, **not** a claim that
every word is code. Admission (which entries exist), image discovery (where images come from) and run-time selection (which
compiled entry executes) stay separate.

## Decisions

1. **ROM-image profile.** An explicit `--bios <file>` selects the profile (no search, download or embedding). The image is mapped at
   main `$000000` (at most 128 KiB; an address past its end inside the ROM window is the typed `bios_absent`; the contract's 256 KiB
   mirroring applies). The main processor resets from the image's vectors and resolves exception vectors from it. The program
   records the image's SHA-256; the run time refuses a missing, wrong-size or different image before any instruction (`bios_refused`).
2. **Broad admission.** Each image of the profile is admitted by broad AOT: every aligned start whose instruction lies inside the
   image and that the CPU owner decodes, lifts and lowers becomes a compiled entry. Starts it rejects have no entry (a transfer there
   is `entry_not_compiled`). ROM images carry no run-time guard (`verification none`); RAM images carry per-instruction guards.
3. **Materialized sub image (authority `bounded_build_time_materialization`).** Epoch: the sub's first *runnable* transition after a
   reset assertion — `SRES` 0 -> 1 with `SBRQ` = 0, or `SBRQ` 1 -> 0 while `SRES` = 1 and the sub has not run since its reset. In this
   profile the sub's reset exception is taken at that transition, after verification, from the verified PRG RAM: the vector fetch
   is a bus cycle the sub cannot make while `SBRQ` grants the bus to main (tag U for the hardware timing; GPGX fetches at the `SRES`
   write, which differs only when a reset is released with the bus still requested; the synthetic profile keeps the contract's
   section 4 rule). Snapshot: PRG RAM `[0, E)`, `E` the end of the last non-zero byte rounded up to 256 bytes. Producer: the build
   runs the generated program (compiled with the images admitted so far) from reset under constant bounds and no input; the run
   writes the snapshot with the identity of the program that produced it; the emitter admits a snapshot only when that identity
   equals the identity it computes for the same disc, image and earlier admissions, and a confirming run must reproduce the final
   outcome. The identity chain binds each admission to its inputs and position; it does **not** prove that bytes came from a run
   (anyone can compute the identity). Execution authority is decision 4's run-time comparison: an admitted image that is not what
   the program actually produces is never executed.
4. **Epoch verification.** At the epoch the run time compares the live `[0, E')` with every admitted materialized image (length and
   SHA-256). No match is the typed stop `materialized_image_unverified` before the sub retires an instruction; in a build pass it
   instead writes the snapshot.
5. **Verified per-instruction variant dispatch (resident/transient).** A RAM image's code is verified per instruction: before an
   instruction runs, its exact bytes are compared with live memory (`byte_identity` as the executable-image contract defines it).
   Writes to other bytes - mutable data, the vector table, other code - never invalidate it; a write that changes its own bytes makes
   it fail closed. Images may overlap (a transient region replaced by a second admitted image): a PC covered by several images has one
   variant per distinct instruction, deduplicated when bytes are identical. A variant matches only when every one of its bytes is
   live; the opcode word fixes the instruction's length, so two distinct variants can never both match: selection needs no priority
   or heuristic, and an unmatched PC is `code_bytes_mismatch`. A stale return or computed transfer into a replaced region therefore
   executes exactly the instruction whose bytes are live - the hardware's behaviour - or stops. ADR 0132's whole-range overlay
   selection for disc module lists is unchanged; this profile does not use it because its images contain writable data.
6. **Copy images (authority `static_proof`).** A RAM image declared as a verbatim copy of an immutable ROM span (explicit descriptor:
   processor, execution base, ROM offset, length) is admitted after the emitter checks its bytes against the ROM (ADR 0049's rule).
7. **Mutable jump targets.** Case 1 (an immutable instruction reading a mutable destination, e.g. `MOVEA.L` then `JMP (An)`) is ordinary
   CPU-owned indirect control: the destination must be a compiled entry. Case 2 (a known instruction whose encoded operand changes in
   RAM): an **operand template** admitted from a build-pass frontier observation (the pass stopped fail-closed at a RAM PC whose live
   bytes are such an instruction; ADR 0097 #10 generalized), authority `bounded_build_time_materialization`, verification
   `structural`. A build pass proposes a frontier where it stopped at the processor's PC with no compiled variant matching the live
   bytes (a code-byte mismatch at any byte, or the dispatch default); an uncompiled computed or template destination is an ordinary
   typed stop, never an admission. Shapes: `JMP (xxx).W` and `JMP (xxx).L` only. The opcode word is guarded; the operand is read live at execution with
   its fixed width; the destination (sign-extended for `.W`) must be even and a compiled entry of the processor, otherwise
   `entry_not_compiled`; timing is the CPU owner's for that instruction. A template is the last variant tried at its PC. Any other
   shape or a changed opcode word fails closed. No run-time decoder is involved: the template's shape is fixed at build time.
8. **Bounds.** At most 4 materialized images and 64 operand templates per program; at most 1 + 4 + 64 build rounds; constant per-round
   instruction budget and wall timeout; every exhaustion is a typed build failure.

## Gates

**G-M (mechanism, synthetic; this task).** All must hold, each with a test:
all executed instructions run as generated-native functions; all executable images are established before the run; no run-time
decoder or interpreter; every control target resolves to a compiled, verified entry; the transformed image matches its authority at
the epoch; data writes neither invalidate unrelated code nor let modified code run unverified; transient replacement is
deterministic; negative cases stop before retiring an instruction from an invalid image; repeated runs and deadline slicing give
identical observable states. Outcome: `MECHANISM_PROVEN` (all), `MECHANISM_PARTIALLY_PROVEN` (name the exact missing generic
capability), `MECHANISM_NOT_VIABLE` (a conflict with generated-native, bounded or verified execution).

**G-A (authentic GO; later, only after `MECHANISM_PROVEN`).** Complete coverage of the BIOS-derived instructions actually executed
along the independently established boot path, including materialized and operand-template instructions (a coverage estimate is
not enough); independent confirmation that boot behaviour and the application handoff match the reference; no unresolved
control-flow or image-identity stop on the accepted path; the required hardware semantics demonstrated, not stubbed; every BIOS
service used after handoff dispatched through verified compiled entries or justified native services within ADR 0130 point 3.

**Recorded deviations (fail-closed).** An odd jump destination is the typed `entry_not_compiled`, where an MC68000 takes an
address-error exception (as for an indirect `JMP` to an odd address elsewhere in the project). An instruction whose bytes straddle two
images that were partly overwritten matches no variant and stops (`code_bytes_mismatch`), where hardware would execute the mixed
bytes. Variant selection reads live bytes, not the processor's prefetch queue; this matters only for self-modification of the next
prefetched word between two admitted variants.

**Consequences.** G-M not proven: no authentic-profile implementation proceeds; the named missing capability becomes the only
admissible next step. G-A failing: SEG-064 stays open or deferred with the failing criterion recorded; Checkpoint E is never
claimed from mechanism evidence, broad-AOT coverage or a synthetic disc alone.

## Result (SEG-064-T002)

**G-M: `MECHANISM_PROVEN`** on a project-authored 3.5 KiB BIOS-like image (`tools/sega_cd_bios_fixture.py`) and its bounded
materialization fixed point (`tools/sega_cd_bios_build.py`), tested by `tests/sega_cd_bios_profile_test.py`.

| capability | result |
| --- | --- |
| A broad AOT of an immutable image | a routine reached only through a data table runs (computed `JSR (A1)`); interleaved data compiles as starts that are never executed; odd targets, targets past the image, the one decoder-rejected word and the sub's code seen through main's window all stop `entry_not_compiled` at the transfer, before it retires |
| B transformed RAM image | main XOR-transforms the sub program into PRG RAM; the build's first pass captures it at the epoch; the admitted image equals the fixture's own pre-transform bytes (SHA-256, computed by the fixture, not the runtime); at run time the epoch verifies it before the sub's reset exception, on both epoch paths (`SRES` with `SBRQ` = 0; reset armed with the bus requested, then `SBRQ` release) and across a repeated reset with a re-written image (two verified epochs, each observed as a reset entry of the sub); incomplete and corrupted images stop `materialized_image_unverified` with the sub having retired nothing |
| C resident / transient / data | data writes next to resident code never disturb it; a resident instruction rewritten without an admitted image stops; T2 (a declared verbatim ROM copy, `static_proof`) replaces T1 deterministically: identical instructions deduplicated, differing ones selected by live bytes; a stale return into mixed T1/T2 bytes stops `code_bytes_mismatch` |
| D mutable jump targets | case 1 (`MOVEA.L` + `JSR (A2)`) uses ordinary CPU-owned indirect control; case 2: three `JMP (xxx).L` RAM thunks (main, built before the epoch; sub, built after it; sub, already inside the materialized image and first run as an exact variant) are admitted from build-pass frontiers and dispatch to their rewritten destinations; odd and uncompiled destinations stop `entry_not_compiled`; a thunk rewritten to `JSR` stops `code_bytes_mismatch` |
| determinism and bounds | 3 runs and 9 deadline partitions identical; budget exhaustion typed; the fixed point converges in 5 rounds and a confirming run reproduces it |
| independence | the pinned Musashi two-processor reference in ROM mode (interprets bytes, knows no images or selection, round-robin interleaving) agrees on both CPUs' registers and PCs, COMSTAT, all of work RAM and all of PRG RAM, and on COMSTAT and both RAMs for the two epoch variants; it shares the profile's choice of taking the sub reset when the bus is released (tag U), so it does not independently settle that hardware timing |

- No run-time decoding, interpretation, compilation or synthesis was needed; templates use a build-time shape and a fixed-width live
  operand. No BIOS-specific fact entered production code (the image check is size, the documented `SEGA` header, reset vectors).
- Synthetic post-BIOS profile: generated C byte-identical for all 34 fixture cases; the fast gate and focused Sega CD / M68K suites
  pass. Two independent adversarial reviews contested, in turn, the SBRQ-release epoch path (the sub started from vectors read before
  verification), the re-proposal of an admitted template at an uncompiled destination, this ADR's provenance wording, a frontier rule
  that then dropped guard mismatches past an instruction's first byte, and a two-epoch test that did not observe the restart; each was
  corrected and covered by a test in the same PR.

**Cost (measured, `gcc -O1`, one strict-C11 translation unit):** the proof program 2.3 MB of C / 2,427 instruction functions /
about 5 s per round; 32,760 functions 27 MB / 58 s / 1.9 GB peak; a 128 KiB main-only image 65,235 functions 63 MB / 837 s / 4.3 GB
(review measurement). The authentic BIOS needs about 127 K functions and roughly 15 rounds (one epoch plus about a dozen thunks):
scale is a **measured cost issue** for G-A, not a mechanism blocker. The Sega CD emitter has no partitioning; the Genesis build's unit
partitioning is a pattern, not reusable code. G-A needs deterministic address-range translation units with per-unit object reuse
across rounds and several frontier admissions per pass.

**Local BIOS diagnostic (normalized, from the ignored SEG-064-T001 reference artifacts):**
- The sub reset-release epoch image is identical across both discs, six START timings and both cartridge configurations, and occurs
  before any input: its inputs are the BIOS and the hardware it reads.
- A sound authority for it exists only once *our* generated main BIOS runs to the release: that needs the main-side hardware it touches
  first (VDP ports and timing, Z80 bus request and the write-only Z80 upload, I/O, the Gate Array, the PRG-RAM window).
- Resident/transient boundaries need no guesses: decision 5 verifies per instruction; the disc SP overwriting transient BIOS code is
  one more overlapping image (the existing disc boot-image producer).
- Every BIOS-built RAM instruction executed on both observed boot paths is a `JMP (xxx).L` (11 templates on the synthetic disc, 12 on
  the authorized title; zero other RAM-built starts): exactly the proven template shape.
- With broad ROM and epoch admission plus templates, no executed BIOS instruction remains uncovered in instruction terms; the open
  items are hardware semantics (G-A), not code admission.

**Continuation:** technically justified. Smallest next scope: (1) emitter/driver scale (address-range units, per-unit object reuse,
multiple frontiers per pass); (2) the main-side pre-release hardware so that the generated main BIOS reproduces the reference epoch
image under this runtime (the first G-A evidence); then the sub-side boot hardware. Checkpoint E remains unclaimed.
