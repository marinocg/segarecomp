# ADR 0271: SEG-122-T011 static work-RAM `JMP (xxx).L` thunks with a live operand - design and pre-registration

- Status: **Accepted design; evaluation criteria frozen.** This record is committed and pushed before any build or measurement of a commercial image. Results and the decision go in a
  later ADR. The criteria in "Frozen evaluation" reproduce the harness task record SEG-122-T011 (merged to the private backlog before the task started) and are not relaxed afterwards.
- Related: ADR 0049 (immutable-copy alias and the per-instruction byte guard), 0097 amendment (materialized RAM `JMP` thunks), 0122 (general RAM-code variants: **not** adopted here),
  0256 (Sega CD static producer recognition: precedent only, not copied), 0269/0270 (selective admission with RAM thunks).

## Problem

Several cartridges (the investigated Sonic & Knuckles images among them) place the level-6 and level-4 interrupt vectors in work RAM and have their boot code write a `JMP (xxx).L` into
that RAM. Today such a stub gets a compiled entry only after a *run* observes it (ADR 0097 amendment): one extra M68K emission per thunk. The materialized form guards the **whole** six
bytes, including the destination, so a thunk whose destination changes later (a level-4 stub retargeted during play) is refused after its first retarget and its interrupts are dropped.

## Decision

A work-RAM `JMP (xxx).L` is a **structural identity (the opcode word) with a live operand (the 32-bit destination)**. It is established statically, compiled in the first M68K emission,
guarded at run time on its opcode word only, and dispatches through the existing compiled-entry authority. Nothing in RAM is ever decoded to choose what to execute: the opcode
comparison is an identity check against a build-time constant, as in ADR 0049.

### Static producer (positive evidence, not an ordering proof)

`populate_immutable_rom_aot_entries` (Genesis machine frontend) already receives the discovered, reachable instruction graph (`FrontendAnalysis::ir`) and the resolved interrupt vectors.
For each vector whose immutable slot points into work RAM (level 6 and level 4 today) it admits an entry iff, using the CPU-owned decoded IR only:

1. the target address is even and `[A, A+6)` lies wholly in the 64 KiB work RAM;
2. a reachable `MOVE.W #$4EF9,(xxx)` stores the JMP opcode word to exactly `A` (absolute `.W` is sign-extended and all addresses are normalized to the 24-bit bus and the work-RAM
   mirror, so `$FFF0.W`, `$FFFFF0.L` and `$E0FFF0.L` name the same bytes);
3. a reachable `MOVE.L` writes the operand at `A+2` (the operand has a constructor; its value is never read);
4. **no other reachable operation** names a destination overlapping the opcode word (any second store, a long store spanning it, a `CLR`...). That is an ambiguous construction: no entry;
5. the CPU-owned decoder accepts the six bytes `4E F9 00 00 00 00` at `A` as exactly `JMP (xxx).L` of length 6 (a representative zero operand).

An address that an existing identity (alias, other entry) already overlaps acquires no static entry. Everything else (an unrelated RAM address with the same store, an unreached
construction, a register-indirect writer alone, a different opcode) yields nothing, and the legacy materialization remains available for it.

### What this does and does not prove

- **Positive evidence:** the reachable cartridge code contains a constant store of the JMP opcode at the root address and an operand store.
- **Not proven:** that the opcode is installed *before* the first interrupt that targets it (no dominance analysis, no ordering, no interrupt-mask reasoning); that no *other* writer
  (register-indirect store, DMA, a store the discovery does not reach, a cartridge-time bank switch) ever changes it. The proof limits are intentional: the runtime guard is
  authoritative, not this analysis.
- **Runtime verification:** at every entry the generated body compares the opcode word (two bytes of work RAM) with `4E F9`. A mismatch is the existing typed refusal
  (`known_but_unemitted_target`) with **nothing of the instruction committed**. An interrupt's architectural entry (stacked frame, mask) has already happened before its handler
  is fetched; a guard refusal does not and cannot undo that, and is not described as doing so.

### Live-operand body

The operand is the known operand of the known, precompiled instruction; the body reads it, it does not select among opcodes:

```
guard: work_ram[A]==4E && work_ram[A+1]==F9        else typed refusal, nothing committed
ea   = be32(work_ram[A+2..A+5]) & 0x00FFFFFF          (24-bit program bus, as for any absolute long target)
if genesis_compiled_entry_lookup(ea) == NULL       -> unresolved_indirect_target stop, nothing committed
pc   = ea ; retire with the established 12 cycles
```

