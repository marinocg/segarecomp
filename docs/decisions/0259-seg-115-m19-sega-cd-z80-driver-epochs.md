# ADR 0259: Sega CD main-side Z80 - image epochs, re-arm and the driver family (SEG-115 / M19)

- Status: Accepted (SEG-115, T001-T002). Builds on ADR 0258 (M18), ADR 0072/0073 (Genesis Z80, image epochs), ADR 0134 (structural variants NO-GO) and ADR 0135
  (static mutation proof spike). It changes none of their conclusions; the Genesis machine is unchanged (both options below are off unless the Sega CD attach sets them).
- Local evidence (never committed): `/tmp/m19`. The authorized disc and BIOS stay in the ignored inputs; this record keeps classes, counts and sizes.

## 1. Root cause (T001, before any change)

An ephemeral probe (a scratch copy of the runtime that logged every 68000 write to Z80 RAM with its time and every Z80 store that changed a byte, the epochs and the first
structural mismatch; not part of the product) was run on the authentic Earthworm Jim SE build of M18.

| Epoch | Hold window of the signature (extents) | Image chosen | What happened |
| --- | --- | --- | --- |
| 1 | 38-byte stub (1) | stub | runs |
| 2 | 5,144 bytes (3) | BIOS sound driver | runs ~37 Mticks, no code-byte store |
| 3 | 5,148 bytes (4), same driver re-uploaded | BIOS driver (new signature) | **first structural mismatch ~2.1 Mticks after activation** |
| 4 | 3,463 bytes (3) | a BIOS image | never executes: the Z80 was already isolated |
| 5 | 39 bytes (2) | stub | never executes (isolated) |
| 6 | **empty** (the whole 8 KiB was written in an earlier hold that a plain resume ended) | the previously bound image (stale) | never executes (isolated) |

Two independent faults:

- **A - activation / upload provenance.** The 68000 writes Z80 RAM only while it holds the bus, and the Genesis hold window ends at *every* runnable transition,
  including a plain BUSREQ release. An upload spread over several holds, or made while the previous image is still being resumed, therefore reaches the next
  `/RESET`-restart epoch with an empty (or partial) window, and a restart with an empty window re-binds the *previous* image (a "plain restart"). This is not
  specific to the title; it needs no title knowledge to explain.
- **B - genuine structural self-modification.** At epoch 3 the driver's own instructions store constants into low code bytes that the Z80 then executes (the
  modifying agent is the Z80 for every differing byte: the live bytes of the mismatching instruction were all Z80-written or unchanged image bytes; no
  registered image has those bytes). The same family modifies the game driver at its initial setup (below).

Answers to the T001 questions: the executable image of each epoch is in the table; the RAM ranges were written by the 68000 (uploads, mailbox/command bytes) and by the Z80
(stack, variables, and the five code bytes); no other registered image explains the live bytes; the mismatch at epoch 3 occurs in the BIOS driver, **before** the game's
driver can run; the game driver never reached its handshake write in M18 because of both faults, in that order (B at epoch 3 isolated the Z80 permanently, then A bound the
stale stub-era image at epoch 6).

## 2. Decision (T002)

