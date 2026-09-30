#!/usr/bin/env python3
"""SEG-009-T012: Master System capability coverage report and ratchet data (test-side measurement).

The denominator is the independent T001 capability list (tests/fixtures/sms-capabilities.json, derived by
tools/sms_capabilities.py). For every in-scope row this tool records which stages are claimed and by which CTest
tests. A claim is a (row-id pattern, CTest test, stage set, needle) group: the test must be registered in
tests/CMakeLists.txt, its source must exist and must contain the needle (a function, fixture or fault name that really
exercises the behaviour), so a claim cannot outlive the test that supports it. This is not line coverage: a claim says
"the named test drives this behaviour", and the contract (section 13/14/18) holds the normative text.

stages
  implemented        a hermetic unit/model test of the production component exercises the row
  reference_validated  compared with a pinned machine/chip reference or an independent model written from the contract
                     (rows whose only evidence is a project decision and that no test compares are "-", not applicable)
  fixture_exercised  a project-authored fixture ROM runs the behaviour
  generated_native_exercised  that fixture is emitted by the SMS route, compiled as strict C11 and executed natively
  mutation_guarded   a deliberately wrong rule/fault for the behaviour is detected by the test

`covered` (the 100% gate) = implemented and fixture/native exercised and (reference validated or not applicable).
Excluded rows carry a reason and evidence tests (proving the typed stop, or device absence), pending T013 approval.

usage: sms_capability_coverage.py [--check] [--update-snapshot] [--report]
"""
import argparse
import fnmatch
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import sms_capabilities as caps  # noqa: E402

SNAPSHOT = ROOT / "tests" / "fixtures" / "sms-capability-coverage.json"
REPORT = ROOT / "docs" / "testing" / "sms-capability-coverage.md"
CMAKE = ROOT / "tests" / "CMakeLists.txt"
STAGES = ("implemented", "reference_validated", "fixture_exercised", "generated_native_exercised", "mutation_guarded")
LETTER = dict(zip("IRFNM", STAGES))

# CTest name -> source file(s) (relative to tests/)
TESTS = {
    "sms_capabilities_test": ["sms_capabilities_test.py"],
    "sms_fixture_rom_test": ["sms_fixture_rom_test.py"],
    "sms_oracle_smoke_test": ["sms_oracle_smoke_test.py"],
    "sms_psg_oracle_smoke_test": ["sms_psg_oracle_smoke_test.py"],
    "sms_machine_tests": ["sms_machine_test.cpp"],
    "sms_bank_crossing_test": ["sms_bank_crossing_test.py"],
    "sms_emission_budget_test": ["sms_emission_budget_test.py"],
    "sms_native_build_test": ["sms_native_build_test.py"],
    "sms_runtime_tests": ["sms_runtime_test.cpp"],
    "sms_machine_scheduler_test": ["sms_machine_scheduler_test.py"],
    "sms_u11_oracle_test": ["sms_u11_oracle_test.py"],
    "sms_psg_differential_test": ["sms_psg_differential_test.py"],
    "sms_psg_native_test": ["sms_psg_native_test.py"],
    "sms_vdp_tests": ["sms_vdp_test.cpp"],
    "sms_vdp_native_test": ["sms_vdp_test.py"],
    "sms_vdp_oracle_test": ["sms_vdp_oracle_test.py"],
    "sms_render_test": ["sms_render_test.py"],
    "sms_render_native_test": ["sms_render_native_test.py"],
    "sms_pad_tests": ["sms_pad_test.cpp"],
    "sms_pad_native_test": ["sms_pad_native_test.py"],
    "sms_hcounter_reference_test": ["sms_hcounter_reference_test.py"],
    "sms_machine_e2e_test": ["sms_machine_e2e_test.py"],
    "sms_machine_e2e_oracle_test": ["sms_machine_e2e_oracle_test.py"],
    "sms_viewer_tests": ["sms_viewer_test.cpp"],
    "sms_viewer_equivalence_test": ["sms_viewer_equivalence_test.py"],
    "sms_fail_closed_inventory_test": ["sms_fail_closed_inventory_test.py"],
}


def g(patterns, test, stages, needle):
    return {"patterns": patterns.split(), "test": test, "stages": stages, "needle": needle}


RENDER_MUTATED = "spr.y_wrap bg.priority bg.hscroll_lock bg.vscroll_lock bg.left_column_blank bg.fine_scroll_gap spr.limit_overflow spr.terminator raster.per_line_latch"

