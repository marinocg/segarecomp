# ADR 0216: SEG-099-T002 - bounded work-variable state and static source-to-mapping proofs for BIOS sector-read loaders

- Status: Accepted (pre-registration, then result and decision). Milestone SEG-099 (M3). Related: ADR 0206 (the static scan), 0211 (static admission), 0212 (constant-argument dispatch), 0215 (the BIOS CD-service ABI).
- Privacy: project-authored fixtures; authentic measurements are counts and classes only.

## Question and pre-registered rule

Can the existing CPU-owned scan prove a module's source and destination when its load arguments are carried through a RAM work area and the documented BIOS CD services, and
can the supported build then compile that module before it ever runs? Written before the authentic comparison; the matrix was fixed before the fixtures were run.

Option `work_state` (`discover-modules --work-state`; the build enables it). It extends the existing walk; there is no second interpreter and no CPU semantics are duplicated
(arithmetic, comparison and condition truth tables are the CPU-owned `m68k_evaluate_*` / `m68k_evaluate_condition` / `m68k_evaluate_dbcc_decrement`):

1. **Abstract RAM**: known bytes of sub PRG RAM / the 2M Word RAM view in copy-on-write 256-byte pages (at most 128 pages per state; beyond that bytes are forgotten, never invented).
   A byte is known only if a modelled instruction stored a known value, a documented service stored disc content, or it is an unmodified byte of the admitted boot image (image bytes are read for non-PC-relative addresses
   only until an unknown/forgotten write may have changed the image). A store through an unknown pointer, an instruction the model does not understand that stores to memory, a foreign call or an unmodelled service forgets
   everything (`unknown alias`). Hardware shadow semantics of M2 are unchanged; a store to a known address outside the Gate Array no longer discards the CD-interface shadow.
2. **Condition codes**: N/Z/V/C known when produced by MOVE/MOVEQ/CLR/TST/CMP*/ADD*/SUB* (and ADDQ/SUBQ) from known operands; `Bcc` / `DBcc` take one arm when every assignment of the unknown flags agrees (the CPU-owned
   truth table), both arms otherwise. Unmodelled instructions that may change them make them unknown.
3. **Documented BIOS services** (ADR 0215), `JSR $5F22` with D0.W known: ROMREADN / ROMREAD read `{sector,count}` (or the sector) from tracked memory at A0; CDCSTOP; CDCSTAT; CDCREAD; CDCACK advances the read position;
   **CDCTRN** with the sub-read device byte (`$FF8004` bits 2-0 = 3, as written by the program) and a position that is a constant transfers exactly one sector to the constant destination A0, the sector's disc bytes becoming known memory (so title code that
   parses the ISO9660 descriptor and directory is *evaluated* over validated disc content; nothing is looked up by name). The registers D0, D1, A0, A1 are clobbered; D2-D7/A2-A6 are preserved (ABI). Services whose two sources agree that they touch no caller memory
   (MSC*, DRV*, CDB*, FDR*, ROMSEEK, CDCSTART, CDCSETMODE) clobber the same registers and forget the read position. Every other function number, every other BIOS vector (`$5F10`, `$5F16`, ...) and an unknown D0 forgets all
   abstract state. Destinations must be sub PRG RAM above the BIOS area or the Word RAM window, and must not overlap the boot image.
4. **Runs**: consecutive sector transfers (disc and memory both +1 sector) form one image; the module is the run, with the union of the entries proven inside it.
5. **Asynchronous entries**: the SP module's interrupt and user entries are walked as roots of their own; whatever they write is unknown to the init and main entries (a store through an unknown pointer or an unmodelled call in them makes all tracked memory unknown there). The walk repeats until the assumed set covers what the asynchronous entries write (at most four passes).
6. Limits are the existing ones. Exhaustion is `scan_limit_exceeded`, nothing is admitted.

Assumptions (stated, not proven; every proposal is still byte-verified against live bytes at run time, so a wrong assumption can only waste compilation, never run wrong code): the stack does not alias tracked memory; the services keep the device byte the program selected; the CDC delivers consecutive sectors from the ROMREADN
position (the program's own header check is not evaluated); the main processor does not write the tracked sub memory; the Word RAM is in 2M mode.

| # | Case (project-authored BIOS image, `tools/sega_cd_bios_services_fixture.py` + `tools/sega_cd_services_sp_fixture.py`) | Expected |
|---|---|---|
| 1 | control: model off | nothing proven (memory-carried arguments) |
| 2 | base: A (2 sectors, entry via memory) and B (1 sector) found by NAME through the directory | both modules, A as ONE two-sector run; supported build admits both before any pass; the same binary runs B |
| 3 | unknown store through a computed pointer / unmodelled service / unsupported service number between store and use | A not proven, `destination_unknown`; never a silent partial inventory |
| 4 | corrupt volume descriptor / directory extent outside the track | no module / `source_outside_track` |
| 5 | loaded, never executed / entry from a register the program cannot know | `loaded_not_executed` / `entry_unknown`, no module |
| 6 | three paths with three selectors | three modules, no merged or invented one |
| 7 | image changed after loading | typed `code_bytes_mismatch` at run time |
| 8 | determinism; changed disc | identical result; refused as another disc |
| 9 | authentic Sonic CD / Earthworm Jim SE | measured, reported as aggregates; no outcome forced |

Gate `MEMORY_CARRIED_DISCOVERY_GO` = rows 1-8 hold through the supported build and launcher AND an authentic module is proven; `PARTIAL` = rows 1-8 hold, the authentic result is zero with a precise cause; `BLOCKED` = rows 1-8 cannot be made to hold soundly.

## Result

`tests/sega_cd_services_loader_test.py`: rows 1-8 hold. On the base fixture the scan completes in 187 states / 477 steps, resolves the `{sector,count}` block from tracked memory, evaluates the directory search over the disc's own descriptor and root directory, proves A (two sectors) and B,
and the supported build admits both before the build pass (which loads A only); the unchanged binary runs B. Adversarial rows: with the work area forgotten the unresolved loops do not converge and the scan reports `scan_limit_exceeded` (nothing admitted): the typed, honest outcome that T003 addresses.

Authentic (authorized local discs, Model 1 BIOS; aggregates only):

| Title | Before (M2) | After (this task) |
|---|---|---|
| Sonic CD | 0 static modules; 1 `computed_control_unresolved`; build 17 rounds, 12 extents, `pcm_unsupported` | **identical** (0 static modules, same build outcome, identical program identity class); the scan completes in 84 states / 1,229 steps, 0 transfers resolved; the SP interrupt entry makes unmodelled calls or unresolved stores, so no tracked memory is trusted (`async_volatile_all`) |
| Earthworm Jim SE | scan exhausts 4,096 states | 16 BIOS calls (all in the modelled ABI set), 1 `{sector,count}` block resolved and 1 unresolved, 1 transfer with an unknown destination, still exhausts 4,096 states (T003) |

Why no authentic module (cause, not a claim of impossibility): the load arguments are produced by command-driven code - the file name comes from the main processor's command, the dispatch on the command is a computed transfer, and the interrupt path is
unanalyzed - so there is no constant file selector on any path the SP-only walk reaches. The model is correct for loaders whose selectors are constants of the SP.

## Decision: `MEMORY_CARRIED_DISCOVERY_PARTIAL`

The mechanism is generic, bounded, deterministic and covered by positive and adversarial fixtures through the supported build and launcher. No authentic title yields a module, because the file selector of every load is supplied by a command from the other processor (see ADR 0215). Not claimed: any authentic coverage gain; completeness of any loader class beyond the sector-read subset.