1. **Accumulate the hold window until an image epoch** (Sega CD only; `GenesisZ80EpochObserver.accumulate_until_epoch`). The window is cleared at an image epoch, not at a plain
   resume, so every 68000 write since the previous epoch is part of the next epoch's signature S1\*. The signature definition itself (extents of 68000-written bytes with the
   bytes at the transition) is unchanged. Consequences, recorded: bytes the 68000 writes between two epochs (command-ring bytes) join the next epoch's identity; a Z80 restart
   with no 68000 write since the previous epoch is still a plain restart; an image whose bytes outside the written extents differ (the Z80's own data) is the same image.
   A new signature is a new image, found by the existing build rounds; an unseen one stays the typed `z80_unsupported` stop of an executable program.
2. **Re-arm an isolated Z80 at the next image epoch** (Sega CD only; `GenesisZ80Machine.rearm_faulted_on_epoch`). Contract section 18.4 made the isolation permanent as "the simplest safe
   choice" and left re-arming a future decision. A genuine epoch (a pristine Z80 that starts from RAM that the epoch's signature selects) runs a *registered* image from the
   architectural reset state; the RAM-backed guard is unchanged, so a mutation of the new image isolates it again. Nothing is interpreted or guessed, no state is fabricated, the
   YM2612 reset is the same device effect as before, `z80_reset` runs at the release exactly as for a healthy Z80, and the count is reported (`z80.recoveries`; the state digest
   changes only for a run that recovered). The isolated period itself stays silent and its effects absent (the recorded deviation of ADR 0258 decision 4).
3. A Z80 that is resumed over code the 68000 has just overwritten (the bus released between the upload and the `/RESET` restart) executes the *old* image's compiled bytes
   against new live bytes; the guard isolates it (epoch 5 above). That is the faithful outcome of an unsupported hardware interleaving (the real Z80 would execute the new
   bytes from a stale program counter), not a defect to hide.

## 3. Tests

`tools/sega_cd_bios_hw_fixture.py` cases 15-16 (hand-assembled Z80 programs; the fixture ROM is 0x200 bytes larger) and `tests/sega_cd_main_z80_test.py`: first upload and
activation, a different image uploaded across two holds *while the previous image runs* and restarted (the case that selected the stale image), a restart without change,
the same program again with different irrelevant Z80 data (the same image, not a new one), an isolated Z80 re-armed by a registered image, identical state for scheduler
partitions; the existing unregistered-image stop (case 14) is unchanged. Negative control (both options off): case 15 faults with `z80_code_mismatch` at the restart and
runs the old image; case 16 stays isolated.

## 4. Feasibility gate for structural variants (T003) - decision **STRUCTURAL_VARIANTS_GO** (bounded; no closure claim)

State after T002 (authentic, local): the epochs of the disc select six distinct images, the BIOS driver's isolation (epoch 3) no longer ends the run (2 recoveries, a Z80 resumed over code the
68000 had just rewritten is isolated once more, the game driver is bound at its epoch), and the **game driver executes ~1,000 Z80 cycles and then meets its own
self-modification**: the first mismatch is a Z80-written byte (a RET) stored over the first byte of a `JR` that the driver executes next. This is fault B, now isolated from fault A.

Evidence for the decision, each from the existing static spike (ADR 0135 tool, run on the epoch-6 snapshot; nothing executed):

- The analyzer, given only the 8 KiB image and the reset vector, lists the actual first modification (the store site that wrote the observed byte, its exact destination and exact value) among 11 candidates over three destination bytes (values `RET`/`EXX`/`NOP`/`EX AF,AF'`; the family ADR 0134 observed). The other bytes the driver writes near it are word *variables* in dead space (`LD (nn),HL`), never code: they correctly are not candidates.
- Completeness is still **not** established and is not claimed: of 481 stores 390 have an unknown destination (62 % because the CPU effect footprint lacks the unsupported forms, 37 % because
  of a call return continuation, the rest memory-loaded pointers). A CPU-owned write footprint would remove the first class but not the second (interprocedural) nor pointers loaded from RAM, so it
  **does not materially change the closure result**; none is built (it is a CPU-semantic extension this milestone must not make merely to improve a workload).
- The emitter fact that makes a small mechanism possible: the broad Z80 AOT already emits **one exact entry for every address** of an image (not only for instruction starts of one decode), so a
  one-byte change alters at most four existing entries (the starts covering that byte) and creates no entry that does not already exist. A variant is therefore **an additional compiled form of an
  existing entry**, selected at that entry by the structural bytes it snapshots anyway; no dispatcher, no discovery loop, no new owner structure.

