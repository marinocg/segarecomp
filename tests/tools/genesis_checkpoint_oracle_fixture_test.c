/*
 * SEG-007-T132: hand-authored synthetic fixture coverage for the
 * independent checkpoint-evidence oracle (tests/oracle/genesis). Every ROM
 * byte here is a project-authored, hand-assembled 68000 opcode word; none
 * of it is derived from, or resembles, any commercial image.
 *
 * The mini-ROM's instruction stream (documented against the Motorola
 * M68000 Family Programmer's Reference Manual's MOVE/MOVEQ/Bcc encoding,
 * and independently cross-checked against this pinned Musashi revision's
 * own m68k_disassemble() before this file was finalized) is:
 *
 *   offset  bytes                          instruction
 *      8    7011                           MOVEQ    #$11,D0
 *     10    7222                           MOVEQ    #$22,D1
 *     12    23FC CAFE BABE 00FF 0010       MOVE.L   #$CAFEBABE,($00FF0010).L
 *     22    33FC 0100 00A1 1100            MOVE.W   #$0100,($00A11100).L   (Z80 BUSREQ assert)
 *     30    33FC 0000 00A1 1200            MOVE.W   #$0000,($00A11200).L   (Z80 RESET assert)
 *     38    33FC 8104 00C0 0004            MOVE.W   #$8104,($00C00004).L   (VDP register #1 <- $04)
 *     46    33FC C000 00C0 0004            MOVE.W   #$C000,($00C00004).L   (address-set word 1: CRAM, addr 0)
 *     54    33FC 0000 00C0 0004            MOVE.W   #$0000,($00C00004).L   (address-set word 2)
 *     62    33FC 0123 00C0 0000            MOVE.W   #$0123,($00C00000).L   (CRAM data write)
 *     70    13FC 00B5 00C0 0011            MOVE.B   #$B5,($00C00011).L     (PSG latch: ch1 volume=5)
 *     78    13FC 007F 00A1 0003            MOVE.B   #$7F,($00A10003).L     (controller DATA1 <- $7F)
 *     86    3439 00C0 0004                 MOVE.W   ($00C00004).L,D2       (checkpoint_pc; VDP status read #1)
 *     92    3639 00C0 0004                 MOVE.W   ($00C00004).L,D3       (VDP status read #2)
 *     98    60FE                           BRA.S    *-2                   (safe idle loop)
 *
 * Reset vector: bytes 0-3 = initial SSP ($00FF8000), bytes 4-7 = initial PC
 * ($00000008).
 */
#include "checkpoint_oracle_test_support.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint16_t POSITIVE_WORDS[] = {
    0x7011, 0x7222, 0x23FC, 0xCAFE, 0xBABE, 0x00FF, 0x0010,
    0x33FC, 0x0100, 0x00A1, 0x1100, 0x33FC, 0x0000, 0x00A1, 0x1200,
    0x33FC, 0x8104, 0x00C0, 0x0004, 0x33FC, 0xC000, 0x00C0, 0x0004,
    0x33FC, 0x0000, 0x00C0, 0x0004, 0x33FC, 0x0123, 0x00C0, 0x0000,
    0x13FC, 0x00B5, 0x00C0, 0x0011, 0x13FC, 0x007F, 0x00A1, 0x0003,
    0x3439, 0x00C0, 0x0004, 0x3639, 0x00C0, 0x0004, 0x60FE};

/* Divergent-input fixture: byte-identical instruction stream except the
 * VDP register-set command's data byte changes from $04 to $05 (word 16:
 * 0x8104 -> 0x8105). Every instruction keeps the same length, so
 * CHECKPOINT_PC/VBLANK_READ_PC below are unaffected. */
static const uint16_t DIVERGENT_WORDS[] = {
    0x7011, 0x7222, 0x23FC, 0xCAFE, 0xBABE, 0x00FF, 0x0010,
    0x33FC, 0x0100, 0x00A1, 0x1100, 0x33FC, 0x0000, 0x00A1, 0x1200,
    0x33FC, 0x8105, 0x00C0, 0x0004, 0x33FC, 0xC000, 0x00C0, 0x0004,
    0x33FC, 0x0000, 0x00C0, 0x0004, 0x33FC, 0x0123, 0x00C0, 0x0000,
    0x13FC, 0x00B5, 0x00C0, 0x0011, 0x13FC, 0x007F, 0x00A1, 0x0003,
    0x3439, 0x00C0, 0x0004, 0x3639, 0x00C0, 0x0004, 0x60FE};

