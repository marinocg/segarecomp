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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "checkpoint_evidence.h"
#include "vdp_render.h"
#include "../../../libs/device/sega/genesis/include/segarecomp/device/sega/genesis/controller_io_contract.h"

#define SCD_MASTER_TICK_TICKS UINT64_C(500000) /* ticks per Genesis master clock: the main cycle (3,500,000 ticks) is 7 of them */
#define SCD_FRAME_TICKS ((uint64_t)GENESIS_NTSC_MASTER_TICKS_PER_FRAME * SCD_MASTER_TICK_TICKS)
#define SCD_ONSET_TICKS ((uint64_t)GENESIS_NTSC_VBLANK_ONSET_TICK * SCD_MASTER_TICK_TICKS)

#define SCD_LINE_TICKS ((uint64_t)GENESIS_NTSC_MASTER_TICKS_PER_LINE * SCD_MASTER_TICK_TICKS)
#define SCD_CAPTURE_MAX 8U

/* SEG-065-T003 (ADR 0147): one requested frame capture - the frame whose active display ends at vertical-blank onset `onset`. */
typedef struct ScdFrameCapture {
  uint64_t onset;          /* 1-based onset count (m->vdp_frames after the onset event) */
  uint32_t status;         /* 0 pending, 1 written, 2 unsupported VDP state, 3 render failed, 4 output failed, 5 incomplete lines */
  uint32_t lines;          /* active lines captured */
  /* Endpoint comparison only: the number of VRAM bytes that differ between the snapshot taken when line 0 begins and VRAM at the
   * vertical-blank onset. Zero means those two images are byte-identical; it is not a write trace (writes that restore the original
   * byte, or transient changes that do not survive to the onset, are not counted). The name is kept for report compatibility. */
  uint32_t vram_display_changes;
  uint16_t width, height;
  char sha256[65];         /* digest of the canonical frame file */
} ScdFrameCapture;

/* The device host and, for the BIOS-image profile, the BIOS image as the host's read-only region (a 68K->VDP DMA source). */
typedef struct ScdVdpHost {
  GenesisRuntime host; /* first member: m->vdp_host points at it */
  GenesisOwnedCartridgeRegion regions[2]; /* [0] the BIOS image; [1] 2M Word RAM as the main processor sees it (SEG-064-T004) */
  /* SEG-065-T003: frame capture (a pure observer: line instants are extra device-event boundaries, no guest state changes) */
  const char *capture_dir;
  uint32_t capture_count;
  ScdFrameCapture captures[SCD_CAPTURE_MAX];
  int32_t capture_line;        /* next active line to sample in the frame being captured, or -1 */
  uint32_t capture_lines_done; /* active lines sampled for the frame being captured */
  uint64_t capture_line_tick;
  GenesisRasterFrameState raster;
  uint8_t vram_line0[GENESIS_VDP_VRAM_BYTES];
  GenesisFrameArtifact frame;
  uint8_t dma_armed;           /* SEG-065-T002: the access being routed performs a 68K->VDP DMA */
  /* SEG-071-T002 (ADR 0177): the reference's cycle-accurate VDP port timing (core/vdp_ctrl.c), BIOS-image profile: the data-port write
   * FIFO (per frame, in master clocks since the frame's origin, the onset) and the end of a VRAM fill (the DMA-busy status bit). */
  uint32_t fifo_cycles[4];
  uint32_t fifo_idx;
  uint64_t fill_end;           /* tick at which the last fill ends (0: none in flight) */
} ScdVdpHost;

