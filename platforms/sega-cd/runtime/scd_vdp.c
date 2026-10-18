/* SEG-062 (ADR 0128): the main CPU's VDP ports are served by the existing Genesis VDP device. A zero-initialised GenesisRuntime is
 * held only as the device host; main accesses to the VDP window go through genesis_route_access unchanged. VDP DMA would read the
 * Genesis host's memory, not the Sega CD's, so any register-1 write that enables DMA (M1, bit 4) is the typed stop
 * vdp_dma_unsupported before it reaches the device.
 *
 * SEG-064-T003 (BIOS-image profile only; contract section 16): the same host also serves the main processor's other Genesis devices
 * - the I/O ports, the Z80 bus request / reset lines and the Z80 RAM window, the PSG - and its time follows the machine: before every
 * access the host's master clock is set to the machine time (one master clock = 500,000 ticks), so the VDP status and HV counter
 * are the Genesis runtime's projections of the executed schedule. The vertical-blank onset is a machine device event; the VDP's
 * vertical interrupt is the main processor's level-6 request (GPGX `vint_pending`: set at every onset, requested while register #1
 * IE0 is set, cleared by the acknowledge). The Z80 never executes here: once it has been runnable, any main read of the Z80 area (its
 * RAM, YM2612) is the typed stop main_device_unsupported. What a running Z80 would do on the 68000 side through its bank window (work
 * RAM, VDP/PSG writes) is not modelled and cannot be detected here: a recorded deviation (contract sections 12 and 16). Setting the
 * H-interrupt enable is a typed stop at the write (the line counter is not modelled). */
#include "scd_runtime.h"

#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "../../../libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h"

#define SCD_MASTER_TICK_TICKS UINT64_C(500000) /* ticks per Genesis master clock: the main cycle (3,500,000 ticks) is 7 of them */
#define SCD_FRAME_TICKS ((uint64_t)GENESIS_NTSC_MASTER_TICKS_PER_FRAME * SCD_MASTER_TICK_TICKS)
#define SCD_ONSET_TICKS ((uint64_t)GENESIS_NTSC_VBLANK_ONSET_TICK * SCD_MASTER_TICK_TICKS)

/* The device host and, for the BIOS-image profile, the BIOS image as the host's read-only region (a 68K->VDP DMA source). */
typedef struct ScdVdpHost {
  GenesisRuntime host; /* first member: m->vdp_host points at it */
  GenesisOwnedCartridgeRegion regions[2]; /* [0] the BIOS image; [1] 2M Word RAM as the main processor sees it (SEG-064-T004) */
} ScdVdpHost;

int scd_vdp_init(ScdMachine *m) {
  ScdVdpHost *h = (ScdVdpHost *)calloc(1U, sizeof(ScdVdpHost));
  m->vdp_host = h;
  m->vdp_onset = SCD_ONSET_TICKS;
  if (h == NULL) return 1;
  if (m->bios != NULL) {
    h->regions[0].begin = 0U;
    h->regions[0].end = m->bios_size;
    h->regions[0].data = m->bios;
    h->regions[0].length = m->bios_size;
    h->regions[1].begin = UINT32_C(0x00200000);
    h->regions[1].end = UINT32_C(0x00200000) + SCD_WORD_RAM_BYTES;
    h->regions[1].data = m->word_ram;
    h->regions[1].length = SCD_WORD_RAM_BYTES;
    h->host.owned_regions = h->regions;
    h->host.owned_region_count = 1U;
  }
  return 0;
}

void scd_vdp_release(ScdMachine *m) {
  free(m->vdp_host);
  m->vdp_host = NULL;
}

/* The device host at the machine's current time (BIOS-image profile; the synthetic profile keeps the host's clock at 0 as before). */
static GenesisRuntime *scd_host(ScdMachine *m) {
  GenesisRuntime *host = (GenesisRuntime *)m->vdp_host;
  if (m->bios != NULL) host->scheduler.master_ticks = m->now / SCD_MASTER_TICK_TICKS;
  return host;
}

static int scd_vdp_enables_dma(uint32_t word) { return (word & 0xFF00U) == 0x8100U && (word & 0x10U) != 0U; }

static GenesisAccessWidth scd_genesis_width(uint32_t width) {
  return width == 1U ? GENESIS_ACCESS_BYTE : width == 2U ? GENESIS_ACCESS_WORD : GENESIS_ACCESS_LONG;
}