#define CHECKPOINT_PC UINT32_C(86)
#define VBLANK_READ_PC_0 UINT32_C(86)
#define VBLANK_READ_PC_1 UINT32_C(92)
#define INITIAL_SSP UINT32_C(0x00FF8000)
#define ENTRY_PC UINT32_C(0x00000008)

static void build_rom(uint8_t *rom, size_t rom_size, const uint16_t *words, size_t word_count) {
  size_t i;
  assert(rom_size >= 8U + word_count * 2U);
  memset(rom, 0, rom_size);
  rom[0] = (uint8_t)(INITIAL_SSP >> 24); rom[1] = (uint8_t)(INITIAL_SSP >> 16);
  rom[2] = (uint8_t)(INITIAL_SSP >> 8); rom[3] = (uint8_t)INITIAL_SSP;
  rom[4] = (uint8_t)(ENTRY_PC >> 24); rom[5] = (uint8_t)(ENTRY_PC >> 16);
  rom[6] = (uint8_t)(ENTRY_PC >> 8); rom[7] = (uint8_t)ENTRY_PC;
  for (i = 0U; i < word_count; ++i) {
    rom[8U + i * 2U] = (uint8_t)(words[i] >> 8);
    rom[8U + i * 2U + 1U] = (uint8_t)words[i];
  }
}

static GenesisCheckpointIdentity make_identity(const char *checkpoint_id, uint32_t instruction_budget) {
  GenesisCheckpointIdentity identity;
  size_t length = strlen(checkpoint_id);
  memset(&identity, 0, sizeof(identity));
  assert(length <= GENESIS_MAX_NAME_LENGTH);
  memcpy(identity.checkpoint_id, checkpoint_id, length);
  identity.checkpoint_id_length = (uint8_t)length;
  memcpy(identity.rom_sha256,
         "0000000000000000000000000000000000000000000000000000000000000", 65U);
  identity.options.schema_version = GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION;
  identity.options.instruction_budget = instruction_budget;
  identity.options.stable_frame_vblank_count = GENESIS_STABLE_FRAME_VBLANK_COUNT;
  return identity;
}

static GenesisCheckpointOracleFixture make_fixture(uint32_t checkpoint_pc) {
  GenesisCheckpointOracleFixture fixture;
  memset(&fixture, 0, sizeof(fixture));
  fixture.checkpoint_pc = checkpoint_pc;
  fixture.vblank_read_pc[0] = VBLANK_READ_PC_0;
  fixture.vblank_read_pc[1] = VBLANK_READ_PC_1;
  fixture.vblank_read_pc_count = 2U;
  return fixture;
}

/* Fixture 1 (positive): every bundle field asserted against a hand-derived
 * literal -- never a second implementation computing the expected value
 * (matching the same discipline the persistent-device-state contract
 * already requires of production tests). */
