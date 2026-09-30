#!/usr/bin/env python3
"""Master System baseline capability list (SEG-009-T001; T012 turns it into the coverage ratchet).

Stdlib only; test-side planning data. Every behaviour of the baseline machine profile
(docs/architecture/master-system-machine-contract.md) is one row with:
  id        stable identifier (never reused);
  area      capability area;
  behavior  one-line behaviour statement (the contract section holds the normative text);
  owner     exactly one SEG-009 task (T002-T007, or T009/T010 for presentation and build);
  evidence  the independent public source(s) and/or pinned-oracle check(s) that define it;
  scope     "in_scope" or "excluded:<reason>";
  fail_closed  for excluded rows that software can select at run time, the typed stop it produces;
  unresolved   the contract's U-item id when a bounded fact is still open (owner resolves it).

Output: tests/fixtures/sms-capabilities.json, byte-for-byte reproducible. `--check` fails when the
committed file differs from a fresh derivation; `--report` prints counts per area, owner and scope.
"""
import argparse
import collections
import json
import pathlib
import sys

SCHEMA = 1
REPO = pathlib.Path(__file__).resolve().parent.parent
OUTPUT = REPO / "tests" / "fixtures" / "sms-capabilities.json"

OWNERS = ("SEG-009-T002", "SEG-009-T003", "SEG-009-T004", "SEG-009-T005", "SEG-009-T006", "SEG-009-T007",
          "SEG-009-T009", "SEG-009-T010")

# Evidence keys resolve to the contract's reference list (section "References").
EVIDENCE = {
    "MD-HW": "Charles MacDonald, SMS/GG hardware notes (2002-11-12)",
    "MD-VDP": "Charles MacDonald, Sega Master System VDP documentation (2002-11-12)",
    "SP-MAP": "SMS Power!, Development/Mappers",
    "SP-MEM": "SMS Power!, Development/MemoryMap",
    "SP-HDR": "SMS Power!, Development/ROMHeader",
    "SP-3E": "SMS Power!, Development/Port3E",
    "SP-CLK": "SMS Power!, Development/ClockRate",
    "SP-PSG": "SMS Power!, Development/SN76489 (Maxim)",
    "SP-PAUSE": "SMS Power!, Development/PauseButton",
    "SP-REGION": "SMS Power!, Development/RegionDetection",
    "SP-BIOS": "SMS Power!, Development/BIOSes",
    "SP-PAL": "SMS Power!, Development/Palette",
    "SP-VREG": "SMS Power!, Development/VDPRegisters",
    "SP-VCNT": "SMS Power!, Development/ScanlineCounter",
    "TMS": "Texas Instruments TMS9918A/TMS9928A/TMS9929A Video Display Processors data manual",
    "Z80-ABI": "segarecomp docs/architecture/z80-master-system-integration-contract.md (SEG-008 contract)",
    "ADR-0050": "segarecomp ADR 0050 (consumer launcher; finite instruction budget amendment)",
    "ADR-0058": "segarecomp ADR 0058 (broad immutable-image AOT, image identity, SMS reference shape)",
    "GPGX-NOTE": "Genesis Plus GX core/sound/psg.c hardware-verification notes (pinned commit, ADR 0062)",
    "ORACLE-SMOKE": "SEG-009-T001 whole-machine oracle smoke (tests/sms_oracle_smoke_test.py)",
    "PSG-SMOKE": "SEG-009-T001 chip-level PSG oracle smoke (tests/sms_psg_oracle_smoke_test.py)",
    "LOCAL-AGG": "SEG-009-T001 sanitized aggregate over authorized local images (task Evidence)",
    "PROJECT": "project decision recorded in the contract/ADR (not a hardware claim)",
}

T2, T3, T4, T5, T6, T7, T9, T10 = OWNERS
IN = "in_scope"


def row(cid, area, owner, behavior, evidence, scope=IN, fail_closed=None, unresolved=None):
    item = {"id": cid, "area": area, "owner": owner, "behavior": behavior, "evidence": list(evidence), "scope": scope}
    if fail_closed:
        item["fail_closed"] = fail_closed
    if unresolved:
        item["unresolved"] = unresolved
    return item


