# MC68000 common startup data-movement batch: `TST`/`MOVE`/`MOVEA`/`CLR`/`LEA`/`JMP`/`JSR` contract (SEG-007-T023)

**Status:** research contract; it supplies no implementation, fixtures, or tests. It independently
re-verifies (and, in two places, corrects) the bit-pattern/legality/condition-code claims a prior
planning pass flagged `[VERIFY]` for a shared MC68000 decode/EA/lift/effect/lowering route covering
`TST`, `MOVE`, `MOVEA`, `CLR`, `LEA`, `JMP`, `JSR`. It does not implement decoding, IR, C emission, or
a runtime, and it introduces no fixtures.
**CPU covered:** original MC68000 only. **Accessed:** 2026-08-13.

## Sources and the boundary of this contract

### A correction to the precedent citation, made explicit before anything else

[`tst-l-absolute-long-contract.md`](tst-l-absolute-long-contract.md) and
[`moveq-contract.md`](moveq-contract.md) cite Motorola's *M68000 Family Reference Manual* (1988),
bitsavers scan <https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf>,
"section 6, instruction entry TST -- Test an Operand ... Instruction Format ... Operation ...
Description ... Condition Codes". Independently re-opening that exact PDF for this task (fetched
2026-08-13, `sha256` of the retrieved 23,248,083-byte file verified locally against the same
bitsavers URL) shows it is Motorola's multi-product **M68000 Family "Selector Guide"/data book**
(Section 1 "Motorola's M68000 Family", Section 2 "Selector Guide", Section 3 "Microprocessor"
[chip-level datasheets: pinouts, electrical characteristics, bus timing -- **no per-instruction
opcode-format pages for any processor**], Section 4 "Coprocessors", Section 5 "DMA Controllers",
Section 6 "**Data Communication Devices**" [MC2681/MC68652/MC68681 UART/communications parts,
unrelated to `TST`], Section 7 "Network Devices", Section 8 "General-Purpose Peripheral Devices",
Sections 9-11 "Mechanical Data"/"Technical Support"/"Development Systems"). This document contains a
mnemonic-only "Instruction Set Summary" table per processor chapter and never contains the
"Instruction Format"/"Operation"/"Condition Codes" per-instruction structure the precedent citations
describe; that structure is not present anywhere in this 608-page file under any section number.

This does not relitigate the two prior contracts' factual claims about `TST.L`/`MOVEQ` -- those
claims are independently correct and are reused verbatim below without re-derivation -- it only
corrects which document actually contains the supporting page content, because this task requires an
independently reproducible primary citation and the previously cited page/section labels do not
resolve in the actual bitsavers file at that URL. The document that does contain exactly the
described "Instruction Format"/"Operation"/"Description"/"Condition Codes" per-mnemonic structure,
consulted below as the primary source for every hardware-fact claim in this contract, is:

1. Motorola, **M68000 Family Programmer's Reference Manual**, part number `M68000PM/AD`, Rev. 1
   (1992). Bitsavers scan:
   <https://bitsavers.org/components/motorola/68000/68000/M68000PM_AD_Rev_1_Programmers_Reference_Manual_1992.pdf>
   (accessed 2026-08-13; 646-page scanned/OCR PDF, `sha256` of the retrieved 2,394,181-byte file
   verified locally against the same bitsavers URL). Cited sections: **Section 2 "Addressing
   Capabilities"** -- 2.1 "Instruction Format" (general instruction word layout and word order),
   2.2.1-2.2.18 (per-mode generation formula, mode/register field values, extension-word counts),
   2.3 "Effective Addressing Mode Summary" (the "control addressing mode" definition and Table 2-4);
   and **Section 4 "Integer Instructions"** -- the individual `CLR`, `JMP`, `JSR`, `LEA`, `MOVE`,
   `MOVEA`, and `TST` entries, each with their own "Operation", "Assembler Syntax", "Attributes",
   "Description", "Condition Codes", "Instruction Format", and "Instruction Fields" headings.
