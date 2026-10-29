/* SEG-065-T002 (ADR 0147): the duration of a 68K->VDP DMA in the BIOS-image profile and the main processor's wait for it, driven
 * through the runtime's own bus ABI (scd_write to the VDP control port, exactly as generated code issues it) with expectations derived
 * by hand from the slot model (contract section 16): per line 16 (H32) / 18 (H40) access slots in the active display, 166 / 204 while
 * blanked; a VRAM word takes two slots; CRAM/VSRAM words one, with 161 / 198 on a blanked line; the frame (GPGX's) starts at the
 * vertical-blank onset, its blank window lasts 37 lines, an active transfer runs to the end of the current line and resumes at the
 * next line start. All durations below are master clocks (3420 per line); the machine's tick is 500,000 per master clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"
#include "runtime.h"

static int failures;
static ScdMachine *m;
static uint8_t bios[0x20000];

#define MASTER UINT64_C(500000)
#define LINE UINT64_C(3420)
#define FRAME (LINE * 262U * MASTER)
#define ONSET (LINE * 224U * MASTER)

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

static int vdp(uint32_t width, uint32_t value) {
  int s = scd_write(m, SCD_MAIN, 0xC00004U, width, value, 0xFF0100U);
  if (s != 0) {
    printf("      stop %s\n", scd_stop_class_name(m->stop.stop_class));
    m->stop.stop_class = SCD_STOP_NONE;
  }
  return s;
}

static void fresh(uint32_t reg1, uint32_t reg12) {
  if (m->vdp_host != NULL) scd_vdp_release(m);
  memset(m, 0, sizeof *m);
  m->bios = bios;
  m->bios_size = sizeof bios;
  m->cpu[SCD_MAIN].cycle_ticks = SCD_MAIN_CYCLE_TICKS;
  m->cpu[SCD_SUB].cycle_ticks = SCD_SUB_CYCLE_TICKS;
  if (scd_vdp_init(m) != 0) exit(2);
  m->vdp_onset = ONSET + 5U * FRAME; /* the last onset (GPGX's frame origin) is ONSET + 4 frames */
  vdp(2U, 0x8100U | reg1);
  vdp(2U, 0x8C00U | reg12);
  vdp(2U, 0x8F02U); /* auto-increment 2 */
}

/* Arm a 68K->VDP DMA of `words` words from work RAM $FF0000 at frame offset `offset` (master clocks after the onset); return the wait
 * in master clocks (0 when nothing waits). `command` is the two-word address command (CD5 set). */
static uint64_t dma_from(uint64_t offset, uint32_t words, uint32_t command, int from_bios) {
  const uint64_t origin = m->vdp_onset - FRAME;
  vdp(2U, 0x9300U | (words & 0xFFU));
  vdp(2U, 0x9400U | ((words >> 8) & 0xFFU));
  vdp(2U, 0x9500U);
  vdp(2U, from_bios ? 0x9600U : 0x9680U); /* source $000000 (the BIOS image) or $FF0000 >> 1 = $7F8000 (work RAM) */
  vdp(2U, from_bios ? 0x9700U : 0x977FU);
  m->now = origin + offset * MASTER;
  m->main_dma_until = 0U;
  if (vdp(4U, command) != 0) return UINT64_MAX;
  return m->main_dma_until == 0U ? 0U : (m->main_dma_until - m->now) / MASTER;
}
static uint64_t dma_at(uint64_t offset, uint32_t words, uint32_t command) { return dma_from(offset, words, command, 0); }


/* ---- Word RAM -> VDP DMA destination layout (the source delay of the reference's vdp_dma_68k_ext) ----------------------------------
 * Independent expectations written by hand from GPGX vdp_ctrl.c: for a source in $200000-$3FFFFF the first destination word is not
 * written (the stale word on the 68K bus; the destination still advances by the auto-increment), the source words follow, one word fewer
 * is transferred (length - 1; the last source word is never read) and the source register advances by length - 1. The DMA's duration is
 * the slot model's for length - 1 words. */
static GenesisRuntime *host(void) { return (GenesisRuntime *)m->vdp_host; }
static uint32_t target_word(uint8_t *buffer, uint32_t byte_address) { return ((uint32_t)buffer[byte_address] << 8) | buffer[byte_address + 1U]; }
static void fill_word_ram(uint32_t byte_offset, const uint16_t *words, uint32_t count) {
  uint32_t i;
  for (i = 0U; i < count; ++i) {
    m->word_ram[byte_offset + 2U * i] = (uint8_t)(words[i] >> 8);
    m->word_ram[byte_offset + 2U * i + 1U] = (uint8_t)words[i];
  }
}
static void sentinels(void) { /* a preinitialized destination: every word 0xA55A / 0x5AA5 pattern, distinct per target */
  GenesisVdpState *v = &host()->devices.vdp;
  uint32_t i;
  for (i = 0U; i < GENESIS_VDP_VRAM_BYTES; i += 2U) { v->vram[i] = 0xA5U; v->vram[i + 1U] = 0x5AU; }
  for (i = 0U; i < GENESIS_VDP_CRAM_BYTES; i += 2U) { v->cram[i] = 0x0AU; v->cram[i + 1U] = 0xCEU; }
  for (i = 0U; i < GENESIS_VDP_VSRAM_BYTES; i += 2U) { v->vsram[i] = 0x01U; v->vsram[i + 1U] = 0x23U; }
}
/* Programs length, source (a main-bus byte address >= $200000 or the BIOS/work RAM) and auto-increment, then arms the DMA with the command
 * in the given shape: shape 0 two 16-bit control writes, shape 1 one 32-bit control write. Returns the wait in master clocks, or
 * UINT64_MAX when the write stopped (m->stop is left for the caller). */