static void test_positive(void) {
  uint8_t rom[8U + sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]) * 2U];
  GenesisCheckpointIdentity identity = make_identity("oracle_positive", 32U);
  GenesisCheckpointOracleFixture fixture = make_fixture(CHECKPOINT_PC);
  GenesisCheckpointEvidenceBundle bundle;
  build_rom(rom, sizeof(rom), POSITIVE_WORDS, sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]));

  assert(genesis_checkpoint_oracle_derive_synthetic(rom, sizeof(rom), identity, &fixture, &bundle) == 0);

  assert(bundle.schema_version == GENESIS_CHECKPOINT_EVIDENCE_SCHEMA_VERSION);
  assert(bundle.identity.checkpoint_id_length == 15U);
  assert(memcmp(bundle.identity.checkpoint_id, "oracle_positive", 15U) == 0);

  /* CPU evidence. MOVEQ #$11,D0 and MOVEQ #$22,D1 sign-extend their
   * (positive) byte immediates; the two later status-port MOVE.W reads
   * land the observed $0008 VBlank bit in D2/D3. Every untouched register
   * stays at its zero reset value except A7, loaded from the ROM's own
   * reset vector. SR after reset is $2700 (S=1, IPL=7 -- Motorola M68000
   * Family Programmer's Reference Manual's documented reset state); every
   * later MOVE/MOVEQ here moves a positive, nonzero value, so N=Z=V=C=0
   * throughout and SR is unchanged at extraction time. */
  assert(bundle.cpu.d[0] == UINT32_C(0x00000011));
  assert(bundle.cpu.d[1] == UINT32_C(0x00000022));
  assert(bundle.cpu.d[2] == UINT32_C(0x00000008));
  assert(bundle.cpu.d[3] == UINT32_C(0x00000008));
  { uint32_t i; for (i = 4U; i < 8U; ++i) assert(bundle.cpu.d[i] == 0U); }
  { uint32_t i; for (i = 0U; i < 7U; ++i) assert(bundle.cpu.a[i] == 0U); }
  assert(bundle.cpu.a[7] == INITIAL_SSP);
  assert(bundle.cpu.sr == UINT16_C(0x2700));
  assert(bundle.cpu.pc_class == GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN);

  /* Device evidence. */
  assert(bundle.device.devices.z80_bus.bus_requested == 1U);
  assert(bundle.device.devices.z80_bus.bus_granted == 1U);
  assert(bundle.device.devices.z80_bus.reset_asserted == 1U);
  { uint32_t i; for (i = 0U; i < GENESIS_Z80_RAM_BYTES; ++i) assert(bundle.device.devices.z80_bus.z80_ram[i] == 0U); }

  assert(bundle.device.devices.vdp.registers[1] == 0x04U);
  { uint32_t i; for (i = 0U; i < GENESIS_VDP_REGISTER_COUNT; ++i) if (i != 1U) assert(bundle.device.devices.vdp.registers[i] == 0U); }
  assert(bundle.device.devices.vdp.control_port_awaiting_second_word == 0U);
  assert(bundle.device.devices.vdp.control_port_first_word == 0U);
  assert(bundle.device.devices.vdp.addressed_pointer == 0U);
  assert(bundle.device.devices.vdp.auto_increment_value == 0U);
  assert(bundle.device.devices.vdp.status_register == 0U); /* cleared by the second status read */
  assert(bundle.device.devices.vdp.data_port_transfer_code == 0x03U);
  assert(bundle.device.devices.vdp.data_port_transfer_code_valid == 1U);
  assert(bundle.device.devices.vdp.cram[0] == 0x01U);
  assert(bundle.device.devices.vdp.cram[1] == 0x23U);
  { uint32_t i; for (i = 2U; i < GENESIS_VDP_CRAM_BYTES; ++i) assert(bundle.device.devices.vdp.cram[i] == 0U); }
  { uint32_t i; for (i = 0U; i < GENESIS_VDP_VRAM_BYTES; ++i) assert(bundle.device.devices.vdp.vram[i] == 0U); }
  { uint32_t i; for (i = 0U; i < GENESIS_VDP_VSRAM_BYTES; ++i) assert(bundle.device.devices.vdp.vsram[i] == 0U); }
  assert(bundle.device.devices.vdp.dma.phase == GENESIS_VDP_DMA_IDLE);

  assert(bundle.device.devices.psg.latched_channel == 1U);
  assert(bundle.device.devices.psg.latched_volume == 1U);
  assert(bundle.device.devices.psg.latch_valid == 1U);
  assert(bundle.device.devices.psg.attenuation[1] == 5U);
  assert(bundle.device.devices.psg.attenuation[0] == 0U && bundle.device.devices.psg.attenuation[2] == 0U &&
         bundle.device.devices.psg.attenuation[3] == 0U);
  { uint32_t i; for (i = 0U; i < 3U; ++i) assert(bundle.device.devices.psg.tone_period[i] == 0U); }
  assert(bundle.device.devices.psg.noise_control == 0U);

  assert(bundle.device.devices.controller_io.data[0] == 0x7FU);
  assert(bundle.device.devices.controller_io.data[1] == 0U && bundle.device.devices.controller_io.data[2] == 0U);
  { uint32_t i; for (i = 0U; i < 3U; ++i) assert(bundle.device.devices.controller_io.ctrl[i] == 0U); }

  assert(bundle.device.devices.interrupt.vblank_pending == 0U); /* cleared by the second read */
  assert(bundle.device.devices.interrupt.vblank_status_read_count == 2U);
  assert(bundle.device.devices.interrupt.vblank_transition_count == 2U);
  assert(bundle.device.devices.interrupt.checkpoint_entered == 1U);
  assert(bundle.device.devices.interrupt.vblank_transition_count_at_checkpoint_entry == 0U);

  /* RAM evidence: digest of the whole 64 KiB, so just prove it is not the
   * all-zero digest (the RAM write did land) without a second SHA-256. */
  {
    uint8_t all_zero[32];
    memset(all_zero, 0, sizeof(all_zero));
    assert(memcmp(bundle.ram.work_ram_digest, all_zero, sizeof(all_zero)) != 0);
  }

  /* Transaction evidence: exactly the one RAM write this oracle's own
   * fixture-authoring invariant logs (see checkpoint_oracle.c). */
  assert(bundle.transaction.has_full_transactions == 1U);
  assert(bundle.transaction.transaction_count == 1U);
  assert(bundle.transaction.transactions[0].ordinal == 0U);
  assert(bundle.transaction.transactions[0].kind == GENESIS_BUS_DATA_WRITE);
  assert(bundle.transaction.transactions[0].address == UINT32_C(0x00FF0010));
  assert(bundle.transaction.transactions[0].raw_byte_count == 4U);
  {
    static const uint8_t expected[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    assert(memcmp(bundle.transaction.transactions[0].raw_bytes, expected, 4U) == 0);
  }
  assert(bundle.transaction.transactions[0].region == GENESIS_REGION_SYNTHETIC_WORK_RAM);

  /* Frame evidence: register #7 was never written, so the background
   * color-index byte is 0 and the whole frame is uniformly 0; the palette
   * snapshot is a verbatim copy of the same CRAM bytes checked above. */
  {
    uint8_t all_zero_pixels[GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT];
    memset(all_zero_pixels, 0, sizeof(all_zero_pixels));
    assert(memcmp(bundle.frame.pixels, all_zero_pixels, sizeof(all_zero_pixels)) == 0);
  }
  assert(bundle.frame.palette_snapshot[0] == 0x01U);
  assert(bundle.frame.palette_snapshot[1] == 0x23U);
  assert(memcmp(bundle.frame.palette_snapshot, bundle.device.devices.vdp.cram, GENESIS_VDP_CRAM_BYTES) == 0);
}