It reuses the computed-jump branch of the M68K emitter and the shared final compiled-entry table; no second target authority is created. Odd, unmapped, uncompiled targets (and,
by lookup, work-RAM targets) all take the existing typed stop. A real 68000 raises an address error for an odd target; the generated program stops fail-closed instead, exactly as
every other computed `JMP` in this codebase does (a known, preexisting simplification, not introduced here).

### Dynamic target authority

The entry's `decoded`/`operation` carry the representative zero operand only so that the CPU-owned decode, lift and timing (12 cycles) are the single source. That operand is never a
fixed successor: `m68k_control_successors` is bypassed for such an entry in the validator, prune and closure of the admission layer, and the codegen exact-PC obligations are empty.
The destinations of a live thunk are dynamic transfers: closure does not follow them, and a selective build that does not admit a destination stops with the typed
`unresolved_indirect_target` (never "covered").

### Identity and precedence with the materialized path

The static entry is a RAM-keyed identity like an alias (`execution_alias`, mandatory for admission, not in any cartridge range) but is **not** `materialized_ram_thunk`. If an observed
`JMP (xxx).L` descriptor later names the same address it is *subsumed*: the static entry stays, nothing is re-emitted, a differing destination is never a conflict. A descriptor of a
different form, or one that overlaps a static thunk without being the same address, is refused (fail closed). The materialized path remains for everything the producer does not
establish; it is not used to establish the two investigated thunks.

### Interrupt delivery

The scheduler, priorities, mask, acknowledgement, timing and `RTE` are untouched. The existing wiring that makes a work-RAM IRQ6/level-4 vector with a compiled entry at its address
an ordinary handler entry now finds the static entry (IRQ6 recognizes it explicitly; vector 28 already accepted any entry at the address). No interrupt handler of an unproven RAM
vector is enabled: without an admitted entry the typed stops are unchanged.

### Alternatives rejected

- Extending the materialized thunk to ignore its operand: still needs an observing run for the first emission.
- Evaluating the constants of the immediate stores to pre-compile the destination: the destination changes (several writers, seven values on the investigated image); folding is the
  scalar-exception path the policy forbids, and would still not follow later retargets.
- ADR 0122 variant framework, a symbolic executor or a Sega CD `producer_scan`: out of scope; the bounded scan above is a single pass over already-decoded operations.
- A new `ExecutableImage` for the thunk: an image carries bytes with a verification mode; this identity has no byte image (its operand is live), so an artificial one would claim
  more than is known. The authority recorded is the discovered graph; the runtime guard is the verification.

## Known limits (checkpoint-1 exit)

- Recognition is limited to `JMP (xxx).L` reached from an immutable interrupt vector; `JMP (xxx).W`, exception vectors other than levels 4 and 6, and constructions that
  compose the opcode in another way (a long store, a register source) are not recognized and stay on the legacy path. No substantial new analysis framework was needed:
  `STATIC_PROOF_BLOCKED` does not apply.
- Late retargeting stores the discovery does not reach cannot create ambiguity evidence either way; their destinations must be compiled targets, or the typed stop applies.
- A selective (Optimized) build must admit every destination the game uses; the destination set is not known statically, and this task does not widen admission to cover it
  (see the results ADR for what the measured routes show).

## Frozen evaluation

Reproduced from the task record: baseline product `main` at the merged SEG-122-T010 head `4ad9911`; targets T1 standalone and T2 combined Sonic & Knuckles (`--mapper lock_on`),
GCC as in ADR 0270, `--jobs 4`, one build at a time, cold caches; finite instruction budget and wall timeout on every run.

- **C1 static:** both RAM `JMP` entries of T1 and T2 are admitted from the cartridge image alone, with no executed guest instruction, and are in the first M68K emission; a negative control (an unrelated RAM address) acquires no entry.
- **C2 one shot:** zero materialization-driven M68K recompilation rounds for those two entries; later observation neither replaces, re-emits nor conflicts with the static entry.
- **C3 fail closed:** synthetic fixtures D-G give the typed stop/refusal with no effect committed; fixture H (legacy) behaves as before.
- **C4 CPU semantics:** the live-operand body equals the pinned Musashi MC68000 on PC, SR, registers and cycles for supported operands, including a changed operand.
- **C5 interrupts:** with the verified instruction installed the level-4 vector is delivered; scheduler priority, mask, acknowledgement, timing and `RTE` are unchanged; delivered and dropped
  interrupt counts per class are reported for the old and new executable; no handler of an unproven RAM vector is enabled.