2. Motorola, **M68000 8-/16-/32-Bit Microprocessors User's Manual**, part number `M68000UM/AD`, Rev.
   8 (1993) (covers MC68000, MC68008, MC68010, MC68HC000, MC68HC001, MC68EC000). Bitsavers scan:
   <https://bitsavers.org/components/motorola/68000/68000/M68000UM_AD_M68000_Microprocessor_Users_Manual_Rev8_1993.pdf>
   (accessed 2026-08-13; 216-page PDF, `sha256` of the retrieved 11,152,468-byte file verified
   locally against the same bitsavers URL). Cited section: **6.3.10 "Address Error"** -- the
   word/long-only, odd-address exception statement used for fact #5 below.
3. Karl Stenerud, **Musashi**, commit `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` (2026-03-08),
   `m68k_in.c` (the `m68kmake` opcode-table generator input, `M68KMAKE_TABLE_BODY` section: per-form
   bit pattern, per-CPU-type `000/010/020/030/040` validity column, and the addressing-mode-letter
   legend at the top of that section) and `m68kcpu.h`/`m68k.h` for the MIT license header.
   <https://github.com/kstenerud/Musashi/tree/313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd> (accessed
   2026-08-13). This is the same pin `moveq-contract.md` § "Independent execution oracle" already
   selected; it is reused verbatim, not re-pinned. Used here strictly as an **independent
   cross-check** of the Motorola manual's bit patterns and per-CPU-type addressing-mode legality --
   never as the sole source for a claim, and never as a console- or instruction-semantics oracle in
   its own right (Musashi is an execution emulator; its opcode table is evidence of what encoding it
   dispatches, not proof of what real MC68000 silicon accepts -- every legality claim below is backed
   by the Motorola manual first and Musashi second).

The source facts are deliberately separate from **project policy**. This contract does not infer
console mapping, timing, exception-frame contents, prefetch effects, self-modifying code, or
peripheral behavior from any of the seven mnemonics below; it also makes no implementation-scope
decision (see "Open questions for the implementation task").

## Hardware contract

### The general 6-bit effective-address field, extension-word counts, and word order

Every instruction below embeds a 6-bit effective-address field at bits 5-0 of its primary word,
`mode` (bits 5-3) then `register` (bits 2-0) -- mode-then-register order, per source 1 § 2.1
"Instruction Format", Figure 2-2 "Single Effective Address Operation Word Format". Per-mode
generation, field values, and extension-word counts (source 1 §§ 2.2.1-2.2.18, individually verified
for each mode; `pm-txt-line` below is the retrieved OCR text's line offset for this task's own
reproducibility, not a stable manual locator):

| Addressing mode | Syntax | mode field | register field | extension words | source |
| --- | --- | --- | --- | --- | --- |
| Data register direct | `Dn` | `000` | reg. no. | 0 | § 2.2.1 |
| Address register direct | `An` | `001` | reg. no. | 0 | § 2.2.2 |
| Address register indirect | `(An)` | `010` | reg. no. | 0 | § 2.2.3 |
| Address register indirect, postincrement | `(An)+` | `011` | reg. no. | 0 | § 2.2.4 |
| Address register indirect, predecrement | `-(An)` | `100` | reg. no. | 0 | § 2.2.5 |
| Address register indirect with displacement | `(d16,An)` | `101` | reg. no. | 1 | § 2.2.6 |
| Program counter indirect with displacement | `(d16,PC)` | `111` | `010` | 1 | § 2.2.11 |
| Absolute short | `(xxx).W` | `111` | `000` | 1 | § 2.2.16 |
| Absolute long | `(xxx).L` | `111` | `001` | 2 | § 2.2.17 |
| Immediate | `#<xxx>` | `111` | `100` | 1 (byte/word) or 2 (long) | § 2.2.18 |

**Absolute-short sign extension (confirmed):** "The 16-bit address is sign-extended to 32 bits before
it is used" (source 1 § 2.2.16, verbatim). This is the well-known MC68000 fact the task asked to
confirm: `absolute.w`'s one extension word is sign-extended, not zero-extended, when resolved to a
32-bit address.

