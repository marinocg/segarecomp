# ADR 0206: SEG-097 - whole-game static AOT, milestone 1: unvisited-module discovery, source-relative execution, experimental M3

- Status: Pre-registration accepted (written and committed before the first run of the T001 matrix). The T001 result and decision are appended
  under "Result" when the matrix has been run. T002..T004 add their own sections.
- Related: ADR 0049 (Genesis copy alias), 0076 (per-image authority), 0102/0103/0109/0137 (frozen M3, B35, policy identities, Genesis Optimized),
  0126/0127/0132 (Sega CD images per processor, overlays, module lists), 0131/0133/0140 (M3 reuse and transfer), 0142 (verified broad AOT),
  0179/0180 (static image reconstruction, jump-slot templates), 0204/0205 (launcher, reference-independent extent discovery).
- Privacy: no BIOS, disc or ROM byte of a commercial title appears here or in any test. Every fixture is project-authored. Any local commercial
  regression control reports classes and counts only.

## Context

Executable-image authority for Sega CD discs comes from three producers: boot IP/SP, the digest-checked module list (ADR 0132) and extents
resolved from what a build pass actually loaded (ADR 0205). The third reaches only code the console executed on the observed route; a module that
gameplay has not yet loaded is invisible. The roadmap needs the opposite capability: find a future module offline, from evidence that exists before
the player's choice, without executing it, without loading it in a build pass and without a hand-written module manifest.

Four concerns stay separate throughout this milestone: **executable-image authority** (which disc bytes may execute, where, and how that is
proven), **instruction admission** (which source-relative offsets are compiled), **generated-code placement** (how many bodies, at which names) and
**runtime dispatch** (which compiled body runs at a guest PC after live-byte verification). T001 is only about the first; it produces the existing
module list, which the unchanged admission/placement/dispatch consume.

## Decision (mechanism under test, fixed before the matrix was run)

A static **loader-operation scan** (`platforms/sega-cd/machine/src/loader_scan.cpp`, `segarecomp sega-cd discover-modules`):

1. **Input.** The disc and nothing else: the admitted boot images (IP/SP, ADR 0126) of the disc itself. No input script, no run, no trace, no
   coordinates, no module list. (The CLI has no such option; the test asserts that discovery executes no guest instruction and reads no disc sector
   other than the boot images and the extents it later digests.)
2. **Walk.** The sub processor's boot image is explored from its entry by a bounded, path-sensitive constant-propagation walk over the CPU-owned
   decode/lift/effect/control-successor APIs (no new decoder, no new instruction semantics). Both arms of every conditional branch are explored
   (that is what makes a module selected by runtime input visible). Calls to constant targets are inlined with a bounded depth so register arguments
   reach the callee. Registers hold a constant or Unknown; every instruction whose effect the walk does not model sets its full architectural write
   footprint (`m68k_operation_effect`) to Unknown; memory is not modelled except the machine's CD hardware registers.
3. **Recognised loader operation** (a property of the machine's documented device model, `docs/architecture/sega-cd-machine-contract.md`, not of any
   disc): a CDD play command with a valid checksum and known MSF written as the five TX words with the word write to `$FF804A`; the CDC destination
   selected as Word RAM; DBC set to a constant transfer of one 2,048-byte user sector; a DMA address written; and the DTRG write that starts the
   transfer. The operation yields (disc LBA of track 1, byte count, processor, destination = Word RAM sub view base + 8 x DMA address). Any operand
   that is Unknown at the trigger is a typed refusal naming it (`lba_unknown`, `destination_unknown`, `length_unsupported`, `destination_unsupported`,
   `cdd_checksum_invalid`, `word_ram_mode_unsupported`); nothing is guessed.
4. **Entry proof.** A module needs an executable claim, not only a load. A load operation becomes a *module candidate* only if, later on the same
   path, a control transfer (JSR/JMP/BSR/BRA with a constant or constant-register target) lands inside the loaded range. Those targets are the entry
   roots. A load never followed by such a transfer on any path is reported as `loaded_not_executed` (data) and admits nothing; this is the decoy and
   data-only discriminator. A computed transfer with Unknown target proves nothing (`entry_unknown`). A broad-AOT policy for modules without roots is
   deliberately *not* introduced in M1 (recorded as a M2 candidate).