- **C6 independent reference:** changed H-interrupt behaviour needs an independent Genesis reference; if none is usable the verdict is capped at `STATIC_THUNKS_ONLY` and default-on promotion is withheld.
  A difference from the old executable is not a regression by itself and must be explained by the previously dropped interrupts.
- **C7 non-regression:** existing suites pass; generated output compiles as strict C11.
- Economics (reported only): M68K emissions, thunks found statically vs by observation, extra M68K rounds, generated C (per emission and cumulative), build CPU and wall, executable size,
  compiled instruction count and admission ratio. The claim is limited to "no materialization-driven M68K recompilation for the two thunks".
- Hygiene: aggregates, classes and digests only; no ROM bytes, RAM dumps or generated artifacts containing protected data.

## Frozen routes and measurement detail (addendum, recorded before the first route run on a commercial image; tightens, never relaxes)

The candidate T1 build (cold, GCC, `-O1`, `--jobs 4`, `--aot-policy optimized --aot-model gen2 --mapper lock_on`) and its static-entry check were made before this addendum;
no candidate **route** has been run yet. From here on:

- **Arms.** `CAND`: this branch. `BASE`: product `main` (SEG-122-T010 merged, tree-identical to the T010 PR head), same options. `BROAD-CAND`: this branch with `--aot-policy compatibility`.
  Baseline and candidate use identical flags, compiler, `--jobs 4`, fresh work directories, one build at a time. One cold build per arm (not the median of three used in ADR 0270; stated as a
  deviation, economics are report-only).
- **Route R1 (both targets, every arm).** Headless frame-keyed pad script `SEGARECOMP_PAD_SCRIPT=300:1:0x80,310:1:0,500:1:0x80,510:1:0,800:1:0x80,810:1:0,1100:1:0x80,1110:1:0,1400:1:0x80,1410:1:0,2000:1:0x80,2010:1:0`
  (Start taps), `--instruction-budget 4294967295`, wall timeout 1800 s, captures at virtual frames 3000, 4500, 6000, 7500 (the run ends when the window completes), `SEGARECOMP_SOUND_SUMMARY=1`.
  A route that does not reach gameplay is recorded as an evidence-limited bounded route; it is not retuned after seeing a result.
- **Compared fields (CAND vs BASE vs BROAD-CAND):** result and stop class, dispatch count, virtual frames, audio frame count and digest, `RUNTIME_NOTES` (dropped H-interrupt count), frame digests.
  Differences between CAND and BASE are expected where BASE dropped level-4 interrupts; they are explained from the dropped count, not hidden. CAND must equal BROAD-CAND on every field,
  with zero typed stops on code BROAD-CAND compiled (a selective build omitting a live destination is reported, not accepted as a pass).
- **Interrupt classes reported:** level-4 (H) undelivered/dropped count, level-6 (V) behaviour, typed stops by class.
- **Independent reference.** No pinned Genesis-level emulator harness exists in the repository; the pinned Musashi comparison covers instruction/exception semantics only (C4). Per C6 the
  verdict cannot exceed `STATIC_THUNKS_ONLY` unless a Genesis reference is run on the same route; a locally installed emulator without a scriptable state/RAM interface is recorded as a gap, not used as proof.

## Frozen route R2 (second addendum, recorded before any R2 run; tightens, never relaxes)

Operator request after the first decision: exercise level-4 delivery with the retained S3&K validation tooling (title input code, SOUND TEST, level select, debug mode armed by A held at level init;
the validation image is the combined image with one byte retargeted, never shipped, local only). Route R2 = the retained "played start" route (`tools/seg122_t005_s3k/shots.sh` logic: level select to the act,
wait 430 polls, run right with jumps until poll +1520), **all 28 acts**, frame exact (`SEGARECOMP_VIEWER_SLICE=1`), original 4:3, headless SDL dummy drivers, unthrottled, muted, finite `--instruction-budget 4294967295`,
wall timeout 600 s per run, captures at polls +425, +900, +1400 after the level-select Start. Arms: `CAND` and `BASE` (product main), both Optimized gen2 selective builds of the validation image, same flags, built one at a time.
Compared per act: run exit and stop class, `RUNTIME_NOTES` (`hint_undelivered`, virtual frames), capture digests. Assertions: `CAND` has no typed stop and `hint_undelivered` 0 on every act; any `CAND`/`BASE`
difference is explained by `BASE`'s undelivered level-4 interrupts (acts with `BASE` undelivered = 0 must be bit-identical); frames at the end of each act inspected for the acts with the largest `BASE` counts.
A stop in `CAND` is reported with a broad replay, not retuned away. This remains evidence for these routes and still has no independent Genesis reference (gate C6 unchanged).