**Immediate byte padding (confirmed):** source 1 § 2.2.18, Table 2-3 "Immediate Operand Location":
byte-size immediate data occupies the "Low-order byte of the extension word" (i.e. the low 8 bits of
one full 16-bit extension word, high byte unused); word-size immediate occupies "The entire extension
word"; long-word immediate occupies two extension words, high-order word first.

**`(d16,PC)` displacement base (implementer note, not itself flagged by the task but load-bearing for
correct EA arithmetic):** source 1 § 2.2.11 states "The value in the PC is the address of the
extension word" -- i.e. the displacement is added to the address of the displacement's own extension
word (equivalently, the primary word's address plus 2 for every whitelisted one-word-primary form
here), not to the address after the complete instruction. Any decoder computing `(d16,PC)` must use
this exact base, not "PC after the whole instruction".

**Word order for multi-extension-word forms (confirmed):** source 1 § 2.1, Figure 2-1 "Instruction
Word General Format" lists, in instruction-stream order: the single effective-address operation word,
then "special operand specifiers", then "**Immediate operand or source effective address extension**
(if any, one to six words)", then "**Destination effective address extension** (if any, one to six
words)". For a two-operand `MOVE` needing extension words on both sides, source extension word(s)
precede destination extension word(s) in the instruction stream -- confirmed exactly as the task
required, decode order is source before destination.

### `TST` -- already implemented for `.L`/absolute-long in this repo

Base pattern `0100 1010 SS mmm rrr`, `SS` = `00` byte / `01` word / `10` long (source 1, `TST`
entry, "Instruction Format" and "Instruction Fields"; already verified for `.L`/absolute-long at
`src/m68k_pipeline.cpp:259-284` and in `tst-l-absolute-long-contract.md`).

