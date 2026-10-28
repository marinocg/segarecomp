# ADR 0211: SEG-098-T002 - static module admission in the BIOS-image build and launcher

- Status: Accepted. The pre-registration is the first part (the gate and matrix were fixed in the task record SEG-098-T002 before the implementation); the result and
  the decision follow. Related: ADR 0142 (verified broad AOT, per-instruction byte guards, overlapping variants), 0205 (discovered disc extents), 0206 (the static loader
  scan and the module list), 0210 (the authentic survey and the module-table roots).
- Privacy: project-authored fixtures only; no commercial byte, address or name.

## Context

The BIOS-image profile (the launcher's route) learns executable disc extents only from what a build pass loaded (ADR 0205). A module no pass loads is invisible, however
well the disc proves where it would go. ADR 0206 proved the proof on the synthetic profile; ADR 0210 found that on authentic-shaped SP images the scan must also read the SP
module jump table. T002 connects the two without a second image registry.

## Decision

1. **A new admission kind `static_disc_extent`** (`AdmissionKind`, `--admit static-extent:<cpu>:<track-1 offset>:<base>:<length>`). It is exactly a discovered disc extent
   (ADR 0205: bytes re-read from the verified disc by the emitter, an ordinary image of the BIOS profile with `immutable_input` authority, `byte_identity` verification,
   per-instruction live-byte guard, overlapping images selected by live bytes) except for the proof: there is no pass identity to check, because no pass explained it; the
   source-to-mapping proof is the static scan. Coordinates and the digest of the bytes enter the profile identity (`staticextent ...`), in chain order.
2. **`segarecomp sega-cd build` runs the static scan first** (`discover-modules --module-table-roots --static-extents`: disc + admitted boot images, executes nothing) and
   places its extents at the head of the admission chain, so the identity of every later admission includes them. Per module: the executable-subrange hull (the header or
   trailing data are outside the mapping) at every proven base. `--static-discovery 0` disables the step (control and escape hatch). No module manifest option exists.
3. **Fail closed, never silently partial.** An incomplete scan (a resource limit) admits nothing (`scan_incomplete`); more than 12 static extents (the 16 disc-image slots are
   shared with resolved extents) admits none (`static_extent_bound_exceeded`; T004 revisits the bound); a failed scan leaves the build exactly as before. Status carries
   `static_discovery` {status, complete, extents, refusals by class} and `static_extents`. A resolved extent equal to a static one is not admitted twice.
4. **What is unchanged.** Runtime dispatch only selects among precompiled variants by live bytes; nothing is generated or decoded at run time; both processors' images
   stay distinct; the boot IP and SP, epoch images and operand templates still come from the build pass's evidence.
5. **Assumption stated.** The scan places the SP at $6000 (`boot_images`, ADR 0141: the BIOS-owned placement). A BIOS that places it elsewhere only makes the proposed extents
   never match live bytes (the guard stops, nothing wrong runs).

## Pre-registered matrix and gate (project-authored BIOS-image fixture, `tools/sega_cd_bios_unvisited_fixture.py`)

The fixture BIOS reads the system area through the CDD/CDC (so the delivery ledger covers it), places the disc SP at $6000 and the IP at $FF0000 and enters the SP through its
module jump table; the disc SP loads module A (default), B (same address as A) or C (two mappings) by the machine input `main:d7`; A has a 64-byte non-code header.

| # | Case | Expected |
|---|------|----------|
| 1 | default build | static extents for A, B, C(x2); only the IP and SP are resolved from evidence; build pass idle |
| 2 | same binary, route 2 / 3 | B and C run with no compile; binary unchanged |
| 3 | control `--static-discovery 0` | route 2 stops typed; different program identity |
| 4 | determinism | identical identity, extents and static list |
| 5 | A's subrange | admitted extent starts at +$40 and has the executable length |
| 6 | header write | A still runs (mutable data outside the executable range) |
| 7 | word inside the range overwritten | typed `code_bytes_mismatch`, patched instruction not run |
| 8 | unknown LBA / destination, PRG RAM destination | refusal class reported, B still runs, C fails closed |
| 9 | changed disc | refused as a different disc |
| 10 | no manifest option | usage error |

Gate: `BIOS_PROFILE_UNVISITED_ADMISSION_GO` = rows 1-10 hold through the supported build path with no manifest and no execution-derived admission of B or C;
`PARTIAL` = the positive rows hold but a stated property (identity, subrange, overlay, negatives) fails; `BLOCKED` = the admission cannot be justified with the existing image authority.

## Result

`tests/sega_cd_bios_unvisited_test.py` (28 checks): every row behaved as pre-registered. On the base build the static scan admitted 4 extents before any pass; the build
resolved exactly 2 extents from evidence (IP, SP); B and C ran from the unchanged binary (counts [0,1,0] and [0,0,2], markers equal the oracle); the control build stopped
typed at B. Variation: row 9 is stronger than registered (the program refuses any disc other than the verified one before running). Row 8's unknown-LBA variant ends in the loader's
own never-satisfied transfer wait (`budget_exhausted`), also with no execution of C.

## Decision: `BIOS_PROFILE_UNVISITED_ADMISSION_GO` (for project-authored BIOS-image discs)

The supported build admits a statically proven, never-visited module with the existing guarded image machinery. Not claimed: any authentic title benefit (ADR 0210 found 0 static
modules on the authentic disc: its loaders go through BIOS services and table dispatch the scan does not yet model - T003); module counts above 12 (T004).
