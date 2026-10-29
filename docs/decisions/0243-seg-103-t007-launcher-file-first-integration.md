# ADR 0243: SEG-103-T007 - File-first (structural) in the Sega CD launcher (addendum to the M7 PR)

- Status: Accepted. Addendum to SEG-103 (M7): a consumer integration authorised on the existing PR. It does not alter ADR 0238-0242 or the evidence behind `M7_GO`; the DMA work is unchanged.
- Related: ADR 0205 (launcher build), 0208 (experimental policy), 0221-0224 (file-first), 0229 (M5 authentic multi-route).

## Problem

The M5-M7 authentic evaluations used `segarecomp sega-cd build --file-first structural`; the launcher's Build did not: it offered Compatibility and Optimized only. A game whose gameplay code is behind a START press therefore built a program that stopped with `entry_not_compiled` in the launcher although the product can compile it.

## Decision

Connect the existing product option; add no discovery mechanism, pipeline or mode of execution.

1. **One selection value.** `scd::EntryRecord` keeps `experimental_m3` (Optimized, earlier release) and adds `file_first`; `select_build_mode` makes them exclusive and `ScdBuildMode {compatibility, file_first, optimized}` is the only thing a build request carries, so the pair the product CLI refuses cannot be formed. A hand-edited record holding both reads as Optimized (the older choice). Existing records are unchanged and written byte-identically unless a new member is set.
2. **Selected, built, effective are separate.** Selected = the stored choice for the next Build. Built = `program_built_mode` (`compatibility|file_first|optimized`, plus `program_file_first_inventory`), set only from a launcher build's structured `status.json` (`file_first` object present => file-first; `aot_policy` requested optimized and effective selective => optimized; else compatibility) and kept for the same program on re-verification. An imported program, or one from an earlier release, is "not recorded" (a record with only `program_experimental` migrates to `optimized`). A file-first build is Compatibility under `aot_policy` and file-first under `file_first`; the launcher reads the two independently and never infers either from the request, the file name or a checkbox.
3. **Fail closed.** A file-first request whose product result carries no `file_first` inventory is not published (typed message). A product failure (for example a disc with no ISO9660 volume, `stage:file_first error:iso9660`) is reported as "File-first build failed: <cause>. Nothing was substituted; choose Compatibility..." - no automatic fallback. A failed or cancelled attempt never replaces the previous completed build (unchanged `.partial`/`.old` publishing).
4. **Surfaces.** GUI: a third tile, "File-first (structural)", marked Recommended; existing games keep their stored selection and new games start with Compatibility (the recommendation is visible, nothing is changed silently); narrow windows use shorter titles. After a Compatibility-or-unknown program stops at an uncompiled entry the page offers "Rebuild with File-first" and says transformed or unavailable code is not fixed by it; after a file-first program does, it says the code is probably not in an eligible file. Headless: `--build --file-first structural`, exclusive with `--aot-policy optimized`, requires `--build`; reports `build_mode_requested`, `build_mode_effective`, `file_first_inventory`.
5. **Test seam unchanged.** `SEGARECOMP_SCD_BUILD_ARGS` stays a developer seam.

An incidental fix found while testing: the catalog clamped a negative program import stamp to 0 on load (a libstdc++ whose file clock epoch is not Unix's gives negative seconds), so a reopened launcher read a built program as changed ("Stale program") and refused to rebuild it. The stamp now round-trips (row in `launcher_sega_cd_test`).

## Evidence (project-authored fixtures, privacy-safe)

`tools/sega_cd_bios_unvisited_fixture.py ... --case unknown_lba --iso --pad 3`: a BIOS-image + ISO9660 disc whose game loads module C (from a load position no static analysis can know) only after START on pad 1, which the no-input build never presses. Through the launcher in a staged installation with the real product CLI, with the same input script:
- legacy Compatibility build: `build_mode_requested/effective = compatibility`, the replay stops `entry_not_compiled`, result words zero (the module never ran, no authority guessed);
- `--build --file-first structural`: requested = effective = `file_first`, `file_first_inventory=complete`, status.json carries `file_first` while `aot_policy` stays Compatibility; the replay runs the module at both bases the loader chose (result words equal the fixture's marker), 2 verified dynamic bindings, no refusal; the stored program is byte-identical to the verified one afterwards; the library stores `file_first` and built mode `file_first/complete`;
- `--file-first` with `--aot-policy optimized`, and without `--build`: usage refusals (exit 2); a disc without an ISO9660 volume: exit 7, "File-first build failed ... ISO9660 ...", nothing attached; Compatibility then builds the same disc.
GUI window tests: Compatibility build, selection changed to File-first leaves the built mode `compatibility` until a rebuild yields `file_first`/`complete`; a failed file-first rebuild leaves the earlier program byte-identical.
Unit tests: command vectors for all three modes (and no option overlap), `file_first` read independently of `aot_policy`, an incomplete inventory never described as complete, persistence/migration/hand-edit rows.

Authentic Sonic CD through the launcher was **not** run here (the launcher needs the packaged toolchain); the product-level authentic evidence is ADR 0227-0240.

## Limits

The inventory is structural and bounded (oversize, duplicate and structureless files are not compiled); file-first can not compile code the disc does not contain verbatim; there is no sound; the headless path stores no import stamp refresh of its own (a first launch refreshes it, as before).
