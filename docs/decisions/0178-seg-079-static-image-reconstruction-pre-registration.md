# ADR 0178: SEG-079 - Sega CD static image reconstruction and one-shot compilation: frozen experimental plan

- Status: Accepted (SEG-079-T001, checkpoint C1). Frozen before any measurement of the authentic sub-BIOS epoch image, the mutable
  templates or a recipe-assisted build. It may only be made stricter later; any other change is recorded as a deviation in the result ADR.
- Later: ADR 0179 reports the result (deviations listed there).
- Scope: how the hypothesis is tested and what each outcome means. It claims nothing about the authentic images.
- Privacy: no BIOS, disc or snapshot byte, hash, offset, destination address, template address or trace appears in this or the result
  ADR. Private intermediates stay in the ignored `.cache/` of the base checkout.

## Context

ADR 0142-0145 admit two kinds of build-time-discovered executable authority in the BIOS-image profile: one materialized sub-CPU runnable
epoch image (ADR 0142 decisions 3-4) and `JMP (xxx).W/.L` operand templates (decision 7), twelve for the authorized title (two main,
ten sub). A build discovers them one per pass (decision 8: 1 + epochs + templates rounds; 16 rounds plus a confirming run for the
latest candidate). Hypothesis H: the epoch image and the template locations are expressible as a deterministic recipe over the user's
BIOS and disc, so a first build needs no sequential discovery.

Fixed inputs: the Sega-CD Model 1 BIOS V1.10 (U) image (the one ADR 0148-0177 use), the Sonic CD (USA) disc, the private snapshot
`epoch0.bin` produced by the established workflow (`tools/sega_cd_bios_build.py`; already reproduced against the independent reference
in ADR 0143), and the existing qualified boot configurations (the private build workflows with different cartridge/disc/START timings;
ADR 0142 "Local BIOS diagnostic" states the image is identical across them).

## Pre-registered method

**Phase A (provenance).** Tool `tools/sega_cd_epoch_provenance.py`, synthetic-tested, bounded and deterministic.
1. *Source search.* Index every aligned-or-unaligned 16-byte window of each immutable input (the BIOS image; for a disc, only the
   extents the existing build already declares) and extend exact matches to maximal runs. Region = maximal run of epoch bytes explained by
   one (source, contiguous source range). Runs of one repeated byte value (zero/`$FF`) and runs below 32 bytes are *never* classified as a
   copy by equality alone; they are initialised/generated or unknown until a writer explains them.
2. *Writer evidence (required to call a region a copy or transform).* Run the generated main BIOS from reset to the epoch with a
   write log of PRG-RAM stores (writer PC, destination, width, value) and of reads of the BIOS image window, in order; both are
   diagnostics outside the program identity, kept private. A region is a **source copy** only if its bytes are written by one bounded loop
   whose written values equal values read from the matching source range at a consistent affine mapping (dest - source constant per
   segment) with no intervening modification. Equal bytes without that writer pairing are **coincidental/unattributed** (class 6).
   The writer's code is inspected only for its class (loop, width, stride, whether a data-dependent step such as a decode, decompress,
   XOR or addition sits between read and write).
3. *Classes.* 1 verbatim copy; 2 reordered/segmented copy (same source bytes, more than one segment or non-monotonic placement);
   3 relocated/patched copy (a copy with a deterministic per-byte or per-word modification, patch list, or address fix-up);
   4 deterministically transformed (a decode/decompress/XOR/arithmetic rule with an independent re-implementation that reproduces the
   region from the source); 5 initialised/generated (constants, zero fill, values from hardware reads or computed from run state);
   6 unknown provenance. Executable bytes are separated from data by whether an executed sub instruction start (from the existing
   reference trace summary) lies in the region; the reconstruction target is the **entire** epoch image (the contract's whole-epoch
   verification, length and SHA-256), not only code.
4. *Cross-configuration validity.* The classification is repeated on the snapshots of every qualified configuration that exists
   privately; a mapping is valid only if identical across them (or the variation is itself explained by an input difference).