GROUPS = [
    g("exec.run_api exec.headless_budget", "sms_machine_scheduler_test", "R", "published Z80 instruction T-states"),
    g("exec.error_surface", "sms_fail_closed_inventory_test", "R", "contract's error table"),
    g("ingest.rom_sizes", "sms_fixture_rom_test", "R", "size code"),
    g("mapper.identity_declared mapper.identity_undeclared_error", "sms_fixture_rom_test", "R", "nothing is inferred from the header"),
    g("view.pacing", "sms_viewer_tests", "R", "test_pacer"),
    g("mapper.cart_ram_* mapper.rom_write_enable_bit", "sms_bank_crossing_test", "FN", "FFFC=08"),
    g("memctl.write_compatible memctl.io_disable", "sms_machine_scheduler_test", "FN", "port_open"),
    # --- ingestion, identity, build route -------------------------------------------------------------------------
    g("ingest.header_platform ingest.rom_sizes ingest.identity_record ingest.checksum_informational", "sms_machine_tests", "I", "test_ingestion"),
    g("ingest.header_platform ingest.checksum_informational", "sms_fixture_rom_test", "RF", "TMR SEGA"),
    g("ingest.rom_sizes ingest.header_platform", "sms_bank_crossing_test", "FN", "SMS_ERROR_ROM_SIZE_UNSUPPORTED"),
    g("ingest.* mapper.identity_*", "sms_native_build_test", "FN", "typed diagnostic"),
    g("mapper.identity_declared mapper.identity_undeclared_error", "sms_machine_tests", "I", "SMS_ERROR_MAPPER_UNDECLARED"),
    g("mapper.identity_declared mapper.identity_undeclared_error", "sms_bank_crossing_test", "FN", "SMS_ERROR_MAPPER_UNDECLARED"),
    g("build.*", "sms_native_build_test", "IFN", "reproduces the T008 artifact digests"),
    g("build.platform_route build.provenance", "sms_bank_crossing_test", "FN", "declaration_source"),
    g("mapper.identity_cli", "sms_native_build_test", "IFN", "--mapper"),
    # --- memory map, mapper, memory control ---------------------------------------------------------------------
    g("mem.* mapper.registers mapper.write_through mapper.bank_masking mapper.reset_values mapper.slot0_remap "
      "mapper.cart_ram_* mapper.rom_write_enable_bit mapper.imageset mapper.code_image_dispatch mapper.rom_only",
      "sms_machine_tests", "IR", "test_reference_differential"),
    g("mapper.bank_masking mapper.slot0_remap mapper.imageset mapper.code_image_dispatch mapper.cart_ram_slot2 mem.code_image_rom_only",
      "sms_machine_tests", "M", "test_contract_agreement_and_mutations"),
    g("mem.* mapper.registers mapper.write_through mapper.bank_masking mapper.slot0_remap mapper.cart_ram_slot2 "
      "mapper.code_image_dispatch mapper.imageset mapper.rom_only mapper.reset_values",
      "sms_bank_crossing_test", "FN", "identity"),
    g("mem.ram_mirror mapper.write_through mapper.bank_masking mapper.reset_values mapper.slot0_remap mem.fixed_first_1k",
      "sms_oracle_smoke_test", "R", "oracle_smoke"),
    g("mapper.registers mapper.write_through mapper.bank_masking mapper.slot0_remap mapper.code_image_dispatch",
      "sms_machine_e2e_test", "FN", "mapper_mask"),
    g("mapper.bank_masking", "sms_machine_e2e_test", "M", "mapper_mask"),
    g("mapper.registers mapper.write_through mapper.bank_masking mapper.slot0_remap mapper.code_image_dispatch mem.ram_mirror",
      "sms_machine_e2e_oracle_test", "R", "Gearsystem"),
    g("mapper.imageset mapper.code_image_dispatch", "sms_emission_budget_test", "FN", "ImageSet"),
    g("mem.code_image_rom_only", "sms_bank_crossing_test", "FN", "mutable_code"),
    g("mapper.cart_ram_* mem.rom_write_ignored", "sms_machine_tests", "I", "test_cart_ram_control_and_rom_only"),
    g("memctl.*", "sms_runtime_tests", "I", "test_no_owner_and_io_disable"),
    g("memctl.post_bios_value memctl.ram_copy_c000 memctl.write_compatible", "sms_runtime_tests", "I", "test_reset"),
    g("memctl.*", "sms_machine_scheduler_test", "FN", "bad memory control"),
    g("memctl.post_bios_value memctl.io_disable memctl.ram_copy_c000 memctl.write_compatible", "sms_oracle_smoke_test", "R", "oracle_smoke"),
    # --- port decode, reset ---------------------------------------------------------------------------------------
    g("io.*", "sms_runtime_tests", "I", "test_port_decode"),
    g("io.unimplemented_class io.80_bf io.40_7f io.c0_ff", "sms_runtime_tests", "I", "test_unimplemented"),
    g("io.*", "sms_machine_scheduler_test", "FN", "unimplemented device ports"),
    g("io.decode_a7_a6_a0 io.write_00_3f io.read_00_3f io.40_7f io.80_bf io.c0_ff", "sms_oracle_smoke_test", "R", "oracle_smoke"),
    g("reset.cpu reset.ram reset.devices bios.not_executed", "sms_runtime_tests", "I", "test_reset"),
    g("reset.cpu reset.ram reset.devices bios.not_executed", "sms_machine_scheduler_test", "FN", "BIOS image"),
    g("reset.vdp_registers", "sms_vdp_tests", "I", "test_reset_state"),
    g("reset.vdp_registers", "sms_vdp_oracle_test", "FNR", "vdp_reset_probe"),
    g("reset.cpu reset.ram reset.devices bios.not_executed", "sms_machine_e2e_oracle_test", "R", "Gearsystem"),
    g("reset.cpu reset.ram reset.devices bios.not_executed", "sms_oracle_smoke_test", "R", "oracle_smoke"),
    # --- timing -----------------------------------------------------------------------------------------------------
    g("timing.timebase_tstates timing.ntsc_262_lines timing.frame_boundary timing.wait_states", "sms_runtime_tests", "I", "test_geometry"),
    g("timing.access_timestamp timing.wait_states", "sms_runtime_tests", "I", "test_straddle_ordering"),
    g("timing.* ", "sms_machine_scheduler_test", "FN", "U11 scheduler ordering"),
    g("timing.access_timestamp", "sms_u11_oracle_test", "FNR", "instruction_start"),
    g("timing.access_timestamp timing.frame_boundary timing.ntsc_262_lines timing.timebase_tstates", "sms_vdp_oracle_test", "R", "vdp_straddle"),
    g("timing.cpu_clock", "sms_viewer_tests", "I", "test_pacer"),
    g("timing.cpu_clock", "sms_viewer_equivalence_test", "FN", "rational frame period"),
    g("timing.vdp_access_slots", "sms_vdp_tests", "I", "test_control_and_data_ports"),
    g("timing.vdp_access_slots", "sms_vdp_native_test", "FN", "vdp_seq_a"),
    g("timing.cpu_clock timing.wait_states timing.vdp_access_slots", "sms_machine_e2e_oracle_test", "R", "Gearsystem"),
    # --- interrupts -------------------------------------------------------------------------------------------------
    g("irq.int_level irq.data_bus_ff irq.im1 irq.trace irq.nmi_pause", "sms_runtime_tests", "I", "test_interrupts"),
    g("irq.ack_status_read irq.enable_gating irq.int_level", "sms_vdp_tests", "I", "test_interrupts"),
    g("irq.int_level irq.data_bus_ff irq.im1 irq.trace irq.nmi_pause", "sms_machine_scheduler_test", "FN", "pause NMI edge"),
    g("irq.int_level irq.ack_status_read irq.enable_gating", "sms_vdp_native_test", "FN", "vdp_irq"),
    g("irq.int_level irq.ack_status_read irq.enable_gating", "sms_vdp_oracle_test", "R", "vdp_irq"),
    g("irq.int_level irq.data_bus_ff irq.im1 irq.nmi_pause irq.ack_status_read", "sms_oracle_smoke_test", "R", "oracle_smoke"),
    g("irq.nmi_pause", "sms_pad_native_test", "FN", "pad_pause"),
    g("irq.nmi_pause", "sms_pad_native_test", "M", "mutation control"),
    g("irq.nmi_pause", "sms_machine_e2e_test", "M", "pause_edge"),
    g("irq.int_level irq.ack_status_read irq.nmi_pause", "sms_machine_e2e_oracle_test", "R", "Gearsystem"),
    # --- VDP ---------------------------------------------------------------------------------------------------------
    g("vdp.* pal.cram_rgb", "sms_vdp_tests", "I", "test_control_and_data_ports"),
    g("vdp.registers_0_10 vdp.cram_32 vdp.data_write_target pal.cram_rgb", "sms_vdp_tests", "I", "test_registers_and_cram"),
    g("vdp.status_flags vdp.status_low_bits", "sms_vdp_tests", "I", "test_status_register"),
    g("vdp.frame_irq_line vdp.line_counter vdp.line_irq_pending vdp.v_counter_ntsc", "sms_vdp_tests", "I", "test_interrupt_timeline"),
    g("vdp.mode4_192 vdp.mode4_224 vdp.mode_check_point vdp.revision_5246 vdp.trace", "sms_vdp_tests", "I", "test_modes"),
    g("vdp.h_counter_latch vdp.v_counter_ntsc", "sms_vdp_tests", "I", "test_v_counter"),
    g("vdp.trace", "sms_vdp_tests", "I", "test_line_hook_and_trace"),
    g("vdp.* pal.cram_rgb", "sms_vdp_native_test", "FN", "vdp_seq_a"),
    g("vdp.control_latch vdp.code0_prefetch vdp.buffered_read vdp.write_loads_buffer vdp.autoincrement_wrap vdp.cram_32 "
      "vdp.first_byte_low_address vdp.line_counter", "sms_vdp_native_test", "M", "mutation controls"),
    g("vdp.autoincrement_wrap vdp.buffered_read vdp.code0_prefetch vdp.control_latch vdp.cram_32 vdp.data_write_target "
      "vdp.first_byte_low_address vdp.registers_0_10 vdp.vram_16k vdp.write_loads_buffer vdp.frame_irq_line vdp.line_counter "
      "vdp.line_irq_pending vdp.v_counter_ntsc vdp.status_flags vdp.status_low_bits pal.cram_rgb",
      "sms_vdp_oracle_test", "R", "vdp_seq_a"),
    g("vdp.h_counter_latch", "sms_hcounter_reference_test", "R", "cycle2hc32"),
    g("vdp.h_counter_latch ioctl.h_latch_trigger", "sms_pad_native_test", "FN", "pad_hlatch"),
    g("vdp.h_counter_latch ioctl.h_latch_trigger", "sms_pad_tests", "I", "hcounter_table"),
    g("vdp.line_counter", "sms_machine_e2e_test", "M", "line_counter"),
    g("vdp.control_latch", "sms_machine_e2e_test", "M", "vdp_latch"),
    g("vdp.revision_5246 vdp.mode4_192 vdp.mode4_224 vdp.trace vdp.status_low_bits vdp.mode_check_point", "sms_machine_e2e_oracle_test", "R", "Gearsystem"),
    g("vdp.* pal.cram_rgb", "sms_machine_e2e_test", "FN", "capability area"),
    # --- raster, background, sprites, framebuffer ---------------------------------------------------------------
    g("raster.* bg.* spr.* fb.artifact pal.cram_rgb", "sms_render_test", "IR", "MUTATIONS"),
    g("spr.y_wrap", "sms_render_test", "IR", "y_wrap_"),
    g("spr.y_wrap", "sms_render_native_test", "FN", "render_ywrap_192"),
    g(RENDER_MUTATED, "sms_render_test", "M", "MUTATIONS"),
    g("raster.* bg.* spr.* fb.artifact", "sms_render_native_test", "FNR", "mutation controls"),
    g(RENDER_MUTATED, "sms_render_native_test", "M", "mutation controls"),
    g("raster.* bg.* spr.* fb.artifact pal.cram_rgb", "sms_machine_e2e_oracle_test", "R", "colour-class bijection"),
    g("raster.* bg.* spr.* fb.artifact pal.cram_rgb", "sms_machine_e2e_test", "FN", "capability area"),
    g("spr.limit_overflow", "sms_machine_e2e_test", "M", "sprite_limit"),
    g("pal.cram_rgb", "sms_vdp_tests", "I", "test_palette_and_seam"),
    g("raster.* bg.* spr.* fb.artifact", "sms_oracle_smoke_test", "R", "oracle_smoke"),
    # --- controllers, I/O control, input --------------------------------------------------------------------------
    g("pad.* ioctl.*", "sms_pad_tests", "I", "sweep"),
    g("pad.port_dc pad.port_dd", "sms_pad_tests", "M", "inverted-polarity mutant"),
    g("pad.* ioctl.*", "sms_pad_tests", "R", "anchors"),
    g("pad.* ioctl.*", "sms_pad_native_test", "FN", "pad_ports"),
    g("pad.read_timing", "sms_pad_native_test", "FN", "pad_boundary"),
    g("pad.* ioctl.*", "sms_oracle_smoke_test", "R", "oracle_smoke"),
    g("pad.port_dc pad.port_dd pad.player2 ioctl.*", "sms_machine_e2e_oracle_test", "R", "Gearsystem"),
    g("input.script", "sms_runtime_tests", "I", "scripted input parser"),
    g("input.script pad.read_timing", "sms_pad_native_test", "FN", "scripted input"),
    g("input.script", "sms_viewer_equivalence_test", "FN", "replayed through the headless driver"),
    g("pad.read_timing", "sms_u11_oracle_test", "R", "instruction_start"),
    # --- PSG and audio -----------------------------------------------------------------------------------------------
    g("psg.* audio.*", "sms_psg_differential_test", "IR", "ares"),
    g("psg.lfsr_16_taps_0_3 psg.divider_16 psg.attenuation", "sms_psg_differential_test", "M", "mutation controls"),
    g("psg.* audio.*", "sms_psg_native_test", "FN", "PSG port mirrors"),
    g("psg.latch_data psg.tone_immediate psg.tone_counter psg.noise_rates psg.lfsr_16_taps_0_3 psg.lfsr_reset psg.attenuation "
      "psg.divider_16 psg.tone_period_0_1 psg.noise_output_phase psg.reset_state", "sms_psg_oracle_smoke_test", "R", "psg"),
    g("psg.lfsr_16_taps_0_3 psg.noise_rates psg.lfsr_reset", "sms_machine_e2e_test", "M", "psg_taps"),
    g("audio.pcm_format audio.decimation psg.mono_mix", "sms_machine_e2e_oracle_test", "R", "RMS"),
    g("audio.write_timestamp", "sms_u11_oracle_test", "R", "instruction_start"),
    g("psg.* audio.*", "sms_machine_e2e_test", "FN", "capability area"),
    # --- execution artifacts, presentation ---------------------------------------------------------------------------
    g("exec.*", "sms_runtime_tests", "I", "test_halt_idle_and_budget"),
    g("exec.mapper_trace", "sms_runtime_tests", "I", "test_mapper_trace"),
    g("exec.state_digest exec.run_api", "sms_runtime_tests", "I", "test_split_equivalence"),
    g("exec.*", "sms_machine_scheduler_test", "FN", "budgets"),
    g("exec.*", "sms_machine_e2e_test", "FN", "determinism"),
    g("exec.error_surface", "sms_fail_closed_inventory_test", "I", "SmsError"),
    g("exec.error_surface", "sms_bank_crossing_test", "FN", "sms_error"),
    g("exec.headless_budget", "sms_bank_crossing_test", "FN", "--cycle-budget"),
    g("view.*", "sms_viewer_tests", "I", "test_run_and_recording"),
    g("view.pacing", "sms_viewer_tests", "I", "test_pacer"),
    g("view.*", "sms_viewer_equivalence_test", "FN", "viewer recorded"),
]

