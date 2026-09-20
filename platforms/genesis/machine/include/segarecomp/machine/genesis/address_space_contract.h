#ifndef SEGARECOMP_MACHINE_GENESIS_ADDRESS_SPACE_CONTRACT_H
#define SEGARECOMP_MACHINE_GENESIS_ADDRESS_SPACE_CONTRACT_H

#include <stdint.h>

/*
 * C/C++ machine-address policy shared by translation-time Genesis routing and
 * the strict-C11 generated runtime.  This owns the established work-RAM window
 * and the VDP register-window recognition boundary that translation-time
 * routing and the generated runtime must agree on; it is not a generic bus or
 * future-machine abstraction.
 */
#define SEGARECOMP_GENESIS_WORK_RAM_BEGIN UINT32_C(0x00FF0000)
#define SEGARECOMP_GENESIS_WORK_RAM_END UINT32_C(0x01000000)

static inline int segarecomp_genesis_work_ram_contains(uint32_t address,
                                                        uint32_t width) {
  const uint32_t size = SEGARECOMP_GENESIS_WORK_RAM_END -
                        SEGARECOMP_GENESIS_WORK_RAM_BEGIN;
  return width != UINT32_C(0) && width <= size &&
         address >= SEGARECOMP_GENESIS_WORK_RAM_BEGIN &&
         address <= SEGARECOMP_GENESIS_WORK_RAM_END - width;
}

/* VDP register window ($00C00000..$00C00020), the same 0x20-byte convention
 * already used for the controller-I/O window.  One source of truth shared by
 * the translation-time device-routing gate and the generated runtime's VDP
 * lane. */
#define SEGARECOMP_GENESIS_VDP_REGION_BEGIN UINT32_C(0x00C00000)
#define SEGARECOMP_GENESIS_VDP_REGION_END UINT32_C(0x00C00020)

static inline int segarecomp_genesis_vdp_region_contains(uint32_t address,
                                                          uint32_t width) {
  const uint32_t size = SEGARECOMP_GENESIS_VDP_REGION_END -
                        SEGARECOMP_GENESIS_VDP_REGION_BEGIN;
  return width != UINT32_C(0) && width <= size &&
         address >= SEGARECOMP_GENESIS_VDP_REGION_BEGIN &&
         address <= SEGARECOMP_GENESIS_VDP_REGION_END - width;
}

/* SEG-007-T115: the remaining reachable runtime-owned device windows whose
 * genesis_route_access lane already exists (see platforms/genesis/runtime/runtime.c)
 * but whose emission-stage static absolute-operand routing arm is added by
 * this task.  Each interval is one source of truth shared byte-for-byte by
 * the translation-time device-routing gate
 * (m68k_route_genesis_device_access) and the generated runtime's own
 * fail-closed recognition predicate, exactly like the VDP window above.  The
 * translation seam only defers the (address, width, direction) shape the
 * runtime owner already supports; it never models a device register. */

/* 68k-side Z80 bus-arbitration control registers: BUSREQ ($A11100) and RESET
 * ($A11200), covered as one tight interval (GTO1 v1.00 p. 76 SS4 "Z80
 * CONTROL").  Runtime owner: the SEG-007-T102 Z80 bus-arbitration policy owner. */
#define SEGARECOMP_GENESIS_Z80_ARBITRATION_REGION_BEGIN UINT32_C(0x00A11100)
#define SEGARECOMP_GENESIS_Z80_ARBITRATION_REGION_END UINT32_C(0x00A11300)
#define SEGARECOMP_GENESIS_Z80_ARBITRATION_BUSREQ_REGISTER UINT32_C(0x00A11100)
#define SEGARECOMP_GENESIS_Z80_ARBITRATION_RESET_REGISTER UINT32_C(0x00A11200)

static inline int segarecomp_genesis_z80_arbitration_region_contains(uint32_t address) {
  return address >= SEGARECOMP_GENESIS_Z80_ARBITRATION_REGION_BEGIN &&
         address < SEGARECOMP_GENESIS_Z80_ARBITRATION_REGION_END;
}

