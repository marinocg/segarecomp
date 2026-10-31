# ADR 0257: SEG-113 (M17) - static BIOS boot-image admission and native build-round elimination

- Status: Accepted. Milestone SEG-113 (M17), on `main` after SEG-111 (ADR 0255, PR #176) and SEG-112 (ADR 0256, PR #177), both merged. Related: ADR 0142 (verified broad AOT), 0178 (provenance), 0205 (execution-observed extents), 0211/0219 (static extents, emitter recomputation), 0254-0256.
- Privacy: classes, counts and sizes only. No code address, byte or digest of a commercial disc, BIOS or RAM appears here; the authentic analysis is local and ephemeral. (The one identifier that must live in the product source - the SHA-256 of the verified BIOS image that keys the authentic profile - is discussed in section 6.)
- Question: when the generator already holds enough independent evidence that executable bytes will be loaded at a known guest location, can it compile them before the first generated-native pass instead of discovering them by running the program? Answer for the two Sega CD boot images: **yes, under a verified profile, class `conditional`** (sections 1-4); for the 48 KiB Earthworm Jim stage: **no small sound extension exists** (section 8).

## 1. Boot-image authority (T001)

What the authentic US Model 1 BIOS does before the first application instruction, established from the BIOS image itself, its execution on two commercial discs, and controlled experiments:

| Question | Main boot area (IP) | Sub boot program (SP) |
|---|---|---|
| Disc source | track 1 user data from byte `$200`, a fixed 32 KiB span (it reaches 512 bytes past the 32 KiB system area, into sector 16) | the SP descriptor span of the disc header (on both authentic discs: 28 KiB starting in the system area) |
| Destination / CPU | main Work RAM `$FF0000`, main CPU | sub PRG RAM `$6000`, sub CPU |
| BIOS operation | the sub BIOS delivers sectors to Word RAM through the CDC; **the main BIOS then copies 8192 longs from Word RAM `$200000` to `$FFFF0000` with an immediate-operand loop** (count, source and destination are immediates in the BIOS code; the IP descriptor of the header is not an input of that loop) and enters `$FF0000` | the sub BIOS reads the module and places it; the SP is then entered through its module jump table |
| Verbatim? | yes, byte-for-byte (the longest matching run of live Work RAM against the disc at the first IP instruction is exactly the 32 KiB span; the same for the SP span on both discs) | yes |
| Transform / patch / init | none observed between the copy and the first IP instruction (the only other main writes to the span are the BIOS's own Work RAM clear that precedes the copy) | none observed |
| Other writers before first execution | none can run: the main CPU enters the IP immediately after the copy; the sub's only Word RAM/PRG writes to these spans are the delivery itself | the sub BIOS places and calls it |
| Already admitted by another image? | the M16 recognizer already walks this span (same `$200` -> `$FF0000` assumption, 0x7E00 bytes, ungated); it admits only the produced fragment, never the span | not admitted |
| Overlays | later stages are loaded over the same guest span from elsewhere on the disc (Earthworm Jim: a 48 KiB stage; Sonic CD: several): they are separate images selected per instruction by the live bytes (ADR 0142 decision 5) and are unaffected | same (overlays of PRG RAM) |
| Documented hardware semantic or one-BIOS observation? | The Word RAM -> Work RAM copy is BIOS code (statically readable). The disc -> Word RAM delivery and the SP placement are sub-BIOS behaviour: observed on two commercial discs with this BIOS (identical extents) and by the independent reference of ADR 0141 (V1), not derivable from the hardware documentation | observation only |

Why the descriptor of the synthetic profile is not the authority: the project's synthetic post-BIOS profile places the *descriptor* span of the IP (a different, much shorter span for both authentic discs) at the base; the authentic BIOS ignores that descriptor on the main side. The SP side coincides with the descriptor.

**Controlled experiment, and why it is not available.** Whether the placement depends on the header (IP size, SP offset/size) cannot be tested with a project-authored disc: the BIOS refuses a disc whose boot area lacks the region's security block, which is commercial code the project must not reproduce (ADR 0130). Both authentic discs carry the same header form, so the observations cannot separate "fixed by the BIOS" from "driven by the descriptor" on the sub side. This is exactly why the proof stays `conditional` on the **exact header form** observed.

### Per-image classification (the feasibility gate)

- `STATIC_BOOT_PROVEN`: **none.** Proving the disc -> Word RAM -> Work RAM chain from first principles needs a loader analysis of the *sub BIOS* (the CD interface model of `loader_scan.cpp` applied to BIOS code) plus the main copy: that is the "third discovery framework" this milestone refuses to build.
- `STATIC_BOOT_CONDITIONAL`: **main boot area** and **sub boot program**, only under a *verified profile*: an exact BIOS image (SHA-256) and the exact IP/SP header form its placement was observed with; (`verified_authentic`). A project-authored fixture BIOS (a 32-byte tag in its header) implements the same contract against the real CDD/CDC path, but **a tag proves nothing about what a BIOS does**: fixture profiles are `test_assumed_fixture`, selected only by the explicit test-only switch `--boot-fixture-profiles 1` (`boot-contract`, `emit`, `build`) and never by default. Eligibility is enforced by `derive_boot_contract` (a pure function of the BIOS image and the disc): any other BIOS image -> `bios_not_verified`; the profile's BIOS but another header form -> `header_not_verified`; no usable boot area / track shorter than the span -> `disc_boot_area` / `boot_area_short`. Nothing is offered then and the existing execution-observed admission (ADR 0205) is unchanged.
- `STATIC_BOOT_UNPROVEN`: every other BIOS (no verification), every other header form, and any disc in which a span does not fit the track.

## 2. Mechanism (T002, T003)

- `libs`-free, platform-local: `platforms/sega-cd/machine/{include,src}/boot_contract.{hpp,cpp}` (~120 lines). A profile table, `derive_boot_contract`, `static_boot_admission`. No new image-producer abstraction: the result is the ordinary `AdmissionKind::static_disc_extent` (immutable disc bytes at a guest base, `immutable_input` authority, `byte_identity` verification, **no pass identity**, role `boot_area_extent` = broad), the same representation the static loader scan uses.
- `segarecomp sega-cd boot-contract --cue --bios` prints the profile, the refusal, the images (cpu, class) and the admissions `static-boot:<cpu>:<track-1 offset>:<base>:<length>:<sha256>`.
- **Emitter recomputation (`emit --admit static-boot:...`).** The coordinates and digest are *not* trusted: the emitter recomputes the contract from the BIOS and the disc it was given and accepts the admission only if cpu, offset, base, length and the SHA-256 of the bytes it reads equal an image of that contract. Refused with typed errors: no verified profile or header form (`static_boot_unverified`), any other coordinate/digest/cpu (`static_boot_unproven`), malformed (`static_boot_syntax`), duplicate (`static_boot_duplicate`), overlapping on one CPU (`static_boot_overlap`), and a changed disc (the digest no longer matches).
- **Build integration.** `segarecomp sega-cd build` asks `boot-contract` first (before static module discovery and static producer discovery) and puts the admissions at the head of the chain, so nothing in the chain depends on a pass. Control: `--static-boot 0`. The ordinary frontier resolver already refuses to re-admit an extent that is in the chain (`uncompiled_entry_in_admitted_extent`); the static boot extents are in the same bookkeeping lists as static module extents (relocation sources, bounds, final-stop location).
- **Observed-extent diagnostics (limited observation).** The static image is never preferred over different live bytes: every executed instruction is compared with the live bytes exactly as before, and the observed extent is still admitted (the existing fallback). For each execution-observed extent the build compares, per static image of the same CPU: destination ranges overlapping (even partially) and the source-to-destination offset. Source in the boot area but outside that image's own source span, mapped over its destination = **contradiction** (`static_boot.contradictions`, inventory `static_boot_contradicted`); source wholly inside the image's own source span at a different offset = **internal source** (`internal_sources`: a relocation of the image's own bytes - the authentic Sonic CD IP does this once - or a shifted placement; undecidable without write tracing, so not a contradiction); source elsewhere on the disc = **later overlay** (`later_overlays`); no destination overlap = unrelated; a frontier inside a static image for which the resolver found no source at all = **unresolved mismatch** (`unresolved_mismatches`), never a BIOS contradiction. Limit: without write tracing, a placement shifted by less than the span (source still inside the image's own span) is reported as an internal source, not a contradiction; the counters are observations, not proofs, and change no behaviour. `status.json` also reports the profile `authority`.

## 3. Fixture matrix (T005)

`tools/sega_cd_bios_boot_contract_fixture.py` builds a project-authored BIOS that implements the contract with the machine's own CDD/CDC path (the sub reads the boot area into Word RAM, reads and places the SP; the main copies the IP span and calls it) and a disc. `tests/sega_cd_static_boot_test.py`:

- `contract` / `ip_short` / `sp_long` / `sp_moved` / `form_ok` (different IP and SP lengths, SP at different offsets, a profile verified for one header form): both images are in the first emission, **2 rounds instead of 4**, the execution-observed extents of the control build are exactly the statically derived images, the programs end in the identical machine state, the program identity is the same at 8 and 2 jobs.
- `no_profile` (unverified BIOS), `form_other` (another header form), an audio first track, an SP descriptor leaving the system area: nothing is offered; the build is the observed one (4 rounds); the emitter refuses a supplied `static-boot:` admission.
- `sp_source` (descriptor and source disagree): the static SP image does not explain the live bytes; the guard stops it, `contradictions` = 1, the observed fallback admits the real bytes (3 rounds), the SP runs, the final machine state equals the observed build's.
- `ip_patched` (a byte rewritten between placement and call): both builds stop at the patched instruction before it retires, at the same tick and machine state; the resolver refuses the bytes (no disc span); no wrong admission.
- `ip_trailing` (only part of the span placed): harmless, no contradiction.
- `sp_shifted` (observed extent overlaps the static SP image only in part, base not covered): contradiction; `stage_overlay` (later stage from unrelated disc content over the IP span): later overlay, no contradiction; `ip_selfcopy`: internal source, no contradiction; `ip_patched`: unresolved mismatch, no contradiction.
- Production path (SEG-113 fix round): a fixture-tagged BIOS, and a tagged but behaviourally unrelated BIOS (another fixture's BIOS with the tag written in), get no profile, no images, the same build as untagged; the emitter refuses `static-boot:` for them; the test-only path is labelled `test_assumed_fixture`.
- Emitter: wrong offset / destination / length / processor / digest, malformed, zero length, duplicate, changed disc.
- Not applicable or not separately tested: global ceilings were not changed (the 64-image bound is shared and counted, two images per boot); "analysis/evidence exhaustion" has no analogue (the contract is a table lookup and two disc reads, no search); an overlap on one CPU cannot be produced from one contract (one image per CPU) and is covered by the duplicate/overlap checks only.

## 4. Authentic results (T004)

Local authentic discs (Earthworm Jim SE, Sonic CD; both US) and the authentic US Model 1 V1.10 and Model 2 V2.00W BIOS images; cold object caches, 16 jobs, one run per cell, no manual input. A = `--static-boot 0` (exactly the M16 build: its program identity equals the one recorded for M16); B = default.

| | EWJ Compat A | EWJ Compat **B** | EWJ file-first A | EWJ file-first **B** | Sonic Compat A | Sonic Compat **B** | Sonic file-first A | Sonic file-first **B** |
|---|---|---|---|---|---|---|---|---|
| rounds | 7 | **5** | 7 | **5** | 12 | **10** | 7 | **5** |
| images before the first emission | 1 (M16 fragment) | **3** (boot area, boot program, fragment) | same as Compat | 3 | 0 | **2** | 0 | 2 |
| images discovered by execution | 3 (IP, SP, stage) | **1** (stage) | 3 | 1 | 8 | **6** | 3 | 1 |
| native objects built | 74 | 73 | 212 | 211 | 83 | 82 | 257 | 256 |
| generated C / functions | 160.4 MB / 166,222 | identical | 218.5 MB / 157,800 | identical | 158.2 MB / 163,302 | identical | 273.0 MB / 150,085 | identical |
| program size | 47.5 MB | 47.5 MB | 87.3 MB | 87.3 MB | 46.8 MB | 46.8 MB | 119.6 MB | 119.6 MB |
| build wall | 2:52 | **2:21** | 3:39 | **2:54** | 6:10 | **5:31** | 4:42 | **3:39** |
| build CPU (user+sys) | 638 s | 629 s | 1119 s | 1059 s | 838 s | 835 s | 1554 s | 1401 s |
| peak memory | 558 MiB | 558 MiB | 614 MiB | 617 MiB | 830 MiB | 830 MiB | 645 MiB | 645 MiB |
| native instructions (main / sub) | 13,197,846 / 15,335,793 | identical | same | identical | 13,615,134 / 17,670,390 | identical | same | identical |
| final stop | `main_device_unsupported` | same | same | same | `backup_ram_unsupported` | same | same | same |
| final machine state, frame and device digests, device log | - | **identical to A in every report field except the program identity** | - | identical | - | identical | - | identical |
| duplicate / spurious admission | - | none; `contradictions` 0 | - | none | - | none | - | none |

- **Exactly two rounds are eliminated in every configuration** - the round that discovered the main boot area and the round that discovered the sub boot program - and no more: the Earthworm Jim stage round, the epoch round, the two operand-template rounds and the confirming round remain. The Earthworm Jim target "seven to five" is measured, not assumed: 7 -> 5 in both modes. The final gameplay stop remains the main-side Z80-window `main_device_unsupported` frontier.
- The saving is wall time (-18 % to -21 % Compat/file-first Earthworm Jim, -10 % Sonic CD Compat), not CPU: the dominant cost is compiling the final program, which is identical in functions and generated C; each eliminated round only saves the units that round rebuilt.
- Sonic CD and the second BIOS (Model 2 V2.00W, which has its own profile row): the observed placements of the control builds are identical on both discs and both BIOS images (the immediate-operand Word RAM -> Work RAM copy sequence occurs exactly once in each BIOS image), and static builds eliminate the same two rounds (Model 2: Earthworm Jim 7 -> 5, Sonic CD 12 -> 10, machine state identical). Source bindings of later stages, relocated fragments and the M16 fragment are unchanged (the extents list of B is A's minus the two boot images; the stage extent is the same).
- Determinism: B's program identity is identical at 16 jobs (cold) and 3 jobs (warm), and the control's identity equals the M16 build's.
- Fixture matrix: `sega_cd_static_boot_test.py` (61 checks) as in section 3.

## 5. Limits and residual risk

- `conditional`, not `proven`: a wrong profile never executes wrong code (every instruction is compared with the live bytes; a mismatch is a typed stop or an observed fallback) but would cost the rounds it was meant to save; the contradiction counter makes that visible (as a limited observation, section 2).
- Fixture contracts are *assumed*, never verified: they exist only to test the machinery (including deliberately wrong loading behaviour) and are unreachable from the production path.
- The authentic profile covers the exact BIOS images and header forms listed in the table of `boot_contract.cpp`; every other BIOS keeps the 7-round-class behaviour.
- The IP span includes 512 bytes beyond the system area; they are admitted as data-looking code exactly as the observed extent did (admission is not a claim that the bytes are code).
- The M16 recognizer keeps its own, ungated, 0x7E00-byte model of the same main boot area; aligning it with the verified contract is possible but changes its proofs and is not part of this milestone.

## 6. The BIOS identifier

The authentic profile is keyed by the SHA-256 of the exact BIOS image. No BIOS byte, code address or disassembly is committed; the digest is a firmware identifier like the ones emulators ship. The fixture tag is not an identifier of anything authentic and carries no authority outside `--boot-fixture-profiles 1`. The project's evidence convention keeps digests out of *documents*, so the digest appears only in `boot_contract.cpp`, in rows that can be deleted without affecting the fixture profiles. This is flagged for human review in the pull request.

## 7. Independent review

One independent adversarial review (HEAD 6822208): **PASS, no blocker, no major**; `agent_verify.py fast` with the static-boot, producer, reconstruction, native-build, extent-discovery and file-first suites: 200/200 passed. Minor findings, handled: (F1) ADR placeholders filled; (F2) the authentic rows' constants are exercised only locally (both BIOS images against both discs return the expected profile and images) - the exact-form logic itself is covered by fixture profile 3; the disagreement criterion has only the `sp_source` positive test; (F3) the criterion is a report, not a safety mechanism: it uses the largest boot-area end rather than the matching image's, and misses an observed extent covering only the middle of a misplaced image; (F4) the fixture tag is a **self-asserted selector**: a BIOS carrying it gets images proposed, which costs at most wasted rounds (bytes are recomputed from the disc and every instruction is guarded); the tag length is now checked in code.

### Fix round (PR #179 follow-up)

Root causes: (1) `match_profile` accepted a header tag as authority, so any tagged BIOS obtained production static-boot authority - fixed by scoping (`BootProfileScope`): authentic rows only unless the test-only switch is given; (2) `note_boot_disagreement` compared against the maximum source end of all images and required the base to be covered, so a partial overlap was missed and a resolver-less mismatch could not be told from a contradiction - fixed by the per-CPU, per-image three-way comparison above. Review of the fix round: REVIEW2-PLACEHOLDER

## 8. Next investigation: the 48 KiB Earthworm Jim stage (not implemented)

Question: can the existing loader scan's `destination_unknown` refusal be resolved with a small extension, so that the stage's cross-CPU delivery is established before the first emission?

Findings (local disassembly of the disc's SP; classes only): the SP's generic loader is an **ISO9660 file reader**. It reads the volume descriptor and directory sectors through the CDC into PRG RAM buffers and then takes the *LBA and length of the file from the directory record it just parsed* (a record field plus a constant offset), clamps the count, and transfers sector by sector with the destination pointer stored in a memory cell that the BIOS-advanced `A0` rewrites after every sector (retry loop included). The scanner refuses at the sector transfer because that pointer is loop-carried and the LBA/count come from bytes of a disc sector the scanner does not read.

So the stage depends on (1) a **loop-carried pointer** (a stride-aware abstract value or bounded unrolling in the scanner), (2) **disc-resident data used as program input** (directory records: resolving them needs concrete evaluation of the file-lookup code over the immutable disc, i.e. partial evaluation), and (3) the **main-side copy from Word RAM** whose source is sub-delivered (cross-CPU delivery, Word RAM ownership and ordering are not modelled by the M16 recognizer, which refuses it as `source_not_immutable`). None is small; (2) is the real obstacle. **Decision: no small sound solution; keep the M15 fallback.** A future milestone would be "disc-data-aware loader evaluation", with the cross-CPU delivery model as a separate piece. The main-side Z80-window hardware frontier is unrelated.

## 9. Decision

**`STATIC_BOOT_GO`**, with the stated qualification: both authentic boot images (main boot area, sub boot program) are admitted before the first emission under a verified profile, class `conditional` (never `proven`), and **exactly two native build rounds are eliminated** in every measured configuration (Earthworm Jim 7 -> 5 in Compatibility and file-first; Sonic CD 12 -> 10 and 7 -> 5; both verified BIOS images), with identical final machine state, frame and device digests. Any other BIOS or header form keeps the execution-observed route. The Earthworm Jim stage stays an M15 fallback round (section 8). Exact-head CI is recorded in the SEG-113 record.