# Explicit "not applicable for reference validation": rows whose only independent basis is a project decision.
# Everything else in scope needs a reference_validated claim.
REFERENCE_NA = "evidence is a project decision (only PROJECT sources); there is no external or contract-derived reference to compare"

# Exclusions: row -> (classification, evidence tests proving the typed stop or the absence of the device, needle)
EXCLUSION_EVIDENCE = {
    "ingest.region_japan": ("typed_stop", "sms_machine_tests", "Japanese region is unsupported"),
    "ingest.game_gear": ("typed_stop", "sms_machine_tests", "a Game Gear header is unsupported"),
    "mapper.codemasters": ("typed_stop", "sms_bank_crossing_test", "SMS_ERROR_MAPPER_UNSUPPORTED"),
    "mapper.korean_msx_janggun": ("typed_stop", "sms_machine_tests", "janggun"),
    "mapper.eeprom_multicart": ("typed_stop", "sms_machine_tests", "eeprom"),
    "mapper.cart_ram_system_overlay": ("typed_stop", "sms_bank_crossing_test", "FFFC=10"),
    "mapper.bank_shift": ("typed_stop", "sms_runtime_tests", "SMS_ERROR_CONTROL_BIT_UNSUPPORTED"),
    "memctl.write_incompatible": ("typed_stop", "sms_machine_scheduler_test", "bad memory control"),
    "bios.image": ("typed_stop", "sms_machine_scheduler_test", "BIOS image"),
    "vdp.mode4_240_ntsc": ("typed_stop", "sms_vdp_tests", "test_mode_stops"),
    "vdp.tms9918_modes": ("typed_stop", "sms_vdp_tests", "test_mode_stops"),
    "vdp.invalid_text_mode": ("typed_stop", "sms_vdp_tests", "test_mode_stops"),
    "vdp.r0_bit0_nosync": ("typed_stop", "sms_vdp_tests", "test_mode_stops"),
    "psg.data_before_latch": ("typed_stop", "sms_psg_native_test", "data byte before any latch byte"),
    "timing.pal": ("baseline_profile_fixed", "sms_machine_tests", "test_profile_has_no_request_channel"),
    "vdp.revision_5124": ("baseline_profile_fixed", "sms_machine_tests", "test_profile_has_no_request_channel"),
    "mem.3d_glasses": ("device_absent", "sms_machine_tests", "3D glasses window"),
    "io.fm_unit": ("device_absent", "sms_runtime_tests", "test_port_decode"),
    "mapper.persistence": ("device_absent", "sms_machine_tests", "persist"),
    "pad.reset_button": ("device_absent", "sms_pad_tests", "anchors"),
    "pad.peripherals": ("device_absent", "sms_pad_tests", "anchors"),
    "psg.gg_stereo": ("device_absent", "sms_runtime_tests", "test_port_decode"),
    "psg.analog_imperfection": ("device_absent", "sms_psg_differential_test", "decimation"),
}
CLASS_TEXT = {
    "typed_stop": "software or input can select it; the typed stop has a hermetic test",
    "baseline_profile_fixed": "the baseline profile is NTSC SMS 2; no ingestion option, header field or driver flag can request the alternative, so no typed stop is claimed (witness: `test_profile_has_no_request_channel` and `sms_dependency_gate_test`)",
    "device_absent": "the device is not part of the baseline machine; the access decodes to no effect/open bus",
}