/* Fixture 2 (determinism): deriving the identical fixture twice must
 * produce byte-identical digests (bundle_digest and every category
 * sub-digest already folded into it). */
static void test_determinism(void) {
  uint8_t rom[8U + sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]) * 2U];
  GenesisCheckpointIdentity identity = make_identity("oracle_determinism", 32U);
  GenesisCheckpointOracleFixture fixture = make_fixture(CHECKPOINT_PC);
  GenesisCheckpointEvidenceBundle first;
  GenesisCheckpointEvidenceBundle second;
  build_rom(rom, sizeof(rom), POSITIVE_WORDS, sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]));

  assert(genesis_checkpoint_oracle_derive_synthetic(rom, sizeof(rom), identity, &fixture, &first) == 0);
  assert(genesis_checkpoint_oracle_derive_synthetic(rom, sizeof(rom), identity, &fixture, &second) == 0);

  assert(memcmp(first.bundle_digest, second.bundle_digest, sizeof(first.bundle_digest)) == 0);
  assert(memcmp(first.ram.work_ram_digest, second.ram.work_ram_digest, sizeof(first.ram.work_ram_digest)) == 0);
  assert(memcmp(first.transaction.transaction_digest, second.transaction.transaction_digest,
                sizeof(first.transaction.transaction_digest)) == 0);
  assert(memcmp(first.frame.frame_digest, second.frame.frame_digest, sizeof(first.frame.frame_digest)) == 0);
  assert(memcmp(&first, &second, sizeof(first)) == 0);
}