int scd_vdp_init(ScdMachine *m) {
  ScdVdpHost *h = (ScdVdpHost *)calloc(1U, sizeof(ScdVdpHost));
  m->vdp_host = h;
  m->vdp_onset = m->bios != NULL ? SCD_BIOS_FIRST_ONSET_MCLK * SCD_MASTER_TICK_TICKS : SCD_ONSET_TICKS;
  if (h == NULL) return 1;
  h->capture_line = -1; /* SEG-065-T003: no line sampling until a requested frame begins */
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

/* SEG-070-T002: a machine-input pad transition reaches the Genesis controller pin model through the host's pad masks. */
void scd_vdp_set_pad(ScdMachine *m, uint32_t port, uint8_t mask) {
  GenesisRuntime *host = (GenesisRuntime *)m->vdp_host;
  if (host == NULL) return;
  if (port == 1U) genesis_runtime_set_pad1(host, mask);
  else if (port == 2U) genesis_runtime_set_pad2(host, mask);
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
/* SEG-065-T002 (ADR 0147): the end tick of a 68K->VDP DMA of `words` words starting at tick `start`, by GPGX's slot model
 * (core/vdp_ctrl.c vdp_dma_update with system.c system_frame_scd): per line 16 (H32) / 18 (H40) access slots in the active display
 * and 166 / 204 while blanked (vertical blank or display disabled); a word to VRAM takes two slots; to CRAM or VSRAM one, and a
 * blanked line then loses its refresh slots (161 / 198). The frame starts at the vertical-blank onset; the blank window lasts until
 * the start of the frame's last line (37 lines); in the active part a transfer runs to the end of the current line and resumes at
 * each line start. Integer arithmetic in master clocks, exactly as GPGX. */
static uint64_t scd_vdp_dma_end(const ScdMachine *m, const GenesisRuntime *host, uint64_t start, uint32_t words, int to_vram,
                                uint32_t *windows) {
  static const uint32_t timing[2][2] = {{16U, 18U}, {166U, 204U}};
  const uint32_t line = GENESIS_NTSC_MASTER_TICKS_PER_LINE, frame_lines = GENESIS_NTSC_LINES_PER_FRAME;
  const uint32_t blank_lines = GENESIS_NTSC_LINES_PER_FRAME - GENESIS_NTSC_VBLANK_ONSET_LINE - 1U; /* 37 */
  const uint16_t *r = host->devices.vdp.registers;
  const uint32_t h40 = r[12] & 1U, display = (r[1] & 0x40U) != 0U;
  const uint64_t frame_start = m->vdp_onset - SCD_FRAME_TICKS; /* the last onset: GPGX's frame origin */
  uint64_t cyc = (start - frame_start) / SCD_MASTER_TICK_TICKS, frames = 0U;
  uint32_t remaining = words;
  *windows = 0U;
  for (;;) {
    const uint32_t l = (uint32_t)(cyc / line);
    const int vblank = l < blank_lines;
    const int blanked = vblank || !display;
    uint32_t rate = timing[blanked][h40] >> (to_vram ? 1U : 0U);
    uint64_t window_end, cap;
    if (!to_vram && rate == 166U) rate = 161U;
    else if (!to_vram && rate == 204U) rate = 198U;
    window_end = vblank ? (uint64_t)blank_lines * line : (uint64_t)(l + 1U) * line;
    cap = ((window_end - cyc) * rate) / line;
    *windows += 1U;
    if (remaining < cap) {
      cyc += ((uint64_t)remaining * line) / rate;
      break;
    }
    remaining -= (uint32_t)cap;
    cyc = window_end;
    if (remaining == 0U) break;
    if (cyc >= (uint64_t)frame_lines * line) { /* on into the next frame's vertical blank */
      cyc -= (uint64_t)frame_lines * line;
      frames += 1U;
    }
  }
  return frame_start + (frames * frame_lines * line + cyc) * SCD_MASTER_TICK_TICKS;
}

static int scd_vdp_dma_prepare(ScdMachine *m, GenesisRuntime *host, uint16_t first, uint16_t second, uint32_t pc, uint32_t *word_ram_lag) {
  uint16_t *r = host->devices.vdp.registers;
  uint32_t length, source, end;
  *word_ram_lag = 0U;
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
    /* GPGX vdp_ctrl.c (a Sega CD source $200000-$3FFFFF, (reg23 & $70) == $10): the source data arrives one bus cycle late - the first
     * word the VDP writes is the stale word on the bus (GPGX writes nothing), the destination address advances by the auto-increment,
     * the source words follow, and the last source word is ignored: dma_length-- and addr += reg15. A one-word transfer has no
     * source word left and is not modelled. */
    if (length == 1U) return scd_fail(m, SCD_MAIN, SCD_STOP_VDP_DMA_UNSUPPORTED, pc, source);
    *word_ram_lag = 1U;
    length -= 1U;
    r[19] = (uint16_t)(length & 0xFFU);
    r[20] = (uint16_t)((length >> 8) & 0xFFU);
  } else if (!(end <= m->bios_size)) {
    return scd_fail(m, SCD_MAIN, SCD_STOP_VDP_DMA_UNSUPPORTED, pc, source);
  }
  scd_log(m, 0x43U, source, length);
  { /* SEG-065-T002: the main processor waits for the transfer (destination code CD3-CD0: 0001 VRAM, 0011 CRAM, 0101 VSRAM) */
    const uint32_t code = (((uint32_t)first >> 14) & 3U) | (((uint32_t)second >> 2) & 0x3CU);
    uint32_t windows = 0U;
    /* GPGX's class: a code with CD2 or CD1 set (CRAM, VSRAM) transfers a word per slot, otherwise (VRAM) a word per two slots */
    const uint64_t end = scd_vdp_dma_end(m, host, m->now, length, (code & 0x06U) == 0U, &windows);
    m->main_dma_until = end;
    m->main_dma_multi = windows > 1U ? 1U : 0U;
    ((ScdVdpHost *)m->vdp_host)->dma_armed = 1U;
    m->vdp_dma_count += 1U;
    m->vdp_dma_stall_ticks += end - m->now;
    scd_log(m, 0x48U, (uint32_t)((end - m->now) / SCD_MASTER_TICK_TICKS), code); /* SEG-065-T002: DMA duration */
  }
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

/* SEG-065-T002: after a 68K->VDP DMA the length registers are 0 and the source registers #21-#22 have advanced by the length (16-bit,
 * register #23 unchanged) - GPGX vdp_dma_update "DMA source address registers are incremented during DMA". */
static void scd_vdp_dma_registers_done(GenesisRuntime *host) {
  uint16_t *r = host->devices.vdp.registers;
  uint32_t end;
  if (!((ScdVdpHost *)host)->dma_armed) return;
  end = ((uint32_t)(r[21] & 0xFFU) | ((uint32_t)(r[22] & 0xFFU) << 8)) + ((uint32_t)(r[19] & 0xFFU) | ((uint32_t)(r[20] & 0xFFU) << 8));
  r[21] = (uint16_t)(end & 0xFFU);
  r[22] = (uint16_t)((end >> 8) & 0xFFU);
  r[19] = 0U;
  r[20] = 0U;
  ((ScdVdpHost *)host)->dma_armed = 0U;
}

/* ---- SEG-071-T002 (ADR 0177): VDP data-port FIFO and VRAM-fill timing of the BIOS-image profile --------------------------------------
 * GPGX core/vdp_ctrl.c vdp_68k_data_w_m5 / vdp_68k_ctrl_r / vdp_dma_update, integer master clocks counted from the frame origin (the
 * last vertical-blank onset, as in scd_vdp_dma_end). `mcycles_vdp` of GPGX is the start of the line in which the instruction begins. */
static const uint32_t scd_fifo_timing_h32[] = {230, 510, 810, 970, 1130, 1450, 1610, 1770, 2090, 2250, 2410, 2730, 2890, 3050, 3350, 3370,
                                               3420 + 230, 3420 + 510, 3420 + 810, 3420 + 970, 3420 + 1130, 3420 + 1450, 3420 + 1610,
                                               3420 + 1770, 3420 + 2090, 3420 + 2250, 3420 + 2410, 3420 + 2730};
static const uint32_t scd_fifo_timing_h40[] = {352, 820, 948, 1076, 1332, 1460, 1588, 1844, 1972, 2100, 2356, 2484, 2612, 2868, 2996, 3124,
                                               3364, 3380, 3420 + 352, 3420 + 820, 3420 + 948, 3420 + 1076, 3420 + 1332, 3420 + 1460,
                                               3420 + 1588, 3420 + 1844, 3420 + 1972, 3420 + 2100, 3420 + 2356, 3420 + 2484};
#define SCD_VDP_BLANK_LINES 37U /* the vertical-blank window of GPGX's frame (status bit 3 set): the first 37 lines from the origin */

/* Master clocks since the frame origin at tick `t`. */
static uint64_t scd_frame_mclk(const ScdMachine *m, uint64_t t) { return (t + SCD_FRAME_TICKS - m->vdp_onset) / SCD_MASTER_TICK_TICKS; }

/* One data-port word write (GPGX vdp_68k_data_w_m5, restricted writes during the active display): the FIFO entry's read-out slot, and
 * the stall of the processor when all four entries are in use (added to the instruction's `main_extra_ticks`). */
static void scd_vdp_fifo_word_write(ScdMachine *m, ScdVdpHost *h, const GenesisRuntime *host) {
  const uint16_t *r = host->devices.vdp.registers;
  uint64_t cycles = scd_frame_mclk(m, m->now + m->main_extra_ticks);
  const uint64_t line_base = (scd_frame_mclk(m, m->now) / 3420U) * 3420U; /* GPGX mcycles_vdp: the line of the instruction's start */
  const uint32_t *timing = (r[12] & 1U) ? scd_fifo_timing_h40 : scd_fifo_timing_h32;
  const uint32_t code = host->devices.vdp.data_port_transfer_code;
  const uint32_t byte_access = (code & 0x06U) ? 0U : 1U;
  uint32_t slot = 0U;
  if (cycles < SCD_VDP_BLANK_LINES * 3420U || !(r[1] & 0x40U)) return; /* vertical blank or display disabled: unrestricted */
  if (cycles < h->fifo_cycles[(h->fifo_idx + 3U) & 3U]) {
    if (cycles < h->fifo_cycles[h->fifo_idx]) { /* all four entries in use: the processor waits for the oldest to be processed */
      const uint64_t resume = (((uint64_t)h->fifo_cycles[h->fifo_idx] + 6U) / 7U) * 7U;
      m->main_extra_ticks += (resume - cycles) * SCD_MASTER_TICK_TICKS;
    }
    cycles = h->fifo_cycles[(h->fifo_idx + 3U) & 3U];
  }
  cycles -= line_base;
  while (cycles >= timing[slot]) slot++;
  h->fifo_cycles[h->fifo_idx] = (uint32_t)(line_base + timing[slot + byte_access]);
  h->fifo_idx = (h->fifo_idx + 1U) & 3U;
}

/* GPGX vdp_dma_update for a DMA fill started at tick `start` with `length` bytes: the tick at which the transfer ends. */
static uint64_t scd_vdp_fill_end(const ScdMachine *m, const GenesisRuntime *host, uint64_t start, uint32_t length) {
  static const uint32_t timing[2][2] = {{16U, 18U}, {166U, 204U}};
  const uint32_t line = GENESIS_NTSC_MASTER_TICKS_PER_LINE, frame_lines = GENESIS_NTSC_LINES_PER_FRAME;
  const uint16_t *r = host->devices.vdp.registers;
  const uint32_t h40 = r[12] & 1U, display = (r[1] & 0x40U) != 0U;
  const uint64_t frame_start = m->vdp_onset - SCD_FRAME_TICKS;
  uint64_t cyc = scd_frame_mclk(m, start), frames = 0U;
  uint32_t remaining = length;
  {
    const uint32_t l0 = (uint32_t)(cyc / line);
    const uint32_t rate0 = timing[(l0 < SCD_VDP_BLANK_LINES) || !display][h40];
    cyc += 2U * (line / rate0); /* the initial data-port write */
  }
  for (;;) {
    const uint32_t l = (uint32_t)(cyc / line);
    const int vblank = l < SCD_VDP_BLANK_LINES;
    const uint32_t rate = timing[vblank || !display][h40];
    const uint64_t window_end = vblank ? (uint64_t)SCD_VDP_BLANK_LINES * line : (uint64_t)(l + 1U) * line;
    const uint64_t cap = ((window_end - cyc) * rate) / line;
    if (remaining < cap) {
      cyc += ((uint64_t)remaining * line) / rate;
      break;
    }
    remaining -= (uint32_t)cap;
    cyc = window_end;
    if (remaining == 0U) break;
    if (cyc >= (uint64_t)frame_lines * line) {
      cyc -= (uint64_t)frame_lines * line;
      frames += 1U;
    }
  }
  return frame_start + (frames * frame_lines * line + cyc) * SCD_MASTER_TICK_TICKS;
}

int scd_vdp_access(ScdMachine *m, uint32_t address, uint32_t width, int is_write, uint32_t *value, uint32_t pc) {
  GenesisRuntime *host = scd_host(m);
  GenesisRuntimeStop stop = {0};
  uint32_t routed = is_write ? *value : 0U;
  uint32_t fill_length = 0U;
  const uint16_t register1_before = host->devices.vdp.registers[1];
  scd_log(m, is_write ? 0x41U : 0x40U, address, routed);
  if (m->bios != NULL && is_write && (address & ~3U) == 0xC00000U) {
    /* SEG-071-T002 (ADR 0177): the FIFO entry (and possibly the stall) of each word, before the data is written; an armed VRAM fill
     * starts with this write. */
    ScdVdpHost *const h = (ScdVdpHost *)host;
    const int fill = host->devices.vdp.dma.phase == GENESIS_VDP_DMA_BUSY && host->devices.vdp.dma.kind == GENESIS_VDP_DMA_VRAM_FILL;
    uint32_t words = width == 4U ? 2U : 1U;
    while (words-- != 0U) scd_vdp_fifo_word_write(m, h, host);
    if (fill) {
      fill_length = (host->devices.vdp.registers[19] & 0xFFU) | ((host->devices.vdp.registers[20] & 0xFFU) << 8);
      if (fill_length == 0U) fill_length = 0x10000U;
    }
  }
  if (m->bios != NULL) {
    if (is_write && (address & ~3U) == 0xC00004U && width != 1U) {
      const int awaiting = host->devices.vdp.control_port_awaiting_second_word != 0U;
      /* The H-interrupt: register #0 IE1 (bit 4) would let the line counter (not modelled; GPGX latches it on active lines whether or
       * not IE1 is set) request level 4. Setting it is the typed stop interrupt_source_unsupported at the write, before any line. */
      const int shape = scd_vdp_sets_ie1(awaiting, width, *value);
      if (shape == 1) return scd_fail_access(m, SCD_MAIN, SCD_STOP_INTERRUPT_SOURCE_UNSUPPORTED, pc, address, width, 1);
      if (shape == 2) return scd_fail_access(m, SCD_MAIN, SCD_STOP_VDP_ACCESS_UNSUPPORTED, pc, address, width, 1);
      uint32_t second = UINT32_MAX, first = host->devices.vdp.control_port_first_word;
      if (width == 2U && awaiting) second = *value & 0xFFFFU;
      else if (width == 4U && awaiting) second = *value >> 16;
      else if (width == 4U && ((*value >> 16) & 0xC000U) != 0x8000U) {
        first = *value >> 16;
        second = *value & 0xFFFFU;
      }
      if (second != UINT32_MAX) {
        uint32_t lag = 0U;
        if (scd_vdp_dma_prepare(m, host, (uint16_t)first, (uint16_t)second, pc, &lag) != 0) return 1;
        if (lag != 0U) { /* the destination advances past the skipped first word (the command's address, rewritten) */
          const uint32_t addr = ((first & 0x3FFFU) | ((second & 3U) << 14)), moved = (addr + host->devices.vdp.registers[15]) & 0xFFFFU;
          const uint32_t nfirst = (first & 0xC000U) | (moved & 0x3FFFU), nsecond = (second & ~3U) | ((moved >> 14) & 3U);
          if (width == 4U && awaiting) *value = (nsecond << 16) | (*value & 0xFFFFU);
          else if (width == 4U) *value = (nfirst << 16) | nsecond;
          else *value = nsecond;
          if (width != 4U || awaiting) host->devices.vdp.control_port_first_word = (uint16_t)nfirst;
          routed = *value;
        }
      }
    }
  } else if (is_write && (address & ~3U) == 0xC00004U &&
             ((width == 2U && scd_vdp_enables_dma(*value)) ||
              (width == 4U && (scd_vdp_enables_dma(*value >> 16) || scd_vdp_enables_dma(*value & 0xFFFFU)))))
    return scd_fail(m, SCD_MAIN, SCD_STOP_VDP_DMA_UNSUPPORTED, pc, *value);
  if (((ScdVdpHost *)host)->dma_armed && width == 4U && host->devices.vdp.control_port_awaiting_second_word) {
    /* SEG-065-T002: the high word of a long write completed a command that started a 68K->VDP DMA; the processor is frozen, so the
     * low word reaches the VDP after the transfer and its register update (GPGX caches it: `cached_write`). */
    uint32_t high = *value >> 16, low = *value & 0xFFFFU;
    if (genesis_route_access(host, address, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &high, &stop) != GENESIS_ACCESS_OK)
      return scd_fail_access(m, SCD_MAIN, SCD_STOP_VDP_ACCESS_UNSUPPORTED, pc, address, width, is_write);
    scd_vdp_dma_registers_done(host);
    if (genesis_route_access(host, address, GENESIS_ACCESS_WORD, GENESIS_ACCESS_WRITE, &low, &stop) != GENESIS_ACCESS_OK)
      return scd_fail_access(m, SCD_MAIN, SCD_STOP_VDP_ACCESS_UNSUPPORTED, pc, address, width, is_write);
    return 0;
  }
  if (genesis_route_access(host, address, scd_genesis_width(width), is_write ? GENESIS_ACCESS_WRITE : GENESIS_ACCESS_READ, &routed,
                           &stop) != GENESIS_ACCESS_OK)
    return scd_fail_access(m, SCD_MAIN, SCD_STOP_VDP_ACCESS_UNSUPPORTED, pc, address, width, is_write); /* SEG-064-T004: with its width and direction */
  /* SEG-071-T002 (ADR 0177): GPGX m68k_set_irq_delay - enabling the vertical interrupt (register #1 IE0) while one is pending, from a MOVE
   * that is not a MOVE.L, lets the next instruction execute before the interrupt is recognised ("IRQ latency"). */
  if (m->bios != NULL && is_write && width != 4U && (address & ~3U) == 0xC00004U && !(register1_before & 0x20U) &&
      (host->devices.vdp.registers[1] & 0x20U) && m->vint_pending)
    m->main_irq_delay = 1U;
  if (m->bios != NULL && fill_length != 0U && host->devices.vdp.dma.phase != GENESIS_VDP_DMA_BUSY)
    ((ScdVdpHost *)host)->fill_end = scd_vdp_fill_end(m, host, m->now + m->main_extra_ticks, fill_length);
  if (m->bios != NULL && !is_write && (address & ~3U) == 0xC00004U && width <= 2U) {
    /* SEG-071-T002 (ADR 0177): GPGX vdp_68k_ctrl_r: the DMA-busy bit until the fill's end, and the FIFO empty / full bits at the
     * instant of the read (the instruction's own cycles are counted: the read is taken as an 8-cycle instruction's). */
    ScdVdpHost *const h = (ScdVdpHost *)host;
    const uint64_t at = m->now + m->main_extra_ticks + UINT64_C(8) * SCD_MAIN_CYCLE_TICKS;
    const uint64_t cycles = scd_frame_mclk(m, at);
    uint32_t word = width == 2U ? routed : (address & 1U) ? routed : routed << 8;
    word &= ~0x0302U;
    if (h->fill_end != 0U && at < h->fill_end) word |= 0x0002U;
    if (cycles >= h->fifo_cycles[(h->fifo_idx + 3U) & 3U]) word |= 0x0200U;
    else if (cycles < h->fifo_cycles[h->fifo_idx]) word |= 0x0100U;
    routed = width == 2U ? word : (address & 1U) ? (word & 0xFFU) : (word >> 8);
  }
  if (!is_write) *value = routed;
  scd_vdp_dma_registers_done(host);
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

const uint8_t *scd_main_stream_byte(const ScdMachine *m, uint32_t address) { return scd_stream_byte(m, address); }

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
  if (address >= UINT32_C(0x00A00000) && address < UINT32_C(0x00A10000)) m->main_extra_ticks += SCD_MAIN_Z80_ACCESS_TICKS;
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
    /* SEG-070-T002: observation of a controller data-port byte read (a word read arrives here as its odd-address byte) */
    if (m->input != NULL && width == 1U && (address == UINT32_C(0x00A10003) || address == UINT32_C(0x00A10005)))
      scd_input_note_read(m, address == UINT32_C(0x00A10003) ? 1U : 2U, routed & 0xFFU);
  }
  return 0;
}

/* ---- vertical blank and the main interrupt request ------------------------------------------------------------------------ */

/* ---- SEG-065-T003: frame capture at requested vertical-blank onsets ------------------------------------------------------------- */

int scd_vdp_capture_request(ScdMachine *m, uint64_t onset, const char *dir) {
  ScdVdpHost *h = (ScdVdpHost *)m->vdp_host;
  if (h == NULL || m->bios == NULL || onset < 2U || h->capture_count >= SCD_CAPTURE_MAX || dir == NULL) return -1;
  h->capture_dir = dir;
  h->captures[h->capture_count].onset = onset;
  h->capture_count += 1U;
  h->capture_line = -1;
  return 0;
}

static ScdFrameCapture *scd_capture_for(ScdVdpHost *h, uint64_t onset) {
  uint32_t i;
  for (i = 0; i < h->capture_count; ++i)
    if (h->captures[i].onset == onset) return &h->captures[i];
  return NULL;
}

static int scd_capture_write(const char *dir, const char *name, uint64_t onset, const void *data, size_t length) {
  char path[1024];
  FILE *f;
  int bad;
  snprintf(path, sizeof path, "%s/%s_%llu.bin", dir, name, (unsigned long long)onset);
  f = fopen(path, "wb");
  if (f == NULL) return 1;
  bad = fwrite(data, 1U, length, f) != length;
  return (fclose(f) != 0) || bad;
}

/* The pre-registered canonical frame (ADR 0146 decision 5): u16 LE width, height, then per pixel (mode << 9) | 9-bit CRAM colour
 * (BBBGGGRRR), row-major. Mode is 1 (normal): capture refuses shadow/highlight and the palette-select mode beforehand. */
static size_t scd_canonical_frame(const GenesisFrameArtifact *f, uint8_t *out) {
  size_t n = 0U;
  unsigned x, y;
  out[n++] = (uint8_t)(f->width & 0xFFU);
  out[n++] = (uint8_t)(f->width >> 8);
  out[n++] = (uint8_t)(f->height & 0xFFU);
  out[n++] = (uint8_t)(f->height >> 8);
  for (y = 0; y < f->height; ++y) {
    const uint8_t slot = f->row_palette[y];
    const uint8_t *pal = slot == 0U ? f->palette_snapshot : f->extra_palettes[slot - 1U];
    for (x = 0; x < f->width; ++x) {
      const unsigned index = f->pixels[(size_t)y * f->width + x] & 0x3FU;
      const unsigned word = ((unsigned)pal[2U * index] << 8) | pal[2U * index + 1U];
      const unsigned c = (((word >> 9) & 7U) << 6) | (((word >> 5) & 7U) << 3) | ((word >> 1) & 7U);
      const unsigned v = (1U << 9) | c;
      out[n++] = (uint8_t)(v & 0xFFU);
      out[n++] = (uint8_t)(v >> 8);
    }
  }
  return n;
}

static void scd_capture_line(ScdMachine *m) {
  ScdVdpHost *h = (ScdVdpHost *)m->vdp_host;
  const GenesisVdpState *vdp = &h->host.devices.vdp;
  GenesisRasterLine *line = &h->raster.lines[h->capture_line];
  memcpy(line->registers, vdp->registers, sizeof line->registers);
  memcpy(line->cram, vdp->cram, sizeof line->cram);
  memcpy(line->vsram, vdp->vsram, sizeof line->vsram);
  if (h->capture_line == 0) memcpy(h->vram_line0, vdp->vram, sizeof h->vram_line0);
  h->capture_line += 1;
  h->capture_lines_done += 1U;
  h->capture_line_tick += SCD_LINE_TICKS;
  if (h->capture_line >= (int32_t)GENESIS_RASTER_MAX_LINES) h->capture_line = -1;
}

static void scd_capture_frame(ScdMachine *m, ScdFrameCapture *c) {
  ScdVdpHost *h = (ScdVdpHost *)m->vdp_host;
  const GenesisVdpState *vdp = &h->host.devices.vdp;
  static uint8_t canonical[4U + 2U * GENESIS_FRAME_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT];
  uint32_t i;
  size_t n;
  c->lines = h->capture_lines_done;
  if (c->lines != GENESIS_RASTER_MAX_LINES) {
    c->status = 5U;
    return;
  }
  for (i = 0; i < GENESIS_VDP_VRAM_BYTES; ++i) c->vram_display_changes += h->vram_line0[i] != vdp->vram[i];
  for (i = 0; i < GENESIS_RASTER_MAX_LINES; ++i) {
    const uint16_t *r = h->raster.lines[i].registers;
    if ((r[12] & 0x08U) != 0U || (r[0] & 0x04U) == 0U) { /* shadow/highlight or the palette-select mode: not modelled */
      c->status = 2U;
      return;
    }
  }
  h->raster.has_vram_journal = 0U;
  if (genesis_vdp_produce_frame_raster(vdp->vram, &h->raster, &h->frame) != 0) {
    c->status = 3U;
    return;
  }
  c->width = h->frame.width;
  c->height = h->frame.height;
  n = scd_canonical_frame(&h->frame, canonical);
  {
    GenesisSha256 sha;
    uint8_t digest[32];
    genesis_sha256_init(&sha);
    genesis_sha256_update(&sha, canonical, (uint32_t)n); /* n <= 4 + 2 * 320 * 448 */
    genesis_sha256_final(&sha, digest);
    for (i = 0; i < 32U; ++i) snprintf(c->sha256 + 2U * i, 3U, "%02x", digest[i]);
  }
  {
    uint8_t vdp_state[2U * GENESIS_VDP_REGISTER_COUNT + GENESIS_VDP_CRAM_BYTES + GENESIS_VDP_VSRAM_BYTES + GENESIS_VDP_VRAM_BYTES];
    size_t k = 0U;
    for (i = 0; i < GENESIS_VDP_REGISTER_COUNT; ++i) {
      vdp_state[k++] = (uint8_t)(vdp->registers[i] >> 8);
      vdp_state[k++] = (uint8_t)vdp->registers[i];
    }
    memcpy(vdp_state + k, vdp->cram, GENESIS_VDP_CRAM_BYTES);
    k += GENESIS_VDP_CRAM_BYTES;
    memcpy(vdp_state + k, vdp->vsram, GENESIS_VDP_VSRAM_BYTES);
    k += GENESIS_VDP_VSRAM_BYTES;
    memcpy(vdp_state + k, vdp->vram, GENESIS_VDP_VRAM_BYTES);
    k += GENESIS_VDP_VRAM_BYTES;
    if (scd_capture_write(h->capture_dir, "canon", c->onset, canonical, n) ||
        scd_capture_write(h->capture_dir, "vdp", c->onset, vdp_state, k) ||
        scd_capture_write(h->capture_dir, "work_ram", c->onset, m->work_ram, sizeof m->work_ram) ||
        scd_capture_write(h->capture_dir, "prg_ram", c->onset, m->prg_ram, sizeof m->prg_ram) ||
        scd_capture_write(h->capture_dir, "word_ram", c->onset, m->word_ram, sizeof m->word_ram) ||
        scd_capture_write(h->capture_dir, "cpus", c->onset, m->cpu, sizeof m->cpu)) {
      c->status = 4U;
      return;
    }
  }
  c->status = 1U;
}

void scd_vdp_capture_report(const ScdMachine *m, FILE *out) {
  const ScdVdpHost *h = (const ScdVdpHost *)m->vdp_host;
  static const char *const names[] = {"pending", "written", "vdp_state_unsupported", "render_failed", "output_failed", "lines_incomplete"};
  uint32_t i;
  if (h == NULL || h->capture_count == 0U) return;
  fprintf(out, ",\"frame_captures\":[");
  for (i = 0; i < h->capture_count; ++i) {
    const ScdFrameCapture *c = &h->captures[i];
    fprintf(out, "%s{\"onset\":%llu,\"status\":\"%s\",\"width\":%u,\"height\":%u,\"vram_display_changes\":%u,\"sha256\":%s%s%s}",
            i ? "," : "", (unsigned long long)c->onset, names[c->status < 6U ? c->status : 0U], c->width, c->height,
            c->vram_display_changes, c->status == 1U ? "\"" : "", c->status == 1U ? c->sha256 : "null", c->status == 1U ? "\"" : "");
  }
  fprintf(out, "]");
}

/* SEG-065-T003: 1 when the event due at `until` is a capture line sample (an observer instant, not a device event). */
int scd_vdp_capture_line_due(const ScdMachine *m, uint64_t until) {
  const ScdVdpHost *h = (const ScdVdpHost *)m->vdp_host;
  return m->bios != NULL && h != NULL && h->capture_line >= 0 && h->capture_line_tick <= until && h->capture_line_tick < m->vdp_onset;
}

uint64_t scd_frame_ticks_since_origin(const ScdMachine *m, uint64_t t) { return t + SCD_FRAME_TICKS - m->vdp_onset; }

uint64_t scd_vdp_next_event(const ScdMachine *m) {
  const ScdVdpHost *h = (const ScdVdpHost *)m->vdp_host;
  uint64_t next = m->vdp_onset;
  if (m->bios == NULL) return UINT64_MAX;
  if (m->vint_at != 0U && m->vint_at < next) next = m->vint_at;
  if (h != NULL && h->capture_line >= 0 && h->capture_line_tick < next) next = h->capture_line_tick;
  return next;
}

void scd_vdp_event(ScdMachine *m) {
  GenesisRuntime *host = scd_host(m);
  ScdVdpHost *h = (ScdVdpHost *)m->vdp_host;
  if (h->capture_line >= 0 && h->capture_line_tick < m->vdp_onset && (m->vint_at == 0U || h->capture_line_tick < m->vint_at)) {
    scd_capture_line(m); /* SEG-065-T003: an active line of a captured frame begins */
    return;
  }
  if (m->vint_at != 0U && m->vint_at < m->vdp_onset) {
    /* SEG-071-T002 (ADR 0177): the vertical interrupt is requested a fixed delay after the onset (GPGX VINT_Hxx_MCYCLE). */
    m->vint_at = 0U;
    m->vint_pending = 1U;
    host->devices.vdp.status_register = (uint16_t)(host->devices.vdp.status_register | 0x0080U); /* F: vertical interrupt occurred */
    scd_log(m, 0x91U, (uint32_t)m->vdp_frames, host->devices.vdp.registers[1]);
    return;
  }
  m->vdp_onset += SCD_FRAME_TICKS;
  m->vdp_frames += 1U;
  m->vint_at = (m->vdp_onset - SCD_FRAME_TICKS) + ((host->devices.vdp.registers[12] & 1U) ? SCD_VINT_H40_MCLK : SCD_VINT_H32_MCLK) * SCD_MASTER_TICK_TICKS;
  memset(h->fifo_cycles, 0, sizeof h->fifo_cycles); /* GPGX system_frame_scd: the VDP FIFO is empty at the frame's start */
  scd_log(m, 0x90U, (uint32_t)m->vdp_frames, host->devices.vdp.registers[1]);
  if (h->capture_count != 0U) {
    ScdFrameCapture *done = scd_capture_for(h, m->vdp_frames);
    if (done != NULL && done->status == 0U) scd_capture_frame(m, done); /* its active display has just ended */
    if (scd_capture_for(h, m->vdp_frames + 1U) != NULL) {              /* the next frame is a target: sample its lines */
      h->capture_line = 0;
      h->capture_lines_done = 0U;
      h->capture_line_tick = m->vdp_onset - SCD_ONSET_TICKS;           /* line 0 of the frame whose onset is next */
    }
  }
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
