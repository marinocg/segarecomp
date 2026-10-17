# ADR 0142: Sega CD verified broad AOT with bounded executable-image materialization

- Status: Accepted (operator-approved prospective amendment of ADR 0130, SEG-064-T002). Mechanism-gate outcome: see "Result" below.
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
   reset assertion — `SRES` 0 -> 1 with `SBRQ` = 0, or `SBRQ` 1 -> 0 while `SRES` = 1 and the sub has not run since its reset. This is
   the hardware event after which the sub fetches its reset vectors from PRG RAM. Snapshot: PRG RAM `[0, E)`, `E` the end of the last
   non-zero byte rounded up to 256 bytes. Producer: the build runs the generated program (compiled with the images admitted so far)
   from reset under constant bounds; the run writes the snapshot with the identity of the program that produced it. The emitter
   accepts a snapshot only when that identity equals the identity it computes for the same disc, image and earlier admissions
   (a provenance chain, not an arbitrary memory dump). A confirming run must reproduce the final outcome.
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
   `structural`. Shapes: `JMP (xxx).W` and `JMP (xxx).L` only. The opcode word is guarded; the operand is read live at execution with
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

**Consequences.** G-M not proven: no authentic-profile implementation proceeds; the named missing capability becomes the only
admissible next step. G-A failing: SEG-064 stays open or deferred with the failing criterion recorded; Checkpoint E is never
claimed from mechanism evidence, broad-AOT coverage or a synthetic disc alone.

## Result

Pending (SEG-064-T002).