**Legal EA-mode set for TST's operand on base MC68000 -- correction to a plausible-looking but wrong
assumption:** source 1's own `TST` table marks `An` (mode `001`) and `#<data>` (mode `111`/register
`100`) with the *same* footnote: "MC68020, MC68030, MC68040, and CPU32. Address register direct
allowed only for word and long." Independently cross-checked against Musashi's `m68k_in.c` opcode
table: `tst 16 . a ... 0100101001001... .......... . . U U U ...` and `tst 32 . a ...` both show
validity `. . U U U` (columns are CPU types `000 010 020 030 040`; the pinned commit's own header
comment names this order) -- i.e. **invalid on 000/010, valid from 020 onward** -- and `tst 8 . i
0100101000111100 .......... . . U U U ...` / the `16`/`32` immediate rows show the identical `. .
U U U` pattern. This is independent corroboration from two separately maintained sources (Motorola's
own footnote grouping and Musashi's per-CPU-type validity table) of the same fact: **on the base
MC68000, `TST`'s legal operand EA set excludes both `An` and immediate entirely** (they are not
"restricted", they are absent until the 68020). `TST`'s manual table further footnotes `(d16,PC)`
and `(d8,PC,Xn)` with "PC relative addressing modes do not apply to MC68000, MC68008, or MC68010" --
confirmed independently by Musashi's `tst 8 . pcdi`/`tst 8 . pcix` (and the 16/32 equivalents) rows,
which also show `. . U U U`. **Consequently `TST`'s legal MC68000 operand-EA set is exactly `{Dn,
(An), (An)+, -(An), d16(An), d8(An,Xn), absolute.w, absolute.l}` -- it does *not* include `An`,
`d16(PC)`, or immediate on the base MC68000**, even though those three are otherwise-legal general
addressing modes for several of this batch's other mnemonics. This is a real, manual-stated,
Musashi-corroborated restriction specific to `TST` on this CPU generation, not an assumption error in
the plan that happened to be flagged `[VERIFY]` -- it is confirmed exactly as the flag anticipated
needed checking, and the answer differs from what a reader might guess by analogy to `MOVE`'s or
`CLR`'s EA-set shape.

### `MOVE`

Base pattern (source 1, `MOVE` entry, "Instruction Format"): bits 15-14 fixed `00`, bits 13-12 =
`SIZE`, bits 11-6 = **destination** field (`REGISTER` bits 11-9 then `MODE` bits 8-6 --
register-then-mode order), bits 5-0 = **source** field (`MODE` bits 5-3 then `REGISTER` bits 2-0 --
mode-then-register order, the same general order every other whitelisted mnemonic's single EA field
uses). This asymmetric field order (destination is register-then-mode; source is mode-then-register)
is confirmed exactly as printed in the manual's own instruction-format diagram, not inferred.

**Size-field convention (confirmed, and confirmed different from `TST`/`CLR`):** source 1, `MOVE`
"Instruction Fields": `01` = byte, `11` = word, `10` = long. Cross-checked against Musashi's opcode
prefixes: `move 8 ...` bit pattern begins `0001` (`SS=01`), `move 16 ...` begins `0011` (`SS=11`),
`move 32 ...` begins `0010` (`SS=10`) -- consistent. This is the documented "common gotcha" the task
named: `MOVE`'s size field is `{01,11,10}` for `{byte,word,long}`, while `TST`/`CLR` use `{00,01,10}`
for the same three sizes -- the two families do not share a size-field convention despite both being
size-suffixed instructions in the same opcode-byte neighborhood.

**Legal source EA (confirmed):** all 9 modes in the general table above, including `An`, `d16(PC)`,
and immediate (source 1's `MOVE` "Source Effective Address field" table lists every mode as legal,
footnoted only "For byte size operation, address register direct is not allowed" -- i.e. `MOVE.B
An,<ea>` specifically is illegal because a byte-size read of a 32-bit-only register has no defined
byte lane, not because `An` is excluded from `MOVE` source generally). Musashi's combined source-EA
row for `MOVE` (e.g. `move 8 d . 0001...000...... A+-DXWLdxI U U U U U ...`) shows full `U U U U U`
validity across all five CPU-type columns, confirming `d16(PC)`/`d8(PC,Xn)` (letters `d`/`x` in
Musashi's allowed-EA legend) **are** legal `MOVE` source addressing modes already on the base
MC68000 -- unlike `TST`, which restricts PC-relative to 68020+. This is a second correction-shaped
clarification worth stating plainly: the PC-relative restriction found for `TST` above is a
`TST`-specific restriction, not a general base-MC68000 restriction on PC-relative addressing; `MOVE`
and `MOVEA` both support `d16(PC)` as a source on the original MC68000.

**Legal destination EA (confirmed):** source 1's `MOVE` "Destination Effective Address field" table
lists exactly the 9-entry "data alterable" set -- `Dn`, `(An)`, `(An)+`, `-(An)`, `d16(An)`,
`d8(An,Xn)`, `absolute.w`, `absolute.l` -- and explicitly marks `An` (`— —`, not present) and
`#<data>` (`— —`, not present) as illegal destinations, with `(d16,PC)`/`(d8,PC,Xn)` also marked `—
—`. This confirms every part of the task's claim: `An` is excluded from `MOVE` destination (that
role belongs to the separate `MOVEA` mnemonic below), immediate is excluded (immediate is never a
legal destination for any instruction, since it isn't a storage location), and `d16(PC)`/PC-relative
destinations do not exist on the MC68000 at all -- source 1 § 2.2.11 independently states of
`(d16,PC)` generally, "This is a program reference allowed only for reads," which is the same fact
stated from the addressing-mode side rather than the per-instruction destination-table side.

### `MOVEA`

Base pattern (source 1, `MOVEA` entry, "Instruction Format"): bits 15-14 `00`, bits 13-12 = `SIZE`,
bits 11-9 = destination register, bits 8-6 fixed `001` (this fixed sub-field is what makes a `MOVE`
primary word a `MOVEA` rather than an ordinary `MOVE` -- destination-mode field value `001` is
exactly address-register-direct, which is excluded from plain `MOVE`'s destination set above; `MOVEA`
is structurally a `MOVE` whose destination-EA mode field happens to select `An`, given its own
mnemonic and its own restricted size field, not an unrelated opcode-byte family). Cross-checked
against Musashi: `movea 16 . . 0011...001...... ... U U U U U ...` (`SS=11` prefix `0011`, fixed
mid-field `001`) and `movea 32 . . 0010...001...... ...` (`SS=10`, prefix `0010`, fixed mid-field
`001`) -- consistent.

**Size restriction (confirmed):** source 1, `MOVEA` "Attributes": `Size = (Word, Long)` only -- no
byte-size `MOVEA`. "Instruction Fields": `11` = word, `10` = long; there is no `01`/byte-size
`MOVEA` encoding at all (that primary-word bit pattern, `SS=01`, is a `MOVE.B` primary word with
destination-mode field `001`, which the plain `MOVE` destination table above already marks illegal
-- i.e. the "byte MOVEA" bit pattern is not a reserved/illegal MOVEA form, it is simply an illegal
`MOVE.B` destination, consistent rather than a separate exception to track).

**Sign extension (confirmed, the specific gotcha the task flagged):** source 1, `MOVEA` "Description",
verbatim: "Moves the contents of the source to the destination address register. The size of the
operation is specified as word or long. **Word-size source operands are sign-extended to 32-bit
quantities.**" And "Instruction Fields": "`11` — Word operation; the source operand is
**sign-extended** to a long operand and all 32 bits are loaded into the address register." This is
unambiguous and stated twice in the same entry: `MOVEA.W` sign-extends, it does not zero-extend.

**Source EA (confirmed):** source 1's `MOVEA` "Effective Address field" table lists all 9 general
modes as legal (`Dn`, `An`, `(An)`, `(An)+`, `-(An)`, `d16(An)`, `d8(An,Xn)`, `absolute.w`,
`absolute.l`, `d16(PC)`, `d8(PC,Xn)`, immediate), with no footnote restricting any of them to a later
CPU -- unlike `TST`, `MOVEA`'s full source-EA set (including `d16(PC)` and immediate) is legal
already on the base MC68000. Cross-checked against Musashi's combined row
`movea 16 . . 0011...001...... A+-DXWLdxI U U U U U ...`, full validity across all five CPU-type
columns.

**Destination (confirmed):** always the address register named by the fixed 3-bit register field at
bits 11-9 -- not a general 6-bit EA field; `MOVEA`'s destination is encoded, never decoded through the
mode/register EA machinery.

### `CLR`

Base pattern `0100 0010 SS mmm rrr` (source 1, `CLR` entry, "Instruction Format"), `SS` = `00` byte /
`01` word / `10` long (the `TST`/`CLR` convention, confirmed identical to `TST`'s and explicitly
*not* `MOVE`'s convention -- cross-checked against Musashi's `clr 8 ...`/`clr 16 ...`/`clr 32 ...`
prefixes `0100001000......`/`0100001001......`/`0100001010......`, i.e. size bits `00`/`01`/`10`
respectively, matching).

**Legal EA (confirmed):** source 1's `CLR` "Effective Address field" table lists exactly the same
8-entry "data alterable" destination set `MOVE`'s destination uses -- `Dn`, `(An)`, `(An)+`, `-(An)`,
`d16(An)`, `d8(An,Xn)`, `absolute.w`, `absolute.l` -- and explicitly excludes `An` (`— —`) and
`#<data>` (`— —`, and structurally `CLR` has no source operand at all, so immediate could never apply
regardless). Musashi's `clr` rows use the allowed-EA string `A+-DXWL...` (no `d`/`x`/`I` letters --
no PC-relative, no immediate), matching.