# Sanitized SEG-027+ scalability aggregates measured for T011 (host cc at -O0; no ROM bytes, addresses or paths).
SCALE_ROWS = [
    ("128 KiB", 52, "132k", "~89 MB", "8 s"),
    ("256 KiB", 54, "263k", "~219 MB", "14 s"),
    ("512 KiB", 58, "525k", "~355 MB", "27 s"),
]
REAL_IMAGE_FINDINGS = [
    "Three authorized local images (128/256/512 KiB) build through `segarecomp build` and run headless with finite "
    "frame budgets; per-image frame/IRQ/mapper-write counts are asserted as aggregates in `tests/sms_local_images_test.py` "
    "(skipped without `games/sms`).",
    "Sanitized aggregates, each image run for 600 frames: frame interrupts accepted about 470-600 per image; the pause NMI "
    "accepted exactly once under scripted input; line interrupts accepted 0 times in all three images (the line interrupt is "
    "therefore validated only by fixtures and the pinned references, not by any local image); mapper writes in the thousands "
    "with 6, 8 and 11 distinct slot-2 bank values; PCM non-constant and identical when the run is split into slices.",
    "128 KiB: one scanline of one frame differs from one reference (a mid-line palette write: the platform renders a "
    "line at its first T-state, one reference applies it to that line and the other to the next). Unresolved U2 "
    "sensitivity; the pinned references disagree with each other.",
    "512 KiB: a timed object enters at a frame that differs by a few frames on all three machines; the software derives a "
    "start offset from the Z80 refresh register (`LD A,R`), which depends on the exact iteration counts of status-polling "
    "loops. Unresolved U2/U11 + `R` sensitivity; the pinned references disagree (no two of the three machines agree).",
    "256 KiB: framebuffers identical to both references for the compared frames.",
    "Mapper identity of the three images is declared per SHA-256 and inferred by a human from the public SMS Power! "
    "mapper/cartridge pages, not from a byte heuristic; it is never derived from ROM contents by the tools. The images "
    "are authorized local images and are not named here.",
]
LIMITS = [
    "A claim is a registered (row pattern, CTest test, stage set, needle) group: the test is registered and its source "
    "contains the needle. It is not proof that the test executes the row; a passing ratchet means the registered tests exist "
    "and still name the behaviour, nothing more.",
    "The mapping from rows to tests is pattern-based: a wildcard group claims every matching row, including rows the test "
    "does not individually assert, so stage counts can over-claim.",
    "`mutation_guarded` under-counts: only behaviours with a deliberate wrong rule in a test are marked. An independent "
    "mutation campaign over the runtime found 86 of 89 valid mutants detected; the survivors were one equivalent mutant, "
    "the post-prefix-run NMI trace branch (S11) and halt-idle with a pending NMI (S13); the last two now have guards.",
    "For example `spr.y_wrap` is also claimed, by the `spr.*` wildcard groups, for the oracle and end-to-end tests that do not "
    "assert it; its real evidence is `sms_render_test` and `sms_render_native_test` against the independent model plus the "
    "unresolved U12 (the references differ from the platform in 224-line zoomed 8x16 at high Y).",
    "Stage letters say nothing about oracle agreement beyond the named reference; unresolved divergences are listed in the "
    "fact table below.",
]
U_STATUS = [
    ("U1", "open", "BIOS-left SP/RAM: `z80_reset` convention kept; no compared difference traces back to it (contract section 18)"),
    ("U2", "unresolved, tolerance asserted", "in-line event offset: offset 0 kept; 0..16 T lead asserted by `sms_vdp_oracle_test`; unresolved U2 sensitivity in the 128 KiB and 512 KiB images (the pinned references disagree)"),
    ("U3", "resolved (T006)", "H counter table and TH latch: `sms_hcounter_reference_test`, `pad_hlatch`"),
    ("U4", "classified", "noise output phase: LFSR sequence agrees with both chip references"),
    ("U5", "classified, stop kept", "PSG data before latch stays a typed stop"),
    ("U6", "classified (T012)", "VDP access-slot loss: not modeled; no public timing source, no compared software depends on it; data-port writes always land"),
    ("U8", "unresolved, masked", "fine-scroll gap: backdrop kept; the comparison masks the gap and checks the mask for staleness"),
    ("U9", "classified", "post-BIOS VDP state: project convention; `vdp_reset_probe` agrees on everything except the first status byte of one reference"),
    ("U10", "classified", "tone period 0/1: toggling kept; departure from SP-PSG recorded"),
    ("U11", "classified", "instruction-start ordering kept; bounded against both references; real-image `LD A,R` sensitivity recorded"),
    ("U12", "unresolved, classified divergence", "sprite Y wrap: native wraps modulo 256; 192-line mode matches Genesis Plus GX (Gearsystem deviates on zoomed 8x16 sprites at high Y); 224-line zoomed 8x16 sprites at high Y: the two references agree with each other and differ from native; no public documentation shows native is wrong, so no renderer change"),
]


