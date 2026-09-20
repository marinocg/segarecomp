/*
 * SEG-007-T132: the independent Genesis checkpoint-evidence oracle.
 *
 * ================ construction rationale (ADR-0012 Decision 4) ================
 * CPU and RAM evidence use section 16(a)'s default: this project's already
 * pinned, independently authored, unmodified local Musashi MC68000 core
 * (.tools/musashi at revision 313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd -- the
 * same checkout tests/m68k_batch_b_musashi_differential_test.py already
 * uses), driven only through this file's own thin memory-bus adapter. Device
 * evidence (Z80 bus, VDP, PSG, controller I/O, VBlank/interrupt state) and
 * frame evidence use section 16(b): new, project-authored code, built from
 * scratch from independently-read public hardware documentation, never from
 * this project's own production runtime.c (which this file neither includes
 * nor links -- see tests/genesis_checkpoint_oracle_independence_test.py).
 * This split is required because no independent Genesis device/VDP emulator
 * is already vendored here, and per section 16's "never merely re-render
 * production's own state" rule the full bundle must be derived from the
 * ROM+options input itself, not from anything runtime.c already computed.
 *
 * ================ bounded scope ================
 * This is NOT a Genesis emulator. It implements only:
 *   - Z80 bus-arbitration control lines ($A11100 BUSREQ, $A11200 RESET);
 *   - one VDP control-port command-word write path (register-set, and a
 *     two-word VRAM/CRAM address-set + the corresponding data-port write);
 *   - the VDP status-register ($C00004 read) VBlank-onset latch;
 *   - one PSG (SN76489) command-byte write ($C00011);
 *   - one controller I/O data/ctrl port pair ($A10003 DATA1, $A10009 CTRL1);
 *   - VDP register #7's documented background-color-index selection, used
 *     only to fill a uniform (non-composited) frame buffer.
 * Every other device register, DMA, tile/plane/sprite decode, and Z80
 * program execution is out of scope and left at its zero-initialized
 * default, exactly like the persistent-device-state contract's own reset
 * discipline (section 8).
 *
 * ================ public sources ================
 * Sega, "Genesis Technical Overview" v1.00 (1991) ("GTO1"):
 *   - p. 7 / p. 2: 68K memory map -- ROM at $000000, 64 KB work RAM at
 *     $FF0000-$FFFFFF (mirrored through $E00000-$FFFFFF; this oracle
 *     recognises only the exact $FF0000-$FFFFFF window, the one its own
 *     fixtures use).
 *   - p. 76 SS4 "Z80 CONTROL": BUSREQ ($A11100, D8/D0 write 1 = request, read
 *     0 = CPU has the bus) and RESET ($A11200, D8/D0 write 0 = assert).
 *   - p. 20 "WRITE1: REGISTER SET" / "WRITE2: ADDRESS SET": the VDP
 *     control-port command-word protocol (register-set vs. two-word
 *     address-set with a CD5-CD0 access-mode code), and p. 27's CD5-CD0
 *     table (VRAM WRITE = 0x01, CRAM WRITE = 0x03).
 *   - p. 28: "VRAM address is increased by the value of REGISTER # 15"
 *     after each data-port access.
 *   - p. 72-75: the three GPIO data/control port pairs (DATA1/CTRL1 at
 *     $A10003/$A1000B... this oracle implements only DATA1 $A10003 /
 *     CTRL1 $A10009, its own bounded pair).
 *   - p. 26 "REGISTER #7": background color select -- bits 0-3 select the
 *     color within a CRAM palette line, bits 4-5 select the palette line
 *     (0-3); combined, a 6-bit index 0-63 into the 64-entry CRAM table.
 * plutiedev.com "VDP status register": documents bit 3 (0x0008) as the "F"
 * (frame interrupt pending / VBlank) flag, and that reading the status
 * register is commonly modelled as clearing latched status flags -- this
 * oracle adopts that "read clears" policy explicitly as ITS OWN
 * project-invented fixture-progression convention (see the block comment on
 * oracle_vdp_status_read below), not a verified hardware timing claim.
 * SMS Power "Development/SN76489": the LATCH/DATA command-byte format this
 * oracle's PSG write implements.
 * plutiedev.com "VDP registers", section "$81xx: mode set register #2":
 * documents bit 6 ("DISP") as "1 to enable rendering" -- this oracle's
 * SEG-007-T132 hardening pass display-enable-state predicate (see
 * oracle_display_state_is_supported below).
 *
 * ================ independence ================
 * This file includes ONLY checkpoint_evidence.h (the shared neutral schema)
 * from anywhere under runtime/, src/, or include/; every device/CPU/RAM/
 * transaction/frame computation here is independently written. It never
 * calls genesis_route_access, genesis_extract_checkpoint_evidence, or any
 * other production function, and never links runtime.c or any libs/cpu/m68k/src
 * object. See tests/genesis_checkpoint_oracle_independence_test.py.
 */
#include "checkpoint_oracle_test_support.h"
#include "sha256_oracle.h"

#include "m68k.h"

#include <string.h>

#define ORACLE_RAM_BEGIN UINT32_C(0x00FF0000)
#define ORACLE_RAM_SIZE UINT32_C(0x00010000)
#define ORACLE_VBLANK_STATUS_BIT UINT32_C(0x0008)

