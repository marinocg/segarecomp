# ADR 0121: Genesis boot compatibility repairs (sign-extended absolute operands, VDP byte reads, HV counter, IRQ6 trampolines, in-place return rewrite)

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
4. A vertical-interrupt handler that is a `push.l abs; rts` trampoline is not a retained static block, so the build did not
   install it as the IRQ6 handler and the runtime silently never delivered the interrupt (the game then waited forever for a
   flag the handler sets). In immutable-ROM AOT mode that handler address is nevertheless an ordinary compiled entry.
5. The inline-data-argument idiom (a routine advances its own saved return address with `MOVE.L An,(A7)` and then `RTS`) was
   a `return_target_mismatch`, because the popped value is not a JSR continuation. ADR 0048 already validates the identical
   situation for `MOVE.L <src>,-(A7)` followed by `RTS`.

## Decision
1. Decoded absolute-long operands whose high byte is $FF with bit 23 set are canonicalized to the 24-bit address, for every
   instruction except JMP/JSR, whose separately pinned control-target policy is unchanged. Any other nonzero high byte keeps
   its raw value and stays `effective_address_not_24bit`.
2. The seam defers BYTE reads of `$C00004`/`$C00005` and of `$C00008`-`$C0000F` exactly as it defers the WORD read; every other
   VDP BYTE read stays an unmapped-data frontier.
3. The runtime answers the HV counter (WORD, or either BYTE lane; V in the high byte, H in the low byte) as a pure function of
   deterministic guest time over the 262-line NTSC frame: V runs $00-$EA then $E5-$FF; H runs $00-$B6 then $E4-$FF in H40 (211
   steps per line) and $00-$93 then $E9-$FF in H32 (171 steps), after the public Genesis Plus GX `core/vdp_ctrl.c` tables. The
   HV latch (register #0 bit 1), interlace and the 240-line mode are not modeled and stay fail-closed.
4. When no static block exists at the IRQ6 handler address, an immutable-ROM AOT entry (not an alias, not a thunk) at exactly
   that address qualifies as the build-resolved handler.
5. The ADR 0048 computed-jump seed set is widened from `MOVE.L <src>,-(A7)` to also include `MOVE.L <src>,(A7)`; the RTS that
   follows still pops the real stack slot and its target must be a final compiled identity.

## Consequences
No interpreter, runtime decoding or new ABI. Static discovery, JMP/JSR target policy and every unmodeled VDP shape are
unchanged. Covered by `m68k_pipeline_tests` (decode canonicalization incl. the non-sign-extended negative, seam shapes and
their unmapped neighbours) and `genesis_hint_region_controller_tests` (HV counter geometry in H40/H32, mirrors, byte lanes,
monotonicity, fail-closed configurations). Observed locally (classifications only): two titles that stopped in their first
frame now run their intro, and a third now reaches and renders its title/intro sequence.
