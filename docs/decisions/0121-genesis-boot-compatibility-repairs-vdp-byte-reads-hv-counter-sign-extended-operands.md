# ADR 0121: Genesis boot compatibility repairs (sign-extended absolute data operands, VDP byte reads, HV counter, IRQ6 trampolines, in-place return rewrite)

## Status
Accepted.

## Context
Three Genesis titles stopped, or silently stalled, before their first visible frame, for five independent bounded reasons. Each
was found with the ordinary stop diagnostics and none needs a new architecture:

1. A data operand written as the sign-extended absolute-long spelling of a top-half 24-bit address (`BTST #n,(<sign-extended VDP register address>).L`)
   was rejected as `effective_address_not_24bit`, although the 68000 drives only A1..A23 and the spelling names the same bus
   address as the absolute-word form that is already canonicalized.
2. The translation-time routing seam deferred only WORD reads of the VDP window, while the runtime already owns the BYTE lanes
   of the control-port status alias (`$C00004`/`$C00005`), so a status poll with a byte read (`BTST`/`TST.B`) was an
   `unmapped_data_access` frontier.
3. The HV counter (`$C00008`-`$C0000F`) was not implemented, so a program that reads it stopped on an unsupported VDP access.
4. A vertical-interrupt handler that is a `MOVE.L #abs,-(A7); RTS` trampoline is not a retained static block, so the build did
   not install it as the IRQ6 handler and the runtime silently never delivered the interrupt (the game then waited forever for a
   flag the handler sets). In immutable-ROM AOT mode that handler address is nevertheless an ordinary compiled entry.
5. The inline-data-argument idiom (a routine advances its own saved return address with `MOVE.L <src>,(A7)` and then `RTS`) was
   a `return_target_mismatch`, because the popped value is not a JSR continuation. ADR 0048 already validates the identical
   situation for `MOVE.L <src>,-(A7)` followed by `RTS`.

## Decision
1. **Layer ownership of absolute-long.** The decoder preserves the raw 32-bit operand (`M68kEffectiveAddress::absolute_address`);
   it never rewrites it. The architectural effective address is therefore exact for LEA and PEA, which write/push
   `absolute_address` unchanged (`LEA ($FFFF8000).L,A0` leaves `$FFFF8000` in A0; `PEA ($FFFF8000).L` pushes `$FFFF8000`).
   The 24-bit bus address of a **data** access is derived at the existing effective-address seam, in
   `m68k_data_access_ea_address` beside `m68k_canonical_ea_address`: an absolute-long operand whose high byte is `$FF` with
   bit 23 set (the sign-extended spelling that absolute-word already yields) names the same A1..A23 location. Static
   data-address validation (discovery, the machine frontend, C3/C4 fact retention and re-verification) and the emitted
   runtime accesses call this one function, so they cannot disagree. JMP/JSR control targets keep
   `m68k_canonical_ea_address` (policy unchanged), and any other nonzero high byte stays `effective_address_not_24bit`.
2. The seam defers BYTE reads of `$C00004`/`$C00005` and of `$C00008`-`$C0000F` exactly as it defers the WORD read; every other
   VDP BYTE read stays an unmapped-data frontier.
