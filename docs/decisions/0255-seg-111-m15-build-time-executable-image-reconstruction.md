# ADR 0255: SEG-111 (M15) - build-time executable image reconstruction (relocated fragments and produced code)

- Status: Accepted (T001 contract). Milestone SEG-111 (M15). Extends ADR 0254 (which scoped M15 to relocation only; its history is not rewritten). Related: ADR 0073 (bounded build-time materialization), 0076 (executable-image authority), 0142 (verified broad AOT, materialized RAM images), 0178 (provenance: causal copying versus coincidence), 0205 (reference-independent extent discovery), 0221-0225 (file-first AOT, dynamic binding, authentic evaluation).
- Privacy: classes, counts, sizes and times only. No address, offset, hash, name or byte of a disc, BIOS or RAM of a commercial title appears in any durable text. Authentic runs and captures stay local and ephemeral.
- Scope extension (explicit): ADR 0254 limited M15 to short relocated fragments. M15 also gates a bounded **produced-code** capability (bytes computed by verified guest code from known inputs, e.g. XOR/table decoding) as a second producer class, through the same admission pipeline. The produced-code part is a feasibility gate (decision B in the milestone brief); relocation (gate A) is the critical path and is never weakened for it.

## 1. What is established today (design questions 1-7)

1. **Where authority is established.** An executable image is an `ExecutableImage` of the generic contract (`libs/recompiler/executable_image.hpp`): bytes, mapping, provenance authority (`immutable_input`, `static_proof`, `bounded_build_time_materialization`) and verification (`byte_identity`). The Sega CD profile builds the set in `platforms/sega-cd/machine/src/bios_profile.cpp` from the BIOS, admitted disc extents and admitted epoch images. Admissions are an ordered, pass-identity-chained list: an admission is accepted only if its `pass_identity` equals the identity of the program built from the admissions before it (`provenance_mismatch` otherwise).
2. **A verified relocated copy needs no new form.** If the copied bytes are an exact span of delivered disc bytes, the existing `extent:<pass>:<cpu>:<disc offset>:<base>:<length>` admission is exactly right: bytes are re-read from the verified disc by the emitter, the program identity includes their digest, every compiled body begins with the live-byte guard.
3. **Bytes that are not a disc span.** The existing materialized authority (`bounded_build_time_materialization`, used by the sub epoch image) already means "bytes observed by a build pass running generated-native code, admitted under the pass identity, verified at run time by byte identity". It is not reusable as is: the epoch admission is a whole PRG-RAM snapshot bound to the sub's runnable-epoch hash check. A produced image needs one new, minimal admission kind carrying processor, base and the bytes, mapped onto the same authority and verification.
4. **Epoch machinery reuse.** The capture/admit/restart loop, the frontier capture (`capture.json`, `region.bin`), pass-identity binding, object cache, variant selection and the confirming run are reused. The epoch snapshot itself is not (item 3).
5. **Capture-to-pass association.** `capture.json` carries the identity of the program that ran the pass; the driver refuses a capture with another identity; the admission string carries it; the emitter re-derives and compares it. Evidence from round N can only be admitted into the chain at position N.
6. **Runtime identity and byte checks.** Unchanged: the dispatcher selects only among precompiled bodies; each body re-checks the live bytes of its instruction (`scd_code_guard`) before it retires; a mismatch is the typed stop `code_bytes_mismatch` before any side effect. The disc digest is verified at start-up.
7. **The producer is verified by construction.** The generated program contains only compiled bodies; there is no interpreter or other execution path. Every instruction that wrote a byte was a compiled instruction whose live bytes had just been compared with an admitted image. "Unverified writer" therefore cannot occur in a generated program; the stream record keeps the writer PC so that the proof is explicit, and the driver refuses a writer CPU that is not the frontier CPU (see 8).

## 2. Decisions

### Evidence: a bounded stream table in the build pass (T002)