def load_rows():
    return json.loads(caps.OUTPUT.read_text(encoding="utf-8"))["capabilities"]


def registered_tests():
    return set(re.findall(r"add_test\(NAME (\w+)", CMAKE.read_text(encoding="utf-8")))


def source_text(test):
    return "\n".join((ROOT / "tests" / rel).read_text(encoding="utf-8", errors="ignore") for rel in TESTS[test])


def validate_groups():
    errors = []
    ctest = registered_tests()
    for index, group in enumerate(GROUPS):
        test = group["test"]
        if test not in TESTS:
            errors.append("group %d: unknown test %s" % (index, test))
            continue
        if test not in ctest:
            errors.append("group %d: %s is not registered in tests/CMakeLists.txt" % (index, test))
        if not all((ROOT / "tests" / rel).exists() for rel in TESTS[test]):
            errors.append("group %d: source of %s missing" % (index, test))
            continue
        if group["needle"] not in source_text(test):
            errors.append("group %d: needle %r not found in %s" % (index, group["needle"], test))
        if any(ch not in LETTER for ch in group["stages"]):
            errors.append("group %d: bad stage letters %s" % (index, group["stages"]))
    for row_id, (_cls, test, needle) in EXCLUSION_EVIDENCE.items():
        if test not in ctest or needle not in source_text(test):
            errors.append("exclusion %s: evidence %s/%r not found" % (row_id, test, needle))
    return errors