typedef struct OracleWorld {
  const uint8_t *rom;
  size_t rom_len;
  uint8_t work_ram[ORACLE_RAM_SIZE];
  GenesisDeviceState devices;
  GenesisBusAccess transactions[GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES];
  uint32_t transaction_count;
  uint64_t total_access_count; /* every logged access ever issued, even past the cap */
  uint64_t ordinal;
  const GenesisCheckpointOracleFixture *fixture; /* NULL for the public/real derive path */
  /* Sticky fail-closed fault latch (SEG-007-T132 hardening pass). Set by any
   * device/memory handler that observes an address, width, or protocol
   * shape it does not recognise; never cleared once set (mirrors
   * production's own sticky `checkpoint_entered` discipline). The step loop
   * checks this immediately after every m68k_execute(0) and stops before
   * evaluating the stable-frame condition or looping again, so no further
   * instruction ever executes against now-untrusted device state and
   * bundle_out is never populated. */
  uint8_t fault;
  int fault_reason;
} OracleWorld;

/* Single-threaded, test-only global oracle instance (mirrors the existing
 * tests/tools/m68k_batch_b_musashi_runner.c pattern of file-scope state
 * feeding Musashi's C memory-interface callbacks, which take no user-data
 * pointer). oracle_derive_common fully re-initializes this before every
 * call, so repeated calls (the determinism fixture) are independent. */
static OracleWorld g_world;

/* Latches the first fault reason only (sticky, first-fault-wins); a later
 * fault of a different shape does not overwrite an already-recorded one,
 * matching the "never clear it once set" discipline the correction request
 * requires. */
static void oracle_fault(int reason) {
  if (!g_world.fault) {
    g_world.fault = 1U;
    g_world.fault_reason = reason;
  }
}

static int oracle_is_rom(uint32_t address, uint32_t width) {
  return (uint64_t)address + width <= g_world.rom_len;
}
static int oracle_is_ram(uint32_t address, uint32_t width) {
  return address >= ORACLE_RAM_BEGIN && (uint64_t)(address - ORACLE_RAM_BEGIN) + width <= ORACLE_RAM_SIZE;
}
static int oracle_is_z80_bus(uint32_t address) {
  return address == UINT32_C(0x00A11100) || address == UINT32_C(0x00A11200);
}
static int oracle_is_vdp(uint32_t address) {
  return address == UINT32_C(0x00C00000) || address == UINT32_C(0x00C00004);
}
static int oracle_is_psg(uint32_t address) { return address == UINT32_C(0x00C00011); }
static int oracle_is_controller_io(uint32_t address) {
  return address == UINT32_C(0x00A10003) || address == UINT32_C(0x00A10009);
}

/* Logs one RAM data-bus transaction into the bounded evidence array,
 * truncating at GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES (contract section
 * 12.4). Device-mapped accesses (Z80 bus/VDP/PSG/controller I/O) are
 * deliberately NOT logged here: the shared GenesisBusRegion enum has only
 * GENESIS_REGION_RAW_CARTRIDGE_ROM and GENESIS_REGION_SYNTHETIC_WORK_RAM
 * members, so this oracle's transaction evidence -- like production's own
 * runtime data bus, which is architecturally separate from its static
 * instruction-fetch path -- covers RAM memory traffic only; device effects
 * are captured instead by the separate GenesisDeviceEvidence snapshot.
 *
 * ROM accesses are never logged either, for a related but distinct reason:
 * this pinned Musashi revision's m68kconf.h documents M68K_SEPARATE_READS
 * (which would route opcode/extension-word fetches through dedicated
 * m68k_read_immediate_* / m68k_read_pcrelative_* hooks, separately from
 * operand data reads) as overridable via `#ifndef`, but turning it on
 * against this exact pinned revision fails to compile (m68kcpu.h's
 * M68K_SEPARATE_READS-guarded code references an undeclared `address`) --
 * a defect in the vendored source this oracle must not patch (ADR-0012
 * Decision 4 requires an UNMODIFIED external core). With
 * M68K_SEPARATE_READS off (the shipped default), every fetch and every
 * operand access reach the same m68k_read_memory_* hooks, so this file
 * classifies by REGION instead: this oracle's own bounded mini-ROM fixtures
 * never read their own code bytes as a data operand, so every ROM-region
 * access is, for these fixtures, necessarily an instruction/extension-word
 * fetch, and every RAM-region access is necessarily an explicit data
 * operand access. That is a fixture-authoring invariant this file's own
 * synthetic mini-ROMs are written to satisfy, not a general Genesis-ROM
 * hardware fact.
 *
 * SEG-007-T132 hardening: this invariant remains valid and sufficient for
 * the internal/synthetic entry point (genesis_checkpoint_oracle_derive_synthetic),
 * where the test author guarantees it by construction for every bundled
 * fixture. It must NEVER be presented as a general property of the public
 * production entry point (genesis_checkpoint_oracle_derive): a real
 * commercial ROM has no such authored guarantee, and this file does not
 * attempt to resolve the underlying fetch/data ambiguity for it (patching
 * the pinned, unmodified vendored Musashi core to fix
 * M68K_SEPARATE_READS's own compile failure is out of this task's bounded
 * scope -- see above). In any case the ambiguity cannot currently manifest
 * in any *successful* public-entry bundle at all: the public entry's
 * checkpoint classifier always returns GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN
 * (see oracle_checkpoint_pc_class_for_target), so `checkpoint_entered` can
 * never become true on that path and the bundle-population code below is
 * provably unreachable from genesis_checkpoint_oracle_derive today. */
static void oracle_log_transaction(GenesisBusKind kind, uint32_t address, const uint8_t *bytes,
                                    uint8_t byte_count, GenesisBusRegion region) {
  ++g_world.total_access_count;
  if (g_world.transaction_count >= GENESIS_MAX_TRANSACTION_EVIDENCE_ENTRIES) return;
  {
    GenesisBusAccess *entry = &g_world.transactions[g_world.transaction_count];
    memset(entry, 0, sizeof(*entry));
    entry->ordinal = g_world.ordinal++;
    entry->kind = kind;
    entry->address = address;
    entry->raw_byte_count = byte_count;
    memcpy(entry->raw_bytes, bytes, byte_count);
    entry->region = region;
  }
  ++g_world.transaction_count;
}