ROWS = [
    # --- ingestion and mapper identity ---------------------------------------------------------------
    row("ingest.header_platform", "ingestion", T2, "TMR SEGA header at $7FF0/$3FF0/$1FF0 with region $4 selects the SMS export profile; absence or ambiguity requires an explicit platform/profile", ["SP-HDR", "MD-HW"]),
    row("ingest.region_japan", "ingestion", T2, "header region $3 (SMS Japan) is not the baseline profile", ["SP-HDR", "SP-REGION"], "excluded:japanese_profile_not_in_baseline", "SMS_ERROR_PROFILE_UNSUPPORTED"),
    row("ingest.game_gear", "ingestion", T2, "Game Gear headers ($5-$7) and Game Gear mode are not the SMS profile", ["SP-HDR", "MD-HW"], "excluded:game_gear_out_of_milestone", "SMS_ERROR_PROFILE_UNSUPPORTED"),
    row("ingest.rom_sizes", "ingestion", T2, "ROM sizes 32/64/128/256/512 KiB accepted; any other size (including 1 MiB) fails closed", ["SP-MAP", "SP-HDR", "ADR-0058"]),
    row("ingest.identity_record", "ingestion", T2, "record SHA-256, size, header class, profile, declared mapper and declaration source", ["PROJECT"]),
    row("ingest.checksum_informational", "ingestion", T2, "header checksum is recorded as match/mismatch only and never accepts or rejects", ["SP-HDR"]),
    row("mapper.identity_declared", "mapper_identity", T2, "mapper family comes only from an explicit declaration (build option, manifest, fixture declaration, recorded local identity); a header never establishes it; baseline families sega and rom_only", ["PROJECT", "SP-MAP"]),
    row("mapper.identity_undeclared_error", "mapper_identity", T2, "an undeclared or unknown mapper identity is the typed error SMS_ERROR_MAPPER_UNDECLARED before emission", ["PROJECT"]),
    row("mapper.identity_cli", "mapper_identity", T10, "segarecomp build requires --mapper sega (or a manifest) for SMS; no default, no autodetection", ["PROJECT"]),
    row("mapper.codemasters", "mapper_identity", T2, "Codemasters mapper declared", ["SP-MAP", "MD-HW"], "excluded:non_baseline_mapper", "SMS_ERROR_MAPPER_UNSUPPORTED"),
    row("mapper.korean_msx_janggun", "mapper_identity", T2, "Korean, MSX/Nemesis, Janggun and other unlicensed mappers declared", ["SP-MAP"], "excluded:non_baseline_mapper", "SMS_ERROR_MAPPER_UNSUPPORTED"),
    row("mapper.eeprom_multicart", "mapper_identity", T2, "EEPROM, multicart and other board variants declared", ["PROJECT"], "excluded:non_baseline_mapper", "SMS_ERROR_MAPPER_UNSUPPORTED"),
    # --- memory map ---------------------------------------------------------------------------------
    row("mem.ram_8k", "memory_map", T2, "8 KiB work RAM at $C000-$DFFF", ["MD-HW", "SP-MEM"]),
    row("mem.ram_mirror", "memory_map", T2, "work RAM mirrored at $E000-$FFFF", ["MD-HW", "SP-MEM", "ORACLE-SMOKE"]),
    row("mem.rom_slots", "memory_map", T2, "three 16 KiB ROM slots at $0000/$4000/$8000", ["SP-MAP", "SP-MEM"]),
    row("mem.fixed_first_1k", "memory_map", T2, "$0000-$03FF always maps ROM offset $0000-$03FF regardless of slot 0", ["SP-MAP", "ORACLE-SMOKE"]),
    row("mem.rom_write_ignored", "memory_map", T2, "writes to ROM slot addresses have no effect (mask ROM)", ["SP-MAP"]),
    row("mem.code_image_rom_only", "memory_map", T2, "code_image reports ROM banks only; RAM and cartridge RAM report 0 (mutable code fails closed)", ["Z80-ABI", "ADR-0058"]),
    row("mem.3d_glasses", "memory_map", T2, "3D glasses register ($FFF8-$FFFB) has no device; writes are ordinary RAM-mirror writes", ["MD-HW", "SP-MEM"], "excluded:peripheral_absent"),
    row("mapper.registers", "mapper", T2, "Sega mapper registers $FFFC-$FFFF: $FFFD/$FFFE/$FFFF select the slot 0/1/2 bank", ["SP-MAP", "MD-HW"]),
    row("mapper.write_through", "mapper", T2, "mapper register writes also write RAM; reads of $FFFC-$FFFF/$DFFC-$DFFF return the RAM copy", ["MD-HW", "SP-MAP", "ORACLE-SMOKE"]),
    row("mapper.bank_masking", "mapper", T2, "bank number is masked to the power-of-two ROM bank count", ["SP-MAP", "ORACLE-SMOKE"]),
    row("mapper.reset_values", "mapper", T2, "power-on values $FFFC=0, $FFFD=0, $FFFE=1, $FFFF=2 (315-5235)", ["SP-MAP", "SP-BIOS", "ORACLE-SMOKE"]),
    row("mapper.slot0_remap", "mapper", T2, "slot 0 remap exposes bank offset $0400+ at $0400-$3FFF", ["SP-MAP", "ORACLE-SMOKE"]),
    row("mapper.cart_ram_slot2", "mapper", T2, "$FFFC bit 3 maps 16 KiB cartridge RAM into slot 2; bit 2 selects bank 0/1 of 32 KiB; data only", ["SP-MAP"]),
    row("mapper.cart_ram_state", "mapper", T2, "cartridge RAM is deterministic in-memory, zero at power-on, never persisted", ["PROJECT"]),
    row("mapper.cart_ram_system_overlay", "mapper", T2, "$FFFC bit 4 (cartridge RAM over $C000-$FFFF)", ["SP-MAP"], "excluded:no_known_software", "SMS_ERROR_CONTROL_BIT_UNSUPPORTED"),
    row("mapper.bank_shift", "mapper", T2, "$FFFC bits 1-0 bank shift non-zero", ["SP-MAP"], "excluded:no_known_software", "SMS_ERROR_CONTROL_BIT_UNSUPPORTED"),
    row("mapper.rom_write_enable_bit", "mapper", T2, "$FFFC bit 7 is accepted and has no effect on a mask ROM", ["SP-MAP", "LOCAL-AGG"]),
    row("mapper.imageset", "mapper", T2, "ImageSet: invariant first 1 KiB image plus one banked image per 16 KiB bank admissible in slots 0/1/2", ["ADR-0058", "Z80-ABI"]),
    row("mapper.code_image_dispatch", "mapper", T2, "code_image(addr) returns the currently mapped bank identity and window base", ["Z80-ABI"]),
    row("mapper.rom_only", "mapper", T2, "rom_only identity: 32 KiB ROM at $0000-$7FFF without mapper; data reads of $8000-$BFFF stop typed", ["SP-MAP", "PROJECT"]),
    row("mapper.persistence", "mapper", T2, "battery-backed save persistence", ["PROJECT"], "excluded:deterministic_in_memory_only"),
    # --- memory control -----------------------------------------------------------------------------
    row("memctl.post_bios_value", "memory_control", T2, "memory control is $AB after the (not executed) BIOS: cartridge, RAM and I/O enabled", ["MD-HW", "SP-3E"]),
    row("memctl.write_compatible", "memory_control", T2, "writes keeping cartridge and RAM enabled and BIOS disabled are accepted (bits 7,5,1,0 have no SMS 2 effect)", ["MD-HW", "SP-3E"]),
    row("memctl.io_disable", "memory_control", T3, "port $3E bit 2 set: reads of $C0-$FF return $FF (SMS 2); YM2413 probes find no FM unit", ["MD-HW", "SP-3E"]),
    row("memctl.write_incompatible", "memory_control", T2, "writes disabling the cartridge or work RAM, or enabling the BIOS slot", ["MD-HW", "SP-3E"], "excluded:bios_and_slot_switching_not_in_baseline", "SMS_ERROR_CONTROL_BIT_UNSUPPORTED"),
    row("memctl.ram_copy_c000", "memory_control", T3, "post-BIOS RAM holds the last port $3E value ($AB) at $C000", ["MD-HW", "SP-BIOS"]),
    # --- I/O decode ---------------------------------------------------------------------------------
    row("io.decode_a7_a6_a0", "io_decode", T3, "only A7, A6 and A0 of the port are decoded; A8-A15 ignored", ["MD-HW", "MD-VDP", "Z80-ABI"]),
    row("io.write_00_3f", "io_decode", T3, "writes $00-$3F: even -> memory control, odd -> I/O control", ["MD-HW"]),
    row("io.read_00_3f", "io_decode", T3, "reads $00-$3F return $FF on SMS 2", ["MD-HW", "ORACLE-SMOKE"]),
    row("io.40_7f", "io_decode", T3, "$40-$7F: writes -> PSG; reads even -> V counter, odd -> H counter", ["MD-HW", "MD-VDP", "ORACLE-SMOKE"]),
    row("io.80_bf", "io_decode", T3, "$80-$BF: even -> VDP data, odd -> VDP control/status", ["MD-HW", "MD-VDP", "ORACLE-SMOKE"]),
    row("io.c0_ff", "io_decode", T3, "$C0-$FF: writes no effect; reads even -> port $DC, odd -> port $DD", ["MD-HW", "ORACLE-SMOKE"]),
    row("io.fm_unit", "io_decode", T3, "YM2413 FM unit ports $F0-$F2 (Japanese SMS / Mark III add-on)", ["MD-HW"], "excluded:fm_absent_on_export_sms2"),
    row("io.unimplemented_class", "io_decode", T3, "a decoded port class whose device is not yet implemented stops typed, never a silent value", ["PROJECT"]),
    # --- reset / BIOS ----------------------------------------------------------------------------------
    row("reset.cpu", "reset", T3, "CPU reset is SEG-008 z80_reset (PC=0, IM0, DI) plus no other register delta", ["Z80-ABI"], unresolved="U1"),
    row("reset.ram", "reset", T3, "work RAM is zero except the $C000 port-$3E copy", ["MD-HW", "PROJECT"], unresolved="U1"),
    row("reset.devices", "reset", T3, "mapper, PSG and I/O control take their documented reset values", ["SP-MAP", "SP-PSG", "MD-HW"]),
    row("reset.vdp_registers", "reset", T3, "VDP registers start at the contract's project-convention values (no public source)", ["PROJECT"], unresolved="U9"),
    row("bios.not_executed", "reset", T3, "the BIOS is never executed; the profile provides the documented post-BIOS state", ["MD-HW", "SP-BIOS"]),
    row("bios.image", "reset", T3, "a supplied BIOS image", ["PROJECT"], "excluded:bios_bytes_never_embedded", "SMS_ERROR_BIOS_UNSUPPORTED"),
    # --- timing ---------------------------------------------------------------------------------------
    row("timing.timebase_tstates", "timing", T3, "the unified timebase is Z80 T-states (u64); 228 T per line, 16 T per PSG tick", ["SP-CLK", "MD-VDP", "TMS"]),
    row("timing.ntsc_262_lines", "timing", T3, "NTSC frame = 262 lines = 59,736 T", ["MD-VDP"]),
    row("timing.frame_boundary", "timing", T3, "a frame ends when the V counter wraps to line 0 (T = k * 59,736)", ["MD-VDP", "PROJECT"]),
    row("timing.cpu_clock", "timing", T3, "CPU clock = master/15 = 315/88 MHz for host-time conversion", ["SP-CLK"]),
    row("timing.pal", "timing", T3, "PAL 313-line timing", ["MD-VDP", "SP-CLK"], "excluded:pal_not_in_baseline", "SMS_ERROR_PROFILE_UNSUPPORTED"),
    row("timing.wait_states", "timing", T3, "no wait states or bus contention; nominal T-states per the Z80 contract", ["Z80-ABI"]),
    row("timing.vdp_access_slots", "timing", T4, "VDP data-port writes always land (active-display access-slot loss not modeled)", ["MD-VDP"], unresolved="U6"),
    # --- interrupts -------------------------------------------------------------------------------------
    row("irq.int_level", "interrupts", T3, "VDP drives /INT as a level: (frame pending and R1.5) or (line pending and R0.4)", ["MD-VDP", "ORACLE-SMOKE"]),
    row("irq.ack_status_read", "interrupts", T4, "a control-port (status) read clears frame/line pending flags and deasserts /INT", ["MD-VDP", "ORACLE-SMOKE"]),
    row("irq.enable_gating", "interrupts", T4, "clearing/setting R1.5 or R0.4 deasserts/asserts /INT while the flag is pending", ["MD-VDP"]),
    row("irq.data_bus_ff", "interrupts", T3, "interrupt acknowledge returns $FF (IM0 = RST 38h; IM2 vector (I<<8)|$FF)", ["MD-HW", "ORACLE-SMOKE", "Z80-ABI"]),
    row("irq.im1", "interrupts", T3, "IM1 handler at $0038", ["MD-HW", "ORACLE-SMOKE"]),
    row("irq.nmi_pause", "interrupts", T6, "pause button press raises one NMI edge; release and holding do nothing", ["MD-HW", "SP-PAUSE", "ORACLE-SMOKE"]),
    row("irq.trace", "interrupts", T3, "interrupt trace artifact (T-state, source, asserted/accepted/deasserted)", ["PROJECT"]),
    # --- VDP ports and state ------------------------------------------------------------------------------
    row("vdp.vram_16k", "vdp_state", T4, "16 KiB VRAM, 14-bit address register, 2-bit code register", ["MD-VDP"]),
    row("vdp.cram_32", "vdp_state", T4, "32-byte write-only CRAM (--BBGGRR), CRAM address wraps modulo 32", ["MD-VDP", "SP-PAL", "ORACLE-SMOKE"]),
    row("vdp.registers_0_10", "vdp_state", T4, "registers 0-10 written through code 2; register numbers 11-15 have no effect", ["MD-VDP"]),
    row("vdp.control_latch", "vdp_ports", T4, "two-byte command word; first/second flag cleared by control read and by data read/write", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.first_byte_low_address", "vdp_ports", T4, "the first control byte updates the address low byte immediately", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.code0_prefetch", "vdp_ports", T4, "code 0 reads VRAM into the buffer and increments the address", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.buffered_read", "vdp_ports", T4, "data reads return the buffer, then refill it from VRAM and increment", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.write_loads_buffer", "vdp_ports", T4, "a data-port write also loads the read buffer", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.data_write_target", "vdp_ports", T4, "codes 0/1/2 write VRAM, code 3 writes CRAM", ["MD-VDP"]),
    row("vdp.autoincrement_wrap", "vdp_ports", T4, "address increments after each data access and wraps past $3FFF", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.status_flags", "vdp_status", T4, "status bits 7/6/5 = frame pending, sprite overflow, sprite collision; cleared by the read", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.status_low_bits", "vdp_status", T4, "status bits 4-0 read as %11111: project convention (documented as garbage; both pinned finalists return it)", ["MD-VDP", "ORACLE-SMOKE", "PROJECT"]),
    row("vdp.frame_irq_line", "vdp_interrupts", T4, "frame pending is set on line $C1 (192) / $E1 (224)", ["MD-VDP", "ORACLE-SMOKE"], unresolved="U2"),
    row("vdp.line_counter", "vdp_interrupts", T4, "line counter decrements on lines 0-192 (0-224), reloads from R10 on other lines and on underflow", ["MD-VDP", "ORACLE-SMOKE"], unresolved="U2"),
    row("vdp.line_irq_pending", "vdp_interrupts", T4, "underflow sets the line pending flag (not visible in status)", ["MD-VDP", "ORACLE-SMOKE"]),
    row("vdp.v_counter_ntsc", "vdp_counters", T4, "V counter NTSC tables: 192 00-DA,D5-FF; 224 00-EA,E5-FF", ["MD-VDP", "SP-VCNT", "ORACLE-SMOKE"]),
    row("vdp.h_counter_latch", "vdp_counters", T4, "port $7F returns the H counter latched by a TH transition", ["MD-VDP", "MD-HW"], unresolved="U3"),
    row("vdp.trace", "vdp_state", T4, "VDP trace artifact: register writes, VRAM/CRAM write digests, status reads, IRQ edges", ["PROJECT"]),
    row("vdp.revision_5246", "vdp_modes", T4, "VDP revision is 315-5246 (SMS 2): no SMS 1 table-mask quirks", ["MD-VDP", "SP-VREG"]),
    row("vdp.revision_5124", "vdp_modes", T4, "315-5124 (SMS 1) mask/zoom quirks", ["MD-VDP", "SP-VREG"], "excluded:vdp_revision_not_in_profile", "SMS_ERROR_PROFILE_UNSUPPORTED"),
    row("vdp.mode4_192", "vdp_modes", T4, "Mode 4, 192 active lines (M4=1 and not an extended/invalid combination)", ["MD-VDP"]),
    row("vdp.mode4_224", "vdp_modes", T4, "Mode 4, 224 active lines (M4=1, M2=1, M1=1, M3=0) on the SMS 2 VDP", ["MD-VDP", "SP-VCNT"]),
    row("vdp.mode4_240_ntsc", "vdp_modes", T4, "Mode 4, 240 lines on NTSC (does not produce a valid display)", ["MD-VDP", "SP-VCNT"], "excluded:invalid_on_ntsc", "SMS_ERROR_VDP_MODE_UNSUPPORTED"),
    row("vdp.tms9918_modes", "vdp_modes", T4, "TMS9918 Graphics I/II, Text, Multicolor and mixed modes (M4=0)", ["MD-VDP", "SP-PAL"], "excluded:legacy_modes_not_in_baseline", "SMS_ERROR_VDP_MODE_UNSUPPORTED"),
    row("vdp.invalid_text_mode", "vdp_modes", T4, "Mode 4 invalid text mode combinations", ["MD-VDP"], "excluded:legacy_modes_not_in_baseline", "SMS_ERROR_VDP_MODE_UNSUPPORTED"),
    row("vdp.r0_bit0_nosync", "vdp_modes", T4, "R0 bit 0 (no sync / monochrome)", ["MD-VDP"], "excluded:no_known_software", "SMS_ERROR_VDP_MODE_UNSUPPORTED"),
    row("vdp.mode_check_point", "vdp_modes", T4, "an excluded mode stops when it would affect output (display enabled at a rendered line)", ["PROJECT"]),
    # --- raster and renderer ----------------------------------------------------------------------
    row("raster.active_area", "raster", T5, "256 x 192 (or 224) active pixels; borders are not part of the framebuffer artifact", ["MD-VDP", "PROJECT"]),
    row("raster.display_enable", "raster", T5, "R1 bit 6 blank: the active area shows the backdrop", ["MD-VDP"]),
    row("raster.per_line_latch", "raster", T5, "horizontal scroll is latched per line; vertical scroll changes apply only from the next frame", ["MD-VDP"], unresolved="U2"),
    row("bg.name_table", "background", T5, "32x28 (32x32 extended) name table words ---pcvhnnnnnnnnn at R2 bits 3-1 ($0700 offsets in extended)", ["MD-VDP"]),
    row("bg.tiles_4bpp", "background", T5, "8x8 patterns, 4 bitplanes, 32 bytes each, 512 patterns", ["MD-VDP"]),
    row("bg.flips_palette", "background", T5, "horizontal/vertical flip and palette select", ["MD-VDP"]),
    row("bg.priority", "background", T5, "priority tiles cover sprites except where the tile pixel is colour 0", ["MD-VDP"]),
    row("bg.hscroll", "background", T5, "R8 horizontal scroll: coarse column and fine pixel", ["MD-VDP"]),
    row("bg.hscroll_lock", "background", T5, "R0 bit 6 locks horizontal scroll to 0 for rows 0-1 (lines 0-15)", ["MD-VDP"]),
    row("bg.vscroll", "background", T5, "R9 vertical scroll, wrapping at 224 (192 mode) or 256 (extended)", ["MD-VDP"]),
    row("bg.vscroll_lock", "background", T5, "R0 bit 7 locks vertical scroll to 0 for columns 24-31", ["MD-VDP"]),
    row("bg.left_column_blank", "background", T5, "R0 bit 5 fills pixels 0-7 with the backdrop colour", ["MD-VDP", "ORACLE-SMOKE"]),
    row("bg.fine_scroll_gap", "background", T5, "the fine-scroll gap at the left edge shows the backdrop", ["MD-VDP"], unresolved="U8"),
    row("bg.backdrop", "background", T5, "R7 selects the backdrop from the sprite palette", ["MD-VDP", "ORACLE-SMOKE"]),
    row("spr.sat", "sprites", T5, "64-entry sprite attribute table at R5; Y+1 placement; X/pattern pairs at +$80", ["MD-VDP"]),
    row("spr.terminator", "sprites", T5, "Y=$D0 ends the list in 192-line mode only", ["MD-VDP"]),
    row("spr.limit_overflow", "sprites", T5, "eight sprites per line; a ninth sets the overflow flag regardless of X/pattern", ["MD-VDP", "ORACLE-SMOKE"]),
    row("spr.collision", "sprites", T5, "overlapping opaque sprite pixels set the collision flag", ["MD-VDP", "ORACLE-SMOKE"]),
    row("spr.priority_order", "sprites", T5, "lower sprite index wins between sprites; colour 0 is transparent", ["MD-VDP", "SP-PAL"]),
    row("spr.shift", "sprites", T5, "R0 bit 3 shifts sprites left by 8", ["MD-VDP"]),
    row("spr.size_8x16", "sprites", T5, "R1 bit 1 selects 8x16 sprites (pattern index bit 0 ignored)", ["MD-VDP"]),
    row("spr.zoom", "sprites", T5, "R1 bit 0 doubles sprite pixels for all eight sprites (SMS 2)", ["MD-VDP", "SP-VREG"]),
    row("spr.pattern_base", "sprites", T5, "R6 bit 2 selects the upper 256 patterns", ["MD-VDP"]),
    row("spr.no_wrap", "sprites", T5, "sprites do not wrap horizontally at either edge", ["MD-VDP"]),
    row("pal.cram_rgb", "palette", T4, "CRAM --BBGGRR to RGB888 by component * 85 (presentation mapping)", ["SP-PAL", "MD-VDP", "PROJECT"]),
    row("fb.artifact", "framebuffer", T5, "framebuffer artifact = 256 x lines bytes of 6-bit CRAM colour values, row-major, plus SHA-256", ["PROJECT"]),
    # --- controllers and I/O control ---------------------------------------------------------------
    row("pad.port_dc", "controllers", T6, "port $DC: P1 up/down/left/right/TL/TR, P2 up/down; active low", ["MD-HW", "ORACLE-SMOKE"]),
    row("pad.port_dd", "controllers", T6, "port $DD: P2 left/right/TL/TR, reset=1, bit 5=1, TH A/B", ["MD-HW", "ORACLE-SMOKE"]),
    row("pad.player2", "controllers", T6, "second standard pad on port B", ["MD-HW"]),
    row("pad.reset_button", "controllers", T6, "reset button (absent on SMS 2; bit 4 reads 1)", ["MD-HW"], "excluded:absent_on_sms2"),
    row("pad.read_timing", "controllers", T6, "a port read returns the pad state at the io_in T-state", ["Z80-ABI", "PROJECT"]),
    row("pad.peripherals", "controllers", T6, "light phaser, paddle, sports pad, multitap, keyboard, Genesis pads", ["MD-HW"], "excluded:peripheral_absent"),
    row("ioctl.direction_level", "io_control", T6, "port $3F TR/TH direction and output-level latches", ["MD-HW"]),
    row("ioctl.output_readback", "io_control", T6, "a pin set as output reads back its output level on $DC/$DD (export console)", ["MD-HW", "SP-REGION", "ORACLE-SMOKE"]),
    row("ioctl.nationalization", "io_control", T6, "export region: TH output levels read back unchanged", ["MD-HW", "SP-REGION", "ORACLE-SMOKE"]),
    row("ioctl.reset_state", "io_control", T6, "port $3F resets to all pins input ($FF)", ["MD-HW"]),
    row("ioctl.h_latch_trigger", "io_control", T6, "a TH change latches the H counter (exact trigger through port $3F open)", ["MD-VDP"], unresolved="U3"),
    row("input.script", "controllers", T6, "scripted input: frame-stamped pad/pause events consumed at frame boundaries", ["PROJECT"]),
    # --- PSG -------------------------------------------------------------------------------------------
    row("psg.latch_data", "psg", T7, "latch/data protocol: latch byte selects channel/type and low 4 bits; data byte updates the latched register (tone high 6 bits, volume, noise)", ["SP-PSG", "PSG-SMOKE"]),
    row("psg.data_before_latch", "psg", T7, "a data byte before any latch byte", ["GPGX-NOTE", "PSG-SMOKE"], "excluded:unresolved_power_on_latch", "SMS_ERROR_PSG_DATA_BEFORE_LATCH", "U5"),
    row("psg.tone_immediate", "psg", T7, "tone registers update immediately on each byte", ["SP-PSG", "PSG-SMOKE"]),
    row("psg.divider_16", "psg", T7, "internal clock = CPU clock / 16 (one chip tick per 16 T)", ["SP-PSG", "SP-CLK"]),
    row("psg.tone_counter", "psg", T7, "10-bit counter reloads with the register and flips the output", ["SP-PSG", "PSG-SMOKE"]),
    row("psg.tone_period_0_1", "psg", T7, "tone period 0 behaves as period 1 and flips every chip tick: project decision departing from SP-PSG constant +1", ["GPGX-NOTE", "SP-PSG", "PSG-SMOKE", "PROJECT"], unresolved="U10"),
    row("psg.noise_rates", "psg", T7, "noise counter reload $10/$20/$40 or tone 2 period; one shift per two expiries", ["SP-PSG", "PSG-SMOKE"]),
    row("psg.lfsr_16_taps_0_3", "psg", T7, "16-bit LFSR, white noise taps bits 0 and 3 fed to bit 15, periodic = bit 0", ["SP-PSG", "PSG-SMOKE"]),
    row("psg.lfsr_reset", "psg", T7, "any noise-register write resets the LFSR to $8000", ["SP-PSG", "PSG-SMOKE"]),
    row("psg.noise_output_phase", "psg", T7, "noise output is bit 0 of the register after the shift (SP-PSG code; prose differs); 1 = channel on", ["SP-PSG", "GPGX-NOTE", "PSG-SMOKE"], unresolved="U4"),
    row("psg.attenuation", "psg", T7, "4-bit attenuation, 2 dB per step, $F silent (integer table)", ["SP-PSG", "PSG-SMOKE"]),
    row("psg.reset_state", "psg", T7, "reset: tone/noise registers 0, volumes $F, LFSR $8000, counters and outputs 0", ["SP-PSG", "GPGX-NOTE", "PSG-SMOKE", "PROJECT"], unresolved="U5"),
    row("psg.mono_mix", "psg", T7, "mono mix: sum of channel levels (bit ? level : 0); unipolar with a fixed offset", ["SP-PSG", "PROJECT"]),
    row("psg.gg_stereo", "psg", T7, "Game Gear stereo register (port $06)", ["MD-HW", "SP-PSG"], "excluded:game_gear_out_of_milestone"),
    row("psg.analog_imperfection", "psg", T7, "analogue decay/filtering of the output stage", ["SP-PSG"], "excluded:digital_model_only"),
    row("audio.write_timestamp", "audio_pipeline", T7, "PSG writes apply at the io_out T-state after catch-up", ["Z80-ABI", "PROJECT"]),
    row("audio.decimation", "audio_pipeline", T7, "deterministic integer box-filter decimation of chip ticks to 44,100 Hz", ["PROJECT"]),
    row("audio.pcm_format", "audio_pipeline", T7, "signed 16-bit little-endian mono PCM plus SHA-256 per run and frame range", ["PROJECT"]),
    # --- execution artifacts and driver --------------------------------------------------------------
    row("exec.run_api", "execution", T3, "run_until_cycle / run_until_frame with typed resumable vs fail-closed stops", ["Z80-ABI", "PROJECT"]),
    row("exec.headless_budget", "execution", T3, "headless driver requires a finite --instruction-budget and frame budget", ["ADR-0050"]),
    row("exec.state_digest", "execution", T3, "machine-state digest (CPU, RAM, mapper, device state)", ["PROJECT"]),
    row("exec.mapper_trace", "execution", T3, "mapper trace artifact (T-state, register, value)", ["PROJECT"]),
    row("exec.error_surface", "execution", T3, "SMS platform errors are a typed surface distinct from Z80 outcomes", ["PROJECT", "Z80-ABI"]),
    # --- presentation and build ------------------------------------------------------------------------
    row("view.pacing", "presentation", T9, "viewer paces at the rational NTSC frame period 59,736 T at 315/88 MHz", ["SP-CLK", "PROJECT"]),
    row("view.input_frames", "presentation", T9, "viewer samples host input only at frame boundaries into the scripted-input structure", ["PROJECT"]),
    row("view.audio_out", "presentation", T9, "SDL3 audio stream fed from the PCM buffer; underrun never affects guest state", ["PROJECT"]),
    row("view.equivalence", "presentation", T9, "recorded viewer input replays headless with identical frame/state/PCM digests", ["PROJECT"]),
    row("build.platform_route", "build", T10, "segarecomp build routes SMS through the T002 emission with explicit platform/profile", ["PROJECT"]),
    row("build.provenance", "build", T10, "cache key and provenance include platform, profile, declared mapper and declaration source", ["PROJECT"]),
]


def validate(rows):
    errors = []
    ids = set()
    for item in rows:
        if item["id"] in ids:
            errors.append("duplicate id %s" % item["id"])
        ids.add(item["id"])
        if item["owner"] not in OWNERS:
            errors.append("%s: owner %s not a SEG-009 capability owner" % (item["id"], item["owner"]))
        if not item["evidence"] or any(key not in EVIDENCE for key in item["evidence"]):
            errors.append("%s: evidence missing or unknown" % item["id"])
        scope = item["scope"]
        if scope != IN and not (scope.startswith("excluded:") and len(scope) > len("excluded:")):
            errors.append("%s: bad scope %s" % (item["id"], scope))
        if scope == IN and "fail_closed" in item:
            errors.append("%s: in-scope row carries a fail-closed class" % item["id"])
    return errors


def document():
    rows = sorted(ROWS, key=lambda item: item["id"])
    return {"schema": SCHEMA, "profile": "sms2-ntsc-export", "owners": list(OWNERS), "evidence": EVIDENCE,
            "capabilities": rows}


def text():
    return json.dumps(document(), indent=2, sort_keys=True) + "\n"


def report(doc):
    rows = doc["capabilities"]
    lines = ["capabilities: %d (in_scope %d, excluded %d, unresolved %d)" % (
        len(rows), sum(r["scope"] == IN for r in rows), sum(r["scope"] != IN for r in rows),
        sum("unresolved" in r for r in rows))]
    for title, key in (("area", "area"), ("owner", "owner")):
        counts = collections.Counter((r[key], r["scope"] == IN) for r in rows)
        lines.append("by %s:" % title)
        for name in sorted({r[key] for r in rows}):
            lines.append("  %-16s in_scope %3d  excluded %3d" % (name, counts[(name, True)], counts[(name, False)]))
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the committed file differs from a fresh derivation")
    ap.add_argument("--report", action="store_true", help="print counts per area, owner and scope")
    args = ap.parse_args(argv)
    errors = validate(ROWS)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    fresh = text()
    if fresh != text():
        print("capability list derivation is not deterministic", file=sys.stderr)
        return 1
    if args.check:
        committed = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if committed != fresh:
            print("tests/fixtures/sms-capabilities.json differs from a fresh derivation", file=sys.stderr)
            return 1
        print("ok: %d sms capabilities reproduce" % len(ROWS))
    elif not args.report:
        OUTPUT.write_text(fresh, encoding="utf-8", newline="\n")
    if args.report:
        print(report(json.loads(fresh)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
