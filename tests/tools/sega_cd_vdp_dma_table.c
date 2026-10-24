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

  scd_vdp_release(m);
  free(m);
  printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
