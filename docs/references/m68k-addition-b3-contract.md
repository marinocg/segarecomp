# MC68000 B3 addition subset

## Public source

Motorola, *M68000 Family Programmer's Reference Manual*, M68000PM/AD Rev. 1,
§4 instruction entries **ADD**, **ADDA**, **ADDI**, and **ADDQ** (public scan:
https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf,
accessed 2026-08-13) defines the selected encodings, operand sizes, effective
address restrictions, and condition-code effects.

## B3 boundary

This implementation selects only ADD.B/W/L, ADDA.W/L, ADDI.B/W/L, and
ADDQ.B/W/L through the existing typed EA/resolver/RMW/materialization/lowering
route. ADD/ADDI/ADDQ ordinary data operations set X and C from unsigned carry
and set N/Z/V from the sized result. ADDA and ADDQ to An leave SR unchanged;
word source operands are sign extended for address arithmetic. ADDQ's encoded
count zero means eight and byte-size An is rejected. No B4 operation is
selected.