/* Flat 68000-visible Z80 program-RAM window: 8 KiB at $A00000 (GTO1 v1.00 68K
 * memory map p. 7 / overview p. 2; Charles MacDonald hardware notes SS2).
 * Runtime owner: genesis_z80_ram_window_access (SEG-007-T103), which also
 * realises the byte count as GENESIS_Z80_RAM_BYTES. */
#define SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BEGIN UINT32_C(0x00A00000)
#define SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BYTES UINT32_C(8192)

static inline int segarecomp_genesis_z80_ram_window_contains(uint32_t address) {
  return address >= SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BEGIN &&
         address < SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BEGIN +
                       SEGARECOMP_GENESIS_Z80_RAM_WINDOW_BYTES;
}

/* Co-located PSG (SN76489) audio port: exactly the odd byte $C00011 (GTO1
 * v1.00 p. 10 "VDP AREA": "PSG 76489").  Runtime owner: genesis_psg_access
 * (SEG-007-T109), routed ahead of the VDP lane because the address is inside
 * the VDP interval. */
#define SEGARECOMP_GENESIS_PSG_PORT_ADDRESS UINT32_C(0x00C00011)

static inline int segarecomp_genesis_psg_port_contains(uint32_t address) {
  return address == SEGARECOMP_GENESIS_PSG_PORT_ADDRESS;
}

/* SEG-007-T171: YM2612 FM synthesis chip register window, $A04000-$A04003
 * (GTO1 v1.00 p. 10 "Z80 AREA" / plutiedev.com "ym2612": PART-I address/status
 * port $A04000, PART-I data port $A04001, PART-II address port $A04002,
 * PART-II data port $A04003). This is the full four-port window recognition
 * boundary only; it does not by itself authorize any access shape -- only the
 * runtime-confirmed PART-I status-port BYTE read is currently accepted (see
 * genesis_ym2612_access / m68k_route_genesis_device_access), one source of
 * truth shared byte-for-byte by the translation-time device-routing gate and
 * the generated runtime's own fail-closed recognition predicate, exactly like
 * the PSG port above. */
#define SEGARECOMP_GENESIS_YM2612_REGION_BEGIN UINT32_C(0x00A04000)
#define SEGARECOMP_GENESIS_YM2612_REGION_END UINT32_C(0x00A04004)
#define SEGARECOMP_GENESIS_YM2612_PART1_ADDRESS_PORT UINT32_C(0x00A04000)
#define SEGARECOMP_GENESIS_YM2612_PART1_DATA_PORT UINT32_C(0x00A04001)
#define SEGARECOMP_GENESIS_YM2612_PART2_ADDRESS_PORT UINT32_C(0x00A04002)
#define SEGARECOMP_GENESIS_YM2612_PART2_DATA_PORT UINT32_C(0x00A04003)

static inline int segarecomp_genesis_ym2612_region_contains(uint32_t address) {
  return address >= SEGARECOMP_GENESIS_YM2612_REGION_BEGIN &&
         address < SEGARECOMP_GENESIS_YM2612_REGION_END;
}

/* Route-provenance raw-instruction-bytes buffer capacity (SEG-007-T113 /
 * ADR 0008). The general-startup C4 decoder's longest accepted instruction is
 * 10 bytes: the MC68000 encoding of MOVE.L with two absolute-long operands
 * (or an immediate-long source plus an absolute-long destination) is one
 * operation word plus two source extension words plus two destination
 * extension words. No form the decoder accepts is longer -- indexed
 * (d8(An,Xn)) EAs are out of scope and always rejected. 12 rounds that true
 * maximum up to the next even byte count, giving one 16-bit word of headroom
 * while keeping the fixed provenance arrays small. This is the single source
 * of truth shared byte-for-byte by the translation-time emission constants
 * and the generated-runtime provenance ABI (GENESIS_MAX_RAW_BYTES); any
 * instruction longer than this still fails closed before any state mutation. */
#define SEGARECOMP_GENESIS_MAX_RAW_INSTRUCTION_BYTES UINT32_C(12)

#endif
