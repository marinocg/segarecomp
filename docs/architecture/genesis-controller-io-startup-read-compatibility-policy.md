# Genesis controller-I/O startup-read compatibility policy (SEG-007-T020)

## Status and boundary

This is a bounded **project compatibility policy**, not an authoritative Genesis-hardware claim or
implementation authorization. It consumes only SEG-007-T017's controller-I/O research contract and
SEG-007-T019's Outcome B closure. It applies only to the functionally identified controller-I/O
four-byte shape at that frontier; it neither names nor derives a private target address.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | An even MC68000 long read uses a high-WORD transfer followed by a low-WORD transfer. CTRL is R/W; BYTE and WORD accesses are supported; on a WORD access only its lower byte is meaningful register data. | T017's cited Motorola/Sega evidence, retained by T019. This does not establish a returned CTRL bit pattern or an upper-byte value. |
| **Project inference** | Within this functional four-byte shape, the CTRL lower-byte lanes occur in transfer order as CTRL1 then CTRL2. | T017 combines the sanitized frontier intersection with the documented transfer and lower-byte facts; T019 retains that classification. It is not a public claim about a private address. |
| **Still unresolved** | Each CTRL read-back byte, each CPU-visible upper-byte lane, initial/reset/power-on applicability at this frontier, and device-specific read effects/order beyond MC68000 transfer order. | T017's explicit research boundary and T019 Outcome B. MacDonald's condition-limited reported defaults do not resolve these items. |
| **Project compatibility policy** | The exact zero values, initial state, side-effect-free behavior, success record, and fail-closed boundary below. | Deliberate deterministic project choice for the unresolved gap; not historical/device truth. |

## Selected deterministic policy

The one accepted selector has **all** of these dimensions, expressed only with public device
identities already recorded by T017: direction is read; width is LONG/32-bit; the address has passed
existing earlier effective-address validation, including alignment;
the high WORD transfer's lower byte resolves to public CTRL1 (`$A10009`); the low WORD transfer's
lower byte resolves to public CTRL2 (`$A1000B`); and transfer order is high-WORD/CTRL1 first then
low-WORD/CTRL2. This selector defines the bounded four-byte controller-I/O shape without recovering
or requiring any private frontier address, image offset, or instruction provenance. For that selector
only, policy state begins with both controller control-register bytes at zero:

```text
Ctrl1 = 0x00
Ctrl2 = 0x00
```

The policy does not rewrite historical truth. It supplies values only for its deterministic result:

| Ordered transfer and lane | Historical/device truth | Policy value |
| --- | --- | --- |
| High WORD, upper lane | Still unresolved | `0x00` |
| High WORD, CTRL1 lower lane | Still unresolved exact CTRL read-back | `0x00` (`Ctrl1`) |
| Low WORD, upper lane | Still unresolved | `0x00` |
| Low WORD, CTRL2 lower lane | Still unresolved exact CTRL read-back | `0x00` (`Ctrl2`) |

Thus high WORD is `0x0000`, low WORD is `0x0000`, and the assembled long result is `0x00000000`.

The result is represented and produced in strict MC68000 transfer order: first the high WORD,
then the low WORD; assembling the long result must preserve that order. The read is
side-effect-free under this policy: it does not mutate Ctrl state, controller input state, or any
other bus/device state.

Charles MacDonald's reported `00` default-state CTRL values are condition-limited secondary-source
context consumed by T019. They are not proof that this initial state is universal hardware reset,
power-on, read-back, or startup behavior. The explicit zero values remain project policy even where
that report is consistent with them.

## Typed result boundary

On success, the future device result has these typed fields:

| Field | Required value/property |
| --- | --- |
| `value` | One 32-bit long value, `0x00000000` under this policy. |
| `word_observations` | Exactly two ordered observations: high WORD `0x0000`, then low WORD `0x0000`; each is width WORD and retains its transfer order. |
| `policy_provenance` | Identifies this SEG-007-T020 project compatibility policy, rather than hardware-fact provenance. |

On failure, no successful value or observations exist. The typed fail-closed result has:

| Field | Required value/property |
| --- | --- |
| `category` | `unsupported_device_region_controller_io`. |
| `source_provenance` | Available typed CPU source provenance from the statically lifted memory operation. |
| `address_provenance` | The typed requested target address and resolver/device diagnostic provenance available at rejection. |
| `width` | Requested access width. |
| `direction` | Requested access direction. |

## Fail-closed boundary and ownership seam

Existing pre-region validation remains authoritative, in this order:

1. `effective_address_not_24bit`
2. `odd_effective_address`
3. Existing ROM/RAM/device/unmapped region resolution

T020 applies only after the effective address has passed those earlier address-validation steps and
the shared Genesis bus/address resolver has recognized or intersected the request with the public
controller-I/O region. This policy does not reclassify arbitrary memory as controller I/O. At that
controller-I/O stage, the exact selector above succeeds; every other otherwise-valid
controller-I/O access shape fails closed with the retained
`unsupported_device_region_controller_io` category: wrong width; wrong direction; a CTRL-lane
arrangement other than high-WORD/CTRL1 then low-WORD/CTRL2; a different controller-I/O register
shape; or a partial/crossing controller-I/O shape that does not match the selector. No value, lane
content, ordering, side effect, or fallback behavior is guessed.

If the request is outside the recognized controller-I/O region, the pre-existing resolver result is
preserved, including `unmapped_data_access` where applicable. ROM behavior and RAM behavior are
unchanged. A crossing or intersecting controller-I/O access is deterministically fail-closed unless
it is the exact policy selector; unrelated unmapped accesses are never changed into a controller-I/O
diagnostic.

A later implementation has one ownership seam only:

```text
statically lifted CPU memory operation
  -> Genesis bus/address resolver
  -> controller-I/O device read
  -> typed device result or typed fail-closed result
```

The CPU operation supplies typed address, width, read intent, and source provenance. The bus/address
resolver recognizes and routes the bounded region without deriving provenance from host storage. The
device supplies the ordered high/low WORD result or the retained failure. No instruction-specific
emitter, including a `TST.L` path, owns controller semantics.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine a policy lane, initial state,
side-effect rule, or failure boundary: it must directly cover the same functional selector and the
exact affected CPU-visible behavior, record its provenance and limits, and receive independent
validation. Address validity and alignment are owned by the earlier effective-address validation
stage and are not part of the controller-device policy selector; that validation must retain its
`effective_address_not_24bit` then `odd_effective_address` precedence. It must also retain
neighboring controller-I/O-stage mismatches as fail-closed (including width, direction,
lane-arrangement, register-shape, and partial/crossing mismatches). A directly supporting hardware
source may establish a hardware fact; a qualified independent observation remains distinct from that
fact; a derivation remains project inference. Evidence that does not establish a lane's exact
CPU-visible behavior leaves policy labeled policy and hardware behavior unresolved. Replacement must
not silently upgrade this zero policy into an authoritative hardware claim.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or
derived fixture. It does not model general controller reads, interactive input, TH/serial behavior,
timing, reset sequencing, other I/O ports, VDP, rendering, audio, Z80 behavior, or a complete Genesis
bus. It adds no decoder, CPU, emitter, runtime, fixture, or validation behavior.

---

# Genesis controller-I/O EXP-port CTRL 3 WORD-read compatibility policy (SEG-007-T038)

## Status and boundary

This is a second, independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict the SEG-007-T020 section above, which remains
unchanged and continues to govern the CTRL1/CTRL2 selector exactly as before. This section consumes
only: SEG-007-T036's confirmed classification (`target_register_class = "ctrl"`, the EXP-port
`CTRL 3` register, `$A1000D`); SEG-007-T033's reached-shape facts (width `word`, direction `read`,
disjoint from the SEG-007-T021 selector); SEG-007-T034's complete public register-map research and
provenance ledger; and the SEG-007-T020 section above as the direct structural template for this
policy's shape and discipline. It applies only to the one functionally identified WORD-read shape
defined below; it neither names nor derives a private target address, and it does not reopen or
extend T036/T034's own verdicts.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | CTRL3's address (`$A1000D`) and BYTE/WORD access support; on a WORD access only the lower byte is meaningful register data. | `GTO1` pp. 72, 74, 75, cited by SEG-007-T034 (restated, not re-derived). This does not establish CTRL3's returned bit pattern, its upper-byte lane's value, or its R/W status specifically. |
| **Project inference** | "CTRL 3" (bare-numbered in `GTO1` p. 74) denotes the EXP port's own CTRL instance; CTRL3's R/W status is extended to it specifically via `GTO1`'s own "each port... functions in the same manner" statement; and CTRL1, CTRL2, and EXP — the three ports `GTO1` p. 72 §2 itself names, each stated to have an identical set of registers including one shared "CTRL (PARALLEL CONTROL)" register-type table entry, though p. 72 §2 never itself uses the name "CTRL 3" (that specific name is introduced only at p. 74) — require that same "each port... functions in the same manner" statement to extend the shared type's R/W status and semantics to the third (EXP) instance specifically. | SEG-007-T034's provenance ledger claim 3, restated unchanged. It is not a public claim about a private address. |
| **Still unresolved** | CTRL3's exact CPU-visible read-back bit pattern beyond its documented write-oriented per-bit configuration meaning (`GTO1` p. 75's CTRL table: `INT`, `PC6`-`PC0`, each `(RW)`); the concrete driven bit pattern of the upper lane (`$A1000C`); any CTRL-register read side effect for any port instance; initial/reset/power-on applicability at this frontier. | SEG-007-T034's Scope-item-6 findings and provenance ledger; SEG-007-T017/T020's identical unresolved treatment already recorded for CTRL1/CTRL2's own upper lanes and read effects. `MCD1`'s condition-limited reported `$00` default for CTRL3 does not resolve these items. |
| **Project compatibility policy** | The CTRL3 lower-byte value, the upper-lane value, side-effect-free behavior, and the typed success/failure result boundary below. | Deliberate deterministic project choice for the unresolved gap, decided and justified separately per dimension below; not historical/device truth. |

## Selected deterministic policy

The one accepted selector has **all** of these dimensions: direction is read; width is WORD (never
BYTE, never LONG, never a write); the address has passed existing earlier effective-address
validation, including alignment, and has been recognized or intersected by the shared Genesis
bus/address resolver as the controller-I/O region; the WORD's lower byte resolves to the public
EXP-port `CTRL 3` register (`m68k_controller_io_ctrl3_register_address`, `$A1000D`); the WORD's
upper byte resolves to the immediately preceding even byte lane (`$A1000C`). This is the same
two-lanes-per-WORD-transfer structure the SEG-007-T020 section above already established for
CTRL1/CTRL2's own lanes, restated here for a single WORD transfer rather than an ordered LONG pair.
This selector applies only within the `general_startup` route's existing controller-I/O resolver
stage — the same stage the T020 section already occupies — never as an instruction-specific (for
example `TST.W`) emitter case. It defines this one bounded WORD-read shape without recovering or
requiring any private frontier address, image offset, or instruction provenance.

This policy decides three distinct dimensions separately; none is a silent copy of another, and none
is described as an authoritative Genesis hardware fact.

**(a) CTRL3 lower-byte value.** `GTO1` p. 72 §2 names exactly three general-purpose I/O ports — CTRL1,
CTRL2, and EXP, never "CTRL3"; that specific name is introduced only at p. 74's address table, per
the provenance table above — and states each of those three ports has an identical set of registers,
including one shared "CTRL (PARALLEL CONTROL)" register-type table entry: its register-type table
lists one CTRL row applying identically to all three port instances, not three separately typed rows,
so this much (a single shared register-type entry, generically applicable per named port) is a
directly documented table structure and not a register this project must reason about from scratch.
Concluding from this that CTRL3 — the EXP port's own instance of that shared register type — carries
the *same R/W status and semantics* as CTRL1/CTRL2,
however, is not itself directly stated for the EXP-port instance; it requires applying `GTO1`'s own
"each port... functions in the same manner" statement to extend CTRL1/CTRL2's documented R/W status
and semantics to the third (EXP) instance specifically (**Project inference**, per the provenance
table above). The SEG-007-T020 section above already applies a project compatibility policy of an
all-zero, "no-special-bits-set" value to CTRL1 and CTRL2 (`INT`=0, "TH-INT PROHIBITED"; `PC6`-`PC0`=0,
"INPUT MODE", per `GTO1` p. 75's per-bit CTRL table). Because CTRL3 is inferred to be the identical
register type as CTRL1/CTRL2 — not a different register with its own distinct documented semantics,
the way SEG-007-T034 found for other candidates such as S-CTRL and TxDATA, which carry additional
serial-mode/interrupt/status semantics CTRL does not — reusing the same zero value for CTRL3 is a
uniform application of this project's existing CTRL-type policy to a third instance of that inferred
same type, not a new analogy to a different register class. This is the "each port... functions in the
same manner" structural-identity argument, applied to the register *type* itself, which is distinct
from (and does not depend on) the RxDATA-specific "unresolved value, deliberately chosen deterministic
zero" analogy SEG-007-T034/T036 considered and declined to extend to CTRL3. `GTO1` does not state
CTRL3's concrete CPU-visible read-back value (**Still unresolved**); this policy's zero value is
therefore labeled **Project compatibility policy**, never a hardware claim:

```text
Ctrl3 = 0x00
```

**(b) Upper-lane (`$A1000C`) value.** Decided separately from (a). `GTO1` p. 74's "for WORD access,
only the lower byte is meaningful" rule (a **Publicly documented hardware fact** applying to the full
15-row port-register table, per SEG-007-T034) means the upper lane carries no meaningful register
data for this WORD transfer; its concrete driven bit pattern remains **Still unresolved**, exactly as
the SEG-007-T020 section above already records for CTRL1/CTRL2's own upper lanes. This policy assigns
it `0x00`, matching the T020 section's existing upper-lane convention for the identical structural
situation (a meaningless-for-WORD-access upper lane immediately preceding a CTRL-type register's
lower lane). This is labeled **Project compatibility policy**, not a hardware claim:

```text
Upper lane ($A1000C) = 0x00
```

**(c) Side-effect-freedom.** Decided separately from (a) and (b). No source located by
SEG-007-T017/T034 documents a CTRL register read side effect for any port instance (**Still
unresolved**); `GTO1`'s p. 74 port-register table documents no read side effect for any of its 15
rows generally. This policy therefore selects side-effect-free behavior (no controller/bus/device
state mutation) for this CTRL3 WORD read, labeled **Project compatibility policy**, matching the
T020 section's identical choice for CTRL1/CTRL2. This choice applies uniformly across instances of
the identical register type for the same reason as (a): side-effect-freedom is not itself
register-type-specific bit content but the absence of any documented read-side-effect mechanism, and
SEG-007-T034's research covered CTRL3 explicitly under that same lack of evidence (not a gap unique
to CTRL1/CTRL2 that CTRL3 might differ from) — so extending T020's identical conclusion to a third
instance of the identical register type is a consistent application of the same reasoning, not a new
unjustified leap.

Thus, under this policy, the assembled WORD result is `0x0000` (upper lane `0x00`, CTRL3 lower lane
`0x00`), and the read is side-effect-free: it does not mutate CTRL3 state, controller input state, or
any other bus/device state.

Charles MacDonald's reported condition-limited `$00` default-state value for CTRL3 (`MCD1`, restated
by SEG-007-T034) is secondary-source context consumed by T034. It is not proof that this initial
state is universal hardware reset, power-on, read-back, or startup behavior. The explicit zero values
above remain project policy even where that report is consistent with them, matching the T020
section's identical treatment of MacDonald's CTRL1/CTRL2 report.

## Typed result boundary

On success, the future device result has these typed fields:

| Field | Required value/property |
| --- | --- |
| `value` | One 16-bit WORD value, `0x0000` under this policy. |
| `lane_observations` | Exactly two ordered byte-lane observations for the single WORD transfer: upper lane (`$A1000C`) `0x00`, then lower lane (CTRL3, `$A1000D`) `0x00`. |
| `policy_provenance` | Identifies this SEG-007-T038 project compatibility policy, distinct from the SEG-007-T020 section's own CTRL1/CTRL2 provenance identity, rather than hardware-fact provenance. |

On failure, no successful value or observation exists. The typed fail-closed result has the same
shape the T020 section already defines, retained unchanged:

| Field | Required value/property |
| --- | --- |
| `category` | `unsupported_device_region_controller_io`. |
| `source_provenance` | Available typed CPU source provenance from the statically lifted memory operation. |
| `address_provenance` | The typed requested target address and resolver/device diagnostic provenance available at rejection. |
| `width` | Requested access width. |
| `direction` | Requested access direction. |

## Fail-closed boundary and ownership seam

Existing pre-region validation remains authoritative, in the same order the T020 section already
establishes:

1. `effective_address_not_24bit`
2. `odd_effective_address`
3. Existing ROM/RAM/device/unmapped region resolution

This CTRL3 policy applies only after the effective address has passed those earlier
address-validation steps and the shared Genesis bus/address resolver has recognized or intersected
the request with the public controller-I/O region — the identical precondition order the T020
section already requires. This policy does not reclassify arbitrary memory as controller I/O, and it
does not alter, narrow, or reorder any of those three steps.

This policy adds **exactly one new selector**; it narrows nothing else. Every other controller-I/O
access shape remains exactly as fail-closed as it already is:

- A BYTE read of CTRL3. *(Superseded by the SEG-007-T111 section below: a BYTE read of CTRL3 at
  `$A1000D` is now an accepted selector. Every other item in this list is unchanged.)*
- A LONG read intersecting CTRL3.
- A write to CTRL3.
- Every other controller-I/O register (DATA, Version, S-CTRL, TxDATA, RxDATA), in any port instance.
- Every other frontier profile.
- The existing SEG-007-T020/T021 CTRL1/CTRL2 selector outside its own already-implemented shape —
  that selector's own success case, fail-closed boundary, and typed result boundary are entirely
  unchanged by this section and are not extended, narrowed, or reinterpreted by it.

Each such case retains the `unsupported_device_region_controller_io` category with the same typed
source/address/width/direction provenance fields defined above. No value, lane content, ordering,
side effect, or fallback behavior is guessed for any of them.

A later implementation has one ownership seam only, reusing exactly the T020 section's own wording:

```text
statically lifted CPU memory operation
  -> Genesis bus/address resolver
  -> controller-I/O device semantics
  -> typed read result or fail-closed result
```