/* SEG-064-T003 (BIOS-image profile): the control-port word `second`, written while the VDP awaits the second word of a command, arms a
 * 68K->VDP DMA when its CD5 is set, DMA is enabled (register #1 bit 4) and register #23 selects the memory family (bit 7 clear) - the
 * host's own arming rule. The host then reads the source through its own memory map, so the source must first be made the Sega CD's:
 * the BIOS image is the host's read-only region and main work RAM is mirrored into the host's work RAM. 2M Word RAM owned by main is a second
 * read-only region (SEG-064-T004). Any other source (1M or unowned Word RAM and the PRG-RAM window, whose Sega CD DMA timing differs) is the
 * typed stop vdp_dma_unsupported. Fill is the host's VRAM fill; copy
 * stays fail-closed in the host. */
static int scd_vdp_dma_prepare(ScdMachine *m, GenesisRuntime *host, uint16_t second, uint32_t pc) {
  const uint16_t *r = host->devices.vdp.registers;
  uint32_t length, source, end;
  if (!((second >> 7) & 1U) || !(r[1] & 0x10U) || (r[23] & 0x80U)) return 0;
  length = (uint32_t)(r[19] & 0xFFU) | ((uint32_t)(r[20] & 0xFFU) << 8);
  if (length == 0U) length = 0x10000U;
  source = ((uint32_t)(r[23] & 0x7FU) << 17) | ((uint32_t)(r[22] & 0xFFU) << 9) | ((uint32_t)(r[21] & 0xFFU) << 1);
  end = source + 2U * length;
  host->owned_region_count = 1U;
  if (source >= UINT32_C(0x00FF0000) && end <= UINT32_C(0x01000000)) {
    memcpy(host->work_ram, m->work_ram, sizeof host->work_ram);
  } else if (source >= UINT32_C(0x00200000) && end <= UINT32_C(0x00200000) + SCD_WORD_RAM_BYTES && !(m->ga.mode & SCD_MODE_1M) &&
             (m->ga.mode & SCD_MODE_RET)) {
    /* SEG-064-T004: 2M Word RAM owned by main is plain memory for the bus (GPGX reads it through the ordinary word-RAM map); a graphics
     * operation or CDC transfer in flight is brought up to this instant first. */
    if (scd_cd_observe(m) != 0) return 1;
    host->owned_region_count = 2U;
  } else if (!(end <= m->bios_size)) {
    return scd_fail(m, SCD_MAIN, SCD_STOP_VDP_DMA_UNSUPPORTED, pc, source);
  }
  scd_log(m, 0x43U, source, length);
  return 0;
}

/* Classifies a control-port write (a word, or a long as two words high first): 1 when it contains a register-#0 write with IE1 set, 2
 * when it contains a word of the form 101x xxxx outside a pending command - a register write for GPGX (vdp_68k_ctrl_w decodes
 * (w & $C000) == $8000, register (w >> 8) & $1F) but the first word of a command for the Genesis device host: the two disagree, so it is
 * not modelled - and 0 otherwise. A word written while the VDP awaits a command's second word is that second word; otherwise a word
 * 10xx xxxx is a register write and any other word is the first word of a two-word command. */
static int scd_vdp_sets_ie1(int awaiting, uint32_t width, uint32_t value) {
  const uint32_t words[2] = {width == 4U ? value >> 16 : value & 0xFFFFU, value & 0xFFFFU};
  uint32_t i;
  for (i = 0; i < (width == 4U ? 2U : 1U); ++i) {
    if (awaiting) {
      awaiting = 0;
      continue;
    }
    if ((words[i] & 0xC000U) == 0x8000U) {
      if ((words[i] & 0xE000U) == 0xA000U) return 2;
      if ((words[i] & 0xFF10U) == 0x8010U) return 1;
    } else {
      awaiting = 1;
    }
  }
  return 0;
}