def measure(rows):
    """Per-row stage mask, claiming tests and coverage."""
    out = {}
    for row in rows:
        if row["scope"] != caps.IN:
            continue
        claims = {stage: set() for stage in STAGES}
        for group in GROUPS:
            if any(fnmatch.fnmatchcase(row["id"], pattern.strip()) for pattern in group["patterns"]):
                for ch in group["stages"]:
                    claims[LETTER[ch]].add(group["test"])
        mask = ""
        for stage in STAGES:
            if claims[stage]:
                mask += "1"
            elif stage == "reference_validated" and row["evidence"] == ["PROJECT"]:
                mask += "-"
            else:
                mask += "0"
        out[row["id"]] = {"mask": mask, "tests": {s: sorted(t) for s, t in claims.items() if t}, "owner": row["owner"],
                          "area": row["area"]}
    return out


def covered(mask):
    i, r, f, n, _m = mask
    return i == "1" and (f == "1" or n == "1") and r in "1-"


def snapshot_doc(rows):
    measured = measure(rows)
    excluded = {}
    for row in rows:
        if row["scope"] == caps.IN:
            continue
        cls, test, needle = EXCLUSION_EVIDENCE.get(row["id"], (None, None, None))
        excluded[row["id"]] = {"reason": row["scope"].split(":", 1)[1], "class": cls, "evidence_test": test,
                               "fail_closed": row.get("fail_closed"), "approval": "pending T013"}
    return {"schema": 1, "stage_order": list(STAGES), "reference_na_reason": REFERENCE_NA,
            "rows": {k: v["mask"] for k, v in sorted(measured.items())},
            "tests": {k: v["tests"] for k, v in sorted(measured.items())},
            "excluded": dict(sorted(excluded.items()))}


