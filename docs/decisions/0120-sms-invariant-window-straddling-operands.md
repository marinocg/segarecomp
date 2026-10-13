# ADR 0120: Invariant-window instructions with operands in the banked slot

## Status
Accepted.

## Context
On the Sega mapper the fixed first 1 KiB is one invariant code image and the bytes from $0400 on belong to whichever
bank slot 0 maps at run time. Real software (Wonder Boy III, Castle of Illusion) has an instruction in the last bytes
of the fixed window, for example `CALL nn` at $03FE, whose operand bytes lie in slot 0. The emitter classified every
such start as `unresolved_fetch_mapping`, so both games stopped with that typed outcome the first time the path ran
(Optimized and Compatibility alike: the start is exposed in both).

## Decision
For a start in an invariant window, up to three bytes after the window that belong to a banked slot decode as
placeholders. If every placeholder is a displacement/immediate payload byte (never a prefix or opcode byte) and the
instruction is at most four bytes, the start is a full owner whose operands come from `rt->live_code`; the owner fills
it at the instruction start (static bytes as constants, placeholder bytes with `z80_read`, a pure ROM read). The existing
`live_operands` lowering contract is reused, so the effect is exact for whichever bank slot 0 maps. A placeholder in a
structural position (a prefix or opcode byte in the next slot) stays a typed `unresolved_fetch_mapping` stub. Banked-window
straddles and RAM operands are unchanged.

## Consequences
No runtime decoding and no new runtime ABI. Generated output is unchanged for every start that does not straddle.
Covered by `sms_bank_crossing_test` (both slot-0 banks), the conformance scenario
`sms_invariant_end_operand_in_slot0`, and the two commercial games run in both policies.
