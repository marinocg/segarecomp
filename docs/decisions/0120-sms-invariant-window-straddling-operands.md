# ADR 0120: Invariant-window instructions with operands in the banked slot

## Status
Accepted.

## Context
On the Sega mapper the fixed first 1 KiB is one invariant code image; the bytes from $0400 on belong to whichever bank slot 0
maps at run time. An instruction whose opcode is in the last bytes of the fixed window but whose immediate or displacement
bytes lie in slot 0 (for example `CALL nn` at $03FE) was classified `unresolved_fetch_mapping`, so generated programs stopped
the first time it ran. Real software uses this shape.

## Decision
The ordinary classification of the invariant window stays authoritative. Only a start it left `unresolved_fetch_mapping`
because of a banked byte directly after the window is re-examined with those (at most three) bytes as placeholders. The
candidate is accepted only if its logical length is at most four bytes (checked before any mask is formed) and every
placeholder byte is a displacement/immediate payload byte according to the Z80 form descriptor. It then becomes a full owner
whose operands come from `rt->live_code`: static bytes as constants and placeholder bytes via `z80_read` (a pure ROM read) at the
instruction start, reusing the `live_operands` lowering contract. Anything else (a prefix or opcode in the next slot, a long
prefix run) keeps the original typed stop. Work per start is constant, so classification stays linear. Banked-window straddles
and RAM operands are unchanged.

## Consequences
No runtime decoding and no new runtime ABI; output is unchanged for every start that does not straddle. Covered by
`sms_bank_crossing_test` (8-bit and 16-bit immediates under two slot-0 banks, prefix/opcode crossings, a 48-prefix run, sanitizer
clean), the conformance scenario `sms_invariant_end_operand_in_slot0`, and commercial runs in both AOT policies.