/* ================ device write/read handlers ================ */

/* GTO1 p. 76 SS4: a BYTE access uses D0, a WORD access uses D8, for both the
 * BUSREQ/BUSACK bit at $A11100 and the RESET bit at $A11200. Any other
 * access width (e.g. a LONG access) is not a documented shape for this
 * control line and previously fell through to the same ternary as a BYTE
 * access (silently mistreating width 4 as width 1) -- now an explicit
 * fault instead. */
static void oracle_z80_bus_write(uint32_t address, uint32_t width, uint32_t value) {
  uint32_t bit;
  if (width != 1U && width != 2U) {
    oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
    return;
  }
  bit = (width == 2U) ? UINT32_C(0x0100) : UINT32_C(0x0001);
  if (address == UINT32_C(0x00A11100)) {
    uint8_t requested = (uint8_t)((value & bit) != 0U);
    g_world.devices.z80_bus.bus_requested = requested;
    /* This oracle's own bounded policy (no Z80 core exists to contend for
     * the bus): grant is immediate and tracks the request exactly. */
    g_world.devices.z80_bus.bus_granted = requested;
  } else if (address == UINT32_C(0x00A11200)) {
    g_world.devices.z80_bus.reset_asserted = (uint8_t)((value & bit) == 0U);
  }
}

/* GTO1 p. 20: one-word register-set command (top 3 bits 100, RS4-0 =
 * bits 12-8, data = bits 7-0), or the first/second word of a two-word
 * address-set command (CD1/CD0 in the first word's top 2 bits, CD5-CD2 in
 * the second word's bits 7-4, A15/A14 in the second word's bits 1-0,
 * A13-A0 in the first word's low 14 bits). This oracle accepts only the two
 * CD5-CD0 codes its own fixtures exercise: VRAM WRITE (0x01) and CRAM WRITE
 * (0x03); every other code is out of this oracle's bounded scope (not a
 * hardware claim) and now latches a fault instead of being silently
 * ignored. */
static void oracle_vdp_control_write(uint16_t word) {
  GenesisVdpState *vdp = &g_world.devices.vdp;
  if (vdp->control_port_awaiting_second_word) {
    uint16_t first = vdp->control_port_first_word;
    uint8_t cd1 = (uint8_t)((first >> 15) & 1U);
    uint8_t cd0 = (uint8_t)((first >> 14) & 1U);
    uint8_t cd5 = (uint8_t)((word >> 7) & 1U);
    uint8_t cd4 = (uint8_t)((word >> 6) & 1U);
    uint8_t cd3 = (uint8_t)((word >> 5) & 1U);
    uint8_t cd2 = (uint8_t)((word >> 4) & 1U);
    uint8_t code = (uint8_t)((cd5 << 5) | (cd4 << 4) | (cd3 << 3) | (cd2 << 2) | (cd1 << 1) | cd0);
    uint32_t addr_high2 = (uint32_t)(word & 3U);
    uint32_t addr_low14 = (uint32_t)(first & UINT32_C(0x3FFF));
    vdp->control_port_awaiting_second_word = 0U;
    vdp->control_port_first_word = 0U;
    if (code == UINT8_C(0x01) || code == UINT8_C(0x03)) {
      vdp->addressed_pointer = (addr_high2 << 14) | addr_low14;
      vdp->data_port_transfer_code = code;
      vdp->data_port_transfer_code_valid = 1U;
    } else {
      oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
    }
    return;
  }
  if ((word & UINT32_C(0xE000)) == UINT32_C(0x8000)) {
    uint8_t reg = (uint8_t)((word >> 8) & 0x1FU);
    uint8_t data = (uint8_t)(word & 0xFFU);
    if (reg < GENESIS_VDP_REGISTER_COUNT) vdp->registers[reg] = data;
    return;
  }
  vdp->control_port_first_word = word;
  vdp->control_port_awaiting_second_word = 1U;
}

/* GTO1 p. 20 CD5-CD0 table / p. 28 auto-increment: writes one big-endian
 * 16-bit word to the target (VRAM code 0x01, CRAM code 0x03) selected by
 * the most recently completed address-set command, then advances the
 * current pointer by REGISTER #15 modulo the target's own byte size. */
static void oracle_vdp_data_write(uint16_t word) {
  GenesisVdpState *vdp = &g_world.devices.vdp;
  uint8_t *target;
  uint32_t target_size;
  uint32_t dest;
  if (!vdp->data_port_transfer_code_valid) {
    oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
    return;
  }
  if (vdp->data_port_transfer_code == UINT8_C(0x01)) {
    target = vdp->vram;
    target_size = GENESIS_VDP_VRAM_BYTES;
  } else if (vdp->data_port_transfer_code == UINT8_C(0x03)) {
    target = vdp->cram;
    target_size = GENESIS_VDP_CRAM_BYTES;
  } else {
    /* Unreachable in practice: oracle_vdp_control_write now faults on any
     * CD5-CD0 code other than 0x01/0x03 before ever setting
     * data_port_transfer_code_valid, so this branch can never observe a
     * third code value. Kept as a defense-in-depth fault rather than a
     * silent no-op. */
    oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
    return;
  }
  dest = vdp->addressed_pointer % target_size;
  target[dest] = (uint8_t)(word >> 8);
  target[(dest + 1U) % target_size] = (uint8_t)word;
  vdp->addressed_pointer = (vdp->addressed_pointer + vdp->auto_increment_value) % target_size;
}

