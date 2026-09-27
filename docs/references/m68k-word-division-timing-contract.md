# MC68000 DIVU.W/DIVS.W timing contract

## Evidence

**Primary source:** NXP, *MC68000 User Manual*, `MC68000UM`, instruction
timing table 8-4 and exception timing table 8-14, accessed 2026-09-19:
<https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf>.

For the MC68000 (the Genesis main CPU), table 8-4 gives maximum register-EA
times of 140 clocks for `DIVU.W` and 158 clocks for `DIVS.W` (marked as maxima), and requires the
word effective-address calculation time to be added. Table 8-14 assigns
divide-by-zero exception processing 38 clocks plus the word EA time. These figures do **not** apply
to later M680x0 variants.

## Selected behavior (SEG-021-T022; supersedes the SEG-007 maximum policy)

`DIVU.W` and `DIVS.W` with a non-zero divisor retire their exact data-dependent
time plus table 8-1's word source-EA cell. Table 8-4 publishes only the maxima
(`< 140`, `< 158`); the exact count is Jorge Cwik's published analysis of the
68000 division microcode (the algorithm Genesis Plus GX implements as
`UseDivuCycles`/`UseDivsCycles`): DIVU overflow 10, otherwise 76..136; DIVS
absolute overflow 16 (18 for a negative dividend), otherwise 120..156 -- never
above the published maxima. The rule is CPU-owned C
(`libs/cpu/m68k/include/segarecomp/cpu/m68k/timing_core.h`), and generated C
materializes the dividend and divisor for it
(`m68k_timing_div_dividend`/`m68k_timing_div_divisor`). The pinned Musashi
core still charges the fixed maxima (including on overflow), so it is not a DIV
timing oracle; `tests/m68k_muldiv_auto_update_generated_test.py` checks the
generated timing against an independent test-owned transcription.

Divide by zero transfers synchronously to vector 5 rather than retiring as a
normal DIV. The exception owner charges table 8-14's 38 plus word-EA clocks at
the entry commit (ADR 0043 §8 implementation note); no normal DIV time is
charged on that path.

No mapper, peripheral, bus-wait-state, self-modifying-code, or later-M680x0
timing behavior is selected by this contract.