static uint64_t arm_offset; /* master clocks after the frame origin at which arm() starts the DMA (0: the onset) */
static uint64_t arm(uint32_t words, uint32_t source_byte, uint32_t increment, uint32_t first, uint32_t second, int shape) {
  const uint32_t src = source_byte >> 1;
  uint32_t status;
  vdp(2U, 0x8F00U | increment);
  vdp(2U, 0x9300U | (words & 0xFFU));
  vdp(2U, 0x9400U | ((words >> 8) & 0xFFU));
  vdp(2U, 0x9500U | (src & 0xFFU));
  vdp(2U, 0x9600U | ((src >> 8) & 0xFFU));
  vdp(2U, 0x9700U | ((src >> 16) & 0x7FU));
  m->now = m->vdp_onset - FRAME + arm_offset * MASTER;
  m->main_dma_until = 0U;
  if (shape == 0) {
    vdp(2U, first);
    status = (uint32_t)scd_write(m, SCD_MAIN, 0xC00004U, 2U, second, 0xFF0100U);
  } else {
    status = (uint32_t)scd_write(m, SCD_MAIN, 0xC00004U, 4U, (first << 16) | second, 0xFF0100U);
  }
  if (status != 0U) return UINT64_MAX;
  return m->main_dma_until == 0U ? 0U : (m->main_dma_until - m->now) / MASTER;
}
static uint16_t b0[0x10000], b1[0x10000];
/* arm() at an absolute tick instead of the frame origin */
static uint64_t arm_at_tick(uint64_t tick, uint32_t words, uint32_t source_byte, uint32_t increment, uint32_t first, uint32_t second);
/* The oracle is written from the contract's word interleave, independently of scd_bank_offset: bank b word i is 2M-space word 2 * i + b. */
static void store_banks(void) {
  uint32_t i;
  for (i = 0U; i < 0x10000U; ++i) {
    const uint32_t w0 = 2U * i, w1 = 2U * i + 1U;
    m->word_ram[2U * w0] = (uint8_t)(b0[i] >> 8); m->word_ram[2U * w0 + 1U] = (uint8_t)b0[i];
    m->word_ram[2U * w1] = (uint8_t)(b1[i] >> 8); m->word_ram[2U * w1 + 1U] = (uint8_t)b1[i];
  }
}
static void fresh_banks(uint32_t reg1, uint32_t reg12) { fresh(reg1, reg12); store_banks(); }
static uint64_t arm_at_tick(uint64_t tick, uint32_t words, uint32_t source_byte, uint32_t increment, uint32_t first, uint32_t second) {
  const uint32_t src = source_byte >> 1;
  vdp(2U, 0x8F00U | increment);
  vdp(2U, 0x9300U | (words & 0xFFU));
  vdp(2U, 0x9400U | ((words >> 8) & 0xFFU));
  vdp(2U, 0x9500U | (src & 0xFFU));
  vdp(2U, 0x9600U | ((src >> 8) & 0xFFU));
  vdp(2U, 0x9700U | ((src >> 16) & 0x7FU));
  m->now = tick;
  m->main_dma_until = 0U;
  vdp(2U, first);
  if (scd_write(m, SCD_MAIN, 0xC00004U, 2U, second, 0xFF0100U) != 0) return UINT64_MAX;
  return m->main_dma_until == 0U ? 0U : (m->main_dma_until - m->now) / MASTER;
}
static uint32_t reg(uint32_t index) { return host()->devices.vdp.registers[index]; }