/*
 * ================ VDP status-register VBlank-onset read/latch ================
 * This oracle's own project-invented "environment stimulus" convention: the
 * fixture driver (genesis_checkpoint_oracle_derive, not any mini-ROM
 * instruction) asserts ORACLE_VBLANK_STATUS_BIT into status_register
 * immediately before stepping a CPU instruction whose address appears in
 * `fixture->vblank_read_pc`, simulating the display hardware's own
 * documented periodic VBlank assertion (GTO1 p. 4's ~59.92 Hz NTSC / ~50 Hz
 * PAL field rate) without modelling real frame timing. A routed status read
 * that observes the bit set while `vblank_pending` was clear is a rising
 * edge: it increments `vblank_transition_count` and sets `vblank_pending`.
 * This oracle then IMMEDIATELY clears both `status_register`'s asserted bit
 * and `vblank_pending` -- a "read clears the flag" policy independently
 * chosen for this fixture harness (plutiedev.com's VDP status-register page
 * documents several status bits as read-and-clear), so a later fixture
 * pulse can register a genuinely fresh rising edge. This is an explicit,
 * replaceable ORACLE FIXTURE POLICY, not a verified hardware timing claim,
 * and is independent of runtime.c's own (sticky, never-clearing) policy for
 * the same field -- the two are not required to agree, since both are
 * labelled project compatibility choices rather than hardware facts.
 */
static uint32_t oracle_vdp_status_read(void) {
  GenesisInterruptState *interrupt = &g_world.devices.interrupt;
  uint16_t status = g_world.devices.vdp.status_register;
  ++interrupt->vblank_status_read_count;
  if ((status & ORACLE_VBLANK_STATUS_BIT) != 0U && !interrupt->vblank_pending) {
    interrupt->vblank_pending = 1U;
    ++interrupt->vblank_transition_count;
  }
  if ((status & ORACLE_VBLANK_STATUS_BIT) != 0U) {
    g_world.devices.vdp.status_register = (uint16_t)(status & (uint16_t)~ORACLE_VBLANK_STATUS_BIT);
    interrupt->vblank_pending = 0U;
  }
  return status;
}

/* SMS Power "Development/SN76489": LATCH byte %1cctdddd, DATA byte
 * %0-DDDDDD updating the last-latched register. This oracle implements
 * exactly the one write shape its own fixtures use (a LATCH volume byte);
 * a DATA byte with no prior LATCH is a documented remaining no-op (see
 * below); the routing of a wrong-width PSG access is faulted one level up,
 * in oracle_route_write. */
static void oracle_psg_write(uint8_t command) {
  GenesisPsgState *psg = &g_world.devices.psg;
  if ((command & 0x80U) != 0U) {
    uint8_t channel = (uint8_t)((command >> 5) & 0x03U);
    uint8_t is_volume = (uint8_t)((command >> 4) & 0x01U);
    uint8_t data = (uint8_t)(command & 0x0FU);
    psg->latched_channel = channel;
    psg->latched_volume = is_volume;
    psg->latch_valid = 1U;
    if (is_volume) {
      psg->attenuation[channel] = data;
    } else if (channel < 3U) {
      psg->tone_period[channel] = (uint16_t)((psg->tone_period[channel] & UINT16_C(0x03F0)) | data);
    } else {
      psg->noise_control = (uint8_t)(data & 0x07U);
    }
    return;
  }
  /* A DATA byte with no prior LATCH is not one of this hardening pass's
   * enumerated fault cases; left as the existing documented no-op. */
  if (!psg->latch_valid) return;
  {
    uint8_t data6 = (uint8_t)(command & 0x3FU);
    uint8_t channel = psg->latched_channel;
    if (psg->latched_volume) {
      psg->attenuation[channel] = (uint8_t)(data6 & 0x0FU);
    } else if (channel < 3U) {
      psg->tone_period[channel] =
          (uint16_t)((psg->tone_period[channel] & UINT16_C(0x000F)) | ((uint16_t)data6 << 4));
    } else {
      psg->noise_control = (uint8_t)(data6 & 0x07U);
    }
  }
}

/* GTO1 p. 72-75: one GPIO data/control port pair. This oracle implements
 * only DATA1 ($A10003) and CTRL1 ($A10009), the pair its own fixtures use. */
static void oracle_controller_io_write(uint32_t address, uint8_t value) {
  if (address == UINT32_C(0x00A10003)) g_world.devices.controller_io.data[0] = value;
  else if (address == UINT32_C(0x00A10009)) g_world.devices.controller_io.ctrl[0] = value;
}

/* ================ Musashi memory-interface glue ================ */

/* Raw ROM byte fetch, used both by the CPU's own instruction/extension-word
 * fetch hooks below (never logged as a transaction -- see
 * oracle_log_transaction's comment) and by the routed ROM-read path. */
static uint8_t oracle_rom_byte(uint32_t address) {
  return (address < g_world.rom_len) ? g_world.rom[address] : 0U;
}

/* Routed data-bus READ (m68k_read_memory_*, also serving every opcode and
 * extension-word fetch -- see oracle_log_transaction's comment on why this
 * pinned Musashi revision cannot separate the two). RAM is logged as
 * transaction evidence; ROM is served but NOT logged (treated as an
 * instruction/extension-word fetch under this file's fixture-authoring
 * invariant); the bounded device lanes are not logged either (see above).
 * Every other address, and any recognised VDP address read at a width other
 * than the one documented status-register shape, now latches a fault via
 * oracle_fault instead of the previous silent fail-open. The returned `0U`
 * placeholder is still required in the fault case: Musashi's memory-read
 * callback ABI has no failure channel, so callbacks may return a harmless
 * placeholder value when their ABI requires one, but they must record the
 * unsupported condition -- which this file does via the sticky fault latch,
 * checked by the step loop immediately after every m68k_execute(0). */
