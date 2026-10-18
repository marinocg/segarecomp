# ADR 0146: SEG-065 - Checkpoint F pre-registration: the Sonic CD title-screen frame, its input schedule and comparison rule

- Status: Accepted (SEG-065-T001). Fixed before any generated-native execution past the SEG-064-T005 interval (ADR 0145). It may only
  be made stricter later; any other change invalidates Checkpoint F.
- Scope: what Checkpoint F compares, under which machine preconditions and inputs, at which frames, and what counts as a pass. It
  claims nothing about the generated program.
- Privacy: no BIOS or disc byte, hash, address, frame image, frame digest, trace or disassembly appears here. The reference capture is
  local and ignored; durable evidence records equality results, counts and classes only.

## Context

Checkpoint E (ADR 0145) covers authentic BIOS reset, the application handoff and a pre-registered 60-NTSC-frame interval after it.
Checkpoint F asks for a deterministic title-screen frame produced by generated-native execution after authentic BIOS boot, equal to an
independent reference. The frame and the input schedule are chosen here, from the independent reference only, so that no runtime work
can tune the target.

Two facts measured on the independent reference (local Genesis Plus GX harness) shape the schedule:

- With an **unformatted** internal backup RAM and no controller input, the application shows its licence and logo sequence and then a
  backup-RAM-not-initialized screen; it never reaches the title. With a **formatted, empty** backup RAM it reaches the title.
- With the formatted backup RAM the path up to the application entry is unchanged: the executed instruction-start sets of both
  processors and the work, PRG and Word RAM at the entry are identical to the unformatted run (and to the SEG-064-T005 reference).

## Decisions

1. **Inputs.** The authorized local US Model 1 class BIOS that SEG-064 verified and the Sonic CD (USA) disc; empty cartridge slot
   (machine contract: cartridge none); controller port 1 a standard pad with no button pressed for the whole run.
2. **Backup RAM precondition.** The 8 KiB internal backup RAM starts formatted and empty: all bytes zero except the final 64 bytes,
   which hold the public backup-RAM format trailer (format signature strings, a block count of `(8192 / 64) - 3` repeated four times,
   the format version bytes). It is a project-authored machine input, contains no commercial byte, and is the state of any console whose
   backup RAM has been formatted from the system menu. Without it the claimed frame does not exist.
3. **Frame selection.** Time-aligned: the frames completed at application entry + 420, + 480 (**primary**) and + 540 NTSC frames, with
   the application entry and the frame length (448,020,000,000 ticks) of ADR 0145. In the reference the title fades in at about entry +
   358 frames and is stable from about entry + 390 (H32, 256 x 224, no shadow/highlight, animated background, blinking prompt); the
   three frames differ from one another. A first draft selected entry + 180/240/300 from a misread contact sheet; those frames are
   black or a fade and would have made the checkpoint trivial, so they were replaced from reference evidence alone, before any
   generated output past ADR 0145's interval existed.
4. **Non-triviality.** Each selected reference frame is a title-screen frame: at least 32 distinct canonical colours and 40,000
   non-black pixels (measured: 38 and 53,822 at each).
5. **Canonical frame.** Per active-display pixel, `(mode << 9) | colour`, where `colour` is the resolved 9-bit CRAM value (`BBBGGGRRR`,
   the backdrop entry for a transparent pixel, palette-mode masking applied) and `mode` is 0 shadow, 1 normal, 2 highlight (1 when
   shadow/highlight is off); row-major, with its dimensions. Output colour scaling (RGB conversion) is not part of the comparison.
6. **Compared state.** At each selected frame: canonical-frame digest and dimensions; VDP registers, VRAM, CRAM, VSRAM; both CPUs'
   registers; work, PRG and Word RAM; the device events that affect the frame (VDP DMA and CRAM/VSRAM writes, graphics-ASIC
   operations, Word RAM ownership changes) classified where they differ. Determinism: repeated runs and at least eight deadline
   partitions give one outcome.
7. **Pass rule.** `TIME_ALIGNED`: canonical digests equal at the primary frame. `EVENT_ALIGNED` (a pass recorded with its limitation):
   equality at all three frames with one constant whole-frame offset `|d| <= 2` whose cause is classified (for example the CD-sector
   phase ADR 0145 observed). Anything else is not Checkpoint F; it is reported with the typed difference.
8. **Reference reproducibility.** The reference capture is repeated: identical canonical frames and VDP state at every selected frame.

## Consequences

- Backup RAM becomes part of the machine the generated program runs on; implementing it (as the BIOS and the application actually
  access it) is runtime work of SEG-065-T002, with its own fixtures, not part of this decision.
- The comparison reuses the existing VDP frame renderer and needs no shadow/highlight support for these frames.