**Read-before-write bus behavior versus condition codes (task-flagged clarification, resolved
explicitly):** source 1's `CLR` entry carries its own boxed **NOTE**: "In the MC68000 and MC68008 a
memory location is read before it is cleared." This is a real-hardware bus-cycle fact (the base
MC68000's `CLR` performs an indivisible read-modify-write bus cycle on its destination, later
processors do not), and it has **no effect on `CLR`'s condition-code result or its final register/
memory value** -- source 1's own `CLR` "Condition Codes" table is unconditional (`N=0, Z=1, V=0, C=0`
always, `X` unaffected), independent of whatever value was read. **For this repo's static
translation, which this project's own non-goals already exclude bus-cycle/timing modeling for, the
prior read has no separate observable consequence the emitter needs to model**: the destination's
final value and the CCR result are fully determined by "write zero, set Z, clear N/V/C, leave X"
regardless of what was read first, so `CLR` may be lowered as a pure write with the documented CCR
effect, with no read-then-write bus-cycle emission required to preserve observable behavior under
this project's own already-stated non-goals (timing/bus-cycle fidelity is out of scope everywhere
else in this repo, and `CLR`'s RMW read has no data-visible effect that a pure-write lowering would
lose).

### `LEA`

Base pattern `0100 RRR 111 mmm rrr` (source 1, `LEA` entry, "Instruction Format"): bits 15-12 fixed
`0100`, bits 11-9 = destination `An` register, bits 8-6 fixed `111` (the sub-field that identifies
`LEA` -- as opposed to some other member of the `0100`-prefixed instruction family -- together with
the `0100` prefix), bits 5-0 = the general EA field for the source. Confirmed against Musashi:
`lea 32 . . 0100...111...... A..DXWLdx. U U U U U ...` (the three literal-dot groups are the
destination-register field, the fixed `111` sub-field, and the source EA field, in that order).

