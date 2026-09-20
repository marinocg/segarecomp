# Genesis MOVE An,USP startup compatibility policy (SEG-007-T085)

## Status and boundary

This is a bounded **project compatibility policy**, not a general MC68000
privilege, exception, or reset model. It consumes the public Motorola
*M68000 Family Programmer's Reference Manual* (1988), §6, **MOVE USP** and
its reset/exception material, available at
<https://bitsavers.org/components/motorola/68000/M68000_Family_Reference_1988.pdf>,
and the repository's existing `M1` citation in
[`docs/references/genesis-rom-startup-contract.md`](../references/genesis-rom-startup-contract.md).

The manual documents the selected form as a two-byte, longword-only,
privileged `MOVE An,USP`: it copies the selected address register to the User
Stack Pointer and does not affect condition codes. It documents that MOVE USP
forms execute only in supervisor mode and otherwise take a privilege-violation
exception. Its reset material establishes supervisor state with interrupt mask
7 at reset.

## Selected compatibility policy

The generated general-startup route supports only `MOVE An,USP`, for every
address-register source including A7, without a privilege check:

```text
GenesisRuntime.usp = GenesisRuntime.a[n]
PC += 2
SR/CCR unchanged
no bus, memory, or stack access
```

The no-check behavior is an explicit project compatibility policy. It permits
the one documented data movement while this project has no user-mode state,
privilege-violation exception delivery, exception frame, or RTE model. It is
not a claim that privilege is absent on MC68000 hardware, nor that this route
implements the hardware's supervisor/user transition rules. `usp` is ordinary
zero-initialized generated-runtime state; this policy makes no claim about a
hardware USP reset value.

## Fail-closed boundary

The reverse `MOVE USP,An` direction, user-mode execution, privilege-violation
delivery, exception frames, RTE, and all neighboring System Control Group
encodings remain unsupported. The decoder selects no reverse-direction form,
and no generated path guesses its result or attempts a runtime opcode decode.
Any later support must add a separately evidenced privilege/exception semantic
owner rather than widening this unconditional policy implicitly.

## Evidence replacement rule

Public evidence for the selected operation's encoding, width, CCR behavior, or
privilege requirement may refine this policy only with project-authored
synthetic validation. Replacing the unconditional policy with modeled
privilege behavior requires an explicit architecture decision covering mode
state and exception delivery; it must not be added as an instruction-local
check.

---

# Genesis MOVE to SR startup compatibility policy (SEG-007-T088)

## Status and boundary