static uint32_t oracle_route_read(uint32_t address, uint32_t width) {
  if (oracle_is_vdp(address)) {
    if (width == 2U && address == UINT32_C(0x00C00004)) return oracle_vdp_status_read();
    oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
    return 0U;
  }
  if (oracle_is_rom(address, width)) {
    uint32_t i;
    uint32_t value = 0U;
    for (i = 0U; i < width; ++i) value = (value << 8) | oracle_rom_byte(address + i);
    return value;
  }
  if (oracle_is_ram(address, width)) {
    uint8_t bytes[4];
    uint32_t i;
    uint32_t value = 0U;
    for (i = 0U; i < width; ++i) {
      bytes[i] = g_world.work_ram[(address - ORACLE_RAM_BEGIN + i) % ORACLE_RAM_SIZE];
      value = (value << 8) | bytes[i];
    }
    oracle_log_transaction(GENESIS_BUS_DATA_READ, address, bytes, (uint8_t)width, GENESIS_REGION_SYNTHETIC_WORK_RAM);
    return value;
  }
  oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
  return 0U;
}

/* Routed data-bus WRITE (m68k_write_memory_*). ROM writes are rejected
 * outright (never applied, never logged) -- this oracle's ROM is read-only
 * cartridge storage, matching the real hardware's own read-only cartridge
 * bus (a ROM write itself is not treated as a fault: it is a documented
 * read-only-bus rejection, not an unrecognised address/width shape). Every
 * recognised device address written at an unsupported width, and every
 * other address this oracle does not recognise at all, now latches a fault
 * via oracle_fault instead of the previous silent no-op. */
static void oracle_route_write(uint32_t address, uint32_t width, uint32_t value) {
  if (oracle_is_z80_bus(address)) { oracle_z80_bus_write(address, width, value); return; }
  if (oracle_is_vdp(address)) {
    if (width != 2U) {
      oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
      return;
    }
    if (address == UINT32_C(0x00C00004)) oracle_vdp_control_write((uint16_t)value);
    else oracle_vdp_data_write((uint16_t)value);
    return;
  }
  if (oracle_is_psg(address)) {
    if (width != 1U) {
      oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
      return;
    }
    oracle_psg_write((uint8_t)value);
    return;
  }
  if (oracle_is_controller_io(address)) {
    if (width != 1U) {
      oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
      return;
    }
    oracle_controller_io_write(address, (uint8_t)value);
    return;
  }
  if (oracle_is_rom(address, width)) {
    /* Read-only cartridge bus: rejected, but not a fault (see above). */
    return;
  }
  if (oracle_is_ram(address, width)) {
    uint8_t bytes[4];
    uint32_t i;
    for (i = 0U; i < width; ++i) {
      uint8_t b = (uint8_t)(value >> ((width - 1U - i) * 8U));
      g_world.work_ram[(address - ORACLE_RAM_BEGIN + i) % ORACLE_RAM_SIZE] = b;
      bytes[i] = b;
    }
    oracle_log_transaction(GENESIS_BUS_DATA_WRITE, address, bytes, (uint8_t)width, GENESIS_REGION_SYNTHETIC_WORK_RAM);
    return;
  }
  oracle_fault(GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_ACCESS);
}

unsigned int m68k_read_memory_8(unsigned int address) { return oracle_route_read(address, 1U); }
unsigned int m68k_read_memory_16(unsigned int address) { return oracle_route_read(address, 2U); }
unsigned int m68k_read_memory_32(unsigned int address) { return oracle_route_read(address, 4U); }
void m68k_write_memory_8(unsigned int address, unsigned int value) { oracle_route_write(address, 1U, value); }
void m68k_write_memory_16(unsigned int address, unsigned int value) { oracle_route_write(address, 2U, value); }
void m68k_write_memory_32(unsigned int address, unsigned int value) { oracle_route_write(address, 4U, value); }

/* Instruction opcode/extension-word and disassembler fetches. With this
 * pinned Musashi revision's shipped M68K_SEPARATE_READS=OFF default (see
 * oracle_log_transaction's comment), these hooks are never actually called
 * -- every fetch instead reaches m68k_read_memory_16/32 above -- but they
 * are still defined, unmodified in behavior from oracle_route_read's own
 * ROM-serving branch, so this file keeps working unchanged if a future,
 * non-broken pinned revision enables real fetch/data separation. */
static uint32_t oracle_program_read(uint32_t address, uint32_t width) {
  uint32_t value = 0U;
  uint32_t i;
  for (i = 0U; i < width; ++i) value = (value << 8) | oracle_rom_byte(address + i);
  return value;
}
unsigned int m68k_read_immediate_16(unsigned int address) { return oracle_program_read(address, 2U); }
unsigned int m68k_read_immediate_32(unsigned int address) { return oracle_program_read(address, 4U); }
unsigned int m68k_read_pcrelative_8(unsigned int address) { return oracle_program_read(address, 1U); }
unsigned int m68k_read_pcrelative_16(unsigned int address) { return oracle_program_read(address, 2U); }
unsigned int m68k_read_pcrelative_32(unsigned int address) { return oracle_program_read(address, 4U); }
unsigned int m68k_read_disassembler_8(unsigned int address) { return oracle_program_read(address, 1U); }
unsigned int m68k_read_disassembler_16(unsigned int address) { return oracle_program_read(address, 2U); }
unsigned int m68k_read_disassembler_32(unsigned int address) { return oracle_program_read(address, 4U); }

/* ================ section 14 canonical serialization (independent) ================
 * Re-implements the shared field-order/big-endian/enum-wire-code/self-
 * exclusion serialization rule from scratch for this oracle, in its own
 * SHA-256 (sha256_oracle.h/.c). It must follow the same *rule* as
 * runtime.c's genesis_sha_* family (so a future T053 comparison is
 * meaningful) but shares no code, header, or object with it. */