**Legal source EA -- "control addressing modes" (confirmed):** source 1 § 2.3 "Effective Addressing
Mode Summary" defines the term the task asked about, verbatim: "**Control addressing modes refer to
memory operands without an associated size.**" Source 1's own `LEA` "Effective Address field" table
lists the concrete set this category resolves to for `LEA`: `(An)`, `d16(An)`, `d8(An,Xn)`,
`absolute.w`, `absolute.l`, `d16(PC)`, `d8(PC,Xn)` -- and explicitly marks `Dn` (`— —`), `An`
(`— —`), `(An)+` (`— —`), `-(An)` (`— —`), and `#<data>` (`— —`) as not present. Musashi's allowed-EA
string for `LEA`, `A..DXWLdx.`, independently matches: letters present are `A` `(An)`, `D` `d16(An)`,
`X` `d8(An,Xn)`, `W` `absolute.w`, `L` `absolute.l`, `d` `d16(PC)`, `x` `d8(PC,Xn)`; the two
placeholder dots mark the absent `+` (postincrement) and `-` (predecrement) letters, and `I`
(immediate) and the `Dn`/`An` register-direct rows are likewise absent from `LEA`'s table entirely
(there is no separate `lea ... d`/`lea ... a` row the way `TST` has). This exactly matches the task's
predicted exclusion set: `Dn`, `An`, `(An)+`, `-(An)`, and immediate are all excluded from `LEA`
source; only the seven control-addressing-mode forms are legal.

### `JMP`

Base pattern `0100 1110 11 mmm rrr` (source 1, `JMP` entry, "Instruction Format": bits 15-6 fixed
`0100 1110 11`, bits 5-0 the general EA field). Confirmed against Musashi:
`jmp 32 . . 0100111011...... A..DXWLdx. U U U U U ...` -- same fixed prefix, and the **identical**
allowed-EA string `A..DXWLdx.` `LEA` uses. Source 1's own `JMP` "Effective Address field" table is
row-for-row identical in structure to `LEA`'s: the same seven control-addressing-mode entries legal,
the same five entries (`Dn`, `An`, `(An)+`, `-(An)`, `#<data>`) marked `— —`. `JMP`'s target EA set is
confirmed identical to `LEA`'s source EA set.

### `JSR`

