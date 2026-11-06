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