5. **Executable subrange.** From the entry roots, the module's own bytes (read from the disc extent at build time) are traversed with the same
   control successors the emitter uses; the permitted executable subrange is the hull `[lowest reachable instruction, end of the highest reachable
   instruction)`, rounded to even. Bytes outside it (a file header, trailing data, sector padding) are never executable authority. A new optional
   manifest directive `range <offset> <length>` carries that subrange to the unchanged `ImageMapping.first_offset/length`.
6. **Output.** A module list in the existing format (`segarecomp.sega_cd.modules.v1`) with extent, SHA-256, owner, bases, entry roots and
   `byte_identity` verification, plus a typed report. The list is then admitted by the **unchanged** `admit_modules` (digest, extent, executable RAM
   region and image-set validation re-checked), so discovery can only propose; admission and runtime verification (per-instruction byte guard and
   whole-range granule digests) remain the authority. Several operations with the same LBA and different destinations form one image with several
   mappings; several images at one base form an overlay group (ADR 0132).
7. **Bounds.** At most 64 load operations, 32 modules, 4,096 walked states, call depth 8, 100,000 steps; any exhaustion is the typed
   `scan_limit_exceeded` and the result is marked `complete=false` (never "whole-game complete"). Limits are resource controls, not evidence.
8. **Identity.** The discovered list text is canonical and deterministic (sorted); its SHA-256 enters the program identity through the existing
   module-list identity. Two builds of the same disc give byte-identical lists.