def diff_masks(old, new):
    drops, improvements = [], []
    for key in sorted(set(old) | set(new)):
        a, b = old.get(key), new.get(key)
        if a is None or b is None:
            (improvements if a is None else drops).append("%s: row %s" % (key, "added" if a is None else "removed"))
            continue
        for stage, x, y in zip(STAGES, a, b):
            if x == y:
                continue
            (drops if (x == "1" and y != "1") else improvements).append("%s.%s %s->%s" % (key, stage, x, y))
    return drops, improvements


def tally(doc):
    masks = doc["rows"]
    total = len(masks)
    result = {}
    for k, stage in enumerate(STAGES):
        applicable = sum(1 for m in masks.values() if m[k] != "-")
        result[stage] = (sum(1 for m in masks.values() if m[k] == "1"), applicable)
    result["covered"] = (sum(1 for m in masks.values() if covered(m)), total)
    return result


def render_report(doc):
    t = tally(doc)
    lines = ["# Master System capability coverage", "",
             "<!-- generated by tools/sms_capability_coverage.py --update-snapshot; do not edit -->", "",
             "SEG-009-T012. The denominator is the independent T001 capability list "
             "(`tests/fixtures/sms-capabilities.json`). A stage is claimed only when a registered CTest test whose source "
             "contains the named needle drives the behaviour. Excluded rows carry a reason and evidence pending SEG-009-T013 "
             "approval.", "", "## Stage totals (in-scope rows)", "", "| stage | rows | applicable | share |", "| --- | --- | --- | --- |"]
    for stage in STAGES + ("covered",):
        a, b = t[stage]
        lines.append("| %s | %d | %d | %.1f%% |" % (stage, a, b, 100.0 * a / b if b else 100.0))
    lines += ["", "`covered` = implemented and (fixture or generated-native exercised) and (reference validated or not applicable). "
              "Reference validation is not applicable (`-`) only for rows whose sole evidence is a project decision. "
              "`mutation_guarded` is reported honestly and ratcheted; it is not part of the 100% gate.", "",
              "## Per area", "", "| area | rows | I | R | F | N | M | covered |", "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    by_area = {}
    for row_id, mask in doc["rows"].items():
        area = row_id.split(".")[0]
        by_area.setdefault(area, []).append(mask)
    for area, masks in sorted(by_area.items()):
        cells = [str(sum(1 for m in masks if m[k] == "1")) for k in range(5)]
        lines.append("| %s | %d | %s | %d |" % (area, len(masks), " | ".join(cells), sum(1 for m in masks if covered(m))))
    lines += ["", "## Exclusions (pending T013 approval)", "", "| row | reason | class | typed stop | evidence test |", "| --- | --- | --- | --- | --- |"]
    for row_id, e in doc["excluded"].items():
        lines.append("| `%s` | %s | %s | %s | `%s` |" % (row_id, e["reason"], e["class"], "`%s`" % e["fail_closed"] if e["fail_closed"] else "-", e["evidence_test"]))
    lines += ["", "Classes: " + "; ".join("`%s` = %s" % kv for kv in CLASS_TEXT.items()) + ".", "",
              "## Unresolved/classified facts", "", "| id | status | note |", "| --- | --- | --- |"]
    for uid, status, note in U_STATUS:
        lines.append("| %s | %s | %s |" % (uid, status, note))
    lines += ["", "## Fail-closed surface", "",
              "Every `SmsError` value has a hermetic test (`sms_fail_closed_inventory_test`). Z80 outcomes reachable through the SMS "
              "mapping (`mutable_code`, `unresolved_fetch_mapping`) have generated-native tests in `sms_bank_crossing_test`; "
              "`no_owner` (every mappable ROM offset is an owner start), `unknown_image_identity` (`code_image` only names "
              "declared images), `im0_unsupported_acknowledge_byte` (the acknowledge byte is always `$FF`) and `excluded_form` "
              "(reserved) are unreachable by construction, each with a witness test.", "",
              "## Sanitized generated-code scale aggregates (SEG-027+ input)", "",
              "Measured on project-irrelevant local images of the three size classes, host `cc -O0`; counts only.", "",
              "| size class | translation units | owners | generated C | compile time |", "| --- | --- | --- | --- | --- |"]
    for row in SCALE_ROWS:
        lines.append("| %s | %d | %s | %s | %s |" % row)
    lines += ["", "## Ratchet limits", ""]
    lines += ["- " + text for text in LIMITS]
    lines += ["", "## Real-image findings (sanitized)", ""]
    lines += ["- " + text for text in REAL_IMAGE_FINDINGS]
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail unless snapshot and report match a fresh measurement")
    ap.add_argument("--update-snapshot", action="store_true", help="deliberately rewrite snapshot and report")
    ap.add_argument("--report", action="store_true", help="print the report")
    args = ap.parse_args(argv)
    errors = validate_groups()
    rows = load_rows()
    for row in rows:
        if row["scope"] != caps.IN and row["id"] not in EXCLUSION_EVIDENCE:
            errors.append("excluded row %s has no evidence entry" % row["id"])
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    doc = snapshot_doc(rows)
    uncovered = [k for k, m in doc["rows"].items() if not covered(m)]
    text = json.dumps(doc, indent=2, sort_keys=True) + "\n"
    if args.update_snapshot:
        SNAPSHOT.write_text(text, encoding="utf-8", newline="\n")
        REPORT.write_text(render_report(doc), encoding="utf-8", newline="\n")
    if args.report:
        print(render_report(doc), end="")
    if args.check:
        old = json.loads(SNAPSHOT.read_text(encoding="utf-8")) if SNAPSHOT.exists() else {"rows": {}}
        drops, improvements = diff_masks(old["rows"], doc["rows"])
        if drops or improvements or SNAPSHOT.read_text(encoding="utf-8") != text or REPORT.read_text(encoding="utf-8") != render_report(doc):
            print("capability coverage differs from the snapshot: drops=%s improvements=%s" % (drops[:10], improvements[:10]), file=sys.stderr)
            return 1
        if uncovered:
            print("rows not covered: %s" % uncovered, file=sys.stderr)
            return 1
        a, b = tally(doc)["covered"]
        print("ok: %d/%d in-scope rows covered, %d exclusions with evidence" % (a, b, len(doc["excluded"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