Soundness argument (why closure is not a precondition). A variant form is the generation-time decode, through the unchanged frontend and lowering rows, of concrete bytes. Selecting it requires the live
structural bytes of that instruction to equal those bytes, checked before any instruction effect, with the same boundary and rejection order as the current guard. Executing the form that matches the live
bytes is exactly executing those bytes. A write the analysis did not list produces live bytes that match no compiled form and the entry fails with the existing `z80_code_mismatch` isolation (ADR 0134 /
contract section 18), so an incomplete candidate set can only lose coverage, never produce a wrong execution. This **amends the revisit condition of ADR 0134** ("a measured closed set"): the NO-GO of
ADR 0134 stays for *observation-driven* variant discovery (execute, fault, compile the observed form, repeat), which is not used; closure of the set is not claimed and `candidate_set_complete` stays false
in every report. ADR 0135's finding (existence of exact-valued writes, not their completeness) is used exactly as stated.

Pre-registered (before the mechanism existed; not adjusted after measurement): per image at most 32 alternative destination bytes (candidates, mirrors counted once) and at most 8 alternative forms per entry;
exceeding either drops *all* alternatives of that image (deterministically, reported) - the image then behaves as today. Only candidate destinations of an exact destination and a finite
value set are used. Alternatives are an input of the generated code and are part of the image's provenance and of the registry identity. Opt-in: the Sega CD build only; the Genesis build and its
emission stay byte-identical.

Stop rules: if the authentic driver then faults at a write that is not a candidate, or the mechanism needs a new dispatcher/owner structure, the result is recorded as `STRUCTURAL_VARIANTS_PARTIAL`
and nothing is widened (no candidate is added from observation, no bound is raised for the workload).

## 5. Alternative forms (T004) and what the authentic driver does now (T005)

Mechanism (as implemented; ~250 lines of emitter, ~40 of runtime header, ~60 of producer): the static mutation analysis moved from `tests/tools` to `libs/cpu/z80/mutation.{hpp,cpp}` (still report-only logic, plus
`static_byte_alternatives`); an executable image may carry `byte_alternatives` (structural images only, sorted, unique, validated); the Z80 projection hands them to the emitter as
`CodeImage::alternatives`; for every entry whose four fetched bytes include an alternative byte the emitter decodes, through the unchanged frontend, each combination of the alternative values, lowers
the forms that decode and have a lowering row (a longer than 4 byte form, a prefix lock or an unlowered form is simply not a form), removes forms whose statically defining bytes equal an existing form
(operand bytes stay live), and emits `z80_live_fetch` (one code_fetch of the longest form), then `if (z80_live_is(...)) { z80_live_enter(); <body> }` for the snapshot form and each alternative, then the
typed mismatch (or, for a snapshot form that is itself a typed stub, that stub). Entries without alternatives, and images without alternatives, are emitted byte for byte as before. More than 8 forms for
an entry drops all alternatives of that image (reported). Mirrors are explicit, because the image is the 8 KiB RAM twice. The Sega CD build opts in (`--z80-alternatives`, default 1; the control 0 is the plain guard); the Genesis build does not;
the generated registry identity includes the alternatives (`z80.alternatives` in `status.json` reports images, bytes, entries, forms, dropped images and `complete:false`).