## Frozen route R3 (third addendum, recorded before any R3 run; tightens, never relaxes)

R2 (played start, all 28 acts) arms debug mode but does not use it. R3 uses the retained debug-mode serpentine flight (`tools/seg122_t005_s3k/sweep.py`, one part, about 17,000 polls) on the three acts where R2's `BASE`
dropped the most level-4 interrupts: AIZ2, CNZ2 and LBZ2. Same arms, image, flags, frame-exact slice, headless drivers, finite budget and 600 s wall timeout as R2; captures at polls 4000, 9000, 14000 and 16500.
Assertions: `CAND` has no typed stop and 0 undelivered level-4 interrupts; `BASE` differences are explained by its undelivered count; a `CAND` stop is replayed on a broad build, not retuned.

## Frozen route R4 (fourth addendum: independent Genesis Plus GX reference route, recorded before any R4 run; tightens, never relaxes)

Reference: the pinned Genesis Plus GX (`tests/genesis_oracle/pins.py`, exact revision, clean tree, built in a temporary copy) driven by the existing libretro host extended with optional, inert-by-default `--pad` and
`--capture` modes (original Z80 smoke unchanged). Native arms `CAND` and `BASE` are non-viewer (capture-hook) Optimized gen2 builds of the same validation image. Region USA/NTSC-U in both. The reference compares
only against `CAND`/`BASE`, never against a Compatibility build of this branch.

Route: the retained played-start route of R2 for CNZ2, LBZ2 and AIZ2 (identical frame-keyed pad events in both machines; frame number = native virtual frame / libretro `retro_run` index), finite frame budget (route end + 20), 600 s wall
timeout. Captures at frame ordinals Start+500 to Start+1080 every 20 frames (30 frames) as raw 320x224 RGB, plus the 64 KiB work RAM at the same ordinals.

Canonical observable (renderer independent): per frame, per row `t(y) = mean(B) - mean(R)` over the 320 columns of that row; a **water-line row** exists at `y` when `|median t(y..y+7) - median t(y-8..y-1)| >= 24` with the
largest such step chosen. Comparison per frame: reference present/absent versus native present/absent, and the row within +-3.
Pass for the independent-reference gate on real routes (all required): (1) CNZ2 and LBZ2: in every captured frame the reference and `CAND` agree on presence, and rows within +-3 where both are present, and the reference
shows a water line in at least 5 of the 30 frames (else the route is non-discriminating and recorded as such); (2) `BASE` disagrees with the reference in at least one frame of those acts (the metric must discriminate the old behaviour);
(3) AIZ2 is reported, not gated; (4) work-RAM byte differences are reported as an aggregate (count) only, not gated, because the machines are not cycle-identical; (5) no typed stop in `CAND`. If the machines' game progress diverges so
that frames cannot be paired by the above, the route fails closed (C6 stays unmet); a result is never tuned afterwards.

## Frozen route R5 (fifth addendum, recorded before any R5 run; R4's outcome is recorded in ADR 0272 and not altered)

R4 (played-start route, frame-paired) was run first and failed closed as its own rule required: the three machines follow the same route for a few hundred frames, then game physics diverges (different score and position at
the same frame), so frame-paired canonical observations cannot be compared. R5 uses the retained debug-mode serpentine flight (`tools/seg122_t005_s3k/sweep.py`, part 0) of CNZ2, LBZ2 and AIZ2: the camera path is a function of
the held buttons only (no game physics), so the machines can be paired by frame. Same reference, native arms `CAND` and `BASE`, image, region, pad events (identical in both machines), finite budget and 900 s wall timeout as R4.
Captures: 64 frames, ordinals Start+600 every 270 (covering the whole flight), raw 320x224 RGB.
Observable: per paired frame the **differing-pixel fraction** `f` = share of the 71,680 pixels with `|dR|+|dG|+|dB| > 48` between the reference frame and the native frame (renderer-independent in the sense that a palette split
changes whole bands, a renderer difference changes few pixels).
Gates for the independent real-route reference (all required, CNZ2 and LBZ2; AIZ2 reported): (1) `CAND`: median `f` <= 2 % and maximum `f` <= 10 % over the 64 frames; (2) discrimination: `BASE` has at least 3 frames with `f` >= 15 %
where `CAND` has `f` < 5 % (else the route does not exercise the H-interrupt and is recorded as non-discriminating, C6 unmet by this route); (3) no typed stop in `CAND`; (4) if `CAND`'s median `f` exceeds 20 % the route is not paired and fails closed.
Thresholds are fixed here and are not revised after the results.