static void oracle_sha_options(OracleSha256 *state, const GenesisDeterministicOptions *options) {
  oracle_sha256_put_u32(state, options->schema_version);
  oracle_sha256_put_u32(state, options->instruction_budget);
  oracle_sha256_put_u32(state, options->stable_frame_vblank_count);
}
static void oracle_sha_bus_access(OracleSha256 *state, const GenesisBusAccess *access) {
  oracle_sha256_put_u64(state, access->ordinal);
  oracle_sha256_put_u8(state, (uint8_t)access->kind);
  oracle_sha256_put_u32(state, access->address);
  oracle_sha256_put_u8(state, access->raw_byte_count);
  oracle_sha256_update(state, access->raw_bytes, access->raw_byte_count);
  oracle_sha256_put_u8(state, (uint8_t)access->region);
}
static void oracle_sha_device(OracleSha256 *state, const GenesisDeviceState *devices) {
  uint32_t i;
  oracle_sha256_put_u8(state, devices->z80_bus.bus_requested);
  oracle_sha256_put_u8(state, devices->z80_bus.bus_granted);
  oracle_sha256_put_u8(state, devices->z80_bus.reset_asserted);
  oracle_sha256_update(state, devices->z80_bus.z80_ram, GENESIS_Z80_RAM_BYTES);
  for (i = 0U; i < GENESIS_VDP_REGISTER_COUNT; ++i) oracle_sha256_put_u16(state, devices->vdp.registers[i]);
  oracle_sha256_put_u8(state, devices->vdp.control_port_awaiting_second_word);
  oracle_sha256_put_u16(state, devices->vdp.control_port_first_word);
  oracle_sha256_put_u32(state, devices->vdp.addressed_pointer);
  oracle_sha256_put_u16(state, devices->vdp.auto_increment_value);
  oracle_sha256_put_u16(state, devices->vdp.status_register);
  oracle_sha256_put_u8(state, devices->vdp.data_port_transfer_code);
  oracle_sha256_put_u8(state, devices->vdp.data_port_transfer_code_valid);
  oracle_sha256_update(state, devices->vdp.vram, GENESIS_VDP_VRAM_BYTES);
  oracle_sha256_update(state, devices->vdp.cram, GENESIS_VDP_CRAM_BYTES);
  oracle_sha256_update(state, devices->vdp.vsram, GENESIS_VDP_VSRAM_BYTES);
  oracle_sha256_put_u8(state, (uint8_t)devices->vdp.dma.phase);
  oracle_sha256_put_u8(state, (uint8_t)devices->vdp.dma.kind);
  oracle_sha256_put_u32(state, devices->vdp.dma.source_address);
  oracle_sha256_put_u32(state, devices->vdp.dma.remaining_length);
  oracle_sha256_put_u32(state, devices->vdp.dma.fill_byte_count);
  oracle_sha256_put_u32(state, devices->vdp.dma.transfer_access_count);
  oracle_sha256_put_u8(state, devices->vdp.dma.write_target_code);
  oracle_sha256_put_u8(state, devices->psg.latched_channel);
  oracle_sha256_put_u8(state, devices->psg.latched_volume);
  oracle_sha256_put_u8(state, devices->psg.latch_valid);
  for (i = 0U; i < 3U; ++i) oracle_sha256_put_u16(state, devices->psg.tone_period[i]);
  oracle_sha256_update(state, devices->psg.attenuation, 4U);
  oracle_sha256_put_u8(state, devices->psg.noise_control);
  oracle_sha256_update(state, devices->controller_io.data, 3U);
  oracle_sha256_update(state, devices->controller_io.ctrl, 3U);
  oracle_sha256_put_u8(state, devices->interrupt.vblank_pending);
  oracle_sha256_put_u32(state, devices->interrupt.vblank_status_read_count);
  oracle_sha256_put_u32(state, devices->interrupt.vblank_transition_count);
  oracle_sha256_put_u8(state, devices->interrupt.checkpoint_entered);
  oracle_sha256_put_u32(state, devices->interrupt.vblank_transition_count_at_checkpoint_entry);
}

/*
 * ================ frame evidence (bounded) ================
 * GTO1 p. 26 REGISTER #7: bits 0-3 select the color within a CRAM palette
 * line, bits 4-5 select the palette line (0-3); the combined 6-bit value is
 * this oracle's "CRAM palette index" byte. This oracle implements ONLY that
 * background-color selection -- no tile/plane/sprite decode or compositing
 * -- and fills the whole 320x224 buffer with that single index byte, a
 * real (if minimal) subset of true VDP composition: a blank display showing
 * only the background color.
 */
static void oracle_fill_frame(GenesisFrameArtifact *frame, const GenesisVdpState *vdp) {
  uint8_t index = (uint8_t)(vdp->registers[7] & 0x3FU);
  memset(frame->pixels, index, sizeof(frame->pixels));
  memcpy(frame->palette_snapshot, vdp->cram, sizeof(frame->palette_snapshot));
}

/* SEG-007-T132 hardening: register #1 ("mode set register #2" in Sega's own
 * numbering; VDP register array index 1) bit 6 is the documented Display
 * Enable bit ("DISP"). Source: plutiedev.com, "VDP registers" reference
 * page, section "$81xx: mode set register #2" -- its bit table documents
 * bit 6 as `DISP`, described as "1 to enable rendering" (i.e. 0 = display
 * output blanked to the backdrop/background color across the active area,
 * not composited from planes/sprites; 1 = normal plane/sprite composition).
 * Cross-checked independently against this repository's own already-cited
 * GTO1 p. 36-38 DMA-enable-bit citation for the same register index
 * (docs/references/genesis-vdp-dma-contract.md), which corroborates that
 * register array index 1 is the register controlling DMA enable and (per
 * plutiedev) display enable together in one byte.
 *
 * This oracle implements ONLY the DISP=0 (display disabled, background-only)
 * subset of real VDP composition (oracle_fill_frame above); it does not
 * independently implement plane/sprite/tile decode or compositing. When
 * DISP=1 at the checkpoint, this oracle's background-only fill would NOT be
 * a complete, correct frame, so this predicate is checked before any frame
 * evidence -- or any other bundle field -- is populated, and the derive
 * entry returns GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_DISPLAY_STATE
 * instead of silently emitting a background-only frame as if it were
 * complete. */