/* Fixture 3 (adversarial negative): the checkpoint PC is never reached
 * within the instruction budget (the ROM's own trailing BRA.S self-loop
 * spins harmlessly instead), so derive() must return nonzero and must
 * never write anything into the caller's output bundle. */
static void test_negative(void) {
  uint8_t rom[8U + sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]) * 2U];
  GenesisCheckpointIdentity identity = make_identity("oracle_negative", 64U);
  GenesisCheckpointOracleFixture fixture = make_fixture(UINT32_C(0xFFFFFFFF)); /* unreachable */
  GenesisCheckpointEvidenceBundle bundle;
  GenesisCheckpointEvidenceBundle sentinel;
  build_rom(rom, sizeof(rom), POSITIVE_WORDS, sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]));

  memset(&bundle, 0xA5, sizeof(bundle));
  sentinel = bundle;
  assert(genesis_checkpoint_oracle_derive_synthetic(rom, sizeof(rom), identity, &fixture, &bundle) != 0);
  assert(memcmp(&bundle, &sentinel, sizeof(bundle)) == 0);
}

/* Fixture 4 (divergent input): the same fixture with one byte changed (the
 * VDP register #1 data byte, $04 -> $05) must diverge in the resulting
 * bundle's relevant per-category value/digest, while everything upstream
 * of that write stays identical. */
static void test_divergent(void) {
  uint8_t base_rom[8U + sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]) * 2U];
  uint8_t divergent_rom[8U + sizeof(DIVERGENT_WORDS) / sizeof(DIVERGENT_WORDS[0]) * 2U];
  GenesisCheckpointIdentity identity = make_identity("oracle_divergent", 32U);
  GenesisCheckpointOracleFixture fixture = make_fixture(CHECKPOINT_PC);
  GenesisCheckpointEvidenceBundle base;
  GenesisCheckpointEvidenceBundle divergent;

  build_rom(base_rom, sizeof(base_rom), POSITIVE_WORDS, sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]));
  build_rom(divergent_rom, sizeof(divergent_rom), DIVERGENT_WORDS,
            sizeof(DIVERGENT_WORDS) / sizeof(DIVERGENT_WORDS[0]));
  assert(sizeof(base_rom) == sizeof(divergent_rom));

  assert(genesis_checkpoint_oracle_derive_synthetic(base_rom, sizeof(base_rom), identity, &fixture, &base) == 0);
  assert(genesis_checkpoint_oracle_derive_synthetic(divergent_rom, sizeof(divergent_rom), identity, &fixture, &divergent) == 0);

  assert(base.device.devices.vdp.registers[1] == 0x04U);
  assert(divergent.device.devices.vdp.registers[1] == 0x05U);
  assert(memcmp(base.bundle_digest, divergent.bundle_digest, sizeof(base.bundle_digest)) != 0);
  /* Everything upstream of the changed byte (CPU D0/D1, the RAM write) is
   * unaffected. */
  assert(base.cpu.d[0] == divergent.cpu.d[0] && base.cpu.d[1] == divergent.cpu.d[1]);
  assert(memcmp(base.ram.work_ram_digest, divergent.ram.work_ram_digest, sizeof(base.ram.work_ram_digest)) == 0);
}

/*
 * ================ SEG-007-T132 hardening: fail-closed fault-latch fixtures ================
 * Every fixture below builds a tiny mini-ROM that issues exactly one
 * unsupported bus access, then asserts genesis_checkpoint_oracle_derive_synthetic
 * returns GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS and leaves the
 * output bundle byte-for-byte untouched (the same
 * assert_rejected_without_output_mutation-style sentinel pattern
 * test_negative already uses above). The checkpoint_pc is deliberately
 * unreachable in every case: the fault must stop the loop before the
 * checkpoint condition could ever matter.
 */
