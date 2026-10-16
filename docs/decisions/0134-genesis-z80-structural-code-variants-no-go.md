# ADR 0134: Genesis Z80 structural code variants - classification and NO-GO

- Status: Accepted (SEG-068-T003). Refines ADR 0073 (sections 5-7) and the Z80 audio contract (sections 6, 7, 18). Adds no mechanism.
- Related: ADR 0073 (materialization, S1*, RAM-backed guard), ADR 0122 (Proposed M68K precedent; not an approved Z80 design and not
  a dependency), `docs/architecture/genesis-z80-audio-contract.md`.

## Context

The SEG-068 compatibility sweep found five locally held images whose Z80 sound subsystem is isolated by `z80_code_mismatch`
(`genesis_audio = degraded`). The question asked first was why the generated-native Z80 meets instruction bytes that differ from the
image compiled at build time, before any mechanism is considered.

## Phase A - classification (aggregates only; no byte, address or disassembly is recorded)

A temporary, uncommitted probe (not part of the product) logged, at the first guard mismatch: the epoch history (signature
extent counts), which instruction form differed, whether that instruction had executed before, who last wrote each differing byte
(M68K or Z80), and compared the live bytes with every registered epoch snapshot. A second temporary experiment replaced the compiled
image by the observed live form (compiled bytes only; the S1* inputs untouched) and re-ran the unchanged production pass to see what
follows. Results (one run per cell, the pass is deterministic and was reproduced by the unchanged confirming run):

| Workload | Epochs seen before fault | Mismatch case | Detail |
| --- | --- | --- | --- |
| Aladdin | 3 (stub, restart, full 8 KiB upload) | C (in place, Z80-written) | opcode and length change (relative jump -> return) before first execution; then the same instruction alternates between a return and an exchange-register-set opcode between executions (>= 3 observed forms, not closed in 6 re-runs) |
| Earthworm Jim | 3 (same shape) | C | identical sequence; return/exchange alternation seen in 2 re-runs |
| Jurassic Park | 2 (stub, full upload) | C | identical sequence; return/exchange alternation seen in 2 re-runs |
| Krusty's Super Fun House | 3 (same shape) | C | the single opcode/length change (relative jump -> return) before first execution; **with that one form compiled the Z80 stays healthy for the whole 600-frame window** (closed, 2 forms) |
| The Lion King | 3 (stub, 29-byte upload, 5,124-byte upload) | C | a two-byte instruction at a low page is overwritten by a different-length instruction after it executed (code overlay, 63 bytes changed in 4 runs); the two forms alternate between epochs (the second form was seen only after the first was compiled) |
| Sonic 1 (control) | 3 | none | no mismatch, unchanged |

Distinctions, per the task:

- **Case A (unseen image, missed registration): excluded.** Every faulting epoch is a registered image; the fault happens after
  activation. The pass stops only because the Z80 is quiesced (contract section 18), which is by construction, not a missed fixed point.
- **Case B (identity insufficient): excluded.** At every fault the differing executable bytes were last written by the **Z80**, never the
  M68K, and the live form equals no registered snapshot. S1* selected the right image; the image changed afterwards. No second
  image shares the signature.
- **Case C (in-place structural modification while the image is active): the cause in all five.** Opcode/selector change in every case;
  instruction-length change in every case; executed bytes in every case; mutation **before first execution** (four titles' first fault)
  and **after earlier executions** (the alternation and the Lion King overlay). The modifying agent is the sound driver itself.
- **Case D (shape-stable operand): excluded.** The differing byte is structural (the guard's structural bit) in all five; no immediate or
  displacement is involved. T012 is correct.
- A plain restart (empty hold window) binds the previously bound image without a lookup; a Z80 self-modification followed by a restart is
  the same mechanism and is caught by the same guard.
- Not a new epoch: no `/RESET` or runnable transition accompanies any of these writes; ADR 0073 forbids deriving an epoch from a mismatch.

## Phase B - existing-mechanism evaluation

- B1 (materialization bug): no. No image is missed.
- B2 (S1* or guard defect): no. The guard fires exactly on a real structural change, before any effect (T012/T013 exhaustive tests).
- B3 (wrong binding of an existing image): no; the live form is in no registry image.
- B4 (precompiled variants selected by byte equality) is the only remaining route; evaluated below.

## Phase C - pre-registered proof gate and decision: NO-GO

Bounds were written **before** the Earthworm Jim, Jurassic Park, Krusty, Lion King and Sonic 1 outcomes were read (only the first
Aladdin mismatch class was known; no cardinality had been measured) and were not adjusted afterwards:

- B1: at most 2 compiled forms per instruction address (the image form plus one alternative);
- B2: at most 16 variant addresses per image (the existing `kMaxImagesCeiling`);
- B3: no discovery runs beyond the existing `kMaxDiscoveryRuns` budget; B4: the observation window is unchanged (600 frames).

Outcome:

1. **Bound B1 is exceeded** by the Aladdin / Earthworm Jim / Jurassic Park driver family (three forms at the first address: image
   form, return, exchange; the alternation continued for as long as it was re-run). That is three of the five workloads and the same
   mechanism. The wording of B1 is ambiguous: counting only *executed* forms (the image form never ran, the driver patched it first) the
   family has 2 and B1 would be met. The NO-GO therefore rests on outcomes 2-3 (closure and design), not on B1 alone; B1 is reported as
   literally exceeded and is not reinterpreted after measurement.
2. **Closure is not established and cannot be by the bounded pass.** The pass observes one new form per fault; it never executes
   an uncompiled form, and a window with no further change proves nothing about forms not yet reached. For the alternating family the
   observed set had not closed; for The Lion King a code overlay (different instruction lengths) recurs per epoch, so an unbounded
   family of overlays is plausible and not excluded.
3. Obligations 4-8 and 10 (decode and lift every candidate through the existing frontend, selection before effects, prefix/length/
   displacement correctness, interrupt handlers, unchanged normal path) cannot be proven without the change itself. A per-address form
   chain would touch the entry emitter, the owner grouping (ADR 0071: one decode per address; a one-byte change alters up to four
   overlapping entries) and body factoring - a new selection structure, not a small delta over the registry/guard. The task's
   boundary is to stop and report that, not to build it speculatively.
4. The only workload that satisfies the bound and closes (Krusty: one patch, then healthy) is blocked by a separate M68K frontier
   (SEG-068-T005); a mechanism justified by one title is not a general capability.

Therefore structural variants are **not implemented**. Fault isolation (contract section 18), S1*, the guard and the materialization
fixed point are unchanged. A Z80 whose driver rewrites its own executable bytes remains an explicit unsupported class; the Z80 is never
recovered, interpreted or guessed, and the M68K never receives a fabricated acknowledgment or RAM write (The Lion King's Z80-written
synchronization flag therefore stays unwritten).

## Consequences and revisit conditions

- Contract sections 6/7/18 are annotated with the measured class: structural change is **Z80-initiated self-modification of
  driver code**, not an identity or epoch defect.
- Re-open only with new evidence: (a) a measured closed form set within a pre-registered bound that covers the Aladdin family
  (for example by showing the never-executed image form is legitimately outside the count, decided as an explicit contract amendment
  *before* measuring), and (b) a design that keeps one decode per address (owners) with selection before effects. Neither exists yet.
- Reusable diagnostic: none was added to production; the probe is described above so the classification is reproducible.