int scd_vdp_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  GenesisRuntime *host = scd_host(m);
  GenesisRuntimeStop stop = {0};
  uint32_t routed = is_write ? *value : 0U;
  scd_log(m, is_write ? 0x41U : 0x40U, address, routed);
  if (m->bios != NULL) {
    if (is_write && (address & ~3U) == 0xC00004U && width != 1U) {
      const int awaiting = host->devices.vdp.control_port_awaiting_second_word != 0U;
      /* The H-interrupt: register #0 IE1 (bit 4) would let the line counter (not modelled; GPGX latches it on active lines whether or
       * not IE1 is set) request level 4. Setting it is the typed stop interrupt_source_unsupported at the write, before any line. */
      const int shape = scd_vdp_sets_ie1(awaiting, width, *value);
      if (shape == 1) return scd_fail_access(m, SCD_MAIN, SCD_STOP_INTERRUPT_SOURCE_UNSUPPORTED, pc, address, width, 1);
      if (shape == 2) return scd_fail_access(m, SCD_MAIN, SCD_STOP_VDP_ACCESS_UNSUPPORTED, pc, address, width, 1);
      uint32_t second = UINT32_MAX;
      if (width == 2U && awaiting) second = *value & 0xFFFFU;
      else if (width == 4U && awaiting) second = *value >> 16;
      else if (width == 4U && ((*value >> 16) & 0xC000U) != 0x8000U) second = *value & 0xFFFFU;
      if (second != UINT32_MAX && scd_vdp_dma_prepare(m, host, (uint16_t)second, pc) != 0) return 1;
    }
  } else if (is_write && (address & ~3U) == 0xC00004U &&
             ((width == 2U && scd_vdp_enables_dma(*value)) ||
              (width == 4U && (scd_vdp_enables_dma(*value >> 16) || scd_vdp_enables_dma(*value & 0xFFFFU)))))
    return scd_fail(m, SCD_MAIN, SCD_STOP_VDP_DMA_UNSUPPORTED, pc, *value);
  if (genesis_route_access(host, address, scd_genesis_width(width), is_write ? GENESIS_ACCESS_WRITE : GENESIS_ACCESS_READ, &routed,
                           &stop) != GENESIS_ACCESS_OK)
    return scd_fail_access(m, SCD_MAIN, SCD_STOP_VDP_ACCESS_UNSUPPORTED, pc, address, width, is_write); /* SEG-064-T004: with its width and direction */
  if (!is_write) *value = routed;
  if (m->bios != NULL && !is_write) scd_log(m, 0x42U, address, routed); /* the observed value is part of the device history */
  return 0;
}

/* GPGX's open bus (core/mem68k.c m68k_read_bus_8/16): a read nothing answers returns the instruction-stream word at the processor's
 * prefetch position, its byte selected by A0. The generated instruction states that position (ScdCpu.prefetch) when its operand
 * reads all follow its extension words; otherwise it is 0 and the read is the typed stop open_bus_read, as are reads by the sub, in
 * the synthetic profile, or with a prefetch position that is not plain memory. */
static const uint8_t *scd_stream_byte(const ScdMachine *m, uint32_t address) {
  address &= UINT32_C(0x00FFFFFF);
  if (address < UINT32_C(0x00200000) && (address & UINT32_C(0x3FFFF)) < UINT32_C(0x20000)) {
    const uint32_t a = address & UINT32_C(0x3FFFF);
    return a < m->bios_size ? &m->bios[a] : NULL;
  }
  return scd_code_byte(m, SCD_MAIN, address);
}

static int scd_prefetch_word(const ScdMachine *m, uint32_t *word) {
  const uint32_t at = m->cpu[SCD_MAIN].prefetch;
  const uint8_t *hi, *lo;
  if (m->bios == NULL || at == 0U || (at & 1U) != 0U) return 0;
  hi = scd_stream_byte(m, at);
  lo = scd_stream_byte(m, at + 1U);
  if (hi == NULL || lo == NULL) return 0;
  /* The level-4 vector words of the BIOS ROM read $FFFF and the H-INT vector register (contract section 16). */
  if (at < UINT32_C(0x00200000) && ((at & UINT32_C(0x3FFFF)) == 0x70U || (at & UINT32_C(0x3FFFF)) == 0x72U)) {
    *word = (at & UINT32_C(0x3FFFF)) == 0x70U ? 0xFFFFU : m->hint_vector;
    return 1;
  }
  *word = ((uint32_t)*hi << 8) | *lo;
  return 1;
}

int scd_open_bus(ScdMachine *m, uint32_t cpu, uint32_t address, uint32_t width, uint32_t *value, uint32_t pc) {
  uint32_t word = 0U;
  if (cpu != SCD_MAIN || !scd_prefetch_word(m, &word)) return scd_fail_access(m, cpu, SCD_STOP_OPEN_BUS_READ, pc, address, width, 0);
  *value = width == 1U ? ((address & 1U) ? (word & 0xFFU) : (word >> 8)) : word;
  scd_log(m, 0x52U, address, *value);
  return 0;
}

/* ---- I/O, Z80 and PSG (BIOS-image profile) ---------------------------------------------------------------------------------- */

static void scd_note_z80(ScdMachine *m, const GenesisRuntime *host) {
  if (host->devices.z80_bus.reset_released && !host->devices.z80_bus.bus_requested) m->z80_ran = 1U;
}