Only when the pass is a materialization pass (`--materialize-dir`), the runtime attaches a fixed 8-slot stream table (<= 4 KiB per stream, ~34 KiB total). It is a pure observer on the existing bus entry points `scd_read` / `scd_write` plus the exception-frame writer; it cannot alter state, timing or ordering, and a null pointer test is its entire cost when absent (final programs and ordinary runs never attach it).

- **Instruction-instance identity**: (processor, retired-instruction count). `scd_retire` increments the count after the instruction, so every bus cycle of one instruction sees the same value; a repeated writer PC in a loop is a different instance each iteration.
- **Causal pairing** (not equality): a write is *paired* only if, in the same instruction instance, the processor performed exactly one data read of the same width whose value equals the written value and whose cell differs from the written cell. Memory-to-memory moves qualify (`move.<b|w|l> (An)+,(Am)+`, absolute, indirect). Register-staged copies, read-modify-write, instructions with several data reads, and computed values do not pair; they are *unpaired* writes.
- **Streams**: ascending contiguous writes of one writer PC by one processor into one RAM (main Work RAM by offset, which normalises the mirror; PRG RAM by offset). A paired stream also requires a contiguous source. Records keep writer CPU and PC, destination, source address/class, bytes, and flags: `unpaired`, `source_gap`, `clobbered` (any later write, including exception frames, touched its bytes), `dropped` (a write-protect drop: the live byte differs from the written one), `too_large`, `volatile` (a device register was read between its first and last write). Table pressure is counted (`evicted`); eviction prefers free, then clobbered, then oldest slots.
- Supported destinations: main Work RAM and sub PRG RAM. Word RAM, Z80 RAM and unlogged writers (in this machine model the only RAM writers are the bus and the exception-frame writer; the modelled CDC DMA targets Word RAM only, so a PRG-RAM DMA target is an explicit unsupported writer, not a silent one) are `unsupported`. Main writes into the PRG-RAM window are recorded (they clobber) but are never a relocation stream for a sub frontier (different visibility rules).
- The report in `capture.json` is versioned (`"streams":{"version":1,...}`), validated by the driver; absence means "no evidence". Nothing from it enters `status.json` except counts and refusal classes.

### Class A: automatic short-fragment relocation (T003) - existing `extent:`

Order of the driver's frontier handling is unchanged for templates and the ordinary exact-source resolver (its 32/64-byte floors are not touched). **Only when that resolver refuses with `no_source` or `too_short`**, a stream candidate is derived by the C++ build driver (library function `derive_relocation`, also exposed as `segarecomp sega-cd derive-relocation` so that the validated disc reader is used and the function is testable):

1. Exactly one stream owns the frontier PC; its writer CPU is the frontier CPU; the frontier RAM is supported; the stream is not `unpaired`/`source_gap`/`clobbered`/`dropped`/`too_large`/`volatile`; the table did not overflow in a way that could hide a writer (`evicted == 0` or the owner was found; an absent owner with evictions is `stream_evicted`).
2. The stream equals the live window at the frontier byte for byte (no later change).
3. The source range lies wholly inside **one admitted disc extent of the same CPU** (the driver passes its extent list: discovered, static and manual); the disc offset is `extent.offset + (source - extent.base)`. The bytes at that disc offset (read with the validated reader, honouring track offsets) must equal the stream. This is the "source bytes are within a verified admitted image and equal what the paired read observed" proof; the delivery ledger is *not* needed (the extent itself was admitted from delivered blocks or a static proof) but a source not covered by an extent is `source_not_admitted`, and a source inside an extent whose disc bytes differ is `source_modified`.
4. The result is trimmed to an even base and even length that cover the owned stream, `>= 4` bytes, and is admitted as `extent:<pass>:<cpu>:<offset>:<base>:<length>`. An identical extent already admitted is `no_progress`.
5. Otherwise the typed refusal is recorded as the frontier (`relocation_*` refusals) and the original `no_source` is preserved alongside it.