Base pattern `0100 1110 10 mmm rrr` (source 1, `JSR` entry, "Instruction Format": bits 15-6 fixed
`0100 1110 10`, bits 5-0 the general EA field). This repo's already-implemented `jsr_absolute_long`
selects the exact word `0x4EB9`
(`src/m68k_pipeline.cpp:257`: `if (word == 0x4EB9U) return select(M68kInstructionKind::jsr_absolute_long,
6U);`); decomposed against the confirmed base pattern, `0x4EB9 = 0100 1110 1011 1001`, i.e.
`0100 1110 10` (the `JSR` prefix) followed by EA field `111 001` -- mode `111`, register `001`,
exactly absolute-long per the general EA table above. This is consistent with, and not a special
case of, the general `JSR` pattern. Confirmed against Musashi:
`jsr 32 . . 0100111010...... A..DXWLdx. U U U U U ...` -- same fixed prefix (differing from `JMP`
only in bit 6, `0` vs `1`), and the same `A..DXWLdx.` allowed-EA string. Source 1's own `JSR`
"Effective Address field" table is again row-for-row identical to `LEA`'s and `JMP`'s: the same seven
control-addressing-mode entries legal, the same five marked `— —`. **`JSR`'s full legal target EA
set on real MC68000 hardware is therefore identical to `JMP`'s and `LEA`'s** -- the seven
control-addressing-mode forms, not narrower. Whether this repo's implementation of `JSR` should be
extended from its current absolute-long-only form to accept that full set is a project scope
decision this contract does not make (see "Open questions for the implementation task").

## Condition-code effects

All seven mnemonics, confirmed against each entry's own "Condition Codes" heading in source 1:

| Mnemonic | X | N | Z | V | C | Citation |
| --- | --- | --- | --- | --- | --- | --- |
| `TST` | unaffected | set if operand negative | set if operand zero | always cleared | always cleared | `TST` "Condition Codes" |
| `MOVE` | unaffected | set if result negative | set if result zero | always cleared | always cleared | `MOVE` "Condition Codes" |
| `MOVEA` | **not affected** (all five bits, entire CCR/SR unchanged) | -- | -- | -- | -- | `MOVEA` "Condition Codes": "Not affected." |
| `CLR` | unaffected | **always** cleared | **always** set | **always** cleared | **always** cleared | `CLR` "Condition Codes": `X — Not affected. N — Always cleared. Z — Always set. V — Always cleared. C — Always cleared.` |
| `LEA` | not affected | not affected | not affected | not affected | not affected | `LEA` "Condition Codes": "Not affected." |
| `JMP` | not affected | not affected | not affected | not affected | not affected | `JMP` "Condition Codes": "Not affected." |
| `JSR` | not affected | not affected | not affected | not affected | not affected | `JSR` "Condition Codes": "Not affected." |

`TST`'s and `MOVE`'s N/Z rows generalize unchanged across byte/word/long: the manual's `TST`/`MOVE`
"Description" text refers only to "the operand"/"the data" without a size-specific carve-out, and
the size field solely selects how many bits are fetched and tested/moved, not a different CCR rule
per size. `MOVEA` is confirmed as the deliberate exception the task flagged: despite sharing a source
mnemonic family with `MOVE`, and despite computing a sign-extended 32-bit value, `MOVEA` leaves every
SR/CCR bit untouched -- this is stated as its own explicit "Not affected" line in its own "Condition
Codes" heading, distinct from `MOVE`'s N/Z-from-result rule immediately above it in the same manual.

## Addressing-mode mechanics

**Byte-access alignment (confirmed):** Motorola, *M68000 8-/16-/32-Bit Microprocessors User's
Manual*, `M68000UM/AD` Rev. 8 (1993) (source 2 above), § 6.3.10 "Address Error", verbatim: "An
address error exception occurs when the processor attempts to access **a word or long-word operand
or an instruction** at an odd address." Only word- and long-size bus accesses (and instruction
fetches, which are always at least one word) require an even address; the manual's statement is
scoped explicitly to word/long-word operands and instructions, so a byte-size data access carries no
alignment restriction at all on the base MC68000.