#define ORACLE_VDP_MODE_REGISTER_2_INDEX 1U
#define ORACLE_VDP_DISPLAY_ENABLE_BIT UINT16_C(0x0040)
static int oracle_display_state_is_supported(const GenesisVdpState *vdp) {
  return (vdp->registers[ORACLE_VDP_MODE_REGISTER_2_INDEX] & ORACLE_VDP_DISPLAY_ENABLE_BIT) == 0U;
}

/*
 * ================ checkpoint classification (public entry only) ================
 * Mirrors runtime.c's own genesis_checkpoint_pc_class_for_target exactly:
 * GenesisCheckpointPcClass has no non-UNKNOWN enumerator yet (T131/T132
 * bound the sticky mechanism, not a real target), so this oracle's real
 * (non-test) classifier always returns UNKNOWN, honestly reflecting the same
 * unresolved fact production itself has not resolved. This is used ONLY by
 * the public genesis_checkpoint_oracle_derive path; the internal/synthetic
 * path below still drives checkpoint-entry detection directly off its own
 * fixture->checkpoint_pc, exactly as it always has. Unlike runtime.c's
 * version, this function has no test-only classify-override macro: the
 * fixture-pointer mechanism is this oracle's own internal test seam, so no
 * second seam is needed here.
 */
static GenesisCheckpointPcClass oracle_checkpoint_pc_class_for_target(uint32_t target) {
  (void)target;
  return GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN;
}

/*
 * Shared step-loop/extraction engine behind both
 * genesis_checkpoint_oracle_derive (fixture == NULL, real classifier) and
 * genesis_checkpoint_oracle_derive_synthetic (fixture != NULL, fixture's own
 * checkpoint_pc/vblank_read_pc stimulus). See checkpoint_oracle.h and
 * checkpoint_oracle_test_support.h for the full per-entry-point contract.
 */