The CPU operation supplies typed address, width, read intent, and source provenance. The bus/address
resolver recognizes and routes the bounded region without deriving provenance from host storage. The
device supplies the typed WORD result or the retained failure. No instruction-specific emitter,
including a `TST.W` path, owns controller semantics.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine this policy's CTRL3 lower-byte
value, upper-lane value, side-effect rule, or failure boundary: it must directly cover this exact
functional selector and the exact affected CPU-visible behavior, record its provenance and limits,
and receive independent validation. Address validity and alignment remain owned by the earlier
effective-address validation stage and are not part of this controller-device policy selector; that
validation must retain its `effective_address_not_24bit` then `odd_effective_address` precedence.
Replacement must also retain neighboring controller-I/O-stage mismatches as fail-closed (including
width, direction, register-shape, and partial/crossing mismatches), and it must not silently narrow
or reopen the unrelated SEG-007-T020 CTRL1/CTRL2 selector above. A directly supporting hardware
source (for example a newly located public source documenting CTRL3's concrete read-back value) may
establish a hardware fact; a qualified independent observation remains distinct from that fact; a
derivation remains project inference. Evidence that does not establish CTRL3's exact CPU-visible
behavior leaves this policy labeled policy and the underlying hardware behavior unresolved.
Replacement must not silently upgrade this zero policy into an authoritative hardware claim.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or
derived fixture. It does not model general controller reads, interactive input, TH/serial behavior,
timing, reset sequencing, other I/O ports, VDP, rendering, audio, Z80 behavior, or a complete Genesis
bus. It makes no VDP, rendering, interactive-input, or title-screen-completion claim. It adds no
controller, bus, device, CPU, emitter, routing, fixture, or validation code change of any kind; it is
a decision record only, exactly like the SEG-007-T020 section above.

---

# Genesis Version-register hardware-configuration compatibility policy (SEG-007-T079)

## Status and boundary

This is a third, independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict either the SEG-007-T020 or SEG-007-T038 sections
above, both of which remain unchanged and continue to govern the CTRL1/CTRL2 and CTRL3 selectors
exactly as before. This section consumes only: SEG-007-T078's confirmed classification (a BYTE read
of the public Version register, `m68k_controller_io_version_register_address`, `$A10001`, confirmed by
direct, privacy-safe reproduction); SEG-007-T034's complete public register-map research and
provenance ledger (in particular claims 5-9, the Version register's per-register findings, and
Scope-item-6's "Version" bullet); and the SEG-007-T020/T038 sections above as the direct structural
template for this policy's shape and discipline. It applies only to the one functionally identified
BYTE-read shape defined below; it neither names nor derives a private target address, and it does not
reopen or extend T034/T078's own verdicts.

Unlike CTRL1/CTRL2/CTRL3 — an R/W register with no write-derived CPU-visible content and no documented
"this value has real observable meaning" semantics, where an all-zero "no special bits set" policy
value is defensible as a deliberate simplification of a register whose real content this project does
not model at all — the Version register's per-bit meaning is fully documented (`GTO1` p. 72 §1, cited
by SEG-007-T034 claim 7) and each of its upper four bits encodes a real, externally observable fact
about which physical console configuration is running: `MODE` (region: domestic/overseas), `VMOD`
(video timing: NTSC/PAL), `DISK` (FDD-unit connection presence), and `RSV` (reserved, "currently not
used"). `VER3-0` (hardware revision nibble) is **not** part of this section's own decision: SEG-007-T034
already established its value is `0x0`, "the present hardware version," per `GTO1`; this section restates
that unchanged, with its existing **Publicly documented hardware fact** label, and does not re-derive it.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | The Version register's address (`$A10001`); its per-bit field layout (`MODE`/`VMOD`/`DISK`/`RSV`/`VER3-0`) and each field's documented 0/1-to-meaning correspondence; BYTE access is documented supported (Sega's own sample code, `GTO1` p. 82); every bit is marked `(R)` (read-only, no write path exists); `VER3-0 = 0x0`, "the present hardware version." | `GTO1` p. 72 §1 and p. 82, cited by SEG-007-T034 claims 5, 7, 9 (restated, not re-derived). This does not establish which physical console configuration this project's generated runtime represents, nor a universal reset/default value for `MODE`/`VMOD`/`DISK`/`RSV` — `GTO1` states none. |
| **Project inference** | `MODE = 1` (overseas) and `DISK = 1` (FDD unit not connected) for this project's own generated runtime, derived below from `GTO1`'s documented bit-meaning fact combined with this project's own already-established target scope (the pinned milestone Sonic input's own public naming, and this project's Mission of translating retail cartridge software, never Sega's own internal FDD-equipped development hardware). This section's independent decoding of the combined byte, and its explicit, separately reasoned convergence with SEG-007-T034's own restated `MCD1`-decoding context (claims 8-9), are also project inference, not hardware fact. | This section's own reasoning below, distinct from and not dependent on `MCD1`'s reported byte as its basis (SEG-007-T034's own decoding of `MCD1`'s `$A0` remains condition-limited secondary-source context, restated unchanged, never promoted to this section's own justification). |
| **Still unresolved** | Any universal/guaranteed reset or power-on value for `MODE`/`VMOD`/`DISK`/`RSV`; WORD/LONG access support for the Version register (no source documents these forms). | SEG-007-T034's own provenance ledger claims 6 and the Version-register per-register findings, restated unchanged. |
| **Project compatibility policy** | `VMOD = 0` (NTSC), `RSV = 0`, side-effect-free behavior, and the exact typed success/failure result boundary below. | Deliberate deterministic project choices for gaps this section's own reasoning could not resolve by citation or scope alone, decided and justified separately per dimension below; not historical/device truth. |

## Selected deterministic policy

The one accepted selector has **all** of these dimensions: direction is read; width is BYTE (never
WORD, never LONG, never a write); the address has passed existing earlier effective-address
validation, and has been recognized or intersected by the shared Genesis bus/address resolver as the
controller-I/O region; the address resolves exactly to the public Version register
(`m68k_controller_io_version_register_address`, `$A10001`). This selector applies only within the
`general_startup` route's existing controller-I/O resolver stage — the same stage the T020/T038
sections above already occupy — never as an instruction-specific (for example `TST.B`) emitter case.
It defines this one bounded BYTE-read shape without recovering or requiring any private frontier
address, image offset, or instruction provenance.

This policy decides four distinct sub-fields separately, plus one composition step; none is a silent
copy of another, and none is described as an authoritative Genesis hardware fact.

**(a) `MODE` (bit 7): domestic vs. overseas region.** `GTO1` p. 72 §1 documents this bit's field
identity and its 0/1-to-meaning correspondence directly (**Publicly documented hardware fact**, per
SEG-007-T034 claim 7): `0` = domestic (Japan), `1` = overseas. `GTO1` states no default/reset value.
This project's own already-established target scope reasonably justifies one specific choice here,
independent of any secondary-source report: the milestone's own pinned Sonic input is publicly named
"Sonic The Hedgehog (USA, Europe)" — a standard commercial regional-release naming convention that
identifies this exact software release as the non-Japan (export/overseas) SKU, not the Japanese
domestic release. Because `MODE` distinguishes only Japan from every non-Japan territory (both "USA"
and "Europe" fall on the same, overseas, side of this one bit), the combined "USA, Europe" naming
unambiguously identifies this project's own target release as overseas without needing to further
resolve which non-Japan territory specifically. This section therefore decides:

```text
MODE = 1 (overseas)
```

labeled **Project inference** — a scope-based inference from this project's own already-established
pinned input, not a claim about universal Genesis hardware default state. This value happens to match
SEG-007-T034's own separately-recorded decoding of `MCD1`'s reported `$A0` byte against `GTO1`'s bit
layout (`MODE = 1`); this is noted here as an explicit, distinct convergence between two independent
reasoning paths — this section's own pinned-input-scope reasoning above, and T034's unrelated secondary-
source decode — not as this section borrowing or resting on that secondary-source value.

**(b) `VMOD` (bit 6): NTSC vs. PAL video timing.** `GTO1` p. 72 §1 documents this bit's field identity
and 0/1-to-meaning correspondence directly (**Publicly documented hardware fact**, per SEG-007-T034
claim 7): `0` = NTSC, `1` = PAL. `GTO1` states no default/reset value. Unlike `MODE`, this project's
own target scope does **not** reasonably justify one specific choice here: the pinned Sonic input's own
"USA, Europe" naming is a merged regional-release tag that spans both an NTSC territory (USA) and a PAL
territory (Europe) under one combined commercial release, so it does not itself disambiguate NTSC from
PAL the way it disambiguates overseas from domestic for `MODE`. This repository's Mission statement
(the project charter) states only that "Genesis, Master System, and Game Gear are the first targets"; it names
no NTSC/PAL sub-choice. No further already-cited public source states a common/default/typical value
for this bit beyond `MCD1`'s condition-limited secondary report, which this section does not use as its
own citation basis (per this task's own Non-goals). With neither a citation nor a scope-based
justification available, this section decides it anyway as an explicitly labeled arbitrary,
deliberately deterministic choice, matching SEG-007-T020's own precedent for its own unresolved CTRL
lanes:

```text
VMOD = 0 (NTSC)
```

labeled **Project compatibility policy** — an arbitrary but deliberate deterministic choice, not a
scope-justified inference and not a hardware claim. This value happens to match SEG-007-T034's own
separately-recorded `MCD1` decode (`VMOD = 0`); this is noted as an explicit, distinct convergence
between this section's own independent zero-default policy choice and that unrelated secondary-source
context, not as this section adopting that source's value.

**(c) `DISK` (bit 5): FDD-unit connection presence.** `GTO1` p. 72 §1 documents this bit's field
identity and 0/1-to-meaning correspondence directly (**Publicly documented hardware fact**, per
SEG-007-T034 claim 7): `0` = FDD unit connected, `1` = FDD unit not connected (`GTO1` documents only
this connected/not-connected distinction; this section states nothing beyond what is printed there,
matching SEG-007-T034's own explicit scoping). `GTO1` states no default/reset value. This project's own
already-established target scope reasonably justifies one specific choice here: this repository's
Mission is to translate "supported Sega software" — retail cartridge titles such as the pinned Sonic
input — preserving "observable console behavior" as run on the shipped consumer Genesis/Mega Drive
console; the FDD unit `GTO1` documents was a Sega development/manufacturing accessory, never a
component of the retail consumer hardware this project's generated runtime represents. This section
therefore decides:

```text
DISK = 1 (FDD unit not connected)
```

labeled **Project inference** — a scope-based inference from this project's own already-established
target (retail consumer Genesis hardware, not Sega's internal development configuration), not a claim
about universal Genesis hardware default state. This value happens to match SEG-007-T034's own
separately-recorded `MCD1` decode (`DISK = 1`); this is noted as an explicit, distinct convergence, not
as this section borrowing that secondary-source value.

**(d) `RSV` (bit 4): reserved.** `GTO1` p. 72 §1 documents this bit as reserved, "currently not used,"
with no further documented meaning, default, or effect (**Publicly documented hardware fact**, per
SEG-007-T034 claim 7, restated). No source documents a concrete value for a reserved bit with no
documented meaning; this section decides it anyway as an explicitly labeled arbitrary, deliberately
deterministic choice, matching SEG-007-T020's own zero-value precedent for its own unresolved,
unmodeled lanes:

```text
RSV = 0
```

labeled **Project compatibility policy**.

**(e) Composition and side-effect-freedom.** `VER3-0 = 0x0` is restated unchanged from SEG-007-T034
(**Publicly documented hardware fact**, `GTO1` p. 72 §1, "the present hardware version is indicated by
$0"), not re-derived. Assembling `MODE`(bit7)=1, `VMOD`(bit6)=0, `DISK`(bit5)=1, `RSV`(bit4)=0, and
`VER3-0`(bits3-0)=`0000` (binary `1010 0000`) gives the single policy byte:

```text
Version = 0xA0
```

This section observes, explicitly and only after independently deciding each sub-field above by its
own separate reasoning, that this composed byte equals SEG-007-T034's own separately-recorded `MCD1`
default-state report for the Version register. This is recorded as a genuine convergence between two
independent reasoning paths (this section's own per-bit pinned-input-scope/target-hardware-scope/
zero-default reasoning above, versus `MCD1`'s wholly separate condition-limited secondary report) —
never as this section simply restating or deriving its value from `MCD1`'s report, which remains, per
SEG-007-T034's own established treatment, condition-limited secondary-source context and not this
section's own citation basis. No source located by SEG-007-T017/T034 or this section documents a
Version-register read side effect (**Still unresolved**); this policy therefore selects side-effect-
free behavior (no controller/bus/device state mutation) for this read, labeled **Project compatibility
policy**, matching the T020/T038 sections' identical choice for CTRL1/CTRL2/CTRL3.

## Typed result boundary

On success, the future device result has these typed fields:

| Field | Required value/property |
| --- | --- |
| `value` | One 8-bit BYTE value, `0xA0` under this policy. |
| `word_observations` | Exactly one populated observation for the single BYTE transfer (`0xA0`, ordinal `0`); the second slot is left at its default-constructed zero state, because a BYTE transfer touches exactly one byte on the bus and has no second lane the way CTRL1/CTRL2's two real WORD transfers, or CTRL3's two real byte lanes within one WORD transfer, do. |
| `policy_provenance` | Identifies this SEG-007-T079 project compatibility policy, distinct from the SEG-007-T020 and SEG-007-T038 sections' own provenance identities, rather than hardware-fact provenance. |

On failure, no successful value or observation exists. The typed fail-closed result has the same shape
the T020/T038 sections already define, retained unchanged:

| Field | Required value/property |
| --- | --- |
| `category` | `unsupported_device_region_controller_io`. |
| `source_provenance` | Available typed CPU source provenance from the statically lifted memory operation. |
| `address_provenance` | The typed requested target address and resolver/device diagnostic provenance available at rejection. |
| `width` | Requested access width. |
| `direction` | Requested access direction. |

## Fail-closed boundary and ownership seam

Existing pre-region validation remains authoritative, in the same order the T020/T038 sections already
establish:

1. `effective_address_not_24bit`
2. `odd_effective_address`
3. Existing ROM/RAM/device/unmapped region resolution

This Version-register policy applies only after the effective address has passed those earlier
address-validation steps and the shared Genesis bus/address resolver has recognized or intersected the
request with the public controller-I/O region — the identical precondition order the T020/T038 sections
already require. This policy does not reclassify arbitrary memory as controller I/O, and it does not
alter, narrow, or reorder any of those three steps.

This policy adds **exactly one new selector**; it narrows nothing else. Every other controller-I/O
access shape remains exactly as fail-closed as it already is:

- A WORD read of the Version register.
- A LONG read of the Version register.
- A write to the Version register (any width).
- Every other controller-I/O register (DATA, CTRL, S-CTRL, TxDATA, RxDATA), in any port instance.
- Every other frontier profile.
- The existing SEG-007-T020/T021 CTRL1/CTRL2 selector and the SEG-007-T038 CTRL3 selector, outside
  their own already-implemented shapes — those selectors' own success cases, fail-closed boundaries,
  and typed result boundaries are entirely unchanged by this section and are not extended, narrowed, or
  reinterpreted by it.

Each such case retains the `unsupported_device_region_controller_io` category with the same typed
source/address/width/direction provenance fields defined above. No value, observation, ordering, side
effect, or fallback behavior is guessed for any of them.

A later implementation has one ownership seam only, reusing exactly the T020/T038 sections' own
wording:

```text
statically lifted CPU memory operation
  -> Genesis bus/address resolver
  -> controller-I/O device semantics
  -> typed read result or fail-closed result
```

The CPU operation supplies typed address, width, read intent, and source provenance. The bus/address
resolver recognizes and routes the bounded region without deriving provenance from host storage. The
device supplies the typed BYTE result or the retained failure. No instruction-specific emitter,
including a `TST.B` path, owns controller semantics.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine this policy's `MODE`/`VMOD`/`DISK`/
`RSV` values, side-effect rule, or failure boundary: it must directly cover this exact functional
selector and the exact affected CPU-visible behavior, record its provenance and limits, and receive
independent validation. Address validity and alignment remain owned by the earlier effective-address
validation stage and are not part of this controller-device policy selector; that validation must
retain its `effective_address_not_24bit` then `odd_effective_address` precedence. Replacement must also
retain neighboring controller-I/O-stage mismatches as fail-closed (including width, direction, and
register-shape mismatches), and it must not silently narrow or reopen the unrelated SEG-007-T020
CTRL1/CTRL2 selector or the SEG-007-T038 CTRL3 selector above. A directly supporting hardware source
(for example a newly located public source documenting a universal Version-register reset value) may
establish a hardware fact; a qualified independent observation remains distinct from that fact; a
derivation remains project inference. Evidence that does not establish a sub-field's exact CPU-visible
behavior leaves policy labeled policy and the underlying hardware behavior unresolved. Replacement must
not silently upgrade this policy's `MODE`/`VMOD`/`DISK`/`RSV` values into an authoritative hardware
claim; `MODE`/`DISK` remain this section's own scope-based project inference, and `VMOD`/`RSV` remain
this section's own arbitrary project compatibility policy, even where a secondary source happens to
agree with the composed byte.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or
derived fixture. It does not model general controller reads, interactive input, TH/serial behavior,
timing, reset sequencing, other I/O ports, VDP, rendering, audio, Z80 behavior, or a complete Genesis
bus. It makes no VDP, rendering, interactive-input, or title-screen-completion claim, and it makes no
claim beyond this one register about which broader physical Genesis hardware unit this project's
generated runtime represents. It adds no controller, bus, device, CPU, emitter, routing, fixture, or
validation code change of any kind beyond the one selector this section defines; it is a decision
record only, exactly like the SEG-007-T020/T038 sections above.

---

# Genesis controller-I/O EXP-port CTRL 3 BYTE-read compatibility policy (SEG-007-T111)

## Status and boundary

This is a fourth, independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict the SEG-007-T020, SEG-007-T038, or SEG-007-T079
sections above, all of which remain unchanged and continue to govern the CTRL1/CTRL2 LONG-read,
CTRL3 WORD-read, and Version-register BYTE-read selectors exactly as before. This section consumes
only: SEG-007-T090's validated runtime-selected continuation handoff (a runtime-execution-stage
**BYTE read**, direct absolute-long `(xxx).L` addressing, of a controller-I/O in-window selector the
contract did not yet route, reached by a `BTST` with an immediate/static bit number); SEG-007-T034's
complete public register-map research and provenance ledger; and the SEG-007-T038 section above as
the direct structural template for this policy's shape and discipline — this selector is the odd/low
**meaningful-data byte lane** (`$A1000D`) of the very same EXP-port CTRL register whose WORD-read
selector T038 already governs.

Because this selector reads exactly the same physical register as the T038 WORD selector, only via a
BYTE rather than a WORD access, every hardware fact, project inference, still-unresolved item, and
compatibility-policy choice T038 recorded for CTRL3's lower byte is carried over here unchanged. This
section adds no new hardware claim beyond restating that BYTE access is documented supported and that
on a BYTE access the lower byte is the meaningful register data.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | CTRL3's address (`$A1000D`) and BYTE/WORD access support; for a BYTE access, the lower byte is the meaningful register data; CTRL is R/W; each general-purpose I/O port "functions in the same manner", extending CTRL1/CTRL2's documented behavior to the EXP port's own CTRL instance. | `GTO1` pp. 72, 74, 75, cited by SEG-007-T034 and restated by the SEG-007-T038 section above (restated, not re-derived). This does not establish CTRL3's returned bit pattern or any CTRL-register read side effect. |
| **Project inference** | "CTRL 3" denotes the EXP port's own instance of the shared "CTRL (PARALLEL CONTROL)" register type; its R/W status and semantics are the identical CTRL register type as CTRL1/CTRL2. | SEG-007-T034's provenance ledger claim 3 and the SEG-007-T038 section's own restatement, unchanged. It is not a public claim about a private address. |
| **Still unresolved** | CTRL3's exact CPU-visible read-back bit pattern beyond its documented write-oriented per-bit configuration meaning; any CTRL-register read side effect for any port instance; initial/reset/power-on applicability at this frontier. | Carried over verbatim from the SEG-007-T038 section's own **Still unresolved** row. `MCD1`'s condition-limited reported `$00` default for CTRL3 does not resolve these items. |
| **Project compatibility policy** | The CTRL3 BYTE value `0x00`, side-effect-free behavior, and the typed success/failure result boundary below. | Deliberate deterministic project choice for the unresolved gap, identical in value and justification to the SEG-007-T038 section's own CTRL3 lower-byte choice; not historical/device truth. |

## Selected deterministic policy

The one accepted selector has **all** of these dimensions: direction is read; width is BYTE (never
WORD, never LONG, never a write); the address has passed existing earlier effective-address
validation (a BYTE access carries no alignment constraint, so the odd address `$A1000D` reaches the
controller-I/O resolver stage); the shared Genesis bus/address resolver has recognized or intersected
the request with the public controller-I/O region; the address resolves exactly to the public
EXP-port CTRL 3 register (`m68k_controller_io_ctrl3_register_address`, `$A1000D`). This selector
applies only within the existing controller-I/O resolver stage — the same stage the T020/T038/T079
sections above already occupy — never as an instruction-specific (for example `BTST.B`) emitter case.

**CTRL3 BYTE value.** `GTO1` p. 75's per-bit CTRL table documents only the register's write-oriented
per-bit configuration meaning (`INT`, `PC6`-`PC0`, each `(RW)`); it does not state CTRL3's concrete
CPU-visible read-back value (**Still unresolved**). Because CTRL3 is inferred to be the identical
CTRL register type as CTRL1/CTRL2, and the SEG-007-T020 and SEG-007-T038 sections already selected an
all-zero "no special bits set" value (INPUT MODE, TH-interrupt prohibited) as a deliberate
simplification of a register whose real content this project does not model, this section reuses the
identical value for the identical register via the narrower BYTE access:

```text
CTRL3 (BYTE, $A1000D) = 0x00
```

labeled **Project compatibility policy** — the same deliberate deterministic choice, for the same
register, as the SEG-007-T038 section's own CTRL3 lower-byte value, not a new or independent claim.
No source located by SEG-007-T017/T034 or this section documents a CTRL-register read side effect
(**Still unresolved**); this policy therefore selects side-effect-free behavior (no
controller/bus/device state mutation) for this read, matching the T020/T038/T079 sections' identical
choice.

## Typed result boundary

On success, the future device result has these typed fields:

| Field | Required value/property |
| --- | --- |
| `value` | One 8-bit BYTE value, `0x00` under this policy. |
| `word_observations` | Exactly one populated observation for the single BYTE transfer (`0x00`, ordinal `0`); the second slot is left at its default-constructed zero state, exactly as the SEG-007-T079 section defines for a BYTE transfer. |
| `policy_provenance` | Identifies this SEG-007-T111 project compatibility policy, distinct from the SEG-007-T020, SEG-007-T038, and SEG-007-T079 sections' own provenance identities, rather than hardware-fact provenance. |

On failure, no successful value or observation exists. The typed fail-closed result has the same
shape the T020/T038/T079 sections already define, retained unchanged (`category` =
`unsupported_device_region_controller_io`; typed source/address/width/direction provenance retained).

## Fail-closed boundary and ownership seam

Existing pre-region validation remains authoritative, in the same order the T020/T038/T079 sections
already establish (`effective_address_not_24bit`, then `odd_effective_address`, then existing
ROM/RAM/device/unmapped region resolution). This policy applies only after those steps and does not
alter, narrow, or reorder any of them.

This policy adds **exactly one new selector**; it narrows nothing else. Every other controller-I/O
access shape remains exactly as fail-closed as it already is:

- A WORD read at `$A1000D` (odd-address fail before the controller-I/O stage) and a LONG read
  intersecting `$A1000D`.
- A BYTE read of the CTRL3 selector's even upper lane (`$A1000C`).
- A write to CTRL3 at any width.
- Every other controller-I/O register (DATA, Version, S-CTRL, TxDATA, RxDATA), in any port instance,
  and every other frontier profile.
- The existing SEG-007-T020/T021 CTRL1/CTRL2 selector, the SEG-007-T038 CTRL3 WORD selector, and the
  SEG-007-T079 Version-register selector, outside their own already-implemented shapes — those
  selectors' own success cases, fail-closed boundaries, and typed result boundaries are entirely
  unchanged by this section.

The DATA registers (which encode genuinely dynamic controller input state, with no defensible
deterministic policy) and the S-CTRL / TxDATA / RxDATA serial registers (distinct serial semantics)
remain outside this section's scope; a later frontier reaching one of those is a different owner, not
a bounded extension here.

The ownership seam is unchanged, reusing exactly the T020/T038/T079 sections' own wording:

```text
statically lifted CPU memory operation
  -> Genesis bus/address resolver
  -> controller-I/O device semantics
  -> typed read result or fail-closed result
```

No instruction-specific emitter, including a `BTST.B` path, owns controller semantics.

## Supersession note

The SEG-007-T038 section's "Fail-closed boundary" list item "A BYTE read of CTRL3." is superseded by
this section: that exact shape is now the accepted SEG-007-T111 selector. The T038 section's own
CTRL3 WORD selector, its typed result boundary, and every other item in its fail-closed list remain
unchanged.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine this policy's CTRL3 BYTE value,
side-effect rule, or failure boundary: it must directly cover this exact functional selector and the
exact affected CPU-visible behavior, record its provenance and limits, and receive independent
validation. Because this value is shared with the SEG-007-T038 section by construction, evidence that
refines CTRL3's read-back value refines both sections together. Replacement must retain neighboring
controller-I/O-stage mismatches as fail-closed and must not silently narrow or reopen the unrelated
SEG-007-T020, SEG-007-T038, or SEG-007-T079 selectors above.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or
derived fixture beyond the already-public `$A1000D`. It does not model general controller reads,
interactive input, TH/serial behavior, timing, reset sequencing, other I/O ports, VDP, rendering,
audio, Z80 behavior, or a complete Genesis bus. It adds one controller-I/O selector plus one bounded
C4 preflight consistency fix that lets a retained `BTST` static memory fact reuse the same routing
owner every other retained read already uses; it is otherwise a decision record, exactly like the
SEG-007-T020/T038/T079 sections above.

---

# Genesis VDP CONTROL-port/status-register WORD-read compatibility policy (SEG-007-T081)

## Status and boundary

This is a fourth, independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict the SEG-007-T020, SEG-007-T038, or SEG-007-T079
sections above, all of which remain unchanged and continue to govern the CTRL1/CTRL2, CTRL3, and
Version-register selectors exactly as before. Unlike those three sections, which are built through the
pre-SEG-007-T042 static-resolver architecture (`src/m68k_pipeline.cpp`'s `m68k_controller_io_access`/
`m68k_route_genesis_device_access`), this section is built exclusively through [the persistent
device-state and checkpoint-evidence contract](genesis-persistent-device-state-and-checkpoint-evidence-contract.md)
(SEG-007-T042, `done`) SS1.1-SS1.3, SS2.1-SS2.4, SS3-SS5, and SS8: the runtime-bridge dispatch pair
`genesis_is_vdp_region`/`genesis_vdp_access` (`tools/genesis_startup_bridge_runtime.c`), reached from
`genesis_route_access` in the same position `genesis_is_device` already occupies, never the static
resolver. This section consumes only: SEG-007-T080's own confirmed outer-boundary classification (a WORD
read within the Genesis VDP register area, per `GTO1` p. 10's "VDP AREA" diagram and `MCD1` SS1); this
task's own further, privacy-safe classification narrowing that WORD read to the VDP CONTROL port's base
address; and the SEG-007-T042 contract as the direct architectural template this section builds through
without making any new architecture decision of its own. It applies only to the one functionally
identified WORD-read shape defined below; it neither names nor derives a private target address beyond
the already-public `GTO1`/`MCD1` addresses cited, and it does not reopen or extend SEG-007-T080's own
verdict.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | The VDP register area's base addresses for the DATA port, CONTROL port, HV COUNTER, and PSG 76489 sub-registers, in that address order; the CONTROL port's own base address also serves as the VDP status-register read path; the status register's own documented per-bit layout includes FIFO-empty/FIFO-full, VBlank/HBlank-pending, DMA-busy, sprite-collision/overflow, odd-frame (interlace), and PAL/NTSC flags; VDP register-set access is documented WORD/LONG (never BYTE-only). | `GTO1` (already cited by SEG-007-T034/T077/T078/T079/T080) p. 10 ("VDP AREA" diagram, naming DATA/CONTROL/HV COUNTER/PSG 76489 sub-registers in address order) and p. 19 (status-register bit layout; "You must use word or long word access to VDP ports when setting the registers"), independently corroborated by `MCD1` (already cited by the same prior tasks) SS1/SS3 (VDP port address table; the control-port base address's own read path returning the status register). This does not establish a concrete status-register bit value at any specific point in a running program, nor a documented power-on/reset default for the status register as a whole. |
| **Project inference** | The one WORD read this task's own bounded reproduction (Step 4 below) confirms is targeted at the VDP CONTROL port's own base address, not the DATA port, HV COUNTER, or PSG 76489 sub-offsets `GTO1` p. 10 also names, and not the CONTROL port's own second documented mirror lane. | This task's own two independently-agreeing direct reproduction techniques (Step 4), applying `GTO1`/`MCD1`'s already-public sub-offset addresses to the already-settled outer VDP-area boundary SEG-007-T080 confirmed. It is not itself a public claim about a private address; only the already-public sub-offset address is named. |
| **Still unresolved** | The status register's real, dynamic, execution-time-dependent bit pattern at any specific point during a running program (VBlank-pending, HBlank-pending, DMA-busy, FIFO-empty/full, sprite-collision/overflow, and odd-frame all vary with real hardware timing/state this project does not currently model); any documented power-on/reset default for the composed status-register word as a whole; read-back behavior of the DATA port, HV COUNTER, or PSG 76489 sub-registers (out of this section's own scope entirely -- see Non-goals). | `GTO1`/`MCD1` document the status register's per-bit *meaning*, not its value at an unspecified point in program execution, and this project's own architecture (SEG-007-T042, cited above) does not yet implement any VDP register-write, DMA, or interrupt state-mutation path that would set any of those bits to a non-default value. |
| **Project compatibility policy** | The exact routed value (the live, zero-initialized `GenesisVdpState.status_register` field, per SEG-007-T042 SS1.3/SS5/SS8, never a fixed hardware-identity byte the way the Version register's `0xA0` is), side-effect-free behavior, and the typed success/failure result boundary below. | Deliberate deterministic project choice within the already-decided SEG-007-T042 architecture, not historical/device truth. |

## Selected deterministic policy

The one accepted selector has **all** of these dimensions: direction is read; width is WORD (never BYTE,
never LONG, never a write); the address has passed existing earlier effective-address validation,
including alignment; the address resolves exactly to the public VDP CONTROL port's base address
(`$C00004`); the returned value is the current value of `GenesisRuntime.devices.vdp.status_register`
(SEG-007-T042 SS1.3), not a hardcoded scalar constant. This selector applies only within
`genesis_route_access`'s own `genesis_is_vdp_region`/`genesis_vdp_access` dispatch stage (SEG-007-T042
SS2.1), reached in the same position `genesis_is_device` already occupies, never as an
instruction-specific emitter case. It defines this one bounded WORD-read shape without recovering or
requiring any private frontier address, image offset, or instruction provenance.

This policy decides three distinct dimensions separately; none is a silent copy of another, and none is
described as an authoritative Genesis hardware fact.

**(a) The routed value is the live `status_register` field, not a fixed policy byte.** Unlike CTRL1/
CTRL2/CTRL3 (an entirely unmodeled register given one deliberately simplified constant value) or the
Version register (a genuinely static hardware-identity byte this project decided once, per-bit, and never
changes at runtime), the VDP status register is architecturally a **live, observed** register under
SEG-007-T042 SS5: "the routed `genesis_*_access` function computes the current bit pattern from
`GenesisDeviceState`'s current fields, returns it as `*value`." This task therefore does not invent a
fixed composed byte the way the Version-register section above did; it implements exactly what SS5
already specifies -- a direct read of `GenesisRuntime.devices.vdp.status_register`. Because this project
currently implements no VDP register-write, control-port-command, DMA, or interrupt state-mutation path
of any kind (those remain SEG-007-T045/T047's own future scope, and this section makes no claim about
when or how they will populate this field), that field remains at its SEG-007-T042 SS8 zero-initialized
default for the entire lifetime of every currently-generated program, so this selector currently and
always returns:

```text
Status register (as currently observed) = 0x0000
```

This is labeled **Project compatibility policy**, not a hardware claim: it does not assert that real
Genesis VDP hardware would report an all-zero status word at this or any other point in a running
program (`GTO1`/`MCD1`'s own documented bit meanings make clear several bits are genuinely dynamic and
execution-dependent -- **Still unresolved**, per the provenance table above); it only implements this
project's own already-decided architecture (SEG-007-T042 SS5/SS8) faithfully, for a register this
project does not yet have any write/mutation path for.

**(b) Sub-offset identity.** Decided separately from (a). `GTO1` p. 10's "VDP AREA" diagram directly
names DATA, CONTROL, HV COUNTER, and PSG 76489 as four distinct, address-ordered sub-registers within the
VDP register area (**Publicly documented hardware fact**, per the provenance table above); `GTO1` p. 19
and `MCD1` SS3 both independently state the CONTROL port's own base address also serves as the VDP
status-register read path. This task's own bounded reproduction (Step 4 below) confirms, via two
independently-agreeing techniques, that the one WORD read SEG-007-T080 already classified as "within the
VDP register area" falls exactly at the CONTROL port's base address, not any of the other three named
sub-offsets. This sub-offset identification is **Project inference** (applying already-public `GTO1`/
`MCD1` addresses to this task's own reproduction result), not itself a new hardware fact.

**(c) Side-effect-freedom.** Decided separately from (a) and (b). No source located by this task documents
a VDP status-register read side effect beyond the status register's own already-documented, individually
named latched bits (for example, a VBlank-interrupt-happened flag that some VDP implementations clear on
status read) -- but per SEG-007-T042 SS5, "a status-register read may clear a documented latched bit... only
if the consuming task cites that clear-on-read behavior as a public fact or labels it an explicit project
compatibility policy; absent such a citation or label, a status-register read is side-effect-free." This
task locates no such citation and does not invent one; this selector is therefore side-effect-free (no
mutation of `status_register` or any other runtime/device state), labeled **Project compatibility
policy**, matching the SS5 default exactly.

## Typed result boundary

Unlike the SEG-007-T020/T038/T079 sections above (built through the pre-SEG-007-T042 static-resolver
typed result, `M68kControllerIoResult`/`M68kControllerIoFailure`), this section is built exclusively
through SEG-007-T042's own already-decided runtime-bridge dispatch boundary
(`tools/genesis_startup_bridge_runtime.h`'s `genesis_route_access`). It adds no static-resolver typed
result of any kind; `src/m68k_pipeline.cpp`/`include/segarecomp/m68k_pipeline.hpp` are unchanged by this
section.

On success, `genesis_route_access` returns `GENESIS_ACCESS_OK` and:

| Field | Required value/property |
| --- | --- |
| `*value` | One 16-bit WORD value, the current `GenesisRuntime.devices.vdp.status_register` field value (`0x0000` for the entire lifetime of every currently-generated program, per this policy's (a) above). |
| `runtime->devices.vdp.status_register` | Unchanged by the read (side-effect-free, per (c) above). |

On failure, `genesis_route_access` returns `GENESIS_ACCESS_FAIL` and populates `*stop_out` with:

| Field | Required value/property |
| --- | --- |
| `stop_class` | `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS`. |
| `diagnostic_category` | `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP`. |

## Fail-closed boundary and ownership seam

Existing pre-region validation remains authoritative, in the same order the SEG-007-T020/T038/T079
sections already establish (`effective_address_not_24bit`, then `odd_effective_address`, then existing
ROM/RAM/device/unmapped region resolution), followed by `genesis_route_access`'s own existing
work-RAM/ROM/controller-I/O-device precedence, entirely unchanged by this section.

This policy adds **exactly one new selector**; it narrows nothing else. Every other VDP-area access shape
remains exactly as fail-closed as before, with `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS`/
`GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP`:

- A BYTE or LONG read of the CONTROL-port base address.
- A write of any width to the CONTROL-port base address.
- The DATA port, HV COUNTER, PSG 76489, or the CONTROL port's own second documented mirror lane, at any
  width or direction.
- Every other address within the recognized VDP-area window this section does not name.
- The existing SEG-007-T020/T021 CTRL1/CTRL2, SEG-007-T038 CTRL3, and SEG-007-T079 Version-register
  selectors, outside their own already-implemented shapes -- those selectors' own success cases,
  fail-closed boundaries, and typed result boundaries are entirely unchanged by this section.

An address outside the recognized VDP-area window entirely remains exactly as fail-closed as before this
section, with the pre-existing `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION`/`GENESIS_DIAG_UNMAPPED_DATA_ACCESS`
result, unchanged by this section.

A later implementation has one ownership seam only, mirroring the SEG-007-T020/T038/T079 sections' own
wording, adapted to SEG-007-T042's own runtime-bridge dispatch shape:

```text
generated Genesis runtime memory/device access
  -> genesis_route_access
  -> genesis_is_vdp_region / genesis_vdp_access
  -> typed OK result (*value) or typed fail-closed result (GenesisRuntimeStop)
```

The generated program supplies the address, width, and direction. `genesis_route_access` recognizes and
routes the bounded VDP-area region. `genesis_vdp_access` supplies the typed WORD result (the live
`status_register` field) or the retained failure. No instruction-specific emitter owns VDP semantics.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine this policy's routed-value rule,
side-effect rule, or failure boundary: it must directly cover this exact functional selector and the
exact affected CPU-visible behavior, record its provenance and limits, and receive independent
validation. Address validity and alignment remain owned by the earlier effective-address validation
stage and are not part of this VDP-device policy selector. Replacement must also retain neighboring
VDP-area-stage mismatches as fail-closed (including width, direction, and sub-offset mismatches), and it
must not silently narrow or reopen the unrelated SEG-007-T020/T038/T079 controller-I/O selectors above. A
directly supporting hardware source (for example a newly located public source documenting a universal
VDP status-register reset value, or this project's own future SEG-007-T045/T047 device-state-mutation
work that would make `status_register` genuinely dynamic) may establish a hardware fact or extend this
project's own architecture; a qualified independent observation remains distinct from that fact; a
derivation remains project inference. Evidence that does not establish this selector's exact CPU-visible
behavior leaves policy labeled policy and the underlying hardware behavior unresolved. Replacement must
not silently upgrade this policy's all-zero observed value into an authoritative hardware claim about real
VDP status-register runtime behavior.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or derived
fixture; every address cited above is already public (`GTO1`/`MCD1`). It does not model the DATA port, HV
COUNTER, PSG 76489, VRAM/CRAM/VSRAM addressing, control-port two-word command sequencing, DMA, interrupts,
rendering, audio, Z80 behavior, general controller reads, interactive input, timing, or a complete Genesis
bus. It makes no rendering, interactive-input, or title-screen-completion claim, and it makes no claim
about real VDP status-register runtime behavior beyond this project's own currently-implemented,
currently-always-zero observed value. It adds no VRAM/CRAM/VSRAM, DMA, or interrupt code change of any
kind; those remain SEG-007-T082/T083/T084's own separate scope. It is a decision record only, exactly like
the SEG-007-T020/T038/T079 sections above.

---

# Genesis VDP CONTROL-port command-word WRITE compatibility policy (SEG-007-T091)

## Status and boundary

This is a fifth, independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict the SEG-007-T020, SEG-007-T038, SEG-007-T079, or
SEG-007-T081 sections above, all of which remain unchanged and continue to govern the CTRL1/CTRL2, CTRL3,
Version-register, and VDP CONTROL-port WORD-read selectors exactly as before -- including SEG-007-T081's
own Privacy note that it "does not model... control-port two-word command sequencing," which this section
now separately supplies. This section is built exclusively through [the persistent device-state and
checkpoint-evidence contract](genesis-persistent-device-state-and-checkpoint-evidence-contract.md)
(SEG-007-T042, `done`) SS1.1-SS1.3, SS2.1-SS2.4, SS3, SS4, and SS8, extending exactly the same
`genesis_is_vdp_region`/`genesis_vdp_access` dispatch pair SEG-007-T081 already reaches from
`genesis_route_access`, never a new dispatch mechanism. It applies only to the WORD- and (per this
section's own second frontier pass, described in (c) below) LONG-write command-word shapes defined below,
addressed exactly at the already-public VDP CONTROL port base address (`$C00004`); it neither names nor
derives any new private target address, and it does not reopen or extend SEG-007-T080 or SEG-007-T081's
own verdicts.

This section's first frontier pass implemented (a) and (b) below (the one-word register-set command and
the non-DMA two-word address-set command), WORD-width only, and deliberately left LONG-word writes to
`$C00004` fail-closed. Re-executing this project's own production route against the same pinned milestone
input immediately afterward reconfirmed the next runtime-selected frontier at the exact same address
(`$C00004`), same command-word family, differing only in access width (LONG instead of WORD) and access
direction already established (WRITE). Per the project charter's Advancement Discipline, this is the same
capability family, same architectural seam (`genesis_vdp_access`), and the same semantic owner as (a)/(b);
the one fact needed to close it -- `GTO1`'s own already-cited long-word-equivalence footnote -- was already
part of this section's own first-pass citation table. This section's own second frontier pass therefore
adds (c) below, through the identical `genesis_vdp_access` dispatch boundary, rather than opening a
separate task.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | The one-word register-set command format (bits 15-13 fixed `100`; RS4-RS0, bits 12-8, the register number; D7-D0, bits 7-0, the data byte); the documented valid register-number range is `#0` through `#23` (24 write-only registers), separate from the one read-only status register SEG-007-T081 already implements; `$C00004` and `$C00006` are documented functionally equivalent for this command, and a long-word write is documented equivalent to two sequential word writes (D31-D16 first); the two-word address-set command format (1st word: CD1,CD0 then A13-A0; 2nd word: eight fixed-zero high bits, then CD5-CD2, two fixed-zero reserved bits, then A15,A14); the six documented non-DMA CD5-CD0 codes for VRAM/CRAM/VSRAM READ/WRITE; the DMA-transfer variant of that same two-word shape, distinguished by CD5 (the 2nd word's bit 7) being set to `1` instead of `0`; VDP register `#15` (`INC7-INC0`) is the documented VRAM/CRAM/VSRAM address auto-increment value. | `GTO1` (already cited by SEG-007-T034/T077/T078/T079/T080/T081), independently re-verified this task directly against the primary PDF's own page images (not merely the lower-fidelity OCR text derivative other sections here also cite): p. 20 ("WRITE1: REGISTER SET" and "WRITE2: ADDRESS SET" diagrams, including the "$C00004 and $C00006 are functionally equivalent" and long-word-equivalence footnotes), p. 22 SS4 "VDP REGISTER" ("VDP has write only register #0 through #23 and read only status register total 25 register"), p. 27 (the WRITE2 CD5-CD0 access-mode table, identical to p. 20's), p. 28-33 (VRAM/CRAM/VSRAM access examples, each reusing the same two-word shape and the same six CD5-CD0 codes), p. 36-38 SS7 "DMA TRANSFER" ("Setting of DMA" step (E), showing the identical two-word shape with CD5=1; REG. #15/#19-23 diagrams). This does not establish any timing, FIFO, or bus-arbitration behavior of these writes, nor any behavior beyond the CPU-visible register-file/pointer state this policy routes. |
| **Project inference** | `auto_increment_value` (SEG-007-T042 SS1.3's own reserved field) is populated as a named post-access side effect of a register-set command-word write to register `#15` specifically, mirroring `registers[15]`'s own value, because GTO1's own two-word address-set diagrams carry no increment-value bits of their own -- register `#15` is the only documented source for this value. | Connects T042 SS1.3's reserved field to the one register GTO1 documents as its source (p. 28: "VRAM address is increased by the value of REGISTER #15"; p. 37's REG.#15 diagram), by direct structural application; it is not itself a new hardware fact, and it does not claim GTO1 states this connection in those exact words. |
| **Still unresolved** | Any FIFO, bus-arbitration, or timing effect of these writes; any documented power-on/reset default for `registers[]`/`control_port_awaiting_second_word`/`control_port_first_word`/`addressed_pointer`/`auto_increment_value` (T042 SS8's zero-initialization default applies instead, per that contract); the CPU-visible effect of any command-word bit pattern this policy does not explicitly recognize (every such pattern remains fail-closed, per this policy's own Selected deterministic policy below, rather than guessed). | `GTO1` documents the command-word *shapes* and the *effect* of the recognized fields; it does not document hardware behavior for a malformed, reserved-bit-violating, or otherwise-undocumented command word, nor a power-on/reset default for any of these registers. |
| **Project compatibility policy** | The exact selectors implemented (the register-set command for RS `#0`-`#23`; the non-DMA two-word address-set command for exactly the six documented CD5-CD0 codes; a LONG write decomposed into two sequential WORD writes through those same two selectors, per (c) below); the exact selectors deliberately narrowed out and left fail-closed (the DMA-triggering two-word variant; the `$C00006` mirror lane; any reserved-bit-violating or undocumented-CD-code word); the LONG-write partial-completion rule (c) below, an explicit, narrow, deliberately documented exception permitting the high (D31-D16) sub-write's own already-successful mutation to remain committed even when the low (D15-D0) sub-write fails and the overall access reports failure; and the typed success/failure result boundary below. | Deliberate deterministic project choice within the already-decided SEG-007-T042 architecture and this task's own bounded Scope, not historical/device truth. The partial-completion rule specifically is justified directly by the same `GTO1` long-word-equivalence citation above (two sequential bus transactions, not one atomic operation), not by project convenience alone. |

## Selected deterministic policy

This policy decides three distinct command-word shapes separately; a write that matches none of them
remains fail-closed.

**(a) One-word register-set command.** Direction is write; width is WORD only (BYTE remains fail-closed at
`$C00004` at every width; LONG is documented separately in (c) below); the address has passed
existing earlier effective-address validation, including alignment; the address resolves exactly to the
public VDP CONTROL port base address (`$C00004`, never the documented `$C00006` mirror lane, which
remains unimplemented for both directions). The word's bits 15-13 must equal `100` exactly; RS4-RS0
(bits 12-8) name the destination register, and D7-D0 (bits 7-0) the data byte written into it:

```text
registers[RS] = D7..D0   (for RS in 0..23; RS >= GENESIS_VDP_REGISTER_COUNT remains fail-closed)
if RS == 15: auto_increment_value = D7..D0   (named post-access side effect; see provenance table)
```

`GENESIS_VDP_REGISTER_COUNT` is `24`, citing `GTO1` p. 22 SS4 ("VDP has write only register #0 through
#23... total 25 register") directly -- a publicly documented hardware fact, not a project policy choice.

**(b) Non-DMA two-word VRAM/CRAM/VSRAM address-set command.** Direction is write; width is WORD only, for
both words of the sequence; both words address exactly `$C00004`. A WORD write to `$C00004` whose bits
15-13 do **not** equal `100` (i.e., not matched by (a) above), while no second word is currently pending,
is accepted as the sequence's first word: it is latched into `control_port_first_word` and
`control_port_awaiting_second_word` is set. The *next* WORD write to `$C00004`, regardless of its own bit
pattern, is always treated as that pending sequence's second word (the VDP's own documented two-word
command state machine draws no separate "abort and restart as first word" case). The second word is
accepted, completing the command, only if **all** of the following hold:

- Its bits 15-8 (the documented fixed-zero high byte) and bits 3-2 (the documented fixed-zero reserved
  pair) are all zero.
- Its bit 7 (`CD5`) is `0` -- **the DMA-transfer request bit**. `GTO1` p. 37's "Setting of DMA" step (E)
  documents the identical two-word shape with this exact bit set to `1` instead. A second word with this
  bit set to `1` is the DMA-triggering variant this policy explicitly narrows out, per this task's own
  bounded Scope (SEG-007-T084's own separate scope); it remains fail-closed.
- The composed 6-bit code `CD5 CD4 CD3 CD2 CD1 CD0` (`CD1`/`CD0` from the first word's bits 15-14; `CD5`-
  `CD2` from the second word's bits 7-4) exactly equals one of the six documented non-DMA codes: `000000`
  (VRAM READ), `000001` (VRAM WRITE), `000011` (CRAM WRITE), `000100` (VSRAM READ), `000101` (VSRAM
  WRITE), or `001000` (CRAM READ). Any other 6-bit value (documented-reserved or otherwise undocumented)
  remains fail-closed.

On acceptance, the composed 16-bit address (`A15`,`A14` from the second word's bits 1-0; `A13`-`A0` from
the first word's bits 13-0) is stored in `addressed_pointer`, and both `control_port_awaiting_second_word`
and `control_port_first_word` are reset to `0`. On rejection, per this policy's own fail-closed boundary
below, **no** `GenesisVdpState` field is mutated -- the pending first-word state, if any, is left exactly
as it was, so a subsequently correctly-shaped second word for that same pending first word still completes
normally afterward.

This policy makes no claim about which VRAM/CRAM/VSRAM memory pool a given completed `addressed_pointer`
value logically targets: `GenesisVdpState` reserves no separate "target memory" field (T042 SS1.3 names
none), and no currently-implemented selector reads or interprets `addressed_pointer` for any purpose --
SEG-007-T083's own separate future scope owns that decision, if it needs one.

**(c) LONG-word write (SEG-007-T091, second frontier pass).** Direction is write; width is LONG; the
address resolves exactly to `$C00004` (never the `$C00006` mirror lane, which remains unimplemented for
every width and direction). `GTO1` p. 20's own long-word-equivalence footnote -- already cited in this
section's first-pass provenance table above, independently re-verified against the primary PDF's own page
images -- states a long-word access is equivalent to two sequential word accesses, with D31-D16 written
first. This is decomposed directly: the 32-bit value is split into `high = D31..D16` and `low = D15..D0`,
and each half is submitted, in that order, through the exact same command-word recognizer (a)/(b) above
already define for a standalone WORD write -- refactored into one shared internal helper
(`genesis_vdp_control_port_write_word` in `tools/genesis_startup_bridge_runtime.c`) so the LONG case cannot
drift from the WORD case's own already-validated logic:

```text
ok_high = write_word(high)
if not ok_high: fail, mutating nothing (see below)
ok_low  = write_word(low)
if not ok_low: fail (see partial-completion rule below)
else: succeed
```

A single call to that shared helper is itself always atomic: for any one word, `genesis_vdp_access`'s
existing (a)/(b) logic performs every validation check for that word before performing any mutation for
that same word (unchanged by this refactor), so a rejected word -- by itself -- still mutates nothing, per
this section's own general fail-closed boundary below. This lets a LONG write complete two chained
register-set commands (a), or a first-word/second-word address-set pair (b), in a single access.

**Partial-completion rule (the one deliberate, explicit, narrow exception in this entire policy).** If
`high` is itself invalid (for example, a register-set command naming `RS >= 24`), `genesis_vdp_access`
never even attempts `low`: the whole LONG write fails exactly like every other rejected access in this
file, mutating **no** `GenesisVdpState` field at all -- this is the ordinary, non-exceptional case, and it
fully honors `genesis_route_access`'s own general "on failure neither it nor the runtime is modified"
contract (stated in `tools/genesis_startup_bridge_runtime.h`'s header doc comment above
`genesis_route_access`). If `high` succeeds but `low` then fails, however, the overall LONG write still
reports `GENESIS_ACCESS_FAIL` -- but `high`'s own already-successful, already-committed mutation is **not**
rolled back. This is a deliberate, explicit, narrow exception to that same general contract, and it is
justified only by this section's own already-cited `GTO1` long-word-equivalence footnote: `GTO1` documents
the long-word write as literally **two independent, sequential bus transactions**, not one atomic
operation. Real 68000/VDP hardware performing this identical two-transaction bus sequence would, by that
same documented model, identically leave the first transaction's effect committed if the second
transaction were separately rejected; treating this pair as a single roll-back-able unit would depart from
the two-sequential-transaction hardware model `GTO1` itself documents, not merely add implementation
complexity. This exception applies **only** to this one LONG-write-into-two-WORD-writes composition; every
other selector in this section (including a standalone rejected WORD write, and (b)'s own "no mutation on
rejection" rule for a single rejected second word) still fully honors the general contract unchanged.

**Deliberately, explicitly narrowed out (left fail-closed) this pass:**

- The DMA-triggering two-word variant (`CD5 = 1`), per (b) above -- SEG-007-T084's own separate scope.
- The `$C00006` CONTROL-port mirror lane, for either direction (SEG-007-T081's own prior narrowing,
  unchanged by this task).
- Any VRAM/CRAM/VSRAM DATA-port (`$C00000`) byte-level read/write access itself (SEG-007-T083's own
  separate scope); this policy only populates the `addressed_pointer`/`auto_increment_value` state a
  future data-port access would consume.
- Every selector SEG-007-T081's own section above already narrows out (BYTE/LONG reads at `$C00004`, the
  HV COUNTER, the PSG 76489 port), entirely unaffected by this addition.

## Typed result boundary

Built exclusively through SEG-007-T042's own already-decided runtime-bridge dispatch boundary
(`tools/genesis_startup_bridge_runtime.h`'s `genesis_route_access`), exactly like the SEG-007-T081 section
above. It adds no static-resolver typed result of any kind.

On success ((a), (b), or (c) above -- for (c), both the `high` and `low` sub-writes succeeded),
`genesis_route_access` returns `GENESIS_ACCESS_OK`. The caller's own `*value` (the word or long value being
written) is left unchanged -- a write never produces an output value. The resulting `GenesisVdpState`
fields are exactly as (a)/(b)/(c) above specify; no field this policy does not name is ever mutated by a
write.

On failure, `genesis_route_access` returns `GENESIS_ACCESS_FAIL` and populates `*stop_out` with:

| Field | Required value/property |
| --- | --- |
| `stop_class` | `GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS`. |
| `diagnostic_category` | `GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP`. |

For every failure case except (c)'s own documented partial-completion exception, no `GenesisVdpState`
field is mutated at all (per (b)'s own "no mutation on rejection" rule, (a)'s own out-of-range rejection,
and (c)'s own "`high` invalid" case, which never even attempts `low`). The sole exception is (c)'s
partial-completion case (`high` succeeds, `low` fails): `high`'s own already-committed mutation remains in
`GenesisVdpState` even though `genesis_route_access` reports `GENESIS_ACCESS_FAIL` for the access as a
whole -- see (c) above for the full justification. This is the only selector in this entire policy (across
both the SEG-007-T081 and SEG-007-T091 sections) where a `GENESIS_ACCESS_FAIL` result does not imply the
runtime is byte-for-byte unchanged.

## Fail-closed boundary and ownership seam

Existing pre-region validation and `genesis_route_access`'s own existing work-RAM/ROM/controller-I/O/VDP
precedence remain entirely unchanged by this section, exactly as the SEG-007-T081 section above already
states.

This policy adds **exactly three new selectors** (the register-set command, the non-DMA two-word
address-set command, and the LONG-write decomposition of either or both through those same two selectors);
it narrows nothing else. Every other VDP-area write shape remains exactly as fail-closed as before, with
`GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS`/`GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_VDP`:

- Every selector this section's own "Deliberately, explicitly narrowed out" list above names.
- A register-set command naming RS `#24`-`#31` (representable by the 5-bit RS4-RS0 field, but outside the
  documented `#0`-`#23` valid range) -- for a LONG write, this is checked on `high` before `low` is ever
  attempted, per (c) above.
- A second word whose composed CD5-CD0 code is not one of the six documented non-DMA codes, or whose
  documented-fixed-zero bits are not all zero.
- A BYTE write of any kind to `$C00004` (LONG is now implemented, per (c) above; BYTE remains
  unimplemented at every width for writes, matching SEG-007-T081's own BYTE-read narrowing).
- Every SEG-007-T020/T038/T079/T081 selector above, outside its own already-implemented shape -- those
  selectors' own success cases, fail-closed boundaries, and typed result boundaries are entirely unchanged
  by this section.

An address outside the recognized VDP-area window entirely remains exactly as fail-closed as before this
section, with the pre-existing `GENESIS_STOP_UNSUPPORTED_MEMORY_REGION`/`GENESIS_DIAG_UNMAPPED_DATA_ACCESS`
result, unchanged by this section.

The ownership seam is identical to the SEG-007-T081 section above:

```text
generated Genesis runtime memory/device access
  -> genesis_route_access
  -> genesis_is_vdp_region / genesis_vdp_access
  -> typed OK result or typed fail-closed result (GenesisRuntimeStop)
```

No instruction-specific emitter owns VDP semantics.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine this policy's command-word recognition
rules, side-effect rule, partial-completion rule, or failure boundary: it must directly cover this exact
functional selector and the exact affected CPU-visible behavior, record its provenance and limits, and
receive independent validation. Address validity and alignment remain owned by the earlier
effective-address validation stage. Replacement must also retain neighboring VDP-area-stage mismatches as
fail-closed, and it must not silently narrow or reopen the unrelated SEG-007-T020/T038/T079/T081 selectors
above, nor silently implement any of this section's own explicitly narrowed-out variants (DMA, the
`$C00006` mirror, data-port access) without its own separate, independently justified task. Replacement
must not silently widen (c)'s own narrow partial-completion exception beyond the exact
LONG-write-into-two-WORD-writes composition it is justified for, nor silently apply it to any other
selector in this document. A directly supporting hardware source (for example a newly located public
source documenting a reset/power-on default for any of these fields, this project's own future
SEG-007-T083/T084 work, or evidence that the two LONG sub-writes are not in fact independent bus
transactions on real hardware, which would require revisiting (c)'s own partial-completion rule) may
establish a hardware fact or extend this project's own architecture; a qualified independent observation
remains distinct from that fact; a derivation remains project inference.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or derived
fixture; every address and bit-field cited above is already public (`GTO1`). It does not model the DATA
port, HV COUNTER, PSG 76489, DMA transfer semantics, interrupts, rendering, audio, Z80 behavior, general
controller reads, interactive input, timing, FIFO/bus-arbitration behavior, or a complete Genesis bus. It
makes no rendering, interactive-input, or title-screen-completion claim. It adds no VRAM/CRAM/VSRAM
data-port, DMA, or interrupt code change of any kind; those remain SEG-007-T083/T084's own separate scope.
It is a decision record only, exactly like the SEG-007-T020/T038/T079/T081 sections above.

---

# Genesis controller-I/O GPIO-register BYTE-WRITE compatibility policy (SEG-007-T121)

## Status and boundary

This is an independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict any SEG-007-T020 / SEG-007-T038 / SEG-007-T079 /
SEG-007-T081 / SEG-007-T091 / SEG-007-T111 section above, all of which remain unchanged and continue
to govern their own read/VDP selectors exactly as before. It is the **write-direction mirror** of the
GPIO-register read selectors: every prior section here selects read-direction accesses only.

This section consumes only: SEG-007-T120's validated normalized handoff (a runtime-selected,
deterministic, provenance-resolved **BYTE WRITE** to a controller-I/O GPIO register in the
`$A10009`-class window, via a direct absolute-long `(xxx).L` `MOVE.B` destination, mapped from
`raw_cartridge_rom`); the SEG-007-T115 generalized routed-device seam
(`docs/architecture/post-seg007-architecture-refactor-contract.md` / the "Emission-stage static
absolute-operand routing" capability) as the direct structural template; and public Genesis
GPIO-port register-map documentation. It applies only to the one functionally identified BYTE-write
family defined below; it names no private target address beyond the already-public GPIO register
addresses, and it does not reopen or extend T120's own verdict.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | The three general-purpose I/O ports (CTRL1, CTRL2, EXP) each expose a DATA register and a CTRL register; the six register addresses are DATA1 `$A10003`, DATA2 `$A10005`, DATA3 `$A10007`, CTRL1 `$A10009`, CTRL2 `$A1000B`, CTRL3 `$A1000D`; each is R/W and supports BYTE and WORD access; on a WORD access only the lower byte is meaningful register data; the CTRL register's low 7 bits (`PC6`-`PC0`) select per-pin direction (1 = output) and bit 7 (`INT`) enables the TH-transition interrupt; the DATA register holds the value driven on output-configured pins. | `GTO1` pp. 72, 74, 75 (already cited by SEG-007-T017/T034 and restated by the read sections above), corroborated by an independent established community Genesis hardware reference (Charles MacDonald's *Sega Genesis I/O port* notes: the `$A1000x` data/control register pairs, the direction-bit and TH-INT-bit layout of the control register). This does **not** establish any TH/TR/TL handshake timing, serial-shift behaviour, interrupt delivery, or a CPU-visible read-back side effect of a CTRL/DATA write. |
| **Project inference** | The reached BYTE write is a plain latched-register store: on startup the program is configuring a GPIO port's direction / driving a data value, not driving a serial protocol (the serial S-CTRL / TxDATA / RxDATA registers, which carry the distinct serial-mode semantics, are a **different owner** and stay fail-closed — matching the SEG-007-T111 section's own stated boundary). | The reached access shape (BYTE write, GPIO data/direction register, no dependent read of the written value in the completed prefix) combined with `GTO1`'s documented register purpose. It is not a public claim about a private address. |
| **Still unresolved** | Any concrete power-on/reset default for the DATA/CTRL latches beyond T042 SS8's zero-initialization; whether a CTRL write that enables `INT` has any startup-observable effect (interrupts are not modelled); the exact CPU-visible read-back of a written DATA/CTRL register (no read selector consumes the latch). | `GTO1` documents the register *purpose* and *write* field layout, not a guaranteed reset value or a read-back-after-write contract; this project models no interrupt or serial path. |
| **Project compatibility policy** | The BYTE-only width restriction; the deterministic latched-store semantics (full 8 bits stored; direction bits govern physical pin drive, which this project does not model); side-effect-freedom with respect to every read selector; and the typed success/failure boundary below. | Deliberate deterministic project choice within the already-decided SEG-007-T042 / SEG-007-T115 architecture, not historical/device truth. |

## Selected deterministic policy

The accepted selector family has **all** of these dimensions: direction is **write**; width is **BYTE**
(WORD and LONG remain fail-closed for writes — the reached form is BYTE, and no WORD/LONG GPIO write is
runtime-selected); the address has passed existing earlier effective-address validation (a BYTE access
carries no alignment constraint); the shared Genesis bus/address resolver has recognized the request as
the controller-I/O region; and the address resolves exactly to one of the six public GPIO register
addresses above (`segarecomp_genesis_controller_io_gpio_register_index`, the shared C/C++ recognizer in
`libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h`). The runtime route reaches a **CTRL3
`$A1000D` BYTE write**. `$A1000D` is simultaneously the SEG-007-T111 CTRL3 BYTE read selector (and the
`$A1000C` even lane the SEG-007-T038 CTRL3 WORD read selector): a BYTE **write** there is a latched
store, while a **read** keeps returning its unchanged read-selector policy constant — the latch is
never read back (the VDP `registers[]` write-only precedent). The controller-I/O policy parity contract
(`tests/controller_io_policy_parity_test.cpp`) is updated to allow exactly this one read-selector
address to additionally accept a BYTE write while asserting its read result is byte-for-byte unchanged.

For that family only, the write is a deterministic latched store into the modeled controller-I/O
register state (`GenesisControllerIoState`, `platforms/genesis/runtime/runtime.h`: `data[3]` for DATA1..DATA3,
`ctrl[3]` for CTRL1..CTRL3), both zero-initialized per SEG-007-T042 SS8 (at power-on all pins read as
input and no value is driven):

```text
DATA1..DATA3 write ($A10003/5/7, BYTE): data[n]  = value & 0xFF
CTRL1..CTRL3 write ($A10009/B/D, BYTE): ctrl[n]  = value & 0xFF
```

The full 8 bits are stored; the CTRL direction bits govern which pins are physically driven on real
hardware, which this project does not model. Bit 7 of a CTRL write (`INT`, TH-interrupt enable) is
stored but has no effect — this project has no interrupt path. The store is **side-effect-free with
respect to every read selector**: no currently-implemented read selector (SEG-007-T020 CTRL1/CTRL2 LONG
read, SEG-007-T038 CTRL3 WORD read, SEG-007-T079 Version BYTE read, SEG-007-T111 CTRL3 BYTE read) reads
`GenesisControllerIoState`; those selectors keep returning their own already-decided policy constants
unchanged. This mirrors the VDP `registers[]` write-only precedent (SEG-007-T091): a modeled latch that
is written but not yet read back.

All validation precedes the single store, so a rejected access mutates nothing (SEG-007-T042 SS3 /
`genesis_route_access`'s "on failure neither it nor the runtime is modified" contract).

## Typed result boundary

Built exclusively through the SEG-007-T115 generalized routed-device seam, exactly like the PSG / Z80
store selectors: the translation-time resolver
(`m68k_route_genesis_device_access`, `platforms/genesis/machine/src/address_space.cpp`) recognizes the
`(address, BYTE, write)` shape and returns the value-free `M68kDeviceRoutedAccess{write}` marker (the
retained static-memory fact carries only the routing decision, region `routed_device`); the runtime
device gate (`genesis_route_access` → `genesis_controller_io_access`,
`platforms/genesis/runtime/runtime.c`) performs the latched store and returns `GENESIS_ACCESS_OK`. It adds no new
IR kind, no new dispatcher, no new diagnostic/frontier class, and no new persistent-state subsystem
beyond the one `GenesisControllerIoState` latch struct.

On failure, `genesis_route_access` returns `GENESIS_ACCESS_FAIL` with `stop_class =
GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` and `diagnostic_category =
GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO` (the retained category), and the translation-time
resolver returns the unchanged `unsupported_device_region_controller_io` / `unmapped_data_access`
result for the shape.

## Fail-closed boundary and ownership seam

Existing pre-region validation remains authoritative in the same order the sections above establish
(`effective_address_not_24bit`, then `odd_effective_address`, then existing
ROM/RAM/device/unmapped resolution). This policy applies only after those steps and does not alter,
narrow, or reorder any of them.

This policy adds **exactly one new selector family** (BYTE write to the six GPIO registers); it narrows
nothing else — every read selector's own read result is byte-for-byte unchanged, including CTRL3
`$A1000D` whose read still returns its policy constant. Every other controller-I/O access shape remains
exactly as fail-closed as before, with `unsupported_device_region_controller_io`:

- A WORD or LONG write to any GPIO register (only BYTE is runtime-selected).
- A write to any serial register: S-CTRL (`$A10013`/`$A10019`/`$A1001F`), TxDATA (`$A1000F`/`$A10015`/`$A1001B`), RxDATA (`$A10011`/`$A10017`/`$A1001D`) — a different owner with distinct serial semantics.
- A write to the Version register (`$A10001`).
- A write to a GPIO register's even (non-register) byte lane, or any partial/crossing shape.
- A **read** of any GPIO write-only register address, and every existing read selector outside its own already-implemented shape — those selectors' own success cases, fail-closed boundaries, and typed result boundaries are entirely unchanged by this section.

The ownership seam is the SEG-007-T115 routed-device seam, unchanged:

```text
statically lifted CPU memory operation
  -> m68k_route_genesis_device_access  (recognize (addr, BYTE, write) GPIO shape)
  -> genesis_route_access -> genesis_controller_io_access  (deterministic latched store)
  -> typed OK result or typed fail-closed result
```

No instruction-specific emitter owns controller semantics.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine this policy's width restriction,
latched-store semantics, side-effect rule, or failure boundary: it must directly cover this exact
functional selector family and the exact affected CPU-visible behaviour, record its provenance and
limits, and receive independent validation. A directly supporting hardware source (for example a
documented power-on default for the DATA/CTRL latches, or a documented CPU-visible read-back-after-write
contract, or this project's own future interrupt/serial work) may establish a hardware fact or extend
this project's architecture; a qualified independent observation remains distinct from that fact; a
derivation remains project inference. Replacement must retain neighbouring controller-I/O-stage
mismatches as fail-closed and must not silently reopen or narrow any read selector above, nor silently
implement a serial-register or WORD/LONG GPIO write without its own separately justified task.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or derived
fixture beyond the already-public `$A1000x` GPIO register addresses. It does not model general
controller input, TH/TR/TL handshake, serial-shift registers, the TH-transition interrupt, gamepad /
keyboard / lightgun peripherals, timing, reset sequencing, other I/O ports, VDP, rendering, audio, Z80
behaviour, or a complete Genesis bus. It makes no rendering, interactive-input, or
title-screen-completion claim. It adds one controller-I/O write-selector family plus its one modeled
latch struct; it is otherwise a decision record, exactly like the SEG-007-T020/T038/T079/T111 sections
above.

---

# Genesis controller-I/O DATA1/DATA2 three-button-pad BYTE-read compatibility policy (SEG-007-T166)

## Status and boundary

This is an independent, bounded **project compatibility policy**, not an authoritative
Genesis-hardware claim or implementation authorization. It is a clearly delineated addition to this
document; it does not rewrite, narrow, or contradict any SEG-007-T020 / SEG-007-T038 / SEG-007-T079 /
SEG-007-T081 / SEG-007-T091 / SEG-007-T111 / SEG-007-T121 section above, all of which remain unchanged
and continue to govern their own read/write selectors exactly as before. It is the **read-direction
mirror** of the SEG-007-T121 GPIO-register write family, for exactly the two general-purpose-input-port
DATA registers that carry the documented three-button joypad protocol -- DATA1 (`$A10003`) and DATA2
(`$A10005`).

This section consumes only: SEG-007-T165's validated normalized handoff (a runtime-selected,
deterministic BYTE READ of DATA1, `$A10003`, reached by the same authorized Sonic Phase-B route that
then immediately repeats the identical routine for DATA2, `$A10005`); the SEG-007-T121 section above as
the direct structural template (CTRL/DATA GPIO-latch mechanism, `GenesisControllerIoState`); and public
Genesis GPIO-port / three-button-controller protocol documentation. It applies only to the one
functionally identified BYTE-read family defined below; it names no private target address beyond the
already-public DATA1/DATA2 addresses, and it does not reopen or extend T165's own verdict.

| Provenance category | Retained claim | Source and limit |
| --- | --- | --- |
| **Publicly documented hardware fact** | DATA1 (`$A10003`) and DATA2 (`$A10005`) are R/W, BYTE/WORD accessible (restated from the SEG-007-T121 section above, unchanged); the paired CTRL register's low 7 bits (`PC6`-`PC0`) select per-pin direction (1 = output) for the corresponding DATA bits 6-0. | `GTO1` pp. 72, 74, 75 (already cited by SEG-007-T017/T034/T121). This does not itself document the joypad-specific button-to-bit mapping below -- `GTO1` documents the generic GPIO register mechanism, not what a specific peripheral (a three-button pad) drives on those pins. |
| **Publicly documented hardware fact** | The three-button joypad protocol: DATA bit 6 is TH (the select line the host drives to multiplex button groups); with TH driven high (`1`), a DATA read returns `?1CBRLDU` on bits 7-0 (bit 7 unlabelled, bit 6 = TH echo, bit 5 = C, bit 4 = B, bit 3 = Right, bit 2 = Left, bit 1 = Down, bit 0 = Up); with TH driven low (`0`), a DATA read returns `?0SA00DU` (bit 5 = Start, bit 4 = A, bits 3-2 forced `0`, bit 1 = Down, bit 0 = Up); a `0` bit means a button is pressed and a `1` bit means released; DATA bit 7 has no corresponding CTRL direction bit and is documented to simply latch whatever value was last written to it, independent of CTRL. | Charles MacDonald, *Sega Genesis hardware notes* ("MCD1", already cited by the SEG-007-T121 section above), `gen-hw.txt` SS3.2 "Gamepad specifics" (mirror: `https://gendev.spritesmind.net/mirrors/cmd/gen-hw.txt`, accessed 2026-09-04): "A gamepad maps the directional pad to the pins mentioned earlier (left, right, up, down), and multiplexes the four buttons (A, B, C, Start) through the TL and TR pins," the `TH = 0 : ?0SA00DU` / `TH = 1 : ?1CBRLDU` table, "A '0' means a button has been pressed, and '1' means a button has been released," and "Bit 7 isn't connected to any pin on the I/O port. It will latch a value written to it." This is independently corroborated by the same class of widely-used community Genesis hardware documentation this project already treats as an acceptable corroborating source at SEG-007-T121 (the `$A1000x` GPIO register family / TH-bit mechanism); no Sega primary source (`GTO1`) located by this or prior tasks documents the joypad-specific button mapping itself -- `GTO1` covers only the generic GPIO register mechanism (the fact above). This does not establish TH/TR/TL handshake timing, the six-button-pad extended sequence (out of this section's scope -- see Non-goals), or any interrupt-driven read behaviour. |
| **Project inference** | The reached BYTE reads at DATA1 then DATA2 are the program's own three-button-pad polling routine (a plain GPIO combinational read of the currently-configured pin directions and driven/input bit values), not a six-button-pad ID sequence or serial-mode access -- matching the SEG-007-T121 section's own "plain latched-register" inference for the paired write family. | The reached access shape (BYTE read of a documented three-button-protocol DATA register, immediately following the SEG-007-T121 CTRL/DATA GPIO writes the same route already performs) combined with `GTO1`/`MCD1`'s documented register purpose. It is not a public claim about a private address. |
| **Still unresolved** | Any concrete power-on/reset default for the DATA/CTRL latches beyond T042 SS8's zero-initialization (restated from T121, unchanged); the documented behaviour of DATA bit 7 when CTRL provides no corresponding direction bit at all (MCD1 states it simply latches the last-written value, which this policy generalizes to the same CTRL-direction+DATA-latch combination rule as every other bit, below); TH/TR/TL handshake timing; the six-button-pad ID/multiplexed-nibble sequence (out of scope). | `GTO1`/`MCD1` document the register *purpose* and the joypad's *combinational* read shape, not a guaranteed reset value, a documented handshake timing model, or (for `MCD1`) an explicit bit-7-under-CTRL-direction rule -- this policy's bit-7 treatment is therefore a deliberate, narrow generalization, not a directly cited fact. |
| **Project compatibility policy** | The deterministic "all buttons released" fixed input constant for every input-configured DATA1/DATA2 pin (port 2 always; port 1 when the host supplies no pad state. SEG-011-T004: player 1 reads a host-supplied 3-button pad mask injected via `genesis_runtime_set_pad1`; the mask is host input, excluded from device checkpoints and divergence digests, and zero by default); the generalized per-bit CTRL-direction combination rule, including its extension to bit 7; side-effect-freedom; and the typed success/failure boundary below. | Deliberate deterministic project choice within the already-decided SEG-007-T042 / SEG-007-T121 architecture and this task's own bounded Scope, not historical/device truth. |

## Selected deterministic policy

The accepted selector family has **all** of these dimensions: direction is **read**; width is **BYTE**
(WORD and LONG remain fail-closed for DATA1/DATA2 reads -- the reached form is BYTE, and this project's
existing fixed-selector table already owns no WORD/LONG DATA1/DATA2 read); the address has passed
existing earlier effective-address validation (a BYTE access carries no alignment constraint); the
shared Genesis bus/address resolver has recognized the request as the controller-I/O region; and the
address resolves exactly to DATA1 (`$A10003`) or DATA2 (`$A10005`) -- reusing the same shared C/C++
recognizer the SEG-007-T121 write family already uses
(`segarecomp_genesis_controller_io_gpio_register_index`,
`libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h`), filtered to slot 0 or 1 with
direction `read`. DATA3 (`$A10007`) read stays deliberately out of scope (see Non-goals): its own
protocol identity (EXP-port peripheral, not a documented three-button pad) is not free to add within
this same generic mechanism without separate research.

Unlike the fixed-constant selectors this document's earlier sections define (CTRL1/CTRL2, CTRL3,
Version), a DATA1/DATA2 BYTE read is **not** a bare constant: it is a deterministic function of the
already-modeled `GenesisControllerIoState` latches (`ctrl[n]`, `data[n]`, `n` = 0 for DATA1/CTRL1, 1 for
DATA2/CTRL2 -- the same latches the SEG-007-T121 write family already stores into, zero-initialized per
T042 SS8) plus one new fixed constant this task adds: the deterministic "all buttons released" input
value. For each DATA bit `i` in `0..6`:

```text
result[i] = ctrl[n] has direction bit i set (output)
              ? data[n]'s own driven bit i         -- host-driven output pin
              : released_input_value(th)'s bit i    -- controller-driven input pin
```

where `th = ctrl[n] has direction bit 6 set ? data[n]'s own driven bit 6 : 1` (bit 6, TH, uses the same
output/input rule as every other bit; if TH is itself configured as an input -- an undocumented,
software-atypical configuration -- this policy's deterministic default assumes the idle/released `1`
state, labelled **Project compatibility policy**, not a hardware claim), and:

```text
released_input_value(th = 1) bits 5..0 = 0b111111  (0x3F: C, B, Right, Left, Down, Up all released)
released_input_value(th = 0) bits 5..0 = 0b110011  (0x33: Start, A, Down, Up released; bits 3-2 forced 0)
```

Bit 7 has no corresponding CTRL direction bit at all (`GTO1`'s CTRL register is only 7 direction bits,
`PC6`-`PC0`; `MCD1` documents DATA bit 7 as unconnected to any pin, simply latching whatever was last
written). This policy generalizes the same CTRL-direction combination rule to bit 7 by treating it as
always host-driven regardless of CTRL (`result[7] = data[n]`'s own bit 7), which reduces to exactly
MCD1's documented "latches whatever was last written" behaviour: **Project compatibility policy**, a
narrow generalization of the per-bit rule above rather than a directly cited fact for this specific bit.

This selector is read-only with respect to the write family above: it never mutates
`GenesisControllerIoState`, matching the SEG-007-T121 write family's own side-effect-freedom promise in
the opposite direction. All validation precedes the single computed read, so a rejected access mutates
nothing (SEG-007-T042 SS3's "on failure neither it nor the runtime is modified" contract, restated
unchanged).

## Typed result boundary

Built exclusively through the SEG-007-T115 generalized routed-device seam, exactly like the SEG-007-T121
write family and the VDP status-register read precedent: the translation-time resolver
(`m68k_route_genesis_device_access`, `platforms/genesis/machine/src/address_space.cpp`) recognizes the (DATA1 |
DATA2, BYTE, read) shape and returns the value-free `M68kDeviceRoutedAccess{read}` marker (the retained
static-memory fact carries only the routing decision, region `routed_device`) -- it never folds this
shape through the fixed-constant `m68k_controller_io_access` table, because the returned value is not
one static constant; the runtime device gate (`genesis_route_access` -> `genesis_controller_io_access`,
`platforms/genesis/runtime/runtime.c`) performs the deterministic combinational read described above and returns
`GENESIS_ACCESS_OK`. It adds no new IR kind, no new dispatcher, no new diagnostic/frontier class, and no
new persistent-state subsystem beyond the one already-modeled `GenesisControllerIoState` latch struct
(no new struct field: the "all released" input constants are fixed values inside the read function, not
captured/mutable state).

On failure, `genesis_route_access` returns `GENESIS_ACCESS_FAIL` with `stop_class =
GENESIS_STOP_UNSUPPORTED_DEVICE_ACCESS` and `diagnostic_category =
GENESIS_DIAG_UNSUPPORTED_DEVICE_REGION_CONTROLLER_IO` (the retained category), and the translation-time
resolver returns the unchanged `unsupported_device_region_controller_io` / `unmapped_data_access` result
for the shape.

## Fail-closed boundary and ownership seam

Existing pre-region validation remains authoritative in the same order the sections above establish
(`effective_address_not_24bit`, then `odd_effective_address`, then existing ROM/RAM/device/unmapped
resolution). This policy applies only after those steps and does not alter, narrow, or reorder any of
them.

This policy adds **exactly one new selector family** (BYTE read of DATA1/DATA2); it narrows nothing
else -- every other read/write selector's own behaviour is byte-for-byte unchanged, including the
SEG-007-T121 DATA1/DATA2 BYTE-write family, which continues to store into the same latches this policy
now also reads. Every other controller-I/O access shape remains exactly as fail-closed as before, with
`unsupported_device_region_controller_io`:

- A WORD or LONG read of DATA1 or DATA2 (only BYTE is runtime-selected).
- A BYTE, WORD, or LONG read of DATA3 (`$A10007`) -- deliberately out of scope, see Non-goals.
- A read of any GPIO write-only-recognized register outside DATA1/DATA2 (CTRL1-3), and every other
  existing read selector outside its own already-implemented shape -- those selectors' own success
  cases, fail-closed boundaries, and typed result boundaries are entirely unchanged by this section.
- Every write shape -- the SEG-007-T121 write family's own success case, fail-closed boundary, and
  typed result boundary are entirely unchanged by this section.
- Every other frontier profile.

The ownership seam is the SEG-007-T115 routed-device seam, unchanged:

```text
statically lifted CPU memory operation
  -> m68k_route_genesis_device_access  (recognize (DATA1|DATA2, BYTE, read) shape)
  -> genesis_route_access -> genesis_controller_io_access  (deterministic combinational read)
  -> typed OK result or typed fail-closed result
```

No instruction-specific emitter owns controller semantics.

## Evidence replacement rule

Only specifically scoped stronger evidence may replace or refine this policy's released-input constant,
TH-bit/button-group mapping, bit-7 generalization, side-effect rule, or failure boundary: it must
directly cover this exact functional selector family and the exact affected CPU-visible behaviour,
record its provenance and limits, and receive independent validation. A directly supporting hardware
source (for example a documented power-on default for the DATA/CTRL latches, a documented TH/TR/TL
handshake timing model, or this project's own future host-input-frontend work) may establish a hardware
fact or extend this project's architecture; a qualified independent observation remains distinct from
that fact; a derivation remains project inference. Replacement must retain neighbouring
controller-I/O-stage mismatches as fail-closed, must not silently reopen or narrow the SEG-007-T121
write family or any other read selector above, and must not silently implement DATA3, the six-button-pad
sequence, or any host input capture without its own separately justified task.

## Privacy and non-goals

This policy records no commercial-ROM address, byte, opcode, disassembly, trace, local path, or derived
fixture beyond the already-public `$A10003`/`$A10005` DATA register addresses. It does not model a host
keyboard/gamepad input frontend, interactive or live input capture, persistence of captured input across
runs, the six-button pad ID sequence or multiplexed-nibble protocol, TH/TR/TL handshake timing, the
TH-transition interrupt, DATA3, serial-shift registers, other I/O ports, VDP, rendering, audio, Z80
behaviour, or a complete Genesis bus. It makes no rendering, interactive-input, or
title-screen-completion claim. It adds one controller-I/O read-selector family and one fixed
"all buttons released" input constant; it adds no new persisted state and no new struct field. It is
otherwise a decision record, exactly like the SEG-007-T020/T038/T079/T111/T121 sections above.