3. **HV counter.** The runtime answers it (WORD, or either BYTE lane; V in the high byte, H in the low byte) as a pure
   function of deterministic guest time, using the same line origin as the HBlank/VBlank status flags (tick 0 of a line is the
   end of the active display, where V increments). The reference is the public Genesis Plus GX `core/hvc.h` tables
   (`cycle2hc32`/`cycle2hc40`) and `core/vdp_ctrl.c`. H32 steps through `$85..$93, $E9..$FF, $00..$84`, each value held 20
   master ticks (171 values). H40 steps through `$A5..$B6, $E4..$FF, $00..$A4` (211 values), each held 16 ticks except the
   horizontal-sync span (values 17..37) whose hold lengths come from the table. V runs `$00-$EA` then `$E5-$FF` over the 262
   lines. Earlier text and an earlier implementation scaled the line linearly from `$00` at tick 0; that is not equivalent
   (the counter at the line origin is `$85`/`$A5`, and H40 is not uniform) and was corrected in the same PR.
   Supported: Mega Drive display mode (register #1 bit 2), 224 lines, no HV latch, no interlace, register #12 bit 5 clear
   (forced HSYNC is a nonstandard clock configuration that the tick-indexed model does not account for, so `reg12=$20` and
   `$A1` are refused rather than read as ordinary H32/H40), and register #12 bits 7 and 0 both set (H40) or both clear (H32).
   Everything else - the latched counter, interlace, forced HSYNC, the 240-line mode, Master System
   mode 4 and a mixed width selection - is not modeled and fails closed as an unsupported VDP access. The machine runs the
   60 Hz schedule in every region profile, so the counter follows the executed schedule, not the nominal region rate.
4. **IRQ6 trampolines.** When no static block exists at the IRQ6 handler address, an entry of the final validated
   immutable-ROM AOT entry map (the same map the dispatch table is emitted from; not an alias, not a work-RAM thunk) at
   exactly that cartridge address qualifies as the build-resolved handler. The CPU-owned exception entry delivers to it; no
   second delivery mechanism exists. A vector that is unmapped/odd rejects the build; a work-RAM vector without an admitted
   guarded entry keeps the typed `irq6_vector_in_work_ram` stop.
5. **In-place return rewrite.** The ADR 0048 computed-jump seed set is widened from `MOVE.L <src>,-(A7)` to also include
   exactly `MOVE.L <src>,(A7)` (long write, destination mode address-indirect, register A7). Both seeds write the slot that
   the following RTS pops; the bounded forward walk (at most 8 identities, both arms of direct branches, any identity that
   writes A7, touches the stack or is not fully classified ends the path) is unchanged. The RTS still reads the real stack
   slot and its target must be a member of the final compiled-entry lookup; every other RTS keeps the stricter JSR-continuation
   validation. The mechanism only applies to RTS identities that are AOT-only (not reached by the static graph). (SEG-068-T005 clarification: only the *marked RTS* must be AOT-only; the push seed and the stack-neutral window identities may be block-route-owned, see the ADR 0048 amendment.)

## Consequences
No interpreter, runtime decoding or new ABI. Static discovery, JMP/JSR target policy and every unmodeled VDP shape are
unchanged. Covered by `m68k_pipeline_tests` (raw decode of absolute-long, the data-access seam function, seam shapes and their
unmapped neighbours, the final-compiled-set IRQ6 membership), `genesis_hint_region_controller_tests` (HV counter against
reference-table samples at every discontinuity in H40/H32, V discontinuity and frame wrap, mirrors, byte lanes, fail-closed
configurations) and `genesis_compat_repairs_generated_differential_test` (generated-native LEA/PEA/BTST/MOVE, in-place return
rewrite and its negatives, the typed rejections and the IRQ6 `MOVE.L #abs,-(A7); RTS` trampoline through exception entry and
RTE, each compared with the pinned Musashi core when its checkout is configured). Observed locally (classifications only):
two titles that stopped in their first frame now run their intro, and a third now reaches and renders its title/intro
sequence.

Pinned-Musashi result (local run, Linux, GCC 13.3, checkout at the pinned revision `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`
with a clean tree, `SEGARECOMP_M68K_COMPAT_REPAIRS_MUSASHI_CHECKOUT`): `genesis_compat_repairs_generated_differential_test`
printed `OK (Musashi compared)`. Compared bit for bit (D0-D7, A0-A7, CCR, the observed RAM ranges, final PC) between generated
native C and Musashi: five scenarios - absolute-long LEA/PEA/BTST/MOVE, in-place rewrite with an immediate source, in-place
rewrite with an address-register source, the previous `-(A7)` push form, and ordinary JSR/RTS. For IRQ6 only the handler-visible
exception frame was compared (SR inside the handler `$2600`, stacked SR `$2000`, stacked PC `$210`, handler marker `$55`); the
full-state comparison and the interrupt timing were not. NOT compared with Musashi (it has no compiled-entry set, VDP or
rejection model): the typed rejections, the fail-closed RTS cases, the IRQ6 negatives and the HV counter, which are checked
against hand-derived values and the Genesis Plus GX reference tables. Without the checkout the test still runs those
hand-derived expectations and reports `no Musashi checkout`; CI does not configure the checkout, so the Musashi comparison is a
local result, not a CI result.

Known limits: the mixed-width and mode-4 HV configurations are refused rather than approximated, and a statically reached
RTS keeps the static return-frame authority, so the rewrite idiom is admitted only where the callee is not statically
reached.