static int oracle_derive_common(const uint8_t *rom_image, size_t rom_len,
                                 GenesisCheckpointIdentity identity,
                                 const GenesisCheckpointOracleFixture *fixture,
                                 GenesisCheckpointEvidenceBundle *bundle_out) {
  uint32_t step;
  GenesisCheckpointEvidenceBundle bundle;
  OracleSha256 digest;
  uint32_t index;

  if (rom_image == NULL || bundle_out == NULL || rom_len == 0U ||
      rom_len > UINT32_C(0x00400000) || identity.options.schema_version != GENESIS_DETERMINISTIC_OPTIONS_SCHEMA_VERSION ||
      identity.options.instruction_budget == 0U || identity.options.stable_frame_vblank_count == 0U ||
      (fixture != NULL && fixture->vblank_read_pc_count > 8U))
    return GENESIS_CHECKPOINT_ORACLE_ERROR_INVALID_ARGUMENT;

  memset(&g_world, 0, sizeof(g_world));
  g_world.rom = rom_image;
  g_world.rom_len = rom_len;
  g_world.fixture = fixture;

  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  m68k_pulse_reset();

  for (step = 0U; step < identity.options.instruction_budget; ++step) {
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
    int pc_is_checkpoint = (fixture != NULL)
        ? (pc == fixture->checkpoint_pc)
        : (oracle_checkpoint_pc_class_for_target(pc) != GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN);
    if (!g_world.devices.interrupt.checkpoint_entered && pc_is_checkpoint) {
      g_world.devices.interrupt.checkpoint_entered = 1U;
      g_world.devices.interrupt.vblank_transition_count_at_checkpoint_entry =
          g_world.devices.interrupt.vblank_transition_count;
    }
    if (g_world.devices.interrupt.checkpoint_entered && fixture != NULL) {
      uint32_t i;
      for (i = 0U; i < fixture->vblank_read_pc_count; ++i) {
        if (pc == fixture->vblank_read_pc[i]) { g_world.devices.vdp.status_register |= (uint16_t)ORACLE_VBLANK_STATUS_BIT; break; }
      }
    }
    (void)m68k_execute(0); /* always executes exactly one instruction (see
                               tests/tools/m68k_batch_b_musashi_runner.c's
                               identical validated precedent) */
    if (g_world.fault) {
      /* Fail closed immediately: do not evaluate the stable-frame condition
       * or execute a further instruction against now-untrusted device
       * state. bundle_out has not been touched at all yet. */
      return g_world.fault_reason;
    }
    if (g_world.devices.interrupt.checkpoint_entered &&
        g_world.devices.interrupt.vblank_transition_count -
                g_world.devices.interrupt.vblank_transition_count_at_checkpoint_entry >=
            identity.options.stable_frame_vblank_count) {
      goto stable;
    }
  }
  return GENESIS_CHECKPOINT_ORACLE_ERROR_CHECKPOINT_NOT_REACHED; /* instruction budget exhausted
                                                                     without satisfying the
                                                                     stable-frame condition */

stable:
  /* Checked before any bundle field is written (see
   * oracle_display_state_is_supported's comment): if unsupported,
   * bundle_out remains completely untouched, exactly like every other
   * failure path. */
  if (!oracle_display_state_is_supported(&g_world.devices.vdp))
    return GENESIS_CHECKPOINT_ORACLE_ERROR_UNSUPPORTED_DISPLAY_STATE;

  memset(&bundle, 0, sizeof(bundle));
  bundle.schema_version = GENESIS_CHECKPOINT_EVIDENCE_SCHEMA_VERSION;
  bundle.identity = identity;
  oracle_sha256_init(&digest);
  oracle_sha_options(&digest, &bundle.identity.options);
  oracle_sha256_final(&digest, bundle.identity.options_digest);

  for (index = 0U; index < 8U; ++index) {
    bundle.cpu.d[index] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + index));
    bundle.cpu.a[index] = m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + index));
  }
  bundle.cpu.sr = (uint16_t)(m68k_get_reg(NULL, M68K_REG_SR) & 0xFFFFU);
  bundle.cpu.pc_class = GENESIS_CHECKPOINT_PC_CLASS_UNKNOWN;

  oracle_sha256_init(&digest);
  oracle_sha256_update(&digest, g_world.work_ram, sizeof(g_world.work_ram));
  oracle_sha256_final(&digest, bundle.ram.work_ram_digest);

  bundle.device.devices = g_world.devices;

  bundle.transaction.has_full_transactions = (uint8_t)(g_world.total_access_count == g_world.transaction_count);
  bundle.transaction.transaction_count = (uint16_t)g_world.transaction_count;
  memcpy(bundle.transaction.transactions, g_world.transactions,
         sizeof(bundle.transaction.transactions[0]) * g_world.transaction_count);
  oracle_sha256_init(&digest);
  for (index = 0U; index < bundle.transaction.transaction_count; ++index)
    oracle_sha_bus_access(&digest, &bundle.transaction.transactions[index]);
  oracle_sha256_final(&digest, bundle.transaction.transaction_digest);

  oracle_fill_frame(&bundle.frame, &g_world.devices.vdp);
  oracle_sha256_init(&digest);
  oracle_sha256_update(&digest, bundle.frame.pixels, sizeof(bundle.frame.pixels));
  oracle_sha256_update(&digest, bundle.frame.palette_snapshot, sizeof(bundle.frame.palette_snapshot));
  oracle_sha256_final(&digest, bundle.frame.frame_digest);

  oracle_sha256_init(&digest);
  oracle_sha256_put_u32(&digest, bundle.schema_version);
  oracle_sha256_update(&digest, (const uint8_t *)bundle.identity.checkpoint_id, bundle.identity.checkpoint_id_length);
  oracle_sha256_put_u8(&digest, bundle.identity.checkpoint_id_length);
  oracle_sha256_update(&digest, (const uint8_t *)bundle.identity.rom_sha256, 64U);
  oracle_sha_options(&digest, &bundle.identity.options);
  oracle_sha256_update(&digest, bundle.identity.options_digest, 32U);
  for (index = 0U; index < 8U; ++index) oracle_sha256_put_u32(&digest, bundle.cpu.d[index]);
  for (index = 0U; index < 8U; ++index) oracle_sha256_put_u32(&digest, bundle.cpu.a[index]);
  oracle_sha256_put_u16(&digest, bundle.cpu.sr);
  oracle_sha256_put_u8(&digest, (uint8_t)bundle.cpu.pc_class);
  oracle_sha256_update(&digest, bundle.ram.work_ram_digest, 32U);
  oracle_sha_device(&digest, &bundle.device.devices);
  oracle_sha256_put_u8(&digest, bundle.transaction.has_full_transactions);
  oracle_sha256_put_u16(&digest, bundle.transaction.transaction_count);
  if (bundle.transaction.has_full_transactions)
    for (index = 0U; index < bundle.transaction.transaction_count; ++index)
      oracle_sha_bus_access(&digest, &bundle.transaction.transactions[index]);
  oracle_sha256_update(&digest, bundle.transaction.transaction_digest, 32U);
  oracle_sha256_update(&digest, bundle.frame.pixels, sizeof(bundle.frame.pixels));
  oracle_sha256_update(&digest, bundle.frame.palette_snapshot, sizeof(bundle.frame.palette_snapshot));
  oracle_sha256_update(&digest, bundle.frame.frame_digest, 32U);
  oracle_sha256_final(&digest, bundle.bundle_digest);

  *bundle_out = bundle;
  return GENESIS_CHECKPOINT_ORACLE_OK;
}

/*
 * ADR-0012 Decision 4 public entry point (checkpoint_oracle.h). `options`
 * and `identity.options` are required to already agree byte-for-byte;
 * disagreement is treated as a caller bug and fails closed rather than
 * silently preferring one copy.
 */
int genesis_checkpoint_oracle_derive(const uint8_t *rom_image, size_t rom_len,
                                      GenesisDeterministicOptions options,
                                      GenesisCheckpointIdentity identity,
                                      GenesisCheckpointEvidenceBundle *bundle_out) {
  if (options.schema_version != identity.options.schema_version ||
      options.instruction_budget != identity.options.instruction_budget ||
      options.stable_frame_vblank_count != identity.options.stable_frame_vblank_count)
    return GENESIS_CHECKPOINT_ORACLE_ERROR_INVALID_ARGUMENT;
  return oracle_derive_common(rom_image, rom_len, identity, NULL, bundle_out);
}

/*
 * Internal/synthetic test-only entry point
 * (checkpoint_oracle_test_support.h), used only by
 * tests/tools/genesis_checkpoint_oracle_fixture_test.c.
 */
int genesis_checkpoint_oracle_derive_synthetic(const uint8_t *rom_image, size_t rom_len,
                                                GenesisCheckpointIdentity identity,
                                                const GenesisCheckpointOracleFixture *fixture,
                                                GenesisCheckpointEvidenceBundle *bundle_out) {
  if (fixture == NULL) return GENESIS_CHECKPOINT_ORACLE_ERROR_INVALID_ARGUMENT;
  return oracle_derive_common(rom_image, rom_len, identity, fixture, bundle_out);
}
