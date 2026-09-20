# Genesis VDP bounded memory-to-target DMA contract

## Scope

This record supplies the public-source basis for SEG-007-T084's bounded
runtime memory-to-VRAM DMA capability and SEG-007-T169's bounded extension to
memory-to-CRAM/VSRAM DMA (the identical transfer engine, selecting a
different documented write target). It is not a general VDP timing, FIFO,
DATA-port, fill, or copy model.

## Public sources

| ID | Source and locator | Fact used |
| --- | --- | --- |
| GTO1 | Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), [Internet Archive PDF](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US.pdf), [text derivative](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US_djvu.txt), accessed 2026-08-26; PDF p. 2 (64 KiB VRAM), pp. 20 and 27 (CONTROL-port command and CD5-CD0 access-mode table, including VRAM/CRAM/VSRAM WRITE codes 0x01/0x03/0x05 and their DMA-family CD5-set counterparts 0x21/0x23/0x25), and §7, pp. 36--38 (memory-to-VDP DMA setup and register diagrams). | The VDP has 64 KiB VRAM, 64 nine-bit CRAM color registers, and 40 ten-bit VSRAM words; a CONTROL-port DMA command can select a VRAM, CRAM, or VSRAM write target via the identical CD5-CD0 code table the non-DMA two-word address-set command already uses, with CD5 additionally set for the DMA family; register #1 enables DMA; registers #19/#20 provide the word count; #21--#23 provide the external-memory source; #23 mode `00` selects memory-to-VDP DMA; and the other documented mode selections are fill and copy, not this transfer. The source and destination advance and the transfer completes after the programmed count. |
| PLUTIE-CMD | Plutiedev, "VDP command reference", <https://plutiedev.com/vdp-commands>; accessed 2026-09-04. | Independent corroboration of the CD5-CD0 code table, including the DMA-family CD5-set write codes, matching GTO1. |

## Implemented typed behavior

The routed CONTROL-port second word accepts only a documented memory-to-target
write DMA command when all of these conditions hold:

- register #1's DMA-enable bit is set;
- register #23 selects documented mode `00` (memory-to-VDP); and
- the command's CD5-CD0 code, masked to its low 5 bits (CD4-CD0, the DMA-family
  bit removed), is one of the three documented write targets: `0x01` (VRAM),
  `0x03` (CRAM), or `0x05` (VSRAM). Every other CD4-CD0 value -- including
  every DMA READ code and every undocumented combination -- remains
  fail-closed, unchanged from before SEG-007-T169.

It creates `GenesisVdpDmaState { busy, source_address, remaining_length,
transfer_access_count, write_target_code }`, clears the completed two-word
command latch, and uses the existing `addressed_pointer` as the selected
target's destination, together with the armed `write_target_code`. For the
selected memory-to-VDP mode, GTO1 §7's register diagram assigns register #23
bit 7 to the non-memory DMA-family selection and bits 6--0 to source address
bits A23--A17; registers #22/#21 provide the next two source fields.
Therefore a clear bit 7 accepts the complete even 24-bit MC68000
source-address range, including the documented Genesis work-RAM window,
while bit 7 set remains the excluded fill/copy family. #19/#20 form the word
count (a zero count represents 65536 words). Each completed transfer reads
one big-endian word through the existing routed ROM/work-RAM access
boundary, stores its two bytes in the armed target's own VDP-owned buffer
(`genesis_vdp_write_target_buffer`, the identical shared lookup the plain CPU
DATA-port write path uses), advances source and target by their documented
units (the target's destination wraps modulo that target's own documented
byte size -- 64 KiB VRAM unchanged; 128-byte CRAM or 80-byte VSRAM, newly
introduced by SEG-007-T169), decrements the count, and changes `busy` to
`idle` at zero.

**SEG-007-T169 project compatibility policy (replaceable, not hardware):** a
CRAM or VSRAM DMA destination additionally requires an even current address,
mirroring the plain CPU DATA-port write's own identical documented-uncertainty
policy (see `docs/references/genesis-vdp-data-port-cpu-write-contract.md`); an
odd CRAM/VSRAM destination fails closed before any mutation. VRAM's own DMA
destination policy is completely unchanged: it never had, and still does not
have, this check.

The bounded VRAM-fill engine (SEG-007-T098/T101) remains explicitly VRAM-only:
its own destination buffer is hardcoded to `vram`, so a fill-mode command
(register #23 bits 7--6 = `10`) paired with a non-VRAM write code (`0x23`
CRAM or `0x25` VSRAM) fails closed at arm time rather than silently mismatch
the fill engine's own hardcoded destination.

Real VDP timing/FIFO scheduling is not modeled. Under SEG-007-T042 §4/§6,
this implementation's deterministic progression is an explicit **project
compatibility policy**: while busy, each successful routed CONTROL-port status
WORD read advances exactly one DMA word. It never depends on a CPU PC, opcode,
wall clock, or instruction count. The status-read value itself remains the
existing T081 policy value; mapping live DMA phase into status bits remains
outside this bounded task.

## Fail-closed boundary

Fill/copy modes for any non-VRAM target, disabled DMA, wrong command codes,
every DMA READ code, unroutable source regions, an odd CRAM/VSRAM DMA
destination, CPU DATA-port access, rendering, and timing/FIFO semantics
remain fail-closed or out of scope. A failed progress access writes no
target byte and leaves the DMA state busy for a later valid access.