Same instruction instance + same width + value equality + a source cell inside an admitted, digest-verified extent is what makes this a causal copy of disc bytes; byte equality at a unique disc location is never sufficient.

### Class B: produced code (T004) - one new admission, same authority

Decision gate B. A stream that is `unpaired` (computed by verified code) and otherwise clean may be admitted as a **produced image** when:

- one stream owns the frontier PC, writer CPU = frontier CPU, supported RAM, `<= 4 KiB`, not clobbered/dropped/volatile/too_large, stream bytes == live window, even base/length;
- the producer's inputs were not device-sourced during production (`volatile` clear); the pass is deterministic by construction (no wall clock, fixed inputs; the build's input script/backup RAM are build-identity inputs) and the **confirming run must reproduce the capture byte for byte** (the confirm check is tightened from "capture present" to "capture identical");
- the capture epoch is the existing fail-closed frontier (first attempted fetch of unadmitted code), and the image boundary is the *produced stream*, never the surrounding RAM.

Admission: `produced:<pass>:<cpu>:<base>:<snapshot file>`; the bytes are read from the ignored build directory like an epoch snapshot; the emitter validates processor/RAM/even/length bounds, the pass identity (chain), adds the bytes' digest to the program identity and creates an `ExecutableImage` with `bounded_build_time_materialization` authority and `byte_identity` verification, producer `sega_cd.produced_image`. Everything downstream (decode, lowering, C11 bodies, guards, variants, object cache) is the existing path. No disc offset is fabricated. Runtime recompilation, transformation or snapshots are not performed.

What this does and does not claim: the admission records that, in the build's declared deterministic input environment, verified code produced exactly these bytes at this epoch. Execution safety is still the per-instruction guard: a final run whose live bytes differ (other input, other disc) stops with `code_bytes_mismatch` before the instruction retires. An arbitrary RAM snapshot (no stream owns the PC, an unowned or multi-writer window, a clobbered stream, an overflow) is never admitted.

### Option A versus Option B (simplicity review)

Option A (extend admission and capture with small producer evidence) is chosen. Code evidence: relocation reduces to the existing `extent:` path plus a derivation function; produced code needs one admission kind (`AdmissionKind::produced_image`, about 30 lines in `bios_profile.cpp`, 15 in the CLI parser); the observer is one runtime file. A common "image producer" layer would duplicate the admission list, the identity chain and the confirm logic without any behavioural gain, so it is rejected. There is no second runner, instruction engine, IR or plugin interface.

## 3. Remaining design questions (8-13)

8. **Final versus intermediate buffer.** A stream is the *final live owner* of its bytes at the epoch (`clobbered` clear, equal to the live window). Whether a producer has finished is not known; the build never claims more than the owned contiguous stream. A later write that changes any byte stops being a clean claim and is refused; a producer that continues past the epoch is handled by the existing frontier loop (the next unadmitted fetch is the next frontier, a new variant, bounded by `kMaxExtents` and the round bound).
9. **Input paths.** Different inputs can yield different images. The build admits the image its declared deterministic inputs produce; `volatile` (device-register reads inside the production window) refuses the obvious input dependence; anything else that differs at run time fails the byte guard. Two images at one address coexist as variants (existing behaviour).
10. **Overlap.** Previously admitted and produced images overlap only through the existing per-instruction variant selection (newest first, live-byte verified); no overlap check weakens the guard.
11. **Ambiguity** is refused: more than one stream owning the PC (cannot happen for equal ownership; two writers interleaved into one byte are `clobbered`), a source straddling extents (`source_not_admitted`), a source read that does not match the written value (unpaired), several data reads (unpaired), mirror aliases (normalised to one cell; the PC alias must be the canonical window), multiple matching extents covering the same source range (the extent list is deduplicated; two *different* extents covering the source with different disc offsets are `ambiguous_source`).
12. **Counts, bounds, identity.** Extents count against the existing `kMaxExtents`/`kMaxDiscImages`; produced images against a small separate bound (8). Each admission is one fixed-point round exactly as today; the program identity includes each admission (coordinates + digest). No dedicated discovery, provenance or transformation round exists: evidence is collected in the normal pass and resolved at the existing frontier. Total rounds are measured and reported (T006).
13. **Unsupported (explicit).** Word RAM / Z80 RAM destinations; register-staged, read-modify-write and multi-read copies as relocation; computed values as exact disc extents; streams > 4 KiB; interleaved or overwritten streams; device-sourced production; ambiguous or unadmitted sources; decompressor libraries; whole-RAM scanning; any runtime compilation.

