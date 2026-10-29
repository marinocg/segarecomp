# ADR 0215: SEG-099-T001 - Sega CD BIOS CD-service ABI and loader-state feasibility

- Status: Accepted (pre-registration, evidence, gate). Milestone SEG-099 (whole-game static AOT, M3). Related: ADR 0206, 0210, 0211, 0212, 0214.
- Privacy: classes and counts only. No commercial address, offset, LBA, name, hash or byte appears here. BIOS function numbers and the ABI below are the public BIOS API.

## Question and pre-registered gate

Can a bounded, source-backed model of the documented BIOS CD services plus the sub program's (SP) work-variable state recover real executable source-to-destination mappings?
Gate: `BIOS_LOADER_ABI_FEASIBLE` only if the ABI facts needed for a useful subset are established from public sources (two sources, or one source plus independent consumer evidence) and the
subset's argument/result/memory effects can be stated without guessing; otherwise `BIOS_LOADER_ABI_UNRESOLVED`, naming the missing facts, and no speculative service semantics are implemented.
Disclosure: the ephemeral local inspection of the two authorized SP images (below) was made before this record was written; it informed the question, it is not an admission input and it
did not choose any register or memory layout (the layouts come from the cited ABI).

## Sources

- A: Sega's CDBIOS include file as transcribed in `drojaazu/mmd_loader` `inc/cdbios.s`. B: `drojaazu/megadev` `lib/sub/bios.def.h`, `lib/sub/cdrom.s`, `docs/cdrom.md` (cites the Mega CD BIOS manual; may share A's origin).
  C: R. Pavone's working Sub-CPU ISO9660 reader (`retrodev.com/cdread.asm`). D: Genesis Plus GX `core/cd_hw/cdc.c`, `scd.c` (hardware behaviour). The Sega manual itself was not retrieved.
- Independent consumer evidence: the BIOS function numbers the static scan observes at the two authorized SPs' BIOS calls (`survey.bios_functions`) are exactly this ABI's disc-read set.

## The ABI (`JSR $5F22`, function number in D0.W; carry clear = ok)

| Fn | Name | Inputs | Outputs | Effects (what a model may rely on) | Confidence |
|---|---|---|---|---|---|
| $20 | ROMREADN | A0 -> two big-endian longs `{first logical sector (LBA), sector count}` in memory | none | starts CDC reading at that sector, stops after `count`; the BIOS pre-seeks, earlier sectors may arrive first. clobbers D0-D1/A0-A1 | A+B, C uses it |
| $17 | ROMREAD | A0 -> one long LBA | none | starts reading, does not stop by itself | A (text), B |
| $89 | CDCSTOP | none | none | stops CDC reads; a sector being read is lost | A+B |
| $8A | CDCSTAT | none | carry clear = a sector is ready | | A+B |
| $8B | CDCREAD | destination device ($FF8004) already set | carry clear: D0.l = header (BCD mm:ss:ff:mode) | caller must CDCACK | A+B |
| $8C | CDCTRN | A0 = data buffer, A1 = 4-byte header buffer; requires device 3 (sub read) | carry clear = ok; A0, A1 advanced | writes the sector's user data at (A0) (2048 bytes for mode 1; the BIOS advance is documented as $920, the exact byte count is unresolved) and the header at (A1) | A+B (call), byte count UNRESOLVED |
| $8D | CDCACK | none | none | the current sector is done | A+B |
| other | MSC*, DRV*, CDB*, FDR*, CDCSETMODE, ROMSEEK, ROMREADE... | | | not modelled (see below) | ROMREADE UNRESOLVED (A and B disagree), CDCSTARTP ($88) UNRESOLVED, CDBSTAT table layout UNRESOLVED |

Further facts: the destination device is selected by the game writing `$FF8004` (bits 2-0 of its high byte: 2 main read, 3 sub read, 4 PCM DMA, 5 PRG RAM DMA, 7 Word RAM DMA; any write re-initialises the
transfer and resets the DMA address; A, B, D). Only device 3 uses CDCTRN; devices 4, 5 and 7 are DMA'd by the CDC to `$FF800A`; device 2 is read by the main CPU through its host-data port, so the sub
program does not know that destination. Sub PRG RAM `$0000-$5FFF` belongs to the BIOS (variables, vector table at `$5F00`): any service may change it. Registers D2-D7 and A2-A6 are preserved (B's clobber
tags; one source, weak support from C). **There is no BIOS service that selects a disc file by name or ISO9660 path** (the complete lists in A and B; the readers in B and C parse ISO9660 inside the game's own sub program).
So file identity and LBA are produced by title code, not by a BIOS call.

## Evidence from the authorized titles (aggregate)

- Both SPs call exactly this ABI's set ($20, $89, $8A, $8B, $8C, $8D, plus device/mode/drive calls) with constant function numbers on the walked paths.
- Sonic CD's SP routes every disc read through one work-area-driven routine: a work area addressed through a fixed address register holds the `{LBA, count}` pair that A0 points at for ROMREADN, the destination pointer
  that is loaded into A0 for CDCTRN, a transfer mode written to `$FF8004`, a retry counter and a return address; the loop advances the pointer by 2,048 and the LBA by 1 per sector. The routine is the *same* for every load.
- Whether a load is a module depends on the caller. The LBA of a named file is computed at run time by the SP itself: it reads sector 16, follows the root directory extent from that buffer, and compares directory
  records with a 12-byte name that its caller (a command received from the main processor) copied into the work area. The Earthworm Jim SE SP contains the same kind of directory-name copy loop (this is what exhausts the scan's 4,096-state bound: a counted loop with post-incremented known pointers whose every iteration is a new state).
- Sonic CD's observed extents are 7 main Work RAM (read by the main CPU through device 2 - invisible to a sub-side model of CDCTRN) and 5 sub PRG RAM; 0 Word RAM.

## Decision

`BIOS_LOADER_ABI_FEASIBLE` for the **sector-read subset**: ROMREADN (`{LBA,count}` through A0), CDCSTOP, CDCSTAT, CDCREAD, CDCTRN (device 3, destination A0), CDCACK and the `$FF8004` device byte. For these the argument, result and memory effects are stated by two
sources and a consumer; the exact byte count of CDCTRN is treated conservatively (2,048 bytes are the sector, the rest of the documented $920 advance is invalidated, never assumed). Everything else stays an unmodelled service (it invalidates abstract memory
and registers D0/D1/A0/A1, and never counts as a load).

`BIOS_LOADER_ABI_UNRESOLVED` for what would be needed to reach an authentic module *without running it*: (1) the BIOS has no file-name service, so the source LBA of a module is title code reading the ISO9660 directory at run time;
(2) the name it looks up is supplied by the main processor's command, i.e. by a second image; (3) a main-processor destination (device 2) is chosen by main-side code. T002 therefore models the sector-read subset
precisely, resolves ISO9660 only by *evaluating* constant-keyed title code over validated disc bytes (never by guessing names), and reports command-supplied names as unresolved.

## Not claimed

No authentic module is claimed discoverable; the ROMREADE, CDCSTARTP, CDBSTAT and CDCTRN byte-count facts are not modelled. No title-specific layout enters production code.
