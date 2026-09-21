# MC68000 brief-format indexed addressing for the `MOVE` family (SEG-007-T120)

## Scope

One bounded addressing-mode extension of the already-supported `MOVE` / `MOVEA` family: address
register indirect with index and 8-bit displacement — `(d8,An,Xn)` — **brief-format extension word
only**, as a source operand. This is not a new instruction kind and introduces no new IR kind, no new
persistent runtime state, and no new diagnostic / frontier class.

## Encoding (Motorola *M68000 Family Programmer's Reference Manual*, brief-format extension word)

The 6-bit effective-address field selects this mode with `mode = 110`, `reg = An`. Exactly one
extension word follows:

```
 15   14 13 12   11   10 9   8    7 .. 0
 D/A  | Xn(3)  | W/L | SCALE | 0 | displacement(8, signed)
```

- **D/A** — index register class: `0` = `Dn`, `1` = `An`.
- **Xn** — index register number (0-7).
- **W/L** — index size: `0` = sign-extended low word, `1` = full long word.
- **SCALE** (bits 10-9) and **bit 8** (brief `0` / full `1`) — always zero on a base MC68000. A
  non-zero value here is a 68020+ full-format / scaled-index encoding and is **out of scope**: it is
  rejected at decode as `valid_but_unsupported_instruction` / `unsupported_instruction_form`
  (fail-closed), never silently ignored. This is a deliberate divergence from a real MC68000 (and
  from the pinned Musashi `CPU_TYPE_000` EA decode, which ignores bits 10-8 in brief format): the
  repository "reject ambiguity explicitly" policy takes precedence for a malformed / higher-part
  encoding that the authorized route never produces. The Musashi cross-check is therefore an
  encoding-structure check for the in-scope brief-format bits only.
- **displacement** — signed 8-bit displacement.

## Effective address

```
EA = An + sign_extend(Xn according to W/L) + sign_extend(d8)
```

computed in 32 bits, then truncated to the 24-bit external address bus (`EA & 0x00FFFFFF`) at the
existing shared MC68000 runtime routing seam — identical to every other register-relative
runtime-routed EA family. No scale, no base displacement, no memory indirection.

## Condition codes

`MOVE` sets `N`/`Z` from the moved value with `V`/`C` cleared and `X` unchanged (the existing
`M68kMoveResultCcrSpecification`); `MOVEA` is CCR-unaffected. Unchanged by this addressing-mode
extension.

## Pipeline integration

- Decode: `m68k_parse_ea_field` (`src/cpu/m68k/decode.cpp`) parses the brief-format word into
  `M68kEaMode::address_index8` with `index_reg` / `index_is_address` / `index_is_long` on the
  existing `M68kEffectiveAddress`. Extension-word bounds are checked exactly like `d16(An)`.
- Legality: `m68k_ea_index8` is exposed through a dedicated `m68k_ea_move_family_source` mask
  (`m68k_ea_move_source | m68k_ea_index8`) consumed by MOVE/MOVEA decode only. The shared
  `m68k_ea_move_source` set — which also gates the ADD/SUB/CMP/AND/OR source operand — is left
  unchanged, so an indexed source on any non-MOVE family instruction stays
  `valid_but_unsupported_instruction`. Indexed `MOVE` **destinations**
  remain `valid_but_unsupported_instruction` (not wired through the C4 static-memory-fact /
  runtime-routed destination machinery), mirroring the SEG-007-T088 / T116 / T118 scope-narrowing
  precedent.
- Lift / effect: unchanged — the decoded EA is carried through to `write_move` / `write_movea` as an
  unresolved fact, exactly like every other memory EA mode.
- C11 lowering: `m68k_emit_runtime_ea_address` gains the `address_index8` arm; `m68k_emit_ea_read`
  routes it through the same runtime bus-address / `genesis_route_access` path as
  `(An)` / `d16(An)`.
- Decode profiles: accepted only by `general_startup`; `genesis_startup` and `direct_flow` continue
  to reject the form.
