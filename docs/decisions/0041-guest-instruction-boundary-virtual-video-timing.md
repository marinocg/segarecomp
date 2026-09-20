# ADR 0041: Guest-instruction-boundary virtual video timing

- Status: Accepted
- Date: 2026-09-18
- Supersedes: ADR 0020's dispatch-count VBlank source and synthetic post-RTE grace only.

## Decision and ordering

Genesis virtual video time advances only when generated C retires one typed, source-provenanced
MC68000 instruction. CPU timing is selected by the generic `cpu/m68k` timing owner; generation
rejects an IR form without a sound typed timing result. A completed instruction supplies an integer
MC68000-cycle count to `genesis_runtime_retire_m68k_instruction`, not to the outer dispatch loop.
The Genesis runtime converts it with `7` master ticks per MC68000 cycle and checked `uint64_t` addition.

NTSC phase is `53,693,175` master ticks/second, `3420` ticks/line, `262` lines/frame, and VBlank
onset at line `224`; frame length is `896,040` ticks and onset phase is `766,080` ticks. Reset phase
is zero (line 0, frame start). `master_ticks` is a monotonically increasing production-only `uint64_t`;
phase is `master_ticks % frame_length`. At every retirement: semantic instruction effects commit;
checked ticks are added; every crossed onset is considered in chronological order; an onset raises one
pending edge only when IE0 is set and no edge is already pending; then normal IRQ6 eligibility/admission
runs before instruction N+1. A long retirement crossing multiple frames never wraps time and does not
invent multiple pending edges while one is pending. Overflow and zero cycles return a stop without
changing phase, pending state, or edge count. Host runner allowance/slicing never enters this sequence.

CPU owns typed cycle classification; Genesis owns only conversion, video phase, VBlank request, and
existing IRQ delivery. The selected emitted instruction families use Section 8 MC68000UM timing rows;
unknown forms remain fail-closed rather than receiving invented timing. DIV uses the documented maximum
basic value as this contract's deterministic policy; exception-entry micro-timing is outside this minimum
retirement contract. This is production state, not checkpoint evidence: phase is not serialized. T254 may observe/slice it
for presentation but cannot advance it. RTE has no synthetic grace; a newly eligible pending IRQ may
be admitted immediately at the next retirement boundary.

Every generated instruction is consequently a possible interrupt boundary. Ordinary retained blocks
remain single C functions, but expose each real instruction-source boundary as an internal resume label;
the final compiled-address set maps every such PC to that same function. Thus an RTE-restored N+1 PC is
dispatchable without replaying N, and block partitioning cannot alter timing or interrupt behavior. The
same final address set remains the sole compiled-membership authority. Any predecessor-derived fold used
after an interior label validates the live architectural state that justifies it; otherwise the
instruction uses its generic routed semantics.

ADR-0020 remains historical. This ADR supersedes only its dispatch-count scheduling source and its
post-RTE grace; IE0 gating, pending/acknowledgment, SR arbitration, frame construction, vector ownership,
and RTE atomicity remain preserved.

## Candidates

| Candidate | Result |
| --- | --- |
| Dispatch count | Rejected: block packing and runner slicing alter event placement. |
| Device-access count | Rejected: a CPU wait loop can have no device access. |
| Scanline-only model | Rejected: lacks a guest execution accounting unit. |
| Full bus/cycle-accurate VDP | Deferred: unnecessary for deterministic VBlank placement. |
| Instruction-boundary MC68000 cycles | Selected: minimal typed guest seam with an explicit extension point for EA/bus timing. |

## Citations

- Sega, *Genesis Technical Overview* v1.00 (1991), pp. 2, 36--38, VDP/NTSC display timing and IE0;
  https://segaretro.org/images/0/0c/Genesis_Technical_Overview.pdf
- Motorola, *M68000 Family Programmer's Reference Manual*, §6.3.9: interrupts are recognized
  between instruction executions; https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf
- Motorola, *MC68000 User's Manual*, §8, 16-Bit Instruction Timing Tables: CPU timing rows used by
  `cpu/m68k`; https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf

## Non-goals

No host pacing, DMA/Z80/audio synchronization, bus contention, regional timing variants, runtime
decode, interpreter, JIT, or renderer work is introduced.