int main(void) {
  char label[200];
  uint64_t w;
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  fresh(0x54U, 0x00U); /* M5 | DMA | display on; H32 */
  w = dma_at(0U, 100U, 0x40000080U);
  snprintf(label, sizeof label, "H32 VRAM 100 words at the onset: 83 words/line, 37-line window -> 100 * 3420 / 83 = 4120 (got %llu)", (unsigned long long)w);
  row(w == 4120U, label);

  w = dma_at(48U * LINE, 100U, 0x40000080U);
  snprintf(label, sizeof label, "H32 VRAM 100 words at the start of active line 10: 8 per line, 12 lines + 4 words = 12 * 3420 + 1710 = 42750 (got %llu)", (unsigned long long)w);
  row(w == 42750U, label);

  w = dma_at(48U * LINE + 1710U, 100U, 0x40000080U);
  snprintf(label, sizeof label, "H32 VRAM 100 words from mid-line: 4 to the line end, then 12 full lines = 1710 + 12 * 3420 = 42750 (got %llu)", (unsigned long long)w);
  row(w == 42750U, label);

  w = dma_at(37U * LINE, 4U, 0x40000080U);
  snprintf(label, sizeof label, "the frame's last line (VBLANK already clear) uses the active rate: 4 words = 4 * 3420 / 8 = 1710 (got %llu)", (unsigned long long)w);
  row(w == 1710U, label);

  w = dma_at(0U, 64U, 0xC0000080U);
  snprintf(label, sizeof label, "H32 CRAM 64 words at the onset: 161 slots (refresh) -> 64 * 3420 / 161 = 1359 (got %llu)", (unsigned long long)w);
  row(w == 1359U, label);

  w = dma_at(48U * LINE, 32U, 0x40000090U);
  snprintf(label, sizeof label, "H32 VSRAM 32 words in the active display: 16 per line = 2 lines = 6840 (got %llu)", (unsigned long long)w);
  row(w == 6840U, label);

  fresh(0x14U, 0x00U); /* display off: every line is blanked */
  w = dma_at(100U * LINE, 166U, 0x40000080U);
  snprintf(label, sizeof label, "display off, active part: 83 words per line, 166 words = 2 lines = 6840 (got %llu)", (unsigned long long)w);
  row(w == 6840U, label);

  fresh(0x54U, 0x81U); /* H40 */
  w = dma_at(250U * LINE, 2000U, 0x40000080U);
  snprintf(label, sizeof label, "H40 VRAM 2000 words from relative line 250: 12 lines * 9 = 108, then the next frame's blank window at 102 per line: "
                                "12 * 3420 + 1892 * 3420 / 102 = 104477 (got %llu)", (unsigned long long)w);
  row(w == 104477U, label);

  w = dma_from(0U, 0U, 0x40000080U, 1);
  snprintf(label, sizeof label, "length 0 is 65536 words (from the BIOS image): 5799 per H40 frame (3774 blank + 225 lines * 9), 11 frames, then 1747 "
                                "* 3420 / 102 = 58575: 11 * 896040 + 58575 = 9915015 (got %llu)", (unsigned long long)w);
  row(w == 9915015U, label);

  /* the wait: the instruction that armed the DMA completes after the transfer */
  fresh(0x54U, 0x00U);
  w = dma_at(0U, 100U, 0x40000080U);
  m->cpu[SCD_MAIN].next = m->now;
  scd_retire(m, SCD_MAIN, 8U);
  row(w == 4120U && m->cpu[SCD_MAIN].next == m->now + (4120U * MASTER) + 8U * SCD_MAIN_CYCLE_TICKS && m->main_dma_until == 0U,
      "retire: the arming instruction's 8 cycles start at the DMA end (next = now + 4120 master clocks + 8 cycles), the wait is consumed");
  scd_retire(m, SCD_MAIN, 4U);
  row(m->cpu[SCD_MAIN].next == m->now + (4120U * MASTER) + 12U * SCD_MAIN_CYCLE_TICKS, "the next instruction is not delayed again");
  row(m->vdp_dma_count == 1U && m->vdp_dma_stall_ticks == 4120U * MASTER, "count 1, stall total 4120 master clocks");

  /* after the transfer: length registers 0, source #21-#22 advanced by the length (16-bit), #23 unchanged */
  {
    const uint16_t *r = ((const GenesisRuntime *)m->vdp_host)->devices.vdp.registers;
    row(r[19] == 0U && r[20] == 0U && r[21] == 0x64U && r[22] == 0x80U && r[23] == 0x7FU,
        "registers after 100 words from $7F8000 (word address): #19/#20 = 0, #21/#22 = $8064, #23 = $7F");
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET; /* 2M Word RAM owned by main: a valid source window crossing a 64 Ki-word boundary */
    vdp(2U, 0x9320U);
    vdp(2U, 0x9400U);
    vdp(2U, 0x95F0U);
    vdp(2U, 0x96FFU);
    vdp(2U, 0x9710U); /* $21FFE0 >> 1 = $10FFF0: #23 = $10, #22 = $FF, #21 = $F0 */
    m->now = m->vdp_onset - FRAME;
    vdp(4U, 0x40000080U);
    r = ((const GenesisRuntime *)m->vdp_host)->devices.vdp.registers;
    row(m->vdp_dma_count == 1U && r[19] == 0U && r[20] == 0U && r[21] == 0x0FU && r[22] == 0x00U && r[23] == 0x10U,
        "Word RAM source delay (GPGX): the last source word is not read, so the advance wraps at 16 bits: $FFF0 + $1F -> #21/#22 = $000F, #23 unchanged ($10)");
  }

  /* a transfer spanning several slot windows: GPGX re-anchors the 68K at the transfer's end at the next line start, so the arming
   * instruction's own cycles are absorbed (the next instruction starts exactly at the end) */
  fresh(0x54U, 0x00U);
  w = dma_at(48U * LINE, 100U, 0x40000080U);
  m->cpu[SCD_MAIN].next = m->now;
  scd_retire(m, SCD_MAIN, 20U);
  row(w == 42750U && m->cpu[SCD_MAIN].next == m->now + 42750U * MASTER && m->main_dma_until == 0U && m->main_dma_multi == 0U,
      "retire after a multi-window transfer (13 windows): the next instruction starts exactly at the DMA end (20 cycles absorbed)");
  scd_retire(m, SCD_MAIN, 4U);
  row(m->cpu[SCD_MAIN].next == m->now + 42750U * MASTER + 4U * SCD_MAIN_CYCLE_TICKS, "the following instruction runs normally");

  /* H40 is register #12 bit 0 (GPGX `reg[12] & 1`); H40 CRAM while blanked loses its refresh slots (198) */
  fresh(0x54U, 0x80U); /* RS0 only: the slot rate follows bit 0 = H32 */
  w = dma_at(0U, 100U, 0x40000080U);
  row(w == 4120U, "register #12 = $80 (bit 0 clear): H32 slot rate (4120)");
  fresh(0x54U, 0x01U);
  w = dma_at(0U, 64U, 0xC0000080U);
  snprintf(label, sizeof label, "H40 CRAM 64 words at the onset: 198 slots -> 64 * 3420 / 198 = 1105 (got %llu)", (unsigned long long)w);
  row(w == 1105U, label);

  /* a long control-port write whose high word starts the DMA: its low word reaches the VDP after the transfer and its register
   * update (GPGX `cached_write`) - here a register #19 write */
  fresh(0x54U, 0x00U);
  vdp(2U, 0x9364U);
  vdp(2U, 0x9400U);
  vdp(2U, 0x9500U);
  vdp(2U, 0x9680U);
  vdp(2U, 0x977FU);
  m->now = m->vdp_onset - FRAME;
  vdp(2U, 0x4000U);
  vdp(4U, 0x00809312U);
  {
    const uint16_t *r = ((const GenesisRuntime *)m->vdp_host)->devices.vdp.registers;
    row(m->vdp_dma_count == 1U && r[19] == 0x12U && r[20] == 0U && r[21] == 0x64U && r[22] == 0x80U,
        "long write: DMA of 100 words, then the low word's register write: #19 = $12, #21/#22 = $8064");
  }

  /* a VRAM fill (register #23 bit 7 set, bit 6 clear) does not hold the processor */
  fresh(0x54U, 0x00U);
  vdp(2U, 0x9310U);
  vdp(2U, 0x9400U);
  vdp(2U, 0x9780U);
  m->now = m->vdp_onset - FRAME;
  m->main_dma_until = 0U;
  vdp(4U, 0x40000080U);
  row(m->main_dma_until == 0U && m->vdp_dma_count == 0U, "VRAM fill: no processor wait");

  /* ---- Word RAM -> VDP DMA: destination words, source advance, registers, duration and the wait (GPGX source delay) ---- */
  {
    static const uint16_t src[8] = {0x1111U, 0x2222U, 0x3333U, 0x4444U, 0x5555U, 0x6666U, 0x7777U, 0x8888U};
    uint32_t shape;
    for (shape = 0U; shape < 2U; ++shape) {
      const uint32_t dest = 0x0100U + shape * 0x0100U; /* a nonzero start; two commands: separate words, then one long */
      fresh(0x54U, 0x00U);
      m->ga.mode = SCD_MODE_RET; /* 2M Word RAM owned by main */
      fill_word_ram(0x1000U, src, 8U);
      sentinels();
      w = arm(4U, 0x201000U, 2U, 0x4000U | dest, 0x0080U, (int)shape);
      snprintf(label, sizeof label, "Word RAM DMA, 4 words, increment 2, %s command: duration is that of 3 words (3 * 3420 / 83 = 123) (got %llu)",
               shape ? "one 32-bit" : "two 16-bit", (unsigned long long)w);
      row(w == 123U, label);
      row(target_word(host()->devices.vdp.vram, dest) == 0xA55AU, "the first destination word is skipped: it keeps its previous value");
      row(target_word(host()->devices.vdp.vram, dest + 2U) == 0x1111U && target_word(host()->devices.vdp.vram, dest + 4U) == 0x2222U &&
              target_word(host()->devices.vdp.vram, dest + 6U) == 0x3333U,
          "source words 0, 1, 2 land at the next three destination words, in order");
      row(target_word(host()->devices.vdp.vram, dest + 8U) == 0xA55AU, "source word 3 is not transferred: the word after the third keeps its value");
      row(reg(19) == 0U && reg(20) == 0U && reg(21) == 0x03U && reg(22) == 0x08U && reg(23) == 0x10U && m->vdp_dma_count == 1U,
          "registers: length 0, the source advanced by three words ($100800 -> $100803), #23 unchanged");
      m->cpu[SCD_MAIN].next = m->now;
      scd_retire(m, SCD_MAIN, 8U);
      row(m->cpu[SCD_MAIN].next == m->now + 123U * MASTER + 8U * SCD_MAIN_CYCLE_TICKS && m->main_dma_until == 0U &&
              m->vdp_dma_stall_ticks == 123U * MASTER,
          "the arming instruction's cycles start at the DMA end (the processor stalls for the 3-word transfer)");
    }
  }
  { /* a non-default increment: the skipped word still advances the destination by it */
    static const uint16_t src[4] = {0xA001U, 0xB002U, 0xC003U, 0xD004U};
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET;
    fill_word_ram(0x2000U, src, 4U);
    sentinels();
    w = arm(4U, 0x202000U, 4U, 0x4000U | 0x0300U, 0x0080U, 0);
    row(w == 123U && target_word(host()->devices.vdp.vram, 0x0300U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0304U) == 0xA001U &&
            target_word(host()->devices.vdp.vram, 0x0308U) == 0xB002U && target_word(host()->devices.vdp.vram, 0x030CU) == 0xC003U &&
            target_word(host()->devices.vdp.vram, 0x0302U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0306U) == 0xA55AU &&
            target_word(host()->devices.vdp.vram, 0x0310U) == 0xA55AU,
        "increment 4: words 0-2 at +4, +8, +12 from the start; the start, the gaps and the fourth slot keep their values");
  }
  { /* the advanced destination crosses into address bits A15-A14 (they live in the command's second word) */
    static const uint16_t src[4] = {0x0A0AU, 0x0B0BU, 0x0C0CU, 0x0D0DU};
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET;
    fill_word_ram(0x0000U, src, 4U);
    sentinels();
    w = arm(4U, 0x200000U, 2U, 0x4000U | 0x3FFEU, 0x0080U | 0x01U, 0); /* destination $7FFE: A15-A14 = 01 */
    row(w == 123U && target_word(host()->devices.vdp.vram, 0x7FFEU) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x8000U) == 0x0A0AU &&
            target_word(host()->devices.vdp.vram, 0x8002U) == 0x0B0BU && target_word(host()->devices.vdp.vram, 0x8004U) == 0x0C0CU,
        "destination $7FFE: the skipped word advances the start to $8000 (A15 set) and words 0-2 follow");
  }
  { /* CRAM and VSRAM destinations (the host's supported write targets) */
    static const uint16_t src[4] = {0x0111U, 0x0222U, 0x0333U, 0x0444U};
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET;
    fill_word_ram(0x0400U, src, 4U);
    sentinels();
    w = arm(4U, 0x200400U, 2U, 0xC000U | 0x0010U, 0x0080U, 0);
    row(w == 63U && target_word(host()->devices.vdp.cram, 0x10U) == 0x0ACEU && target_word(host()->devices.vdp.cram, 0x12U) == 0x0111U &&
            target_word(host()->devices.vdp.cram, 0x14U) == 0x0222U && target_word(host()->devices.vdp.cram, 0x16U) == 0x0333U &&
            target_word(host()->devices.vdp.cram, 0x18U) == 0x0ACEU,
        "CRAM destination: first word skipped, words 0-2 at the next three entries, duration 3 * 3420 / 161 = 63");
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET;
    fill_word_ram(0x0400U, src, 4U);
    sentinels();
    w = arm(4U, 0x200400U, 2U, 0x4000U | 0x0004U, 0x0090U, 0);
    row(w == 63U && target_word(host()->devices.vdp.vsram, 0x04U) == 0x0123U && target_word(host()->devices.vdp.vsram, 0x06U) == 0x0111U &&
            target_word(host()->devices.vdp.vsram, 0x08U) == 0x0222U && target_word(host()->devices.vdp.vsram, 0x0AU) == 0x0333U &&
            target_word(host()->devices.vdp.vsram, 0x0CU) == 0x0123U,
        "VSRAM destination: first word skipped, words 0-2 follow");
  }
  { /* a one-word Word RAM DMA has no source word left: typed stop, nothing written */
    static const uint16_t src[2] = {0x1357U, 0x2468U};
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET;
    fill_word_ram(0x0000U, src, 2U);
    sentinels();
    w = arm(1U, 0x200000U, 2U, 0x4000U | 0x0040U, 0x0080U, 0);
    row(w == UINT64_MAX && m->stop.stop_class == SCD_STOP_VDP_DMA_UNSUPPORTED && m->vdp_dma_count == 0U &&
            target_word(host()->devices.vdp.vram, 0x0040U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0042U) == 0xA55AU,
        "a one-word Word RAM DMA stops vdp_dma_unsupported and writes nothing");
    m->stop.stop_class = SCD_STOP_NONE;
  }
  { /* work RAM and BIOS sources are not delayed: every word lands from the start, the source advances by the full length */
    static const uint16_t src[4] = {0x9101U, 0x9202U, 0x9303U, 0x9404U};
    fresh(0x54U, 0x00U);
    {
      uint32_t i;
      for (i = 0U; i < 4U; ++i) {
        m->work_ram[2U * i] = (uint8_t)(src[i] >> 8);
        m->work_ram[2U * i + 1U] = (uint8_t)src[i];
      }
    }
    sentinels();
    w = arm(4U, 0xFF0000U, 2U, 0x4000U | 0x0100U, 0x0080U, 0);
    row(w == 164U && target_word(host()->devices.vdp.vram, 0x0100U) == 0x9101U && target_word(host()->devices.vdp.vram, 0x0102U) == 0x9202U &&
            target_word(host()->devices.vdp.vram, 0x0104U) == 0x9303U && target_word(host()->devices.vdp.vram, 0x0106U) == 0x9404U &&
            target_word(host()->devices.vdp.vram, 0x0108U) == 0xA55AU && reg(21) == 0x04U && reg(22) == 0x80U && reg(23) == 0x7FU,
        "work RAM source (no delay): all four words land from the start, 4 * 3420 / 83 = 164, source advanced by four");
    fresh(0x54U, 0x00U);
    sentinels();
    memset(bios, 0, sizeof bios);
    bios[0x40] = 0x7AU; bios[0x41] = 0x01U; bios[0x42] = 0x7BU; bios[0x43] = 0x02U; bios[0x44] = 0x7CU; bios[0x45] = 0x03U;
    bios[0x46] = 0x7DU; bios[0x47] = 0x04U;
    w = arm(4U, 0x000040U, 2U, 0x4000U | 0x0200U, 0x0080U, 0);
    row(w == 164U && target_word(host()->devices.vdp.vram, 0x0200U) == 0x7A01U && target_word(host()->devices.vdp.vram, 0x0202U) == 0x7B02U &&
            target_word(host()->devices.vdp.vram, 0x0204U) == 0x7C03U && target_word(host()->devices.vdp.vram, 0x0206U) == 0x7D04U &&
            reg(21) == 0x24U,
        "BIOS source (no delay): all four words land from the start, source advanced by four");
  }

  { /* the ownership and range guards are unchanged: a Word RAM source the main processor cannot read as plain memory is a typed stop */
    static const uint16_t src[2] = {0x1357U, 0x2468U};
    uint32_t k;
    for (k = 0U; k < 3U; ++k) {
      static const char *const names[3] = {"sub-owned 2M Word RAM", "the 1M cell image ($220000)", "a 2M source crossing the end of Word RAM ($240000)"};
      fresh(0x54U, 0x00U);
      m->ga.mode = k == 0U ? 0U : k == 1U ? (SCD_MODE_1M | SCD_MODE_RET) : SCD_MODE_RET;
      fill_word_ram(0x0000U, src, 2U);
      sentinels();
      w = arm(4U, k == 2U ? 0x23FFFCU : k == 1U ? 0x220000U : 0x200000U, 2U, 0x4000U | 0x0040U, 0x0080U, 0);
      snprintf(label, sizeof label, "Word RAM DMA from %s stops vdp_dma_unsupported and writes nothing", names[k]);
      row(w == UINT64_MAX && m->stop.stop_class == SCD_STOP_VDP_DMA_UNSUPPORTED && m->vdp_dma_count == 0U &&
              target_word(host()->devices.vdp.vram, 0x0042U) == 0xA55AU,
          label);
      m->stop.stop_class = SCD_STOP_NONE;
    }
  }

  /* ---- SEG-103-T002/T003 (ADR 0238, 0239): VDP DMA from the 1M-mode Word RAM bank assigned to main ---------------------------------------
   * The oracle is written from the contract's word interleave, independently of scd_bank_offset: bank b word i is 2M-space word 2 * i + b.
   * A linear read of the 256 KiB array (the naive mapping) gives other words, which the rows below also assert. */
  {
    uint32_t i;
    for (i = 0U; i < 0x10000U; ++i) { /* distinct, position-dependent words in each bank: neither bank equals the other or the linear array */
      b0[i] = (uint16_t)(0x1000U + i * 7U);
      b1[i] = (uint16_t)(0x8000U ^ (i * 13U + 5U));
    }
    {
      uint32_t bank, shape;
      for (bank = 0U; bank < 2U; ++bank) {
        const uint16_t *const want = bank ? b1 : b0, *const other = bank ? b0 : b1;
        for (shape = 0U; shape < 2U; ++shape) {
          const uint32_t first = 0x0120U + 0x20U * shape;
          fresh_banks(0x54U, 0x00U);
          m->ga.mode = (uint8_t)(SCD_MODE_1M | (bank ? SCD_MODE_RET : 0U)); /* main sees bank RET */
          sentinels();
          w = arm(4U, 0x200010U, 2U, 0x4000U | first, 0x0080U, (int)shape); /* nonzero source offset (word 8) and destination */
          snprintf(label, sizeof label, "1M bank %u, %s command: duration of 3 words (123), got %llu", bank, shape ? "32-bit" : "16-bit", (unsigned long long)w);
          row(w == 123U, label);
          row(target_word(host()->devices.vdp.vram, first) == 0xA55AU && target_word(host()->devices.vdp.vram, first + 2U) == want[8] &&
                  target_word(host()->devices.vdp.vram, first + 4U) == want[9] && target_word(host()->devices.vdp.vram, first + 6U) == want[10] &&
                  target_word(host()->devices.vdp.vram, first + 8U) == 0xA55AU,
              "the selected bank's words 8-10 land after the skipped first destination word; the word after keeps its value");
          row(want[8] != other[8] && want[8] != target_word(m->word_ram, 2U * 8U) /* the linear array's word 8 */ &&
                  target_word(host()->devices.vdp.vram, first + 2U) != target_word(m->word_ram, 0x10U),
              "the oracle distinguishes the bank interleave from the other bank and from a linear read of the array");
          row(reg(19) == 0U && reg(20) == 0U && reg(21) == 0x0BU && reg(22) == 0x00U && reg(23) == 0x10U && m->vdp_dma_count == 1U,
              "registers: length 0, source word address $100008 advanced by three, #23 unchanged");
          m->cpu[SCD_MAIN].next = m->now;
          scd_retire(m, SCD_MAIN, 8U);
          row(m->cpu[SCD_MAIN].next == m->now + 123U * MASTER + 8U * SCD_MAIN_CYCLE_TICKS && m->vdp_dma_stall_ticks == 123U * MASTER,
              "the processor stalls for the 3-word transfer");
        }
      }
      /* successive transfers across a RET change and a 2M -> 1M -> 2M mode change on one machine (no stale staged data) */
      fresh_banks(0x54U, 0x00U);
      sentinels();
      m->ga.mode = SCD_MODE_1M;
      arm(3U, 0x200100U, 2U, 0x4000U | 0x0400U, 0x0080U, 0);
      row(target_word(host()->devices.vdp.vram, 0x0402U) == b0[0x80] && target_word(host()->devices.vdp.vram, 0x0404U) == b0[0x81], "bank 0: two words");
      m->ga.mode = SCD_MODE_1M | SCD_MODE_RET;
      arm(3U, 0x200100U, 2U, 0x4000U | 0x0500U, 0x0080U, 0);
      row(target_word(host()->devices.vdp.vram, 0x0502U) == b1[0x80] && target_word(host()->devices.vdp.vram, 0x0504U) == b1[0x81], "RET switched: bank 1's words");
      m->ga.mode = SCD_MODE_RET; /* back to 2M: the linear array, through the unchanged 2M path */
      arm(3U, 0x200100U, 2U, 0x4000U | 0x0600U, 0x0080U, 0);
      row(target_word(host()->devices.vdp.vram, 0x0602U) == target_word(m->word_ram, 0x100U) && target_word(host()->devices.vdp.vram, 0x0604U) == target_word(m->word_ram, 0x102U) &&
              target_word(host()->devices.vdp.vram, 0x0602U) != b0[0x80] && target_word(host()->devices.vdp.vram, 0x0602U) != b1[0x80],
          "1M -> 2M: the 2M transfer reads the linear array again (not the staged copy)");
      row(m->vdp_dma_count == 3U, "three transfers counted");
      /* the staged copy is a snapshot taken at the arming instant: a later Word RAM write does not change the destination */
      m->ga.mode = SCD_MODE_1M;
      arm(3U, 0x200100U, 2U, 0x4000U | 0x0700U, 0x0080U, 0);
      m->word_ram[2U * (2U * 0x80U)] ^= 0xFFU;
      row(target_word(host()->devices.vdp.vram, 0x0702U) == b0[0x80], "a Word RAM write after the transfer does not change the transferred VRAM");
      m->word_ram[2U * (2U * 0x80U)] ^= 0xFFU;
      /* 128 KiB window wrap: from the last words of the bank to its first (not to $220000, not to the other bank) */
      fresh_banks(0x54U, 0x00U);
      sentinels();
      m->ga.mode = SCD_MODE_1M | SCD_MODE_RET;
      w = arm(8U, 0x21FFF8U, 2U, 0x4000U | 0x0200U, 0x0080U, 0); /* 7 words after the delay: bank words $FFFC-$FFFF then 0-2 */
      row(w != UINT64_MAX && target_word(host()->devices.vdp.vram, 0x0200U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0202U) == b1[0xFFFC] &&
              target_word(host()->devices.vdp.vram, 0x0204U) == b1[0xFFFD] && target_word(host()->devices.vdp.vram, 0x0206U) == b1[0xFFFE] &&
              target_word(host()->devices.vdp.vram, 0x0208U) == b1[0xFFFF] && target_word(host()->devices.vdp.vram, 0x020AU) == b1[0] &&
              target_word(host()->devices.vdp.vram, 0x020CU) == b1[1] && target_word(host()->devices.vdp.vram, 0x020EU) == b1[2] &&
              target_word(host()->devices.vdp.vram, 0x0210U) == 0xA55AU,
          "window wrap: the words after the last bank word are the bank's first words (bank 1)");
      row(reg(19) == 0U && reg(20) == 0U && reg(21) == 0x03U && reg(22) == 0x00U && reg(23) == 0x10U,
          "after the wrap the source registers are $0003 (16-bit), #23 unchanged");
      /* length 0 = 65536 words: 65535 transferred, the source window walked once round (VRAM overwritten once) */
      fresh_banks(0x54U, 0x00U);
      sentinels();
      m->ga.mode = SCD_MODE_1M;
      w = arm(0U, 0x200000U, 2U, 0x4000U | 0x0000U, 0x0080U, 0);
      row(w != UINT64_MAX && m->vdp_dma_count == 1U && reg(19) == 0U && reg(20) == 0U && reg(21) == 0xFFU && reg(22) == 0xFFU &&
              target_word(host()->devices.vdp.vram, 0x0002U) == b0[32768U] && target_word(host()->devices.vdp.vram, 0x0000U) == b0[32767U] &&
              target_word(host()->devices.vdp.vram, 0xFFFEU) == b0[65534U],
          "1M length 0 (65536 words): 65535 words from the bank, VRAM overwritten once, the source advanced by $FFFF words");
      /* increment 4 and CRAM / VSRAM destinations */
      fresh_banks(0x54U, 0x00U);
      sentinels();
      m->ga.mode = SCD_MODE_1M | SCD_MODE_RET;
      w = arm(4U, 0x200020U, 4U, 0x4000U | 0x0300U, 0x0080U, 0);
      row(w == 123U && target_word(host()->devices.vdp.vram, 0x0300U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0304U) == b1[16] &&
              target_word(host()->devices.vdp.vram, 0x0308U) == b1[17] && target_word(host()->devices.vdp.vram, 0x030CU) == b1[18] &&
              target_word(host()->devices.vdp.vram, 0x0302U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0310U) == 0xA55AU,
          "1M increment 4: the skipped word advances the destination by 4");
      fresh_banks(0x54U, 0x00U);
      sentinels();
      m->ga.mode = SCD_MODE_1M;
      w = arm(4U, 0x200400U, 2U, 0xC000U | 0x0010U, 0x0080U, 0);
      row(w == 63U && target_word(host()->devices.vdp.cram, 0x10U) == 0x0ACEU && target_word(host()->devices.vdp.cram, 0x12U) == b0[0x200] &&
              target_word(host()->devices.vdp.cram, 0x14U) == b0[0x201] && target_word(host()->devices.vdp.cram, 0x16U) == b0[0x202],
          "1M CRAM destination: first entry skipped, the bank's words follow, 63");
      fresh_banks(0x54U, 0x00U);
      sentinels();
      m->ga.mode = SCD_MODE_1M;
      w = arm(4U, 0x200400U, 2U, 0x4000U | 0x0004U, 0x0090U, 0);
      row(w == 63U && target_word(host()->devices.vdp.vsram, 0x04U) == 0x0123U && target_word(host()->devices.vdp.vsram, 0x06U) == b0[0x200] &&
              target_word(host()->devices.vdp.vsram, 0x08U) == b0[0x201] && target_word(host()->devices.vdp.vsram, 0x0AU) == b0[0x202],
          "1M VSRAM destination: first entry skipped, the bank's words follow");
      /* timing: the same shapes as the 2M path (the source class does not change the slot model) at several phases / widths */
      {
        static const struct { uint32_t reg1, reg12; uint64_t offset; uint32_t words; } shapes[] = {
            {0x54U, 0x00U, 0U, 101U}, {0x54U, 0x00U, 48U * LINE, 101U}, {0x54U, 0x01U, 48U * LINE + 1710U, 101U}, {0x54U, 0x81U, 250U * LINE, 2001U},
            {0x14U, 0x00U, 100U * LINE, 167U}, {0x54U, 0x00U, 37U * LINE, 5U}};
        uint32_t k;
        for (k = 0U; k < sizeof shapes / sizeof shapes[0]; ++k) {
          uint64_t w1m, w2m;
          fresh_banks(shapes[k].reg1, shapes[k].reg12);
          arm_offset = shapes[k].offset;
          m->ga.mode = SCD_MODE_1M;
          w1m = arm(shapes[k].words, 0x200000U, 2U, 0x4000U | 0x0080U, 0x0080U, 0);
          fresh_banks(shapes[k].reg1, shapes[k].reg12);
          m->ga.mode = SCD_MODE_RET;
          w2m = arm(shapes[k].words, 0x200000U, 2U, 0x4000U | 0x0080U, 0x0080U, 0);
          arm_offset = 0U;
          snprintf(label, sizeof label, "timing shape %u: 1M duration equals the 2M duration of the same lagged length (%llu / %llu)", k,
                   (unsigned long long)w1m, (unsigned long long)w2m);
          row(w1m != UINT64_MAX && w1m == w2m && w1m != 0U, label);
        }
        fresh_banks(0x54U, 0x00U);
        arm_offset = 48U * LINE;
        m->ga.mode = SCD_MODE_1M;
        w = arm(101U, 0x200000U, 2U, 0x4000U | 0x0080U, 0x0080U, 0);
        arm_offset = 0U;
        row(w == 42750U, "1M, H32 active line 10, 100 words after the delay: 42750 (the hand-derived value of the 2M table)");
      }
      /* the observer of the unmodelled hazard (ADR 0238 question 11): a sub-side RET change while a 1M-bank transfer waits is counted, not modelled */
      fresh_banks(0x54U, 0x00U);
      sentinels();
      m->ga.mode = SCD_MODE_1M;
      w = arm(4U, 0x200000U, 2U, 0x4000U | 0x0040U, 0x0080U, 0);
      row(w == 123U && m->vdp_dma_bank_count == 1U && m->main_dma_bank_until == m->main_dma_until && m->wordram_mode_changes_in_dma == 0U,
          "a 1M-bank transfer is counted and its wait end recorded");
      row(host()->owned_region_count == 1U && host()->owned_regions[0].begin == 0U && host()->owned_regions[0].end == m->bios_size,
          "after the transfer the host's regions are the default again (the staged bank copy is gone)");
      m->now += 1U; /* the sub processor's instruction inside the wait: RET 0 -> 1 */
      scd_word_ram_sub_mode_write(m, SCD_MODE_1M | SCD_MODE_RET, 0x1000U);
      row(m->wordram_mode_changes_in_dma == 1U, "a RET change by the sub inside the wait is counted");
      m->now = m->main_dma_bank_until + 1U; /* after the wait */
      scd_word_ram_sub_mode_write(m, SCD_MODE_1M, 0x1000U);
      row(m->wordram_mode_changes_in_dma == 1U, "a RET change after the wait is not");
      fresh_banks(0x54U, 0x00U);
      m->ga.mode = SCD_MODE_RET;
      arm(4U, 0x200000U, 2U, 0x4000U | 0x0040U, 0x0080U, 0);
      row(m->vdp_dma_bank_count == 0U && m->main_dma_bank_until == 0U, "a 2M transfer is not a bank transfer");
      /* a CDC transfer in flight into the bank that main now sees: the writer was latched on the sub's bank, the sub then flipped RET, so the
       * bank main reads is the one the transfer is filling. The DMA arms at an instant inside the transfer: scd_cd_observe must bring the
       * transfer up to that instant first, so exactly the bytes moved so far are in the staged words. */
      {
        static const ScdDiscTrack tracks[2] = {{1U, 0U, 0U, 2048U, 0U, 0U, 40U}, {2U, 2U, 1U, 2352U, 0U, 190U, 75U}};
        static ScdDisc disc;
        const uint64_t bt = SCD_DMA_SUB_CYCLES_PER_BYTE * SCD_SUB_CYCLE_TICKS;
        uint32_t v, ga;
        fresh_banks(0x54U, 0x00U);
        sentinels();
        disc.tracks = tracks;
        disc.track_count = 2U;
        m->disc = &disc;
        m->ga.ctl = 0x02U;
        m->ga.sub_halted = 1U;
        m->ga.prg_window = 1U;
        m->ga.mode = SCD_MODE_1M; /* RET 0: main sees bank 0, the sub (and the CDC writer) bank 1 */
        scd_devices_power_on(m);
        for (v = 0U; v < 32U; ++v) m->cdc.buffer[v] = (uint8_t)(0xA0U + v);
        m->now = 1000U;
        {
          static const uint8_t seq[][2] = {{0x04U, 0x07U}, {0x05U, 0x01U}, {0x07U, 0x02U}, {0x07U, 0x0FU}, {0x07U, 0x00U}, {0x07U, 0x00U}, {0x07U, 0x00U}, {0x07U, 0x00U}};
          for (v = 0U; v < sizeof seq / sizeof seq[0]; ++v) {
            ga = seq[v][1];
            scd_ga_sub_access(m, seq[v][0], 1U, 1, &ga, 0x6000U);
          }
        }
        row(m->cdc.dma_state == SCD_DMA_RUNNING && m->cdc.dma_target == SCD_DMA_TARGET_BANK1, "setup: a CDC transfer of 16 bytes runs into the sub's bank (bank 1)");
        ga = SCD_MODE_1M | SCD_MODE_RET; /* the sub flips RET: main now sees bank 1, the writer keeps its latched target */
        scd_ga_sub_access(m, 0x03U, 1U, 1, &ga, 0x6000U);
        m->stop.stop_class = SCD_STOP_NONE;
        row((m->ga.mode & SCD_MODE_RET) != 0U && m->cdc.dma_target == SCD_DMA_TARGET_BANK1, "setup: main's bank is the transfer's target");
        w = arm_at_tick(m->now + 8U * bt, 6U, 0x200000U, 2U, 0x4000U | 0x0200U, 0x0080U);
        row(w != UINT64_MAX && m->cdc.dma_done == 8U && target_word(host()->devices.vdp.vram, 0x0200U) == 0xA55AU &&
                target_word(host()->devices.vdp.vram, 0x0202U) == 0xA0A1U && target_word(host()->devices.vdp.vram, 0x0204U) == 0xA2A3U &&
                target_word(host()->devices.vdp.vram, 0x0206U) == 0xA4A5U && target_word(host()->devices.vdp.vram, 0x0208U) == 0xA6A7U &&
                target_word(host()->devices.vdp.vram, 0x020AU) == b1[4],
            "a 1M transfer armed inside the CDC transfer sees exactly the 8 bytes moved so far (scd_cd_observe runs first); the rest is the old bank");
      }
      /* the 1M deadline-independent behavior: staging is synchronous, so a DMA armed one instant later gives the same bytes and duration */
      /* refusals: length one, the cell image, the mirrors; nothing is mutated */
      {
        static const struct { uint32_t source, length; const char *name; } refused[] = {
            {0x200000U, 1U, "length one"}, {0x220000U, 4U, "the cell image page ($220000)"}, {0x23FFF0U, 4U, "the end of the cell image page"},
            {0x240000U, 4U, "the first mirror page ($240000)"}, {0x3FFFF0U, 4U, "the last mirror page"}};
        uint32_t k;
        for (k = 0U; k < sizeof refused / sizeof refused[0]; ++k) {
          uint16_t before19, before21;
          fresh_banks(0x54U, 0x00U);
          sentinels();
          m->ga.mode = SCD_MODE_1M;
          w = arm(refused[k].length, refused[k].source, 2U, 0x4000U | 0x0040U, 0x0080U, 0);
          before19 = (uint16_t)reg(19);
          before21 = (uint16_t)reg(21);
          snprintf(label, sizeof label, "1M DMA from %s is refused: vdp_dma_unsupported, no registers, VRAM, stall or arm state changed", refused[k].name);
          row(w == UINT64_MAX && m->stop.stop_class == SCD_STOP_VDP_DMA_UNSUPPORTED && m->vdp_dma_count == 0U && m->main_dma_until == 0U &&
                  before19 == (refused[k].length & 0xFFU) && before21 == (((refused[k].source >> 1) & 0xFFU)) && reg(23) == (((refused[k].source >> 17) & 0x7FU)) &&
                  target_word(host()->devices.vdp.vram, 0x0040U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0042U) == 0xA55AU,
              label);
          m->stop.stop_class = SCD_STOP_NONE;
        }
      }
    }
  }
  { /* length 0 is 65536 words: the lagged transfer is 65535 words (registers #19/#20 = $FF/$FF before the transfer) */
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET;
    sentinels();
    w = arm(0U, 0x200000U, 2U, 0x4000U | 0x0000U, 0x0080U, 0);
    row(w != UINT64_MAX && m->vdp_dma_count == 1U && reg(19) == 0U && reg(20) == 0U && reg(21) == 0xFFU && reg(22) == 0xFFU && reg(23) == 0x10U &&
            target_word(host()->devices.vdp.vram, 0x0002U) == 0x0000U, /* VRAM wraps twice: word 1 is overwritten by source word 32768 (zero) */
        "length 0 (65536 words) from $200000: 65535 words transferred and the source advanced by $FFFF words");
  }
  { /* a 32-bit write whose high word completes the command (the VDP awaits the second word) and whose low word is a register write */
    static const uint16_t src[4] = {0x0C01U, 0x0C02U, 0x0C03U, 0x0C04U};
    fresh(0x54U, 0x00U);
    m->ga.mode = SCD_MODE_RET;
    fill_word_ram(0x0800U, src, 4U);
    sentinels();
    vdp(2U, 0x8F02U);
    vdp(2U, 0x9304U);
    vdp(2U, 0x9400U);
    vdp(2U, 0x9500U | (((0x200800U >> 1) & 0xFFU)));
    vdp(2U, 0x9600U | (((0x200800U >> 1) >> 8) & 0xFFU));
    vdp(2U, 0x9700U | (((0x200800U >> 1) >> 16) & 0x7FU));
    m->now = m->vdp_onset - FRAME;
    vdp(2U, 0x4000U | 0x0600U);       /* first command word */
    vdp(4U, (0x0080U << 16) | 0x8F04U); /* second word + register #15 = 4 written after the transfer (GPGX cached write) */
    row(m->vdp_dma_count == 1U && target_word(host()->devices.vdp.vram, 0x0600U) == 0xA55AU && target_word(host()->devices.vdp.vram, 0x0602U) == 0x0C01U &&
            target_word(host()->devices.vdp.vram, 0x0604U) == 0x0C02U && target_word(host()->devices.vdp.vram, 0x0606U) == 0x0C03U &&
            target_word(host()->devices.vdp.vram, 0x0608U) == 0xA55AU && reg(15) == 0x04U,
        "32-bit write completing a pending command: the destination skips the first word; the low word's register write lands after the transfer");
  }

  scd_vdp_release(m);
  free(m);
  printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