Not claimed: that the scan finds loaders that use BIOS file services, tables, computed LBAs, decompression, PRG RAM or PCM destinations, or loaders
in modules (nested). Those are refused or invisible and are M2 scope. The DAC (buffer start) is assumed to be the sector's user data when the
transfer is one full sector (the machine contract's `PT + 4` idiom); a loader that points it elsewhere yields bytes that fail the runtime digest
check and stops fail-closed, never executes other bytes.

## Pre-registered synthetic matrix (project-authored; `tools/sega_cd_fixture_disc.py` fixture `unvisited`)

Modules: A (sector with a 64-byte non-code header; code at +0x40), B (code from +0), C (loaded at two Word RAM bases), D (loaded as data, never
executed), X (instruction-like decoy sector, never referenced). The main program reads port 1; the sub waits for the choice and loads A (default), B
(button B) or C (button C) with a shared register-argument loader routine.

| # | Case | Expected |
|---|------|----------|
| 1 | default build; default input | discovery lists A, B, C from the disc alone; the program built from that list runs A; B and C never executed |
| 2 | same binary, input selects B (never visited on the first route) | B runs, correct marker, no compile after start |
| 3 | A and B both at `$080000` | one overlay group, two candidates, each selected by live bytes |
| 4 | A has a header | executable subrange starts at the entry; header bytes outside the mapping; list `range` != whole extent |
| 5 | C at two bases | one image, two mappings, both verified, both run |
| 6 | decoy X | not in the list |
| 7 | D loaded, not executed | `loaded_not_executed`, not in the list |
| 8 | destination from an input-dependent register | refusal `destination_unknown`, nothing admitted for that load |
| 9 | LBA from an input-dependent register | refusal `lba_unknown` |
| 10 | PRG RAM destination | refusal `destination_unsupported` |
| 11 | wrong digest in the discovered list | `digest_mismatch` at emit |
| 12 | live bytes of the executable range patched before the jump | typed stop `image_bytes_mismatch`/`code_bytes_mismatch`, patched instruction not executed |
| 13 | decoy bytes loaded where the entry expects a different module | other module's identity selected, or typed stop; never a mixture |
| 14 | two discoveries of one disc | byte-identical list and report |
| 15 | discovery executes nothing | no guest instruction, no run, no disc read beyond the boot images and the digest reads |

Controls (non-claims): no module is named by a filename or a magic string; no ISO9660 name is consulted; the oracle's expected layout is read
only by the test and never passed to the tool.

## Decision gate (pre-registered)

- `STATIC_UNVISITED_DISCOVERY_GO`: every row above behaves as expected; B (and C) are compiled into the binary before they were ever executed or
  observed and then execute from that binary; the proof is source -> mapping from the loader's own operations, not from a trace, a name or a manual list.
- `STATIC_UNVISITED_DISCOVERY_PARTIAL`: the positive rows hold but a stated sub-capability fails (e.g. only one mapping per module, or no subrange).
- `STATIC_UNVISITED_DISCOVERY_BLOCKED`: the positive rows cannot be made to pass without a broad symbolic-execution framework or fixture-specific
  logic. The evidence limitation is recorded and this branch of the implementation stops; the manual list is not substituted.

## Result (T001)

Run: `tests/sega_cd_unvisited_discovery_test.py` (40 checks, CI test `sega_cd_unvisited_discovery_test`), `tests/sega_cd_modules_build_test.py` (new `range`
rows) and the existing module/overlay/executable-image suites. Every positive and negative row behaved as pre-registered except the deviations listed
below; nothing was guessed.

| # | Result |
|---|--------|
| 1 | discovery of the base disc: A, B, C from the disc alone (exit 0, complete); the default route runs A only (`[1,0,0]`, marker A) |
| 2 | the same binary, run with the machine input selecting B, executes B (`[0,1,0]`, marker B); selecting C executes C at both bases (`[0,0,2]`); the binary is byte-identical before and after |
| 3 | A and B are two candidates of one overlay group at `$080000` (A's subrange starts at `$080040`); the existing selector chooses by live bytes |
| 4 | A: executable subrange `[0x40, 0x5C)`; its 64-byte header is outside the mapping, and a loader write into the header leaves A running (`header_patched`) |
| 5 | C: one image, two verified mappings (`$090000`, `$0A0000`), both run |
| 6 | decoy X is neither an operation nor a module |
| 7 | D (loaded, never executed): `loaded_not_executed`, admits nothing |
| 8-10 | `destination_unknown`, `lba_unknown`, `destination_unsupported`: only the unprovable load is refused; the independent loads and the other mapping of C stay discovered |
| 11 | a list whose digest names other bytes: `digest_mismatch`; another disc's list: `disc_identity_mismatch` (the unchanged admission) |
| 12 | a word of the executable range changed before the call: typed stop `image_bytes_mismatch`, no instruction of the wrong image retired |
| 13 | not repeated here: a mixture of two images is the existing `image_partially_overwritten` row of `sega_cd_overlay_test` (ADR 0132); T001 adds no selection logic |
| 14 | two discoveries and two emits from the discovered list: byte-identical list, identity, report, program identity and generated C |
| 15 | discovery reads no sector beyond the boot images and the proven ones: changing unrelated sectors (and the decoy) changes nothing but the disc identity |
| bounds | unbounded recursion ends in `scan_limit_exceeded`, `complete=false`, no module proposed (exit 9) |

Deviations from the pre-registration, each conservative or presentational: (a) the runtime input is the machine input `--set main:d7=N`, because the synthetic
post-BIOS profile maps no controller port (the controller-driven variant needs the BIOS-image profile, below); (b) DOUTEN unknown at DTRG proposes the load
instead of refusing (a proposal can only fail closed at run time; a known-clear DOUTEN starts nothing), so `cdc_state_unknown` is reserved for an unknown register
pointer and is not currently emitted; (c) the permitted tool budget is 4,096 states / 100,000 steps as registered; the fixture uses 79 states and 493 steps.

## Decision (T001): `STATIC_UNVISITED_DISCOVERY_GO`, for a stated scope

Modules B and C were admitted, compiled into the binary and verified byte-for-byte against the disc before any run had loaded or executed them, from the
sub boot image's own CD-load operations, without a manifest, trace, file name or coordinate given to the tool. The mechanism is not a broad symbolic-execution
framework: it is a bounded path-sensitive constant walk over the existing CPU-owned decode/lift/effect/control APIs, a model of the machine's documented CD
interface, and the existing module-list admission. The generalizable parts are: the source-to-mapping proof (LBA -> destination), the entry proof, the
executable-subrange hull and the separation of proposal from admission.

What is NOT claimed (the stated scope): loaders that call BIOS file/CD services (the BIOS-image profile, where the module list is not wired), tables,
computed LBAs/destinations, partial-sector or multi-sector transfers, nested loaders (the walk does not enter loaded code), interrupt-driven loaders, PRG RAM or
PCM destinations, decompression. The 2M Word RAM mode is assumed (its mode bit is main-owned); a 1M-mode loader would produce bytes that fail the runtime digest
check. Nothing here says any commercial title's unvisited modules are found; that is the M2 question. Open limits are carried to SEG-097.