int scd_main_device_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  GenesisRuntime *host = scd_host(m);
  GenesisRuntimeStop stop = {0};
  uint32_t routed = is_write ? *value : 0U;
  scd_log(m, is_write ? 0x45U : 0x44U, address, routed);
  if (!is_write && address < UINT32_C(0x00A10000) && m->z80_ran)
    return scd_fail_access(m, SCD_MAIN, SCD_STOP_MAIN_DEVICE_UNSUPPORTED, pc, address, width, 0); /* would observe the Z80 */
  /* Version register: a Mega-CD is attached, so bit 5 (DISK, "expansion unit") reads 0 (GTO1 I/O section; GPGX io_reset sets it only
   * when system_hw != SYSTEM_MCD). The region bits are the host's configured profile, as on the Genesis. */
  if (!is_write && address == UINT32_C(0x00A10001) && width == 1U) {
    *value = segarecomp_genesis_region_version_value(host->region_profile) & 0xDFU;
    scd_log(m, 0x46U, address, *value);
    return 0;
  }
  /* GPGX ctrl_io_read_word: a word read of an I/O register returns its byte on both lanes. */
  if (!is_write && width == 2U && address >= UINT32_C(0x00A10000) && address < UINT32_C(0x00A10020)) {
    uint32_t byte = 0U;
    if (scd_main_device_access(m, address + 1U, 1U, 0, &byte, pc) != 0) return 1;
    *value = (byte << 8) | byte;
    return 0;
  }
  /* Z80 BUSACK: the bit (D0 for a byte at $A11100, D8 for the word) reads 0 iff the bus is granted; GPGX returns the remaining bits
   * from the prefetch (open bus), so do we - with the same fail-closed rule as every open-bus read. */
  if (!is_write && (address & ~UINT32_C(1)) == UINT32_C(0x00A11100) && (width == 2U || address == UINT32_C(0x00A11100))) {
    uint32_t bus = 0U;
    const uint32_t bit = width == 2U ? 0x100U : 0x01U;
    if (scd_open_bus(m, SCD_MAIN, address, width, &bus, pc) != 0) return 1;
    *value = host->devices.z80_bus.bus_granted ? (bus & ~bit) : (bus | bit);
    scd_log(m, 0x46U, address, *value);
    return 0;
  }
  if (genesis_route_access(host, address, scd_genesis_width(width), is_write ? GENESIS_ACCESS_WRITE : GENESIS_ACCESS_READ, &routed,
                           &stop) != GENESIS_ACCESS_OK)
    return scd_fail_access(m, SCD_MAIN, SCD_STOP_MAIN_DEVICE_UNSUPPORTED, pc, address, width, is_write);
  if (is_write) scd_note_z80(m, host);
  else {
    *value = routed;
    scd_log(m, 0x46U, address, routed);
  }
  return 0;
}

/* ---- vertical blank and the main interrupt request ------------------------------------------------------------------------ */

uint64_t scd_vdp_next_event(const ScdMachine *m) { return m->bios != NULL ? m->vdp_onset : UINT64_MAX; }

void scd_vdp_event(ScdMachine *m) {
  GenesisRuntime *host = scd_host(m);
  m->vdp_onset += SCD_FRAME_TICKS;
  m->vdp_frames += 1U;
  m->vint_pending = 1U;
  host->devices.vdp.status_register = (uint16_t)(host->devices.vdp.status_register | 0x0080U); /* F: vertical interrupt occurred */
  scd_log(m, 0x90U, (uint32_t)m->vdp_frames, host->devices.vdp.registers[1]);
}

uint32_t scd_main_request_level(const ScdMachine *m) {
  const GenesisRuntime *host = (const GenesisRuntime *)m->vdp_host;
  if (m->bios == NULL || host == NULL) return 0U;
  return (m->vint_pending && (host->devices.vdp.registers[1] & 0x20U)) ? 6U : 0U;
}

/* The highest level that could still wake a stopped main processor (for the STOP and idle rules). */
uint32_t scd_main_wake_level(const ScdMachine *m) {
  const GenesisRuntime *host = (const GenesisRuntime *)m->vdp_host;
  if (m->bios == NULL || host == NULL) return 0U;
  return (host->devices.vdp.registers[1] & 0x20U) ? 6U : 0U;
}

void scd_main_interrupt_acknowledge(ScdMachine *m, uint32_t level) {
  GenesisRuntime *host = (GenesisRuntime *)m->vdp_host;
  if (level == 6U) {
    m->vint_pending = 0U;
    host->devices.vdp.status_register = (uint16_t)(host->devices.vdp.status_register & ~0x0080U);
  }
}
