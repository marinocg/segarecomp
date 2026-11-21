# ADR 0319: A scaled indexed jump into a run of `BRA.W` slots names its own target set

- Status: Accepted (SEG-151-T002, delivered in the same combined PR as ADR 0318: the Krisalis games it unblocks).
- Builds on: ADR 0009 (computed indirect control-flow target resolution), 0054 (strict extent policy of the experimental recovery), 0180 (controlled, bounded broadening with run-time guards). Changes none of their rules; it adds one structurally bounded candidate source.

## Context

With the sound handshake satisfied (ADR 0318), The Lion King and Boogerman stopped with `unresolved_indirect_target / reached_unresolved_direct_edge` at the entry of the revision-3 Krisalis 68000 sound module: `lsl.w #2,d7` / `jmp table(pc,d7.w)` over twenty `bra.w` slots.
The function number in `d7` comes from many callers (constants and values loaded from tables), so no finite-value proof bounds it; the jump stayed on the Tier-2 path, which only accepts targets that are already compiled entries, and the slots are reachable only through this jump.

## Decision

`compute_indirect_target_set` falls back, when the finite-value proof fails, to a shape-bounded set: the control EA is `(d8,PC,Dn.W)`, **the instruction immediately before the jump is `LSL.W #2,Dn` on the same register** (the index is scaled to a 4-byte stride), and the table the jump enters (`PC + d8`) is a contiguous run of `BRA.W` slots
(`$6000` followed by an even, non-zero displacement) in **immutable cartridge bytes**: at least 2 and at most 64 slots, the run ending at the first word that is not a slot. The extent must be **established**: that terminating word (after the last slot, the 64th included) has to be readable from immutable bytes and not be a slot. A table with a 65th (or later) slot is not a proven run of at most 64 and is refused as a whole; so is one whose terminator cannot be read. The candidate set is exactly that run (index values `0, 4, 8, ...`). Every candidate passes the existing target-admission rule; the generated Tier-1 code still computes the target at run time and
stops with the typed unresolved-indirect stop unless it is a member. No other shape changes: a missing scaling instruction, a different register, a single slot, a table in bytes the environment does not call immutable, or a table that is not `BRA.W` slots keeps today's unproven behaviour.

## Why this is not the heuristic ADR 0054 rejects

ADR 0054 refuses to take an *operand width* (0..255) as a table extent. Here the extent is read from the table itself under a fixed shape (stride 4 matching the scaling instruction, slot opcode, immutable bytes, a hard bound) and every member is a branch the program owns; over-admission can only add compiled entries that are `BRA.W` instructions, and execution still requires the run-time membership test. This is the ADR 0180 pattern (controlled broadening behind a guard), not a proof about which values the register may hold.

## Evidence

`cpu_m68k_slot_run_discovery_tests` (synthetic): a three-slot table resolves to its three slots with index values 0/4/8; runs of 2, 63 and 64 slots with a read terminator are accepted; a 65- and a 100-slot table, a 64-slot table and a 3-slot table without a readable terminator are refused; odd and zero displacements end a run; no scaling, the wrong register, one slot, an odd-displacement terminator (a run of two), a non-immutable environment and a non-slot table behave as before. The pre-existing discovery tests are unchanged. Real images: docs/testing/genesis-native-krisalis-validation.md.

## Note: the VDP write-FIFO status bit

Sensible Soccer's own blank screen was not a sound or discovery problem: its VDP helper polls status bit 9 (write FIFO empty) before control-port bursts and the runtime never reported it. Every VDP write is applied at once, so the FIFO is always empty: bit 9 is now always set (bit 8, full, stays clear). That game also checks the console region; with the Europe region (the launcher passes the header's) it runs.