5. *Output.* Sanitized table: class, region count, total bytes, % of snapshot, independently supported (writer evidence **and** an
   independent reproduction), unresolved dynamic behaviour. No offsets.

**Phase B (reconstruction).** Representation preference exactly as the task states (contiguous slice; ordered slices; slices +
explicit initialisation; one independently justified transform; else stop). Reconstruct from BIOS (+ disc if needed) and the recipe only,
without executing the BIOS; require equal length, bytes, SHA-256 and executable-image interpretation, repeated-run determinism, and the
same recipe on every qualified configuration. The prototype and its committed tests use project-authored synthetic inputs only.
*Stop rule:* if any class 4/5/6 region is unresolved by a bounded rule, record exactly which and stop; no arbitrary guest execution, VM
or transformation framework.

**Phase C (templates).** For each of the twelve admitted templates, using a local analysis of the admission sequence and the same
write log: record the processor, whether its opcode word is written by an executed *immutable* instruction (BIOS ROM, or an
immutable disc extent) with a constant source (class **S**, statically justifiable: writer and destination derivable from immutable
instructions by a bounded analysis), is present in the reconstructed epoch image (**E**, justifiable only after reconstructing the
epoch), is written by a copy loop of a verified reconstructed image (**E**), is written by arbitrary run-time-dependent code (**R**,
dependent on runtime observation), or cannot be explained (**U**). A previously observed PC is never authority by itself. Only counts
by class are reported.

**Phase D (schema).** Minimal records only for what the evidence supports; reject unknown fields, overlap, bad bounds, wrong
input identity; no bytes; no host paths. The distributability gate is evaluated against `docs/testing/commercial-games.md` with the
five questions of the task; no policy is widened. Default for any real commercial-derived recipe: **private, locally regenerated**.

**Phase E (one-shot).** Attempted only if Phase B reconstructs the full epoch (byte-identical) *and* Phase C classifies every template S
or E. It requires a verified static producer in the executable-image authority contract (no fabricated pass identities); if that
refactor is more than small it is documented as design plus a successor and not implemented.

**Baselines (recorded where available):** conventional build (rounds, entries, C size, CPU/wall, units rebuilt/reused, peak memory,
program identity, epoch equality, outcome digests, byte-guard/typed-stop behaviour) from the existing private fixed-point reports;
Checkpoint H remains `TIME_ALIGNED`/strict-equivalence failing as recorded and is not altered.

## Decision rules (pre-registered)

Epoch image: `FULLY_RECONSTRUCTABLE` iff 100 % of bytes are in classes 1-5 *with writer evidence and independent reproduction* and the
recipe reproduces the SHA-256 on every qualified configuration without executing the BIOS; `PARTIAL` iff a nonzero proper part is
(quantified; cause classified); `NOT_RECONSTRUCTABLE` iff no bounded rule explains a material part; `EVIDENCE_INSUFFICIENT` iff the
writer log or snapshots cannot be produced.
Templates structurally admissible iff all twelve are class S or E. Distributable schema safe iff the gate's five questions have no
adverse answer under the unamended policy. Cold one-shot feasible iff epoch FULL **and** templates admissible **and** a static producer
extension is legitimate. Opt-in implementation justified iff cold one-shot is feasible **and** the authority extension is small.
Primary outcome: FULL_STATIC_RECONSTRUCTION_SUPPORTED iff all five hold in the tested configuration; PARTIAL_... iff some image or
template is static and the rest is not; STATIC_RECONSTRUCTION_NOT_SUPPORTED iff the epoch or templates depend on runtime facts a bounded
recipe cannot express; otherwise EVIDENCE_INSUFFICIENT. The five answers are reported separately.

## Checkpoints

C1 this plan. C2 Phase A. C3 Phase B. C4 Phase C. C5 Phase D. C6 Phase E (conditional). C7 independent adversarial review (synthetic
adversarial matrix of the task section 9 plus review of the conclusions), result ADR 0179, handoff. If C2-C4 disprove H the dependent
implementation work stops there.