## 4. Component-level change plan

| Component | Change |
|---|---|
| `platforms/sega-cd/runtime/scd_reloc.c` (new), `scd_runtime.h`, `scd_machine.c`, `scd_main.c` | stream-table observer; hooks in `scd_read`/`scd_write`/frame writer; attach on `--materialize-dir`; `streams` object in `capture.json` |
| `platforms/sega-cd/machine/{include,src}/relocation.{hpp,cpp}` (new) | `derive_relocation` (Class A) and the produced-image check (Class B), pure functions over parsed evidence + `DiscImage`, typed refusals |
| `apps/segarecomp/sega_cd_command.cpp` | `sega-cd derive-relocation` subcommand; `produced:` admission syntax |
| `apps/segarecomp/sega_cd_build.cpp` | after the unchanged template/resolve steps, relocation then produced candidate; tightened confirm check; round count in status |
| `platforms/sega-cd/machine/src/bios_profile.cpp`, `bios_profile.hpp` | `AdmissionKind::produced_image`, identity text, `ExecutableImage` creation |
| `tools/sega_cd_bios_loader_fixture.py` | `relocated_*` cases unchanged; `decoded*` produced-code cases and relocation negatives |
| `tests/sega_cd_relocation_test.py`, `tests/sega_cd_produced_image_test.py` (new), `tests/sega_cd_derived_image_test.py` | native-build tests; the M14 diagnostic stays a differential oracle |
| docs | this ADR; commercial-games doc unchanged (aggregates only) |

The Python builder (`tools/sega_cd_bios_build.py`) is untouched and remains a test twin; the native build is the only production path.

## 5. Falsifiable acceptance criteria

- **A1** Fixture `relocated`: the native `sega-cd build` without any manual extent explains the `no_source` frontier, admits one `extent:`, runs the fragment to its marker, and the program identity equals that of the M14 manual-extent build of the same extent.
- **A2** Authentic Earthworm Jim SE (Compatibility and `--file-first structural`, no manual input): the old `entry_not_compiled`/`no_source` stop no longer occurs at that frontier; the build reaches the main-side Z80-window `main_device_unsupported` stop; the two modes are machine-equal; builds at 4 and 16 jobs have identical program identity.
- **A3** Negatives (each: typed refusal, wrong instruction never retires, no stale image, deterministic evidence): equal bytes without causal read; register-staged copy; source outside any admitted extent; source modified (disc differs); destination overwritten; incomplete stream; interleaved/overlapping; multiple readers; stream over 4 KiB; evicted history; Word RAM destination; main writer for a sub frontier; wrong pass identity; wrong disc; repeated proposal with no progress.
- **B1** Fixture `decoded` (XOR) and a second small transform (add-constant) run through admission `produced:` with no pre-supplied output image; bytes differ from every disc span; the final program executes the produced body; marker equals the independent oracle; repeated builds give identical identity.
- **B2** Negatives: corrupted payload; partial output; changed after capture; producer code changed; device-dependent output (`volatile`); output beyond RAM; arbitrary RAM bytes with no stream; different pass identity; runtime byte mismatch stops before retirement.
- **R** The observer is inert when absent (final programs unchanged: identical generated C and program identity for every existing case; measured null-check cost); existing Sega CD tests and Sonic CD routes are unchanged and admit no spurious relocation.

## 6. Results

Filled in by checkpoints T003-T007 (below).