static void assert_fault_and_untouched(const uint8_t *rom, size_t rom_len, const char *checkpoint_id,
                                        uint32_t instruction_budget) {
  GenesisCheckpointIdentity identity = make_identity(checkpoint_id, instruction_budget);
  GenesisCheckpointOracleFixture fixture = make_fixture(UINT32_C(0xFFFFFFFF)); /* unreachable */
  GenesisCheckpointEvidenceBundle bundle;
  GenesisCheckpointEvidenceBundle sentinel;
  memset(&bundle, 0xA5, sizeof(bundle));
  sentinel = bundle;
  assert(genesis_checkpoint_oracle_derive_synthetic(rom, rom_len, identity, &fixture, &bundle) ==
         GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
  assert(memcmp(&bundle, &sentinel, sizeof(bundle)) == 0);
}

/* Fixture 5: an unmapped READ address ($00800000 -- not ROM, not RAM, not
 * the one recognised VDP status read) must fault. MOVE.W ($00800000).L,D2. */
static void test_unmapped_read(void) {
  static const uint16_t WORDS[] = {0x3439, 0x0080, 0x0000, 0x60FE};
  uint8_t rom[8U + sizeof(WORDS) / sizeof(WORDS[0]) * 2U];
  build_rom(rom, sizeof(rom), WORDS, sizeof(WORDS) / sizeof(WORDS[0]));
  assert_fault_and_untouched(rom, sizeof(rom), "oracle_unmapped_read", 8U);
}

/* Fixture 6: an unmapped WRITE address ($00800000) must fault.
 * MOVE.W #$0000,($00800000).L. */
static void test_unmapped_write(void) {
  static const uint16_t WORDS[] = {0x33FC, 0x0000, 0x0080, 0x0000, 0x60FE};
  uint8_t rom[8U + sizeof(WORDS) / sizeof(WORDS[0]) * 2U];
  build_rom(rom, sizeof(rom), WORDS, sizeof(WORDS) / sizeof(WORDS[0]));
  assert_fault_and_untouched(rom, sizeof(rom), "oracle_unmapped_write", 8U);
}

/* Fixture 7: a two-word VDP control-port address-set command whose CD5-CD0
 * code is VSRAM WRITE (0x05, GTO1's own documented table -- see
 * docs/references/genesis-vdp-data-port-cpu-write-contract.md), a code this
 * oracle does not implement, must fault after the second word completes the
 * command. First word $4000 (CD1=0,CD0=1), second word $0010
 * (CD5-CD2=0001) -> code = 0b000101 = 0x05. */
static void test_unsupported_vdp_control_code(void) {
  static const uint16_t WORDS[] = {0x33FC, 0x4000, 0x00C0, 0x0004,
                                    0x33FC, 0x0010, 0x00C0, 0x0004, 0x60FE};
  uint8_t rom[8U + sizeof(WORDS) / sizeof(WORDS[0]) * 2U];
  build_rom(rom, sizeof(rom), WORDS, sizeof(WORDS) / sizeof(WORDS[0]));
  assert_fault_and_untouched(rom, sizeof(rom), "oracle_unsupported_vdp_code", 8U);
}

/* Fixture 8: a VDP data-port write with no prior completed address-set
 * command (no pending transfer code) must fault.
 * MOVE.W #$0123,($00C00000).L. */
static void test_vdp_data_write_without_pending_transfer(void) {
  static const uint16_t WORDS[] = {0x33FC, 0x0123, 0x00C0, 0x0000, 0x60FE};
  uint8_t rom[8U + sizeof(WORDS) / sizeof(WORDS[0]) * 2U];
  build_rom(rom, sizeof(rom), WORDS, sizeof(WORDS) / sizeof(WORDS[0]));
  assert_fault_and_untouched(rom, sizeof(rom), "oracle_vdp_data_no_pending", 8U);
}

/* Fixture 9: an unsupported access WIDTH -- a WORD write to the PSG
 * command port ($C00011), which only supports BYTE writes -- must fault.
 * MOVE.W #$00B5,($00C00011).L. */
static void test_unsupported_width_psg_word_write(void) {
  static const uint16_t WORDS[] = {0x33FC, 0x00B5, 0x00C0, 0x0011, 0x60FE};
  uint8_t rom[8U + sizeof(WORDS) / sizeof(WORDS[0]) * 2U];
  build_rom(rom, sizeof(rom), WORDS, sizeof(WORDS) / sizeof(WORDS[0]));
  assert_fault_and_untouched(rom, sizeof(rom), "oracle_unsupported_psg_width", 8U);
}

/* Fixture 10 (adversarial, unsupported display state): identical to the
 * positive fixture's instruction stream except VDP register #1 is set to
 * $44 instead of $04 -- i.e. the same Mode5 bit (bit 2) plus the documented
 * Display Enable bit (bit 6; see oracle_display_state_is_supported's
 * citation) -- so the checkpoint/stable-frame condition is otherwise
 * satisfied exactly as in test_positive, but frame derivation must now
 * refuse instead of emitting a background-only frame as if it were
 * complete. */
static void test_unsupported_display_state(void) {
  static const uint16_t WORDS[] = {
      0x7011, 0x7222, 0x23FC, 0xCAFE, 0xBABE, 0x00FF, 0x0010,
      0x33FC, 0x0100, 0x00A1, 0x1100, 0x33FC, 0x0000, 0x00A1, 0x1200,
      0x33FC, 0x8144, 0x00C0, 0x0004, 0x33FC, 0xC000, 0x00C0, 0x0004,
      0x33FC, 0x0000, 0x00C0, 0x0004, 0x33FC, 0x0123, 0x00C0, 0x0000,
      0x13FC, 0x00B5, 0x00C0, 0x0011, 0x13FC, 0x007F, 0x00A1, 0x0003,
      0x3439, 0x00C0, 0x0004, 0x3639, 0x00C0, 0x0004, 0x60FE};
  uint8_t rom[8U + sizeof(WORDS) / sizeof(WORDS[0]) * 2U];
  GenesisCheckpointIdentity identity = make_identity("oracle_unsupported_display", 32U);
  GenesisCheckpointOracleFixture fixture = make_fixture(CHECKPOINT_PC);
  GenesisCheckpointEvidenceBundle bundle;
  GenesisCheckpointEvidenceBundle sentinel;
  build_rom(rom, sizeof(rom), WORDS, sizeof(WORDS) / sizeof(WORDS[0]));

  memset(&bundle, 0xA5, sizeof(bundle));
  sentinel = bundle;
  assert(genesis_checkpoint_oracle_derive_synthetic(rom, sizeof(rom), identity, &fixture, &bundle) ==
         GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_DISPLAY_STATE);
  assert(memcmp(&bundle, &sentinel, sizeof(bundle)) == 0);
}

/* Fixture 11 (public entry, ADR-0012 Decision 4 boundary proof): the public
 * genesis_checkpoint_oracle_derive can never currently succeed (its
 * classifier always returns GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN), so it
 * must always return GENESIS_CHECKPOINT_ORACLE_ERROR_CHECKPOINT_NOT_REACHED
 * -- never GENESIS_CHECKPOINT_ORACLE_OK -- for a reasonable synthetic
 * ROM/options, and must leave the output bundle completely untouched. */
static void test_public_entry_never_reaches_checkpoint(void) {
  uint8_t rom[8U + sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]) * 2U];
  GenesisCheckpointIdentity identity = make_identity("oracle_public_entry", 64U);
  GenesisDeterministicOptions options = identity.options;
  GenesisCheckpointEvidenceBundle bundle;
  GenesisCheckpointEvidenceBundle sentinel;
  build_rom(rom, sizeof(rom), POSITIVE_WORDS, sizeof(POSITIVE_WORDS) / sizeof(POSITIVE_WORDS[0]));

  memset(&bundle, 0xA5, sizeof(bundle));
  sentinel = bundle;
  assert(genesis_checkpoint_oracle_derive(rom, sizeof(rom), options, identity, &bundle) ==
         GENESIS_CHECKPOINT_ORACLE_ERROR_CHECKPOINT_NOT_REACHED);
  assert(memcmp(&bundle, &sentinel, sizeof(bundle)) == 0);
}

int main(void) {
  test_positive();
  test_determinism();
  test_negative();
  test_divergent();
  test_unmapped_read();
  test_unmapped_write();
  test_unsupported_vdp_control_code();
  test_vdp_data_write_without_pending_transfer();
  test_unsupported_width_psg_word_write();
  test_unsupported_display_state();
  test_public_entry_never_reaches_checkpoint();
  printf("genesis_checkpoint_oracle_fixture_test: 11/11 fixtures passed\n");
  return 0;
}
