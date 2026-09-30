# ADR 0061: Master System Baseline Machine Profile, BIOS Policy, Mapper Identity and Timebase

- Status: Accepted
- Date: 2026-09-30
- Task: SEG-009-T001
- Related: `docs/architecture/master-system-machine-contract.md` (normative detail and citations),
  `docs/architecture/z80-master-system-integration-contract.md` (SEG-008 CPU boundary), ADR 0056 (IM0 RST-only),
  ADR 0058 (image identity, SMS reference shape), ADR 0062-0064.

## Context

SEG-009 delivers one Master System machine profile end to end. Every later task needs the same answers to: which
console, TV standard and region; what state the machine is in when cartridge code starts; how the mapper is chosen;
and which clock unit ties the CPU, VDP and PSG together. The SEG-008 Z80 support already fixes the CPU side (16-bit
port ABI, level `int_line`, latched NMI, RST-only IM0, absolute T-state deadlines, fail-closed mutable code).

## Decision

1. **Profile `sms2-ntsc-export`.** Master System II (315-5246 VDP with integrated PSG), NTSC 262 lines, export region.
   - The SMS 2 port map is fully documented: `$00-$3F` reads return `$FF`, whereas the SMS 1 returns the last
     instruction byte (open bus). The interrupt-acknowledge bus reads `$FF`, whereas the SMS 1 presents a random
     byte. `$FF` is `RST 38h`, so IM0 software stays inside the SEG-008 RST-only IM0 contract; a random byte would
     not (MacDonald hardware notes §3, §5).
   - The 315-5246 has no SMS 1 table-mask quirks, zooms all eight sprites and adds the 224-line mode, which is
     therefore in scope. The 240-line mode "does not work on NTSC machines" and fails closed (MacDonald VDP §6, §11).
   - NTSC export matches MacDonald's measurement hardware (an NTSC SMS 2) and the authorized local images (3/3 SMS
     export headers).
   - PAL, Japanese region, the SMS 1 VDP, Game Gear, SG-1000 and Mark III are not approximated. A request for any of
     them is `SMS_ERROR_PROFILE_UNSUPPORTED`. A later profile adds them as data plus its own evidence.
2. **BIOS policy: documented post-BIOS state, no BIOS execution.** BIOS bytes are never embedded, required or
   accepted (`SMS_ERROR_BIOS_UNSUPPORTED`). The profile provides the state the export BIOS leaves:
   - memory control `$AB`, and its RAM copy at `$C000` (MacDonald §4, §6: "Game software uses this value");
   - mapper `0,0,1,2`: the BIOS never programs the mapper and relies on bank 1 in slot 1 (SMS Power *BIOSes*);
   - PSG silent and I/O pins as inputs. The VDP register values are a project convention: no public source states
     them and the references disagree on R1/R6 (open fact U9, owned by T003, attributed by T011).

   The CPU starts from SEG-008 `z80_reset`. BIOS-left values that no public source documents (SP in particular;
   the references disagree) are the bounded open fact U1, owned by T011, which attributes any title that depends
   on them. Running a BIOS would add slot switching through port `$3E`, BIOS-from-RAM execution (mutable code under
   the Z80 contract) and a copyrighted input. None of this is needed to start a cartridge, so it stays outside the
   baseline. If T011 evidence shows that BIOS execution changes baseline compatibility, it records that boundary and
   the smallest evidence-backed task.
3. **Mapper identity is declared, never inferred.**
   - The header can establish platform/region/profile only.
   - Declaration sources, first present wins, conflicts fail: build option (`--mapper`), cartridge manifest
     (`<rom>.mapper.json` with a matching SHA-256), fixture-builder declaration, and a recorded local identity for an
     authorized image.
   - Baseline families: `sega` (any accepted size) and `rom_only` (32 KiB, no mapper).
   - Errors: `SMS_ERROR_MAPPER_UNDECLARED` (none/unknown) and `SMS_ERROR_MAPPER_UNSUPPORTED` (a known non-baseline
     family), both at generation time before emission.
   - No byte-pattern heuristic, size rule or silent Sega default. A SHA-256 -> mapper database is possible later
     work outside SEG-009.

   Rationale: headers are frequently wrong (SMS Power *ROM header*), and the same header shape is used by
   Codemasters and Korean boards. A wrong mapper silently produces a machine that runs incorrectly, which is worse
   than a typed stop.
4. **Sega mapper completeness.**
   - Cartridge RAM in slot 2 (bits 3 and 2 of `$FFFC`) is in the baseline as data-only, 32 KiB, zeroed and not
     persisted: "emulating a full mapper in all cases does not cause any problems" (SMS Power *Mappers*).
   - Bit 7 is accepted as a no-op, because one authorized local image sets it.
   - Bit 4 and the bank-shift bits have no known software and fail closed at the write.
5. **Timebase: Z80 T-states.**
   - Line = 342 pixel periods at the TMS9918A pixel clock (master / 10). The CPU runs at master / 15. So a line is
     exactly 228 T, a frame 59,736 T and a PSG tick 16 T.
   - The only sub-T quantity is the pixel position (3/2 x T in the line), computed exactly when needed. The platform
     therefore uses the CPU's own `cycles` counter as the machine clock, with no second counter and no rounding.
   - Host time is derived only for pacing and audio rate, from the rational CPU clock 39,375,000 / 11 Hz.
6. **Unmodelled timing.**
   - There are no wait states or bus contention (Z80 contract §8).
   - Memory/I/O callbacks carry the instruction-start T-state (SEG-008 ABI). Whether ordering accesses there is
     sufficient around device events is the bounded open fact U11 (T003/T004/T006/T007).
   - In-line event positions are the bounded open fact U2. The event *line* is fixed now; its T offset defaults to 0
     until T004 resolves it against the references.

## Alternatives rejected

- **SMS 1 (315-5124) baseline.** It has an open-bus I/O read and a random IM0/IM2 data byte (incompatible with the
  IM0 contract or needing a guessed byte), a zoom bug and table masks. Its only exclusive user named in the
  documents is *Y's (J)*.
- **PAL first.** PAL is not what MacDonald measured, not the local image region, and has a different V counter
  table. It stays profile data.
- **Executing a user-supplied BIOS.** Needs slot switching, RAM-resident code and copyrighted input, for no
  cartridge-start benefit.
- **Header- or size-based mapper choice.** Rejected by the milestone contract and by evidence that headers are
  unreliable.
- **Master-clock ticks as the timebase.** Exact too, but a second unit for no benefit: every periodic event is
  already an integer number of T-states.

## Consequences

- T002 implements ingestion, the mapper identity check and the full Sega mapper. T003 implements the reset and
  post-BIOS state, the port decoder and the scheduler on the T-state clock. T010 adds `--mapper` and manifests
  to the build and the launcher.
- `SMS_ERROR_*` classes (contract §13) are the typed platform error surface, distinct from `Z80Outcome`.
- Open facts U1-U11 (contract §14) each have one owner and a resolution method. None is implemented by guessing.