This is a second, independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict the SEG-007-T085 MOVE An,USP section above,
which remains unchanged and continues to govern its own selector exactly as before. Both sections
address the same class of question -- a System Control Group instruction the public Motorola
*M68000 Family Programmer's Reference Manual* (1988, `M1`, already cited by
[`docs/references/genesis-rom-startup-contract.md`](../references/genesis-rom-startup-contract.md))
documents as privileged -- so this section is appended here rather than as a new file, matching this
project's own established multi-section-per-themed-document precedent (see
[`genesis-controller-io-startup-read-compatibility-policy.md`](genesis-controller-io-startup-read-compatibility-policy.md)'s
own four appended sections).

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | `M1` §6 documents **MOVE to SR** (opcode word `0100 0110 11 mmm rrr`, `0x46C0`-`0x46FF`) as a privileged, word-size-only System Control Group instruction that loads the entire Status Register (CCR bits and the supervisor/trace/interrupt-mask bits) from its decoded source operand, and documents that it executes only in supervisor mode, otherwise taking a privilege-violation exception. `M1`'s reset material (already cited by the SEG-007-T085 section above) establishes supervisor state with interrupt mask 7 at reset. The exact encoding-structure fact that this instruction's own legal source-EA set (per the public opcode map) excludes address-register-direct is independently corroborated by the pinned Musashi adapter's own disassembler opcode table (`m68kdasm.c`'s `g_opcode_info`: mask `0xffc0`, base `0x46c0`, legal-EA legend `0xbff`) -- consulted here strictly as a structural encoding-table cross-check (the same style SEG-007-T025's shift/rotate work already established), never as a hardware-semantics or privilege-behavior authority. | `M1` §6 MOVE-to-SR entry and reset material; Musashi's own opcode table (encoding structure only). |
| **Project inference** | None beyond the encoding-structure cross-check above. | -- |
| **Still unresolved** | This project has no general privilege/exception-delivery architecture, no user/supervisor mode-state concept, and no privilege-violation exception frame. Whether real hardware would take a trap for any specific execution context is therefore not modeled. | Same architectural gap the SEG-007-T085 section above already records. |
| **Project compatibility policy** | The unconditional, no-privilege-check data-movement policy below, and this task's own deliberately narrowed project-selected source-EA subset. | Deliberate project choice, matching the SEG-007-T085 section's own precedent. |

## Selected compatibility policy

The generated general-startup route supports `MOVE <ea>,SR` for exactly two source-operand forms --
data-register-direct (`Dn`) and immediate (`#imm`) -- without a privilege check:

```text
GenesisRuntime.sr = (uint16_t)(source operand)
PC += instruction length
no bus, memory, or device access
```

The no-check behavior is an explicit project compatibility policy, matching the SEG-007-T085 section's
own precedent exactly: it permits the one documented data movement while this project has no
user-mode state, privilege-violation exception delivery, exception frame, or RTE model. It is not a
claim that privilege is absent on MC68000 hardware, nor that this route implements the hardware's
supervisor/user transition rules. The destination is this project's own already-existing generic
16-bit `sr`/`status_register` runtime field (the same field every other selected CCR-affecting
instruction already reads and writes) -- no new persistent-state concept is introduced.

This task additionally, separately from the privilege question, deliberately narrows MOVE to SR's
otherwise-architecturally-legal source-EA set to only these two forms, excluding the remaining seven
legal forms (the four An-indirect-family memory modes and the three absolute/PC-relative control-
addressing forms: `(An)/(An)+/-(An)/d16(An)`, `abs.W`, `abs.L`, `d16(PC)`) as a project scope decision,
not an architectural exclusion: those forms require either
this project's separate C4 static-memory-fact/gap-tracking system or its runtime-routed memory-access
context, neither of which this task wires up for this new kind (see
`m68k_ea_move_to_sr_source`'s own doc comment in `include/segarecomp/m68k_pipeline.hpp` for the full
reasoning). A later task may widen this source set once it also wires that routing/fact machinery for
this kind.

## Fail-closed boundary

Address-register-direct (`An`) source, every one of the seven excluded memory/absolute/PC-relative
source forms named above, user-mode execution, privilege-violation delivery, exception frames, RTE,
and every neighboring System Control Group encoding remain unsupported. The decoder selects no
excluded form, and no generated path guesses its result or attempts a runtime opcode decode.

## Evidence replacement rule

Public evidence for the selected operation's encoding, width, SR/CCR behavior, or privilege
requirement may refine this policy only with project-authored synthetic validation. Replacing the
unconditional policy with modeled privilege behavior requires an explicit architecture decision
covering mode state and exception delivery; it must not be added as an instruction-local check.
Widening the selected source-EA subset requires wiring this new kind into this project's existing C4
static-memory-fact/gap-tracking or runtime-routing machinery first; it must not silently reuse another
kind's fact/routing wiring without independent re-verification.

---

# Genesis MOVE from SR startup compatibility policy (SEG-007-T116)

## Status and boundary

This is a third, independent, bounded **project compatibility policy**, appended here rather than as
a new file for the same reason the SEG-007-T088 section above was (this project's established
multi-section-per-themed-document precedent). It does not rewrite, narrow, or contradict either
section above; both continue to govern their own selectors exactly as before. Unlike the two
sections above, `MOVE from SR` is documented by the public Motorola *M68000 Family Programmer's
Reference Manual* (1988, `M1`, already cited by
[`docs/references/genesis-rom-startup-contract.md`](../references/genesis-rom-startup-contract.md)
and [`docs/references/m68k-move-from-sr-contract.md`](../references/m68k-move-from-sr-contract.md))
as **unprivileged on the original MC68000** -- it became privileged only on the MC68010 and later.
So for this project's MC68000-only scope there is no privilege question to policy around at all;
this section exists mainly to record the one **SR-bit-value policy** the *read* exposes.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | `M1` §6 documents **MOVE from SR** (opcode word `0100 0000 11 mmm rrr`, `0x40C0`-`0x40FF`) as a word operation `SR -> Destination` whose destination is any data-alterable EA, which affects **no** condition codes (X/N/Z/V/C all "not affected"), and which is **not privileged on the MC68000** (a register or ordinary-memory destination raises no exception). The exact encoding-structure fact -- `mask 0xffc0 / base 0x40c0`, legal-EA legend `0xbf8` byte-for-byte identical to `CLR`'s (the data-alterable set) -- is independently corroborated by the pinned Musashi adapter's own disassembler opcode table, consulted strictly as a structural encoding cross-check (never a hardware-semantics or privilege authority), the same style the SEG-007-T088 section above and SEG-007-T025 already use. A second independent secondary statement of the same operation/size/privilege/condition-code facts is recorded in the contract doc. | `M1` §6 MOVE-from-SR entry; Musashi's own opcode table (encoding structure only). |
| **Project inference** | None beyond the encoding-structure cross-check above. | -- |
| **Still unresolved** | Nothing for MC68000 scope: the instruction is unprivileged and has no exception, memory (for a `Dn` destination), or interrupt effect. The project still has no user/supervisor mode-state concept, so nothing here would change if a later target needed the MC68010+ privileged behavior. | -- |
| **Project compatibility policy** | The SR-bit-value policy and the deliberately narrowed `Dn`-only destination subset below. | Deliberate project choice, matching the SEG-007-T088 section's own precedent. |

## Selected compatibility policy

The generated general-startup route supports `MOVE SR,Dn` for every data register, with **no
privilege check** (there is none to perform on the MC68000) and **no condition-code effect**:

```text
GenesisRuntime.d[n] = (GenesisRuntime.d[n] & 0xFFFF0000) | ((uint16_t)GenesisRuntime.sr)
PC += 2
SR/CCR unchanged
no bus, memory, or device access
```

The source is this project's own already-existing generic 16-bit `sr`/`status_register` runtime
field -- the exact field `MOVE to SR` (SEG-007-T088) and every selected CCR-affecting instruction
already read and write. **No new persistent-state concept is introduced.**

### SR-bit-value policy the read exposes

`MOVE from SR` copies the *entire* 16-bit `sr` field, including the supervisor bit, trace bit, and
interrupt-mask bits, not just the low CCR byte. This project does **not** maintain those upper SR
bits coherently against real MC68000 semantics (it has no supervisor/user transition model, no
trace handling, and no interrupt-mask enforcement); it maintains only the CCR bits, via the shared
`M68kMoveResultCcrSpecification` / arithmetic-flag owners, plus whatever a prior `MOVE to SR` /
`MOVE An,USP`-adjacent write last stored. The **compatibility policy** is: `MOVE from SR` returns
the current `sr` field verbatim, whatever it holds. This is sufficient for the common real-code
idiom of saving SR, masking interrupts via a later `MOVE to SR`, and restoring the saved word,
because the saved-then-restored round trip is bit-exact regardless of whether this project models
what the upper bits *mean*. It is **not** a claim that the upper bits equal what MC68000 hardware
would show at that point. Replacing this with a coherently-maintained SR model requires an explicit
architecture decision covering supervisor/user mode state, the trace bit, and interrupt-mask
semantics; it must not be added as an instruction-local fixup.

## Fail-closed boundary

Every non-`Dn` destination (the six memory-alterable modes), address-register-direct (never a legal
destination on any 68000-family part), the reverse `MOVE <ea>,SR` direction (its own established
`move_to_sr` kind, unchanged), `MOVE <ea>,CCR` and `MOVE CCR,<ea>` (distinct System Control Group
forms, not selected), MC68010+ privileged execution, privilege-violation delivery, and every
neighboring System Control Group encoding remain unsupported. The decoder selects no excluded form,
and no generated path guesses its result or attempts a runtime opcode decode.

## Evidence replacement rule

Public evidence for the selected operation's encoding, width, or condition-code behavior may refine
this policy only with project-authored synthetic validation. Widening the destination-EA subset
beyond `Dn` requires wiring this kind into this project's existing C4 static-memory-fact /
runtime-routing machinery first (identical to the SEG-007-T088 rule for `MOVE to SR`'s sources); it
must not silently reuse another kind's fact/routing wiring without independent re-verification.
Adopting the MC68010+ privileged behavior, or a coherently-maintained upper-SR-bit model, requires
an explicit architecture decision, not an instruction-local check.

---

# Genesis MOVE to CCR startup compatibility policy (SEG-007-T118)

## Status and boundary

This is a fourth, independent, bounded **project compatibility policy**, appended here rather than as
a new file for the same reason the SEG-007-T088 / SEG-007-T116 sections above were (this project's
established multi-section-per-themed-document precedent). It does not rewrite, narrow, or contradict
any section above; each continues to govern its own selector exactly as before. Like `MOVE from SR`,
`MOVE <ea>,CCR` is documented by the public Motorola *M68000 Family Programmer's Reference Manual*
(1988, `M1`, already cited by
[`docs/references/genesis-rom-startup-contract.md`](../references/genesis-rom-startup-contract.md)
and [`docs/references/m68k-move-to-ccr-contract.md`](../references/m68k-move-to-ccr-contract.md)) as
**unprivileged on every 68000-family part** -- so for this project's MC68000-only scope there is no
privilege question to policy around at all. This section exists to record the one **SR-sub-field
policy** the write exposes: that only the CCR (low) byte of the 16-bit `sr` field is written.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | `M1` §6 documents **MOVE to CCR** (opcode word `0100 0100 11 mmm rrr`, `0x44C0`-`0x44FF`) as a word operation `Source -> CCR` whose source is any data addressing mode, which reads the source as a word but moves only the low-order byte into the CCR (upper byte ignored), which sets **all** condition codes (X/N/Z/V/C from source bits 4..0), and which is **not privileged on any 68000-family part** (a register or ordinary-memory source raises no exception). The exact encoding-structure fact -- `mask 0xffc0 / base 0x44c0`, legal-EA legend `0xbff` (the data addressing-mode set) -- is independently corroborated by the pinned Musashi adapter's own disassembler opcode table, consulted strictly as a structural encoding cross-check (never a hardware-semantics or privilege authority), the same style the SEG-007-T088 / SEG-007-T116 sections already use. A second independent secondary statement of the same operation/size/privilege/condition-code facts is recorded in the contract doc. | `M1` §6 MOVE-to-CCR entry; Musashi's own opcode table (encoding structure only). |
| **Project inference** | None beyond the encoding-structure cross-check above. | -- |
| **Still unresolved** | Nothing for MC68000 scope: the instruction is unprivileged and has no exception, memory (for a `Dn` source), or interrupt effect. | -- |
| **Project compatibility policy** | The SR-sub-field write policy and the deliberately narrowed `Dn`-only source subset below. | Deliberate project choice, matching the SEG-007-T088 / SEG-007-T116 sections' own precedent. |

## Selected compatibility policy

The generated general-startup route supports `MOVE Dn,CCR` for every data register, with **no
privilege check** (there is none to perform) and a **full condition-code replacement**:

```text
GenesisRuntime.sr = (uint16_t)((GenesisRuntime.sr & 0xFF00) | (GenesisRuntime.d[n] & 0x001F))
PC += 2
upper (system) byte of SR unchanged; CCR bits 7..5 (unimplemented, read as 0) unchanged
no bus, memory, or device access
```

The mask is `0x001F`, not `0x00FF`: CCR bits 7..5 are unimplemented on every 68000-family part and
always read as 0, so the source's bits 7..5 are discarded. This keeps the generated program
byte-identical to the pinned Musashi oracle, whose `m68ki_set_ccr` consults only `BIT_4..BIT_0`.

The destination is the CCR sub-field of this project's own already-existing generic 16-bit
`sr`/`status_register` runtime field -- the exact field `MOVE to SR` (SEG-007-T088), `MOVE from SR`
(SEG-007-T116), and every selected CCR-affecting instruction already read and write. **No new
persistent-state concept is introduced.**

### SR-sub-field policy the write exposes

`MOVE <ea>,CCR` writes **only** the five implemented CCR bits (X/N/Z/V/C, bits 4..0) of the `sr`
field; the upper (system) byte -- supervisor bit, trace bit, interrupt-mask bits -- and CCR bits 7..5
(unimplemented, always read 0) are preserved verbatim. This matches real MC68000 semantics exactly
and requires no upper-SR-bit model: whatever those bits held before the instruction, they hold
afterwards. This is the write-direction counterpart of the SEG-007-T116 `MOVE from SR` note that this
project does not model the upper SR bits coherently -- `MOVE to CCR` never touches them, so the
question does not arise. The condition-code bits are replaced wholesale from the source's low five
bits (bits 4..0); the shared `M68kMoveResultCcrSpecification` / arithmetic-flag owners are not
involved because this is a direct architectural copy, not a computed result.

## Fail-closed boundary

Every non-`Dn` source (the six memory modes, the two PC-relative modes, immediate),
address-register-direct (never a legal source on any 68000-family part), the reverse-shaped
`MOVE CCR,<ea>` (68010+ only, absent on the MC68000), `MOVE <ea>,SR` and `MOVE SR,<ea>` (their own
established `move_to_sr` / `move_from_sr` kinds, unchanged), `ANDI/ORI/EORI to CCR` (distinct
byte-operation immediate-logical forms), and every neighboring System Control Group encoding remain
unsupported. The decoder selects no excluded form, and no generated path guesses its result or
attempts a runtime opcode decode.

## Evidence replacement rule

Public evidence for the selected operation's encoding, width, or condition-code behavior may refine
this policy only with project-authored synthetic validation. Widening the source-EA subset beyond
`Dn` requires wiring this kind into this project's existing C4 static-memory-fact / runtime-routing
machinery first (identical to the SEG-007-T088 / SEG-007-T116 rule); it must not silently reuse
another kind's fact/routing wiring without independent re-verification.