Tests: `tests/z80_live_alternatives_test.py` (differential against the immutable image of the live bytes: RET/EXX/NOP at one byte; 3-byte -> 1- and 2-byte; overlapping entries started at both; JR NZ -> JR Z; IM 1 handler and RETI -> RETN; operand
mutation live with no form; mutate after the original form executed and a toggle back, hand-derived registers; unknown replacement = typed mismatch before any effect; the per-entry bound), fixture case 17 end to end (a
program patching a routine's first byte JR -> RET -> JR, the control `--z80-alternatives 0` isolates), `z80_static_mutation_tests`, executable-image validation tests.

Authentic Earthworm Jim SE (local; probe on the final runtime, nothing fabricated): the build is 11 rounds as before; the static analysis supplies 4 alternative destination-byte values to exactly one of the six images (the game driver; 6
entries, 8 forms, none dropped); **the Z80 stays healthy through 200 M main instructions** (446 M Z80 cycles, fault 0, 2 recoveries earlier in the run, 6 epochs, 6 images).
T005 evidence (aggregate counters of the ephemeral probe; epoch 6 = the game driver): the Z80 read 4,045 distinct bytes the 68000 had written after its previous read (sound commands), the 68000 wrote 6,454
bytes, the Z80 wrote 18 M RAM bytes (its working state and mailbox), 11,954 YM2612 register-port writes, 371 PSG writes and 5,977 YM status reads, and the 68000 read 1,748 bytes the Z80 had written (the handshake/status mailbox)
- the polling that kept M18's flag permanently unwritten now terminates. No reference emulator is available here, so this is observed behaviour, not reference-verified behaviour.

## 6. The shared YM2612 and PSG (T006)

- **Attach.** `scd_z80.c` (compiled with `-DSCD_Z80_AUDIO=1` when the sound devices are linked) creates the existing `GenesisAudio` (the one `Sn76489`, the vendored ymfm YM2612 behind its C ABI, the Genesis mixer) on the same device host and installs `host->audio_hooks`: the 68000's `$A04000` / `$C00011` routes and the Z80's `$4000` / `$7F11` view reach one device, one clock, one write order. No second YM2612 or PSG exists; nothing of the Genesis devices changed (Z80-origin access counters were added to the machine, counts only).
  `segarecomp sega-cd build` compiles the six extra sources (the C++ core with `--cxx`, derived from `--cc` exactly as `segarecomp build` does); `--z80-audio 0` is the control and reproduces M18 (a YM read answers 0, writes are discarded).
- **Time.** The Z80 and both devices are run to guest time at every vertical-blank onset, at every audio output emission point and at every main access (the existing route). The scheduler visits events in time order and stamps every main access with the event time, so no main access can still precede a synchronisation point; the Z80 is a lazy follower whose results do not depend on the cadence (Genesis contract section 10). A typed Z80 stop met while running ahead is not a machine stop: the Z80 stays where it stopped and the main processor meets the same stop at its next access. The YM2612 reset on the Z80's `/RESET` is the Genesis hook, unchanged.
- **Mixing adapter (about 60 lines).** The Genesis mixer already yields the frozen 44.1 kHz stereo stream from guest time 0. Output frame `n` of the Sega CD mix adds the Genesis mixer's frame `n - 2` (read by index from its ring, nothing consumed, so the Genesis digest is unchanged) to the RF5C164 sample and the CD-DA frame before the existing saturating clamp. The two-frame trail is the smallest fixed delay for which the Genesis frame is always closed when the Sega CD frame is due (a YM2612 native sample, 1,008 master ticks, is shorter than an output frame, 1,217); it is deterministic and independent of scheduler slicing (a counter `fm_missing` would show a violation: 0 in every run below). Without the mix (`--audio-*` not requested, no viewer audio) nothing reads the Genesis frames and no guest state differs (below).
- **PSG idle level.** On the authentic disc the driver leaves one PSG tone channel at a finite attenuation with period 0 for the whole run; the device library's output for that is a constant level (504 in the mixed scale). It is inaudible and kept as the device produces it; no claim of fidelity is made for it.
- **Tests.** Fixture case 18 (a 31-register YM2612 patch with busy polling, a PSG tone, a marker: exactly the Z80's 62 YM and 3 PSG writes reach the device, the busy flag is polled, the mixed stream carries FM frames and is non-silent, identical state/stream across scheduler partitions, identical guest state with the output disabled, and the `--z80-audio 0` control is the PCM-only silence) and case 19 (the 68000 sees the busy flag, a timer A overflow in guest time, and the Z80 `/RESET` clearing it).

## 7. Authentic gameplay and audio (T007; local, bounded, no reference emulator)

Earthworm Jim SE, Model 1 BIOS V1.10, formatted-empty backup RAM, the M18 deterministic input scripts (START, RIGHT held), compared with the M18 runs of the same scripts.

| Check | Result |
| --- | --- |
| Build | 11 rounds, as M18; one extra image kind: the same six Z80 images; alternatives on exactly one image (4 alternative byte values, 6 entries, 8 forms, none dropped) |
| Frames | All 30 captured frames (experiments B, C, D0, D2: 22; E: 8 including frame 19,999) are byte-identical to M18's; the final-frame hashes of C, D0, D2 and E (frame 19,999) equal M18's |
| Input | The pad read sequence (onset, port, value) is identical to M18 in all four experiments (48,472 / 65,536 logged reads); only the read *ticks* differ |
| Instructions | E: main 167,487,109 -> 167,495,117 (+8,008), sub 285,068,534 -> 285,068,626 (+92), total +8,100 (+0.002 %); C +3,882, D0 +3,520, D2 +3,568. Cause: the sound handshake the driver now writes ends the 68000's polling earlier and the YM/status accesses cost their normal bus time; the video and the input stream are unchanged |
| Z80 | healthy to the end of every run (E: 1.11 x 10^9 Z80 cycles, vs 143,605 before isolation; 6 epochs; 2 recoveries: the BIOS driver's own rewrite at epoch 3 - no alternatives were proven for that image - and the stale-image interval of epoch 5); 84,105 BUSREQ cycles |
| Sound devices | E: 9,879 YM2612 register-port writes and 44 PSG writes from the Z80 (+8 PSG writes from the 68000), 10,878 status reads, 17.78 M YM samples, mixer fault 0, `fm_missing` 0 |
| Output | E: 14.718 M mixed frames (333.7 s), the FM/PSG part present in all but the first two; the difference to the same run without the devices shows the idle PSG level plus FM/PSG bursts (strongest at ~15-19 s, the console start-up sound, peak 8.7 k of 32 k), and bursts during play; digests recorded locally; not verified by listening |
| Determinism | Slice seeds 0, 1, 3, 7: identical report (frame hash, instructions, CPU state digests, device log, Z80 digest, fm_psg and, with the output on, the PCM digest); the run with the output disabled has the same guest state (only the observer counters of the PCM synthesiser's catch-up differ, as they already did in M17) |
| Sonic CD | build 10 rounds with the same program identity as M18; with and without backup RAM the stop, instruction count and frame hash equal M18's (the existing `backup_ram_unsupported` / `code_bytes_mismatch` frontiers are unchanged); its Z80 driver now runs (8 M Z80 cycles, 3,759 YM writes) |

## 8. Limits and remaining blockers

- No reference emulator ran here: the Z80 behaviour is *observed*, the audio is not reference-checked or verified by listening, and the level of FM/PSG against CD-DA/PCM is the Genesis devices' own scaling summed with theirs (not established).
- The candidate set is not complete (ADR 0135 limits unchanged); a write the analysis did not list isolates the Z80 until the next image epoch (e.g. the BIOS driver's own rewrite at epoch 3, which the analysis does not list). Whether the game driver performs other modifications later in a longer play session is not known; the run above covers 19,999 frames (~5.5 min of guest time).
- The window accumulation (Sega CD only) lets bytes the 68000 writes between two epochs (command-ring bytes) join the next epoch's identity; a restart after such writes may need one extra build round, and a build exceeding the image bound stops fail-closed.
- A Z80 resumed over code the 68000 has just rewritten (bus released between upload and restart) is isolated until the restart epoch (faithful to the unsupported interleaving, not corrected).
- Not done: Z80 timing contention on the 68000 bus, the Z80 banked window outside the BIOS image, and any title other than the two measured.

## 9. Verdict

**SCD_Z80_AUDIO_GO** for the authentic Earthworm Jim SE driver over the verified 19,999-frame window (pending the independent review and exact-head CI recorded in the backlog record): the game's sound driver is selected by its epoch, runs generated-native past its former fault, consumes the 68000's sound commands and writes the handshake the 68000 polls, programs the shared YM2612/PSG, the output reaches the existing 44.1 kHz stream, and the established gameplay path (frames, input stream, determinism) is unchanged. The smallest next blockers: static coverage of the BIOS driver's rewrite (no candidate is proven for that image), a reference run for the Z80/YM behaviour, and the FM/PSG level against CD-DA/PCM.
