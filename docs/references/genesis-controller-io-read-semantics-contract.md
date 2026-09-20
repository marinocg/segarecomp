# Genesis controller-I/O read-semantics contract (SEG-007-T017, integrated by SEG-007-T019)

**Status:** research-only record; it supplies no implementation, fixture, decoder, IR, emitter, or
runtime behavior, and authorizes none. It extends SEG-007-T011's/T015's/T016's already-committed,
sanitized findings with further public-hardware-documentation research at the same frontier. It does
not resolve every question it poses; several parts are recorded as an explicit, bounded research
boundary rather than guessed.
**CPU/platform covered:** original MC68000 (16-bit external data bus); Genesis/Mega Drive
controller-I/O address space. **Accessed:** 2026-08-12. **T019 decision:** Outcome B — retain the
existing controller-I/O fail-closed boundary. This is not an implementation authorization.

## Privacy boundary (read first)

This record never states, computes, or implies the private Sonic ROM's actual target address for the
frontier access. The "four-byte range" this task researches is identified **only functionally**
throughout ("the CTRL1/CTRL2-intersecting four-byte range", "the range this frontier's `TST.L` access
covers"), never as a numeric address literal. The two named controller-port register addresses this
record cites (CTRL1, CTRL2) and the public bounding I/O interval are **already-committed, public
Genesis hardware addresses** — restated verbatim from SEG-007-T011's/SEG-007-T015's own already
-committed evidence and from `docs/references/tst-l-absolute-long-contract.md`, not newly derived from
the private ROM. No hex address literal, opcode, extension word, disassembly, or ROM byte belonging to
the Sonic image appears anywhere below.

## Frontier consumed (restated, not re-derived)

This task consumes, without re-deriving, the sanitized access-shape facts already committed by
SEG-007-T011/T015/T016:

- The frontier access is a single 32-bit (long) **read**, by the previously classified `TST.L`
  absolute-long instruction (SEG-006-T001/SEG-007-T004/SEG-007-T007/SEG-007-T011).
- On an MC68000, this operand access is architecturally performed as two 16-bit external bus
  transfers (SEG-007-T011's Evidence, "Round 3 correction").
- The four-byte addressed range intersects the documented Player 1 and Player 2 controller
  control/direction byte lanes (CTRL1, CTRL2) together with the adjacent even-byte lanes
  (SEG-007-T011's Evidence, "Sanitized classification").
- SEG-007-T015 recognizes this same intersection as a distinct, narrowly-scoped, fail-closed
  diagnostic `unsupported_device_region_controller_io`, using the public bounding interval
  `[0x00A10000, 0x00A10020)` (already committed in `docs/references/tst-l-absolute-long-contract.md`
  § "Data-read region resolution"), and deliberately models no read-back value.
- SEG-007-T016 independently, deterministically reproduced this exact diagnostic twice against the
  real, authorized local Sonic input via the production `genesis-general-startup` ingress: exit class
  `rejected`, empty stdout, no controller read value, no continuation past the unresolved operand.

This task's own contribution is additional public-hardware research at that same functionally
-identified frontier: the MC68000 two-16-bit-transfer bus model applied to it, the documented and
undocumented disposition of every byte lane it covers, and an explicit statement of what remains
unresolved.

## Source facts

1. Motorola, *M68000 8-/16-/32-Bit Microprocessor User's Manual*, Rev. 8 (1993) — a different,
   independently verified Motorola primary source from the one already pinned as `M1` in
   `docs/references/tst-l-absolute-long-contract.md` (this document does not relitigate or restate
   `M1`'s own citation accuracy; it independently locates and verifies its own primary source for the
   one new fact this task needs). Verified scan:
   <https://bitsavers.org/components/motorola/68000/68000/M68000UM_AD_M68000_Microprocessor_Users_Manual_Rev8_1993.pdf>
   (accessed 2026-08-12). § 2.4 "Data Organization In Memory" (pp. 2-6–2-7, Figure 2-6 "Data
   Organization in Memory") states: "Bytes are individually addressable. As shown in Figure 2-5, the
   high-order byte of a word has the same address as the word. The low-order byte has an odd address,
   one count higher. Instructions and multibyte data are accessed only on word (even byte) boundaries.
   If a long-word operand is located at address n (n even), then the second word of that operand is
   located at address n+2." Figure 2-6 itself draws the 32-bit ("1 LONG WORD = 32 BITS") case as two
   rows labelled "HIGH ORDER" (first, at the base address) and "LOW ORDER" (second, at address+2), with
   the figure captioned as showing "the order of accessing the data from the processor." § 5.1 "Data
   Transfer Operations" / § 5.1.1 "Read Cycle" (p. 5-1) states: "During a read cycle, the processor
   receives either one or two bytes of data from the memory or from a peripheral device. If the
   instruction specifies a word or long-word operation, the [MC68000 family devices in 16-bit mode]
   processor reads both upper and lower bytes simultaneously by asserting both upper and lower data
   strobes." Together these establish: a 32-bit read at even address `n` is two sequential 16-bit bus
   cycles — the high-order 16 bits transferred first, at address `n`, and the low-order 16 bits
   transferred second, at address `n+2` — each individual 16-bit transfer itself moving both its bytes
   in one bus cycle via the upper/lower data-strobe pair.
2. PlutieDev, *I/O Ports* (<https://plutiedev.com/io-ports>), accessed 2026-08-12 (the same page
   SEG-007-T011 accessed 2026-08-11 as `IO1`; restated, with one additional quote newly read this
   session). "Basic usage" section, table "Control and data port addresses": Player 1 control port
   `$A10009`, data port `$A10003`; Player 2 control port `$A1000B`, data port `$A10005`; Modem control
   port `$A1000D`, data port `$A10007`. The page states control ports are configured by writing
   ("Control port: sets the direction of each pin"; "Writing a 0 sets the pin to input... 1 sets the
   pin to output", bit 7 = external interrupt enable) and gives no read-back statement for the control
   port specifically anywhere on the page. The page separately states, for the **data** port (a
   different, non-adjacent register — see "Byte-lane disposition" below): "Reading back from the data
   port returns the current values of all pins (both input and output)." The page also states every
   register in that table is individually byte-sized and two bytes apart from its sibling port's other
   register: "They're at these addresses, and in all case they're byte-sized (also note how each port
   address is two bytes apart)." Every address the table names is odd; the page names no register, and
   states no behavior, for any even byte adjacent to these odd-addressed registers.
3. PlutieDev, *Memory Map* (<https://plutiedev.com/memory-map>), accessed 2026-08-12 (the same page
   already cited as `S2` in `docs/references/genesis-rom-startup-contract.md` and restated by
   SEG-007-T011). "68000 address space" table, row `$A10000 – $A10FFF`, area "I/O, etc." The page gives
   no sub-region byte-lane breakdown within that interval.
4. Sega Enterprises, *Genesis Technical Overview* v1.00 (1991) — **independently re-verified this
   correction session** against a publicly accessible mirror, distinct from the unreachable Sega Retro
   archive entry this task's first session relied on. The Sega Retro archive entry
   (<https://segaretro.org/Genesis_Technical_Overview>) remained unreachable this session too (a plain
   `curl`/`WebFetch` request returned an automated anti-bot access-denial page); this task instead
   located and fetched a different, publicly accessible mirror hosted on the Internet Archive:
   <https://archive.org/details/Genesis_Technical_Overview_v1.00_1991_Sega_US> (item identifier
   `Genesis_Technical_Overview_v1.00_1991_Sega_US`), whose original "Text PDF" file was downloaded
   directly (<https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US.pdf>,
   `curl -I` returned `200 OK`, `application/pdf`, 292,381 bytes, accessed 2026-08-12) and inspected
   directly with `pdftotext -layout` on a per-page basis, cross-checked against the same item's
   independently-produced `_djvu.txt` OCR derivative
   (<https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US_djvu.txt>,
   `200 OK`, accessed 2026-08-12) to guard against single-pipeline OCR/extraction error. Both
   extractions agree on every fact quoted below. Verified locators and quotes:
   - PDF page 9, section "_ I/O AREA _": a memory-map diagram showing `$A10000` through `$A10020` as
     the I/O port register area (Version No., DATA (CTRL1/CTRL2/EXP), CONTROL (1/2/E), TxDATA/RxDATA/
     S-MODE), followed immediately by `$A10020`: "ACCESS PROHIBITED" through `$A1FFFF` — independently
     corroborating (not superseding) SEG-007-T015's already-committed `[0x00A10000, 0x00A10020)`
     bounding interval from this same primary source.
   - PDF page 72, "4. SYSTEM I/O", "§ 2 I/O PORT": "The MEGA DRIVE has the three general purpose I/O
     ports, CTRL1, CTRL2 and EXP. Although each port differs from the others in physical shape it
     functions in the same manner. Each port has the following [5] REGISTERs for CONTROL." followed by
     a table:
     ```
     DATA       (PARALLEL DATA)        : R/W
     CTRL       (PARALLEL CONTROL)     : R/W
     S-CTRL     (SERIAL CONTROL)       : R/W
     TxDATA     (Txd DATA)             : R/W
     RxDATA     (Rxd DATA)             : R
     ```
     (both the `pdftotext` extraction and the independent `_djvu.txt` derivative render this table
     identically at lines "R/W", "R/W", "R/W", "R/W", "R" for these five rows in order). This directly
     documents the CTRL (PARALLEL CONTROL) register — the register type CTRL1/CTRL2 instantiate — as
     **R/W**, i.e. both readable and writable, not write-only.
   - PDF page 74, immediately following the port address-mapping table (which lists, among others,
     `$A10009 : CTRL 1` and, per the independent `_djvu.txt` derivative, `$A1000B : CTRL 2` — the
     `pdftotext` pipeline alone misrenders this specific digit as a letter, "$A1OOOB", a known OCR
     digit/letter confusion on this scanned page; the `_djvu.txt` derivative's independent OCR pass
     renders it correctly as `$A1000B`, matching the address already committed elsewhere in this
     record and in SEG-007-T011/T015): "Both BYTE and WORD access are possible. However, in the case
     of WORD access, only the lower byte is meaningful." (present verbatim, identically, in both the
     `pdftotext` and `_djvu.txt` extractions). This is stated immediately under the same address table
     that lists CTRL1 (`$A10009`) and CTRL2 (`$A1000B`), i.e. it applies to this register set including
     CTRL1/CTRL2.
   - PDF page 75: a per-bit breakdown of the CTRL register, "CTRL designates the I/O direction of each
     port and the INTERRUPT CONTROL of TH", with bit table `CTRL   INT  PC6  PC5  PC4  PC3  PC2  PC1
     PC0`, each bit individually marked `(RW)` — for example "INT (RW) 0: TH-INT PROHIBITED / 1: TH-INT
     ALLOWED" and "PC6 (RW) 0: PD6 INPUT MODE / 1: OUTPUT MODE" (same pattern for PC5–PC0) — a
     bit-level corroboration of the register-level R/W fact above, and independently corroborating (not
     superseding) `IO1`'s bit-7-interrupt-enable/bits-6-0-direction-select semantics from a second,
     primary, Sega-authored source.
   This source does **not** state that a CTRL1/CTRL2 read returns the last-written value, the external
   pin level, or any other specific bit pattern; it documents the register as R/W and specifies the
   BYTE/WORD access-width rule, nothing more about the exact returned value. It also does not state
   what concrete value appears on the upper byte lane of a WORD access; "only the lower byte is
   meaningful" is a statement about which byte carries meaningful register data, not a statement of the
   upper byte's electrical/bit content. Neither of those omissions is filled in by this task; see claims
   6a/6b, 7a/7b/7c, and 9a/9b below.
5. SEG-007-T011's Evidence, "Sanitized classification (device/region + access kind)" and "Known vs.
   unresolved" — the source of the sanitized access-shape facts this task consumes (see "Frontier
   consumed" above); not re-derived here.
6. SEG-007-T015's Evidence and `docs/references/tst-l-absolute-long-contract.md` § "Data-read region
   resolution" (SEG-007-T015 subsection) — the source of the recognized fail-closed
   `unsupported_device_region_controller_io` category and the public bounding interval
   `[0x00A10000, 0x00A10020)` used by that recognition; not re-derived here.
7. SEG-007-T016's Evidence — the source of the confirmation that this frontier is the real, currently
   reachable, deterministically reproduced production stop; not re-derived here.

## Public locators

| ID | Public source, URL, access date, and locator | Fact used here |
| --- | --- | --- |
| M2 | Motorola, *M68000 8-/16-/32-Bit Microprocessor User's Manual*, Rev. 8 (1993), [bitsavers scan](https://bitsavers.org/components/motorola/68000/68000/M68000UM_AD_M68000_Microprocessor_Users_Manual_Rev8_1993.pdf), accessed 2026-08-12; § 2.4 "Data Organization In Memory", Figure 2-6 (pp. 2-6–2-7); § 5.1.1 "Read Cycle" (p. 5-1). | The MC68000 two-16-bit-transfer bus model for a 32-bit read (high-order word first at the base address, low-order word second at base address + 2); each 16-bit transfer moves both its bytes in one bus cycle. |
| IO1 | PlutieDev, *I/O Ports* (<https://plutiedev.com/io-ports>), accessed 2026-08-12 (restated from SEG-007-T011, accessed 2026-08-11); "Basic usage" section, table "Control and data port addresses". | CTRL1 (`$A10009`)/CTRL2 (`$A1000B`) documented as per-pin direction registers, write-oriented, no control-port read-back statement; data port (a different register) documented read-back statement; every named register is byte-sized and odd-addressed; no even-lane statement. |
| S2 | PlutieDev, *Memory Map* (<https://plutiedev.com/memory-map>), accessed 2026-08-12 (restated from SEG-007-T011/SEG-005). | Public bounding interval `$A10000–$A10FFF`, "I/O, etc."; no sub-region byte-lane detail. |
| GTO1 | Sega Enterprises, *Genesis Technical Overview* v1.00 (1991), Internet Archive mirror, item `Genesis_Technical_Overview_v1.00_1991_Sega_US`: [details page](https://archive.org/details/Genesis_Technical_Overview_v1.00_1991_Sega_US), [PDF](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US.pdf), [djvu.txt derivative](https://ia801909.us.archive.org/24/items/Genesis_Technical_Overview_v1.00_1991_Sega_US/Genesis_Technical_Overview_v1.00_1991_Sega_US_djvu.txt), accessed 2026-08-12 (independently re-fetched and directly inspected this correction session; the Sega Retro archive entry remained unreachable). PDF p. 9 ("I/O AREA" map), p. 72 (§ 2 "I/O PORT" R/W register table), p. 74 (address mapping + "Both BYTE and WORD access are possible..." statement), p. 75 (per-bit CTRL table). | CTRL (PARALLEL CONTROL) register documented **R/W**; CTRL1 (`$A10009`)/CTRL2 (`$A1000B`) instantiate that register type; BYTE and WORD access are both documented as possible for this register set; for WORD access, only the lower byte is documented as meaningful; per-bit R/W corroboration of the direction/interrupt-enable bits; the `$A10000`–`$A10020` I/O interval, corroborating (not superseding) `T015E`'s already-committed bounding interval. Does not state a concrete read-back bit value or an upper-byte WORD-access value. |
| T011E | SEG-007-T011, Evidence section, the SEG-007-T011 backlog record, accessed 2026-08-12. | Sanitized access-shape facts this task consumes without re-deriving (32-bit read, two 16-bit MC68000 bus transfers, CTRL1/CTRL2 + adjacent even-byte-lane intersection; "Known vs. unresolved" boundary). |
| T015E | SEG-007-T015, Evidence section, the SEG-007-T015 backlog record, accessed 2026-08-12. | The recognized `unsupported_device_region_controller_io` category and the public bounding interval `[0x00A10000, 0x00A10020)` it uses; its own restatement of `GTO1`. |
| T016E | SEG-007-T016, Evidence section, the SEG-007-T016 backlog record, accessed 2026-08-12. | Confirms this is the real, currently reachable, deterministically reproduced production frontier. |
| MCD1 | Charles MacDonald, *Sega Genesis hardware notes* v0.8 (03/02/01), [`gen-hw.txt` SpritesMind mirror](https://gendev.spritesmind.net/mirrors/cmd/gen-hw.txt), accessed 2026-08-12; § 3.0 "Input and Output", paragraph beginning "Here's a read-out of the I/O registers in their default state", the immediately following register table, and the paragraphs "Each one can be read at an even address" and "Bits 7-0 of the Ctrl registers can be read or written." | Public secondary technical-source report: the default-state table lists CTRL port A and B as `00`; I/O registers may be read at their corresponding even addresses; CTRL bits 7–0 are readable/writable. It is not Sega primary documentation, a published real-hardware test corpus, or an independently qualified oracle for this project. |

## Per-claim provenance ledger

Every factual claim in this record is labeled exactly one of: **publicly documented hardware fact**
(specific citation), **independent empirical-oracle observation** (oracle named, independence
justified), **project inference** (inference and basis stated), or **still unresolved**. Musashi has
no hardware/device semantic authority in this repository (SEG-007-T011's established rule) and is
never used as an oracle for any claim below.

| # | Claim | Label | Citation / basis |
| --- | --- | --- | --- |
| 1 | A 32-bit read at an even address `n` is two sequential MC68000 external 16-bit bus transfers: high-order 16 bits at `n` (first), low-order 16 bits at `n+2` (second). | Publicly documented hardware fact | `M2` § 2.4, Figure 2-6; § 5.1.1. |
| 2 | This frontier's four-byte range intersects CTRL1/CTRL2 plus adjacent even-byte lanes (restated, not re-derived). | Project inference | Basis: SEG-007-T011's Evidence "Sanitized classification," produced by that task's own bounded, privacy-safe reinspection plus public-source cross-reference; restated here without re-deriving. |
| 3 | Given claims 1 and 2: the first (high-order) 16-bit transfer covers the even byte lane immediately preceding CTRL1, together with CTRL1 itself; the second (low-order) 16-bit transfer covers the even byte lane between CTRL1 and CTRL2, together with CTRL2 itself. | Project inference | Combines claim 1 (public bus-transfer-order fact) with claim 2 (already-committed, sanitized intersection shape) by pure structural reasoning; adds no new address and reveals no private value. |
| 4 | CTRL1 = `$A10009`, CTRL2 = `$A1000B`; each is a per-bit direction register for its port's parallel pins (bits 6–0 direction select, bit 7 TH external-interrupt enable); documented as an **R/W** (readable and writable) parallel-control register, not a write-only target. | Publicly documented hardware fact | `GTO1` (Sega, primary source), PDF p. 72 § 2 "I/O PORT" ("CTRL (PARALLEL CONTROL): R/W") and p. 74 (address mapping listing CTRL1/CTRL2 as instances of that register), corroborated at the bit level by p. 75's per-bit `(RW)` table. `IO1` (PlutieDev, secondary source) restated from SEG-007-T011 for the direction-bit/interrupt-enable-bit *semantics* specifically (bits 6–0 direction select, bit 7 TH external-interrupt enable), which remains accurate and is corroborated, not contradicted, by `GTO1`'s own per-bit table. |
| 5 | The data port register (e.g. `$A10003`/`$A10005`) — a different, non-adjacent register from CTRL1/CTRL2 and **not** part of this frontier's four-byte range — is documented to return, on read, "the current values of all pins (both input and output)." | Publicly documented hardware fact | `IO1`, "Reading back from the data port returns the current values of all pins (both input and output)." Recorded here only to make explicit that this documented behavior belongs to a **different** register than CTRL1/CTRL2 and must not be applied to them by analogy (see claims 6a/6b). |
| 6a | CTRL1/CTRL2 are readable — the register is documented **R/W**, i.e. a read of the register is a supported access form. | Publicly documented hardware fact | `GTO1`, PDF p. 72 § 2 "I/O PORT" ("CTRL (PARALLEL CONTROL): R/W"), corroborated by p. 75's per-bit `(RW)` table (see claim 4). |
| 6b | The exact bit value(s) a read of CTRL1/CTRL2 returns — the stored control byte verbatim, some transformed/latched state, or something else. | Still unresolved | `GTO1` documents the register as R/W (claim 6a) and documents each bit's *write* meaning (direction select / interrupt enable), but nowhere states what a *read* of the register returns as a bit pattern (no statement like "reading returns the last-written control byte"). `IO1` documents no read statement for the control port anywhere on its page either. No other source located this session documents it. This task explicitly searched for, and did not find, a citable statement equating CTRL-port read-back with either "last written value" or "external pin level," despite this being a plausible pattern for simple 8-bit direction/GPIO-style registers on other platforms. No such equivalence is asserted here without a citation. |
| 7a | For a WORD (16-bit) CPU access to this Genesis I/O register set, only the lower byte of that transfer is documented as meaningful register data. | Publicly documented hardware fact | `GTO1`, PDF p. 74, immediately following the CTRL1/CTRL2 address-mapping table: "Both BYTE and WORD access are possible. However, in the case of WORD access, only the lower byte is meaningful." This is a direct restatement of what Sega documents; it names no specific address, register instance, or frontier lane. |
| 7b | Given claim 1's MC68000 byte/word ordering and claim 3's already-established frontier layout, the even lane immediately preceding CTRL1 is the upper byte of the WORD transfer whose lower byte is CTRL1, and the even lane between CTRL1 and CTRL2 is the upper byte of the WORD transfer whose lower byte is CTRL2. Therefore `GTO1`'s lower-byte-only-meaningful rule (claim 7a) applies to those two specific frontier lanes: each is not meaningful register data for its WORD transfer (a semantic statement, not a numeric value — see claim 7c). | Project inference | Basis: claims 1 + 3 + 7a. Combines claim 1 (public MC68000 byte/word-addressing fact), claim 3 (already-established, sanitized frontier byte-lane layout), and claim 7a (Sega's general lower-byte-only-meaningful rule for this register set) by direct structural application to this specific frontier's two even lanes; adds no new address and reveals no private value. `GTO1` itself names no specific frontier lane — only this project's own inference connects its general rule to these two lanes. |
| 7c | The concrete CPU-visible/electrical bit pattern actually present on either non-meaningful upper byte lane during those WORD transfers — e.g. whether it reflects an adjacent register, a fixed/open-bus value, or something else. | Still unresolved | `GTO1`'s "only the lower byte is meaningful" (claim 7a) is a statement about which byte carries meaningful *register* data, not a statement of the electrical/bit content actually driven onto that byte lane during the bus transfer. No source located this session (`GTO1`, `IO1`, `S2`) states a concrete value, disposition (open-bus/reserved/mirrored/fixed/implementation-defined), or named register for either lane. This task does not infer, guess, or assume any of those dispositions (`0x00`, `0xFF`, open bus, mirrored data, fixed data, reserved value, or otherwise) without an independent citation. |
| 8 | Reset/power-on default state of CTRL1, CTRL2, or the two adjacent even lanes. | Still unresolved | Not addressed by `IO1` or `S2`. `GTO1` was independently re-verified this correction session (source fact 4) across PDF pp. 9, 72, 74, 75; none of those pages, nor any other page located, states a reset/power-on default value for CTRL1, CTRL2, or the two adjacent lanes. This task records no reset-value claim from any source. |
| 9a | This Genesis I/O register region (including CTRL1/CTRL2) supports both BYTE and WORD MC68000 accesses as documented access forms; a WORD access is not an unsupported, undefined, or device-rejected access shape. | Publicly documented hardware fact | `GTO1`, PDF p. 74: "Both BYTE and WORD access are possible." (`IO1` separately states every named register "in all case[s]... [is] byte-sized," which describes the register's own storage width, not the CPU access widths the device accepts; `GTO1` is the source for the access-width fact itself.) |
| 9b | Each of the two 16-bit transfers this frontier's 32-bit (long) `TST.L` read decomposes into (per claim 1, an MC68000 architectural fact) is itself a supported WORD access to this device region. | Project inference | Combines an MC68000-architecture fact (claim 1: a 32-bit read at an even address is two sequential 16-bit/WORD external bus transfers) with a device-documentation fact (claim 9a: Sega documents WORD access as a supported access form for this I/O register region) by direct inference — the CPU issues WORD-shaped bus cycles for this access, and the device is documented to support WORD-shaped bus cycles — rather than restating a single hardware source that itself addresses long/`TST.L` accesses specifically. `GTO1` does not itself discuss 32-bit/long accesses to this region or `TST.L`; this inference is this project's own composition of two independently-sourced facts, not a merged hardware fact. |
| 10 | Ordering/side-effect concerns specific to this read (e.g., whether reading CTRL1 or CTRL2 has a documented side effect, or whether the two 16-bit transfers must be treated as ordered for a device-specific reason beyond the general MC68000 bus-transfer order in claim 1). | Still unresolved | No source located this session documents any read side effect for CTRL1/CTRL2 specifically. Claim 1 (`M2`) establishes only the general MC68000 architectural transfer order, not any Genesis-I/O-chip-specific interlock or side effect. |
| 11 | An independent empirical oracle capable of validating the resulting read value/formula for this frontier. | Still unresolved | No independent oracle was found or justified this session (the "independent empirical-oracle observation" label exists in this record's label set but is deliberately unused here). Musashi is explicitly excluded from this role by SEG-007-T011's established rule (no hardware/device semantic authority). This task did not invent or select a substitute; see "What would independently validate this" below. |
| 12 | This is the real, currently reached, deterministically reproduced production frontier at this exact intersection. | Project inference | Basis: SEG-007-T016's independently validated project evidence (`T016E`) — two identical `genesis-general-startup` runs against the pinned, authorized local Sonic input deterministically produced `unsupported_device_region_controller_io`, empty stdout, exit class `rejected`. This is this project's own prior, independently-validated task evidence, not public hardware documentation, so it is categorized as a project inference rather than a hardware-fact claim; it is restated here, not re-derived. |

## T019 integration ledger: T017 and MacDonald fact groups

This is the closure ledger required by SEG-007-T019. Each retained group has exactly one provenance
classification. `MCD1` is a public secondary technical note, but its reported defaults are not thereby
an independent empirical oracle observation: the notes do not provide the independent, repeatable
real-hardware observation/provenance required for that label in this contract. A report of an initial
value remains conditioned source reporting, not a universal reset, power-on, or read-back claim.

| Fact group | Classification | Retained meaning and limit |
| --- | --- | --- |
| T017: ordered long-read decomposition into high WORD then low WORD, and both byte strobes in each WORD transfer | Publicly documented hardware fact | `M2` claim 1. This is the MC68000 transfer model, not a Genesis-device value model. |
| T017: CTRL1/CTRL2 occupy the meaningful lower-byte positions in the functionally identified four-byte range | Project inference | T017 claims 2, 3, and 7b combine the sanitized prior intersection shape with `M2` ordering and `GTO1`'s lower-byte-meaningful WORD rule. It does not reveal or derive a private target address. |
| T017: CTRL bits/register are R/W; BYTE and WORD accesses are possible; only a WORD transfer's lower byte is meaningful register data | Publicly documented hardware fact | `GTO1`, pp. 72, 74–75; T017 claims 4, 6a, 7a, and 9a. Readability does not specify the returned CTRL bit pattern. |
| T017: exact CTRL1/CTRL2 read-back bits, concrete upper-lane bits, device-specific read side effects/order, and a qualified independent oracle | Still unresolved | T017 claims 6b, 7c, 10, and 11. The CPU transfer order is known, but no selected source specifies those device-value or oracle facts. |
| MacDonald: the I/O registers may be read through their corresponding even addresses | Publicly documented hardware fact | `MCD1` § 3.0 states: "Each one can be read at an even address" and gives an example. It is additionally compatible with `GTO1`'s documented BYTE/WORD access support, but neither source here defines every byte returned by the long read. |
| MacDonald: CTRL bits 7–0 are readable/writable | Publicly documented hardware fact | `MCD1` § 3.0 states: "Bits 7-0 of the Ctrl registers can be read or written." This is independently supported by Sega primary source `GTO1`; it adds no new CTRL read-back-value claim. |
| MacDonald: its documented/default-state register readout lists CTRL port A/CTRL1 and CTRL port B/CTRL2 as `00` | Publicly documented hardware fact | `MCD1` § 3.0 introduces the table as "a read-out of the I/O registers in their default state" and lists `A10009h = 00` (Ctrl register for port A) and `A1000Bh = 00` (Ctrl register for port B). This means a public secondary technical source reports those values under that stated condition; it is neither a universal reset/power-on claim nor an independent empirical-oracle observation. |
| MacDonald-reported default-state CTRL values are guaranteed to equal the CTRL bytes at this specific SEG-007 startup frontier | Still unresolved | `MCD1` does not establish that its reported default state is universal at power-on, guaranteed across reset/console revisions, preserved until this startup access, unaffected by prior writes, or sufficient to determine either CTRL byte at this frontier. It also cannot determine the unresolved upper-byte lanes or the complete 32-bit `TST.L` result. |

No retained group is an **Independent empirical-oracle observation**. In particular, neither Musashi
nor an emulator implementation detail has controller-I/O hardware-semantic authority.

## T019 implementation-boundary decision — Outcome B

**Outcome B is selected.** The existing `unsupported_device_region_controller_io` failure remains the
only selected behavior for this frontier. The MacDonald access and R/W reports narrow no missing
byte-value question: they do not specify the two CTRL read-back bytes in the required state nor either
upper-byte lane's CPU-visible value. Consequently they cannot justify a deterministic 32-bit `TST.L`
result, continuation, or synthetic default.

If later independently justified evidence permits refinement, it must introduce one reusable
Genesis bus/device read boundary, consumed by statically lifted CPU memory operations, with an explicit
address, width, ordered MC68000 WORD-transfer request, and typed success-or-fail-closed result. That
boundary—not a `TST.L`/`test_absolute_long` emitter case—would own controller-I/O lane selection and
device read semantics and could subsequently serve other legitimate CPU operations. It must preserve
the established unsupported-region result for unselected registers, widths, lanes, timing, mapper,
self-modifying-code, and peripheral behavior.

This decision creates no implementation and no automatic further public-source-search task. A future
refinement may reconsider the boundary only from specifically recorded, independently adequate
evidence; until then, the unresolved values block implementation but do not broaden the current
fail-closed recognition. No private-ROM-derived content is used or added by this integration.

## MC68000 two-16-bit-transfer bus model applied to this range

Per claim 1 (`M2`) and claim 2 (restated SEG-007-T011/T015 shape), this frontier's 32-bit `TST.L` read
is architecturally two ordinary MC68000 external bus read cycles, in this order:

1. **First (high-order) 16-bit transfer**, at the base (lower) address of the four-byte range: covers
   the even byte lane immediately preceding CTRL1, together with CTRL1 itself.
2. **Second (low-order) 16-bit transfer**, two bytes higher: covers the even byte lane between CTRL1
   and CTRL2, together with CTRL2 itself.

Both transfers move their two constituent bytes simultaneously within their own bus cycle (`M2` § 5.1.1:
"the processor reads both upper and lower bytes simultaneously by asserting both upper and lower data
strobes"). This is a general MC68000 architectural fact, not a Genesis-I/O-device-specific behavior on
its own; combined with `GTO1`'s device-side confirmation that WORD access to this I/O region is a
supported access form (claim 9a) and that only the lower byte of such a WORD access is meaningful
register data (claim 7a, a general Sega fact naming no specific lane), each of these two 16-bit
transfers is now a documented-and-inferred WORD access to this device region (claim 9b) whose upper
byte (the even lane preceding CTRL1, and the even lane between CTRL1/CTRL2, respectively — connected to
`GTO1`'s general rule only by this project's own inference, claim 7b) is not meaningful register data
— though the concrete bit pattern actually present there remains unresolved (claim 7c).

## Byte-lane disposition (per lane, functionally identified)

| Lane (functional position within the range) | Named register? | Documented purpose | Documented readability | Documented read-back value | Disposition |
| --- | --- | --- | --- | --- | --- |
| Even lane immediately preceding CTRL1 (upper byte of the first WORD transfer) | No (not named by `IO1`, `S2`, or `GTO1`) | none documented | **inferred not meaningful register data** for this WORD access (claim 7b, applying `GTO1`'s general rule, claim 7a) | not applicable — not meaningful register data; concrete bit pattern **still unresolved** (claim 7c) | not-meaningful-for-WORD-access lane (inferred); concrete value undocumented |
| CTRL1 (`$A10009`) | Yes — Player 1 control/direction register | per-bit I/O-pin direction select (bits 6–0), TH external-interrupt enable (bit 7) | **documented R/W** — readable (claim 4, 6a) | exact bit value **still unresolved** (claim 6b) | documented purpose and readability; **read-back value undocumented** |
| Even lane between CTRL1 and CTRL2 (upper byte of the second WORD transfer) | No (not named by `IO1`, `S2`, or `GTO1`) | none documented | **inferred not meaningful register data** for this WORD access (claim 7b, applying `GTO1`'s general rule, claim 7a) | not applicable — not meaningful register data; concrete bit pattern **still unresolved** (claim 7c) | not-meaningful-for-WORD-access lane (inferred); concrete value undocumented |
| CTRL2 (`$A1000B`) | Yes — Player 2 control/direction register | same as CTRL1, for Player 2 | **documented R/W** — readable (claim 4, 6a) | exact bit value **still unresolved** (claim 6b) | documented purpose and readability; **read-back value undocumented** |

No lane in this range is documented as open-bus, reserved, mirrored, fixed, or implementation-defined
by any source located this session; each of those would be a specific claim this research did not find
support for. What changed from the prior version of this record: the two even lanes are no longer
blanket-"undocumented" — `GTO1` documents a general lower-byte-only-meaningful rule for this register
set's WORD accesses (claim 7a), and this project infers (claim 7b, not a direct Sega statement about
these specific lanes) that the rule applies to these two lanes as the not-meaningful upper byte of
their respective transfer — but the concrete bit pattern physically present on each of the four lanes
during a read (CTRL1/CTRL2's stored-byte read-back, and the two even lanes' actual driven value)
remains unresolved for all four.

## Resulting observable 32-bit value

No concrete 32-bit constant can be stated, and no complete, fully-specified formula can be given from
public sources alone: every one of the four byte lanes in this range still has an unresolved concrete
-value dimension (claims 6b, 7c), even though this session's correction established several previously
-unresolved *structural/documentation* facts (claims 6a, 7a, 7b, 9a, 9b) that narrow, but do not close,
the boundary. The only part of the "value" question this research can answer is **structural**, not
numeric:

```text
byte[0] = <even lane preceding CTRL1>    -- not meaningful for this WORD access (claim 7b, applying
                                             GTO1's general rule, claim 7a); concrete bit pattern:
                                             still unresolved (claim 7c)
byte[1] = CTRL1 read-back                 -- readable, R/W (claim 4, 6a); exact value: still unresolved (claim 6b)
byte[2] = <even lane between CTRL1/CTRL2> -- not meaningful for this WORD access (claim 7b, applying
                                             GTO1's general rule, claim 7a); concrete bit pattern:
                                             still unresolved (claim 7c)
byte[3] = CTRL2 read-back                 -- readable, R/W (claim 4, 6a); exact value: still unresolved (claim 6b)
32-bit value = (byte[0] << 24) | (byte[1] << 16) | (byte[2] << 8) | byte[3]
             (per M2's byte/word ordering, claim 1)
```

This shape (which lane occupies which byte position, which MC68000 bus transfer carries which half,
that CTRL1/CTRL2 are readable, and that the two even lanes are inferred to not carry meaningful register
data for their WORD access, by applying `GTO1`'s general rule to this specific frontier layout) is
established (claims 3, 6a, 7a, 7b); the actual byte **values** are not, for any of the four lanes. This
task deliberately does not guess a value or invent a decision procedure for the remaining unresolved
dimensions (CTRL1/CTRL2's exact read-back bit value; the even lanes' concrete driven bit pattern); per
this task's Acceptance, that remains an explicit research boundary.

## What would independently validate this

None of the following are claimed to exist or to have been used by this task; they are recorded only
as what a later task would need, distinct from this record's own derivation sources (`M2`, `IO1`, `S2`,
`GTO1`, and `T011E`/`T015E`/`T016E`):

- A public, citable source that explicitly documents CTRL1/CTRL2's exact read-back bit value and the
  even lanes' concrete driven bit pattern in this exact range (for example, a Sega official developer
  manual excerpt beyond what `GTO1` is now confirmed to cover, or a well-established secondary Genesis
  hardware reference this task did not locate — this session's attempt to reach Charles MacDonald's
  Genesis-hardware notes remained unreachable/rate-limited from this session's fetch tooling, distinct
  from the now-resolved `GTO1` search).
- A real-hardware test corpus (for example, a logic-analyzer capture or a homebrew test ROM run on
  physical Genesis hardware) with a published, citable report of the exact bytes read back from this
  register pair and its adjacent lanes, under stated prior-write and reset conditions.
- A capable, genuinely independent emulator/oracle with its own citable, published hardware-accuracy
  provenance for Genesis I/O-chip register read-back specifically (not merely general MC68000 CPU
  correctness) — distinct from Musashi, which this repository's established rule (SEG-007-T011)
  already excludes from this role. This task did not identify such an oracle and does not invent one.

No currently available independent oracle is identified by this task. This is recorded as a genuine
research boundary, not a gap this task attempts to fill with an unjustified substitute.

## Unresolved research boundary (explicit, per this task's Acceptance)

```text
Known (public hardware facts, this session, including this correction cycle):
- a 32-bit read at an even address is two sequential MC68000 16-bit bus
  transfers, high-order word first (at the base address), low-order word
  second (at base address + 2) (M2);
- CTRL1 ($A10009) and CTRL2 ($A1000B) are documented R/W (readable and
  writable) per-bit I/O-pin direction registers with a TH
  external-interrupt-enable bit (GTO1, corroborated by IO1's bit semantics,
  restated from SEG-007-T011);
- CTRL1/CTRL2 are therefore readable -- a read of the register is a
  documented, supported access form (GTO1, this correction cycle);
- both BYTE and WORD MC68000 accesses to this I/O register region
  (including CTRL1/CTRL2) are documented as possible; for a WORD access,
  only the lower byte is documented as meaningful register data, as a
  general rule for this register set (GTO1, this correction cycle;
  claim 7a) -- applying that general rule to this frontier's specific
  layout (claims 1 + 3), the upper byte of each WORD transfer here (the
  even lane preceding CTRL1, and the even lane between CTRL1/CTRL2,
  respectively) is inferred to be NOT meaningful register data (claim 7b,
  a project inference, not a direct GTO1 statement about these specific
  lanes);
- each of the two 16-bit transfers this frontier's 32-bit read decomposes
  into (M2) is itself a supported WORD access to this device region,
  combining the CPU-architecture fact (M2) with the device-documentation
  fact (GTO1) by inference (project inference, not a single merged
  hardware fact);
- the data port register (a different, non-adjacent register) is
  documented to return the current pin values on read (IO1); this fact
  does not transfer to CTRL1/CTRL2 by analogy and is not applied to them;
- combining the above with SEG-007-T011's/T015's already-committed
  intersection shape, the first MC68000 bus transfer covers the even lane
  preceding CTRL1 plus CTRL1, and the second covers the even lane between
  CTRL1/CTRL2 plus CTRL2 (structural fact only, no numeric address).

No longer unresolved (narrowed or removed by this correction cycle, using
a newly located, independently re-verified public mirror of GTO1):
- whether CTRL1/CTRL2 are readable at all -- now a documented hardware
  fact (GTO1: "CTRL (PARALLEL CONTROL): R/W");
- whether WORD access to this I/O region is a supported access form at
  all -- now a documented hardware fact (GTO1: "Both BYTE and WORD access
  are possible");
- whether the two even lanes carry meaningful register data for their
  WORD access -- now inferred NOT meaningful (claim 7b), by applying
  GTO1's general, directly-documented rule (claim 7a: "in the case of
  WORD access, only the lower byte is meaningful") to this frontier's
  already-established layout (claims 1 + 3); this is a real, citable
  disposition for those lanes, distinct from a blanket "undocumented,"
  though it is this project's own inference connecting GTO1's general
  rule to these specific lanes, not a direct GTO1 statement naming them.

Still unresolved:
- the exact CTRL1 read-back bit value (the register is documented
  readable, but no source states what bit pattern a read returns);
- the exact CTRL2 read-back bit value (same reasoning);
- the concrete CPU-visible bit pattern actually present on the
  not-meaningful upper byte lane of each WORD transfer -- "not meaningful"
  is a semantic/register-meaning statement, not a statement of the
  physical/electrical bit pattern actually driven onto that lane;
- reset/power-on default state of CTRL1, CTRL2, or the two adjacent even
  lanes;
- any device-specific read ordering/side-effect concern for this region
  beyond the general MC68000 architectural transfer order established by
  M2 (claim 1);
- a currently available, genuinely independent oracle or additional
  public source to validate a future resolution of the above -- still
  none identified this session.

Consistent with SEG-007-T011's own "Known vs. unresolved" pattern and this
milestone's governing Acceptance ("unsupported reads/ports/commands fail
closed"): implementation is NOT yet justified by this record for any of the
"Still unresolved" items above, even though several previously-unresolved
items above have now been narrowed to documented facts. A future
implementation task must not guess a concrete value for CTRL1/CTRL2's
read-back or for the not-meaningful upper-byte lanes from this record; it
must either locate a citable source this research did not find, obtain
real-hardware or independently-provenanced-oracle evidence, or continue to
fail closed.
```

## Non-goals

- Implementing any controller, bus, or device behavior, runtime routing change, CPU-instruction
  change, or synthetic/placeholder register value; this record is research-only, matching
  SEG-007-T017's own Non-goals.
- Modeling interactive input, TH-interrupt/serial-mode behavior, controller data-port protocol, VDP,
  rendering, audio, Z80, or timing/scheduler behavior.
- Reinspecting the private Sonic ROM, rerunning the Musashi adapter, or deriving any new private
  frontier fact; every access-shape fact here is restated, not re-derived, from SEG-007-T011/T015/T016.
- Designing or scoping the eventual bus/device implementation task.
- Broadening into general Genesis controller-I/O register coverage beyond this frontier's specific
  four-byte range (for example, the data port, serial-mode registers, or the modem port are mentioned
  above only to state that they are **not** part of this range, never as new coverage).
- Relitigating or re-verifying SEG-007-T011's/SEG-007-T015's/SEG-007-T016's own already-committed
  citations or conclusions (for example, `M1`'s citation in
  `docs/references/tst-l-absolute-long-contract.md`, or SEG-007-T015's `unsupported_device_region_controller_io`
  recognition and its `[0x00A10000, 0x00A10020)` bounding interval); this record independently locates
  and verifies the one new primary source it needed for the bus-transfer-order fact (`M2`), and, as of
  this correction cycle, independently re-verifies `GTO1` against a newly located public mirror for the
  specific R/W and BYTE/WORD-access facts this correction required — both are treated as this record's
  own primary-source work, not as relitigating a different task's prior conclusion — and otherwise
  restates prior evidence verbatim.

## Notes

- This record was produced entirely from already-committed, sanitized project evidence
  (SEG-007-T011/T015/T016) plus newly fetched public documentation (`M2`, restated/re-quoted `IO1`,
  restated `S2`, and, as of this correction cycle, an independently re-verified `GTO1`); no local Sonic
  ROM, Musashi adapter, or scanner tooling was used or needed by this task, consistent with its Scope.
- **Correction-cycle update (this session).** The Sega Retro archive page for *Genesis Technical
  Overview* remained unreachable to this session's fetch tooling (an automated anti-bot access-denial
  page). This correction cycle instead located and independently re-fetched a different, publicly
  accessible mirror of the same primary source on the Internet Archive (item
  `Genesis_Technical_Overview_v1.00_1991_Sega_US`; see `GTO1` in "Public locators" and source fact 4
  above), directly inspected its PDF and an independent `_djvu.txt` OCR derivative, and used it to
  establish (as documented hardware facts, not inherited/unverified restatements) that CTRL1/CTRL2's
  underlying CTRL register is R/W, and that both BYTE and WORD access are supported for this I/O region
  with only the lower byte meaningful for a WORD access. The claims and sections above were revised
  accordingly; see the per-claim provenance ledger (claims 4, 6a/6b, 7a/7b/7c, 9a/9b) and the
  "Unresolved research boundary" section for exactly what narrowed and what remains unresolved.
- **Second correction-cycle update (this session): claim 7 provenance split.** A further human review
  found claim 7a (as it read after the first correction cycle) improperly merged a direct Sega hardware
  fact (the general "WORD access -> only the lower byte is meaningful" rule) with a frontier-specific
  project inference (applying that rule to this frontier's two specific even lanes) under a single
  "Publicly documented hardware fact" label. This was split into exactly three claims: `7a` now states
  only the general Sega-documented rule, naming no specific lane, labeled "Publicly documented hardware
  fact"; `7b` is the new, explicitly labeled "Project inference" that applies `7a`'s general rule to
  this frontier's two specific even lanes, combining it with claims 1 and 3; `7c` (previously `7b`)
  remains "Still unresolved" for the concrete bit pattern question, unchanged in substance. Every
  section referencing the old `7a`/`7b` pair (the bus-model discussion, byte-lane disposition table,
  resulting-value section, and unresolved-boundary block) was updated to cite `7a`/`7b`/`7c` precisely
  and to distinguish the directly-documented general rule from this project's own inference applying it
  to these specific lanes. No hardware conclusion changed; only the provenance attribution became more
  precise. No numeric upper-byte value is asserted anywhere.
- This task's own web research also attempted, and failed, to reach Charles MacDonald's
  widely-referenced Genesis-hardware technical notes (a well-known secondary community reference for
  exactly this kind of byte-lane/read-back question); the hosting site and its Wayback Machine mirror
  were both unreachable from this session's fetch tooling (rate-limited/blocked). This is recorded as
  a bounded tooling limitation of this session, not evidence that no such source exists. This does not
  affect the `GTO1` facts above, which were independently re-verified via a different public mirror.