**Post-increment/pre-decrement width, including the `A7` exception (confirmed):** source 1 § 2.2.4
"Address Register Indirect with Postincrement Mode", verbatim: "After the operand address is used, it
is incremented by one, two, or four depending on the size of the operand: byte, word, or long word,
respectively. ... **If the address register is the stack pointer and the operand size is byte, the
address is incremented by two to keep the stack pointer aligned to a word boundary.**" Source 1 §
2.2.5 "Address Register Indirect with Predecrement Mode" states the symmetric predecrement rule
verbatim: "it is decremented by one, two, or four depending on the operand size: byte, word, or long
word, respectively. ... If the address register is the stack pointer and the operand size is byte,
the address is decremented by two to keep the stack pointer aligned to a word boundary." Both
statements confirm exactly the rule the task described: the increment/decrement amount equals the
operand size in bytes (1/2/4) for every address register except `A7`, where a byte-size access still
adjusts `A7` by 2.

## Open questions for the implementation task

None of the hardware facts requested above remain ambiguous after independently re-checking the
primary Motorola sources and cross-checking Musashi's opcode table; every claim in "Hardware
contract" and "Condition-code effects" above is backed by an explicit manual statement (quoted where
the task specifically flagged a gotcha) and, where useful, corroborated by Musashi's independently
generated per-CPU-type validity table. The two genuine surprises found while checking --
(a) `TST`'s exclusion of `An`, `d16(PC)`/`d8(PC,Xn)`, and immediate on the base MC68000 (all three
added only from the 68020 onward), and (b) the precedent-citation mismatch corrected at the top of
this document -- are both resolved and stated plainly above, not left open.

What remains is **project scope**, not a hardware fact, and is explicitly the implementer's decision,
not this contract's:

- Whether `JSR`'s implemented EA-mode set should be extended from its current single absolute-long
  form (`0x4EB9`) to accept the full seven-entry control-addressing-mode set the hardware permits
  (identical to `JMP`'s and `LEA`'s legal target set, confirmed above) is a scope decision for this
  task's implementer, not a hardware constraint -- real MC68000 hardware accepts the full set for
  `JSR` exactly as it does for `JMP`/`LEA`; there is no hardware reason to keep `JSR` narrower than
  `JMP`.
- Whether the shared route accepts `TST.An`/`TST.d16(PC)`/`TST #imm` forms at all is not a scope
  choice in the same sense: those three forms are not legal MC68000 encodings (see `TST`'s hardware
  contract above), so a decoder must reject them the same way it already rejects any other invalid
  primary word for this CPU generation -- there is no permissive interpretation to choose between.
- Whether `CLR`'s confirmed read-before-write MC68000/MC68008 bus behavior needs any representation
  at all in this project's static translation is answered above (no observable consequence given
  this project's existing timing/bus-cycle non-goals), but the implementer still decides the concrete
  lowering shape (e.g. whether to model it as a single write instruction or as an explicit
  read-then-write pair purely for emitted-code readability/parity with real bus cycles) -- that is a
  code-generation style choice with no hardware-behavior consequence either way, not a correctness
  requirement this contract imposes.
- The exact typed representation of every decode/IR/effect/lowering structure this shared route uses
  (enum values, struct field names, diagnostic categories, memory-effect-kind additions, and so on)
  is implementation detail this contract does not specify, consistent with `tst-l-absolute-long-
  contract.md`'s and `moveq-contract.md`'s own scope: this document supplies hardware facts and their
  citations only.

## Non-goals

- Implementing decoding, IR, C emission, a runtime, fixtures, or tests: this document is research
  only, matching the precedent contracts' own "Status" framing.
- Any MC68020+-only addressing-mode extension (scaled index, base/outer displacement, memory
  indirect, bd/od forms) or any MC68020+-only relaxation of `TST`'s addressing-mode set: this
  contract covers the base MC68000 only, as flagged throughout.
- Timing, bus-cycle count, prefetch, exception-frame contents, self-modifying code, or peripheral
  behavior for any of the seven mnemonics.
- Selecting, scoping, or designing the actual decode/IR/lift/effect/lowering route implementation:
  see "Open questions for the implementation task" for what remains a scope decision rather than a
  hardware fact.
