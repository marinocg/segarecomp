/* SEG-065-T003 (ADR 0146 decision 5, ADR 0147 decision 5): frame capture at a vertical-blank onset in the BIOS-image profile.
 * A VDP host with project-authored state (Mode 5, H32, empty VRAM, so every pixel is the backdrop) is driven through the machine's own
 * device events; expectations are derived by hand from the canonical form: per pixel (mode << 9) | BBBGGGRRR, mode 1 (normal).
 *   - backdrop CRAM word $0A64 (----BBB-GGG-RRR- = B 5, G 3, R 2) -> 9-bit $15A -> canonical $035A;
 *   - CRAM rewritten to $0E00 (B 7) just before line 100 starts -> rows 100-223 read $03C0, rows 0-99 $035A (per-line sampling);
 *   - shadow/highlight or the palette-select mode on any line -> vdp_state_unsupported, no file;
 *   - a capture whose frame had already begun when it was requested -> lines_incomplete.
 * usage: sega_cd_frame_capture_table <scratch-dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_runtime.h"

static int failures;
static ScdMachine *m;
static uint8_t bios[0x20000];
static const char *dir;

#define MASTER UINT64_C(500000)
#define LINE (UINT64_C(3420) * MASTER)
#define FRAME (LINE * 262U)

static void row(int ok, const char *label) {
  printf("%s %s\n", ok ? "ok   " : "FAIL ", label);
  if (!ok) ++failures;
}

static void port(uint32_t address, uint32_t width, uint32_t value) {
  if (scd_write(m, SCD_MAIN, address, width, value, 0xFF0100U) != 0) {
    printf("      stop %s\n", scd_stop_class_name(m->stop.stop_class));
    m->stop.stop_class = SCD_STOP_NONE;
  }
}

static void fresh(uint32_t reg0, uint32_t reg12) {
  if (m->vdp_host != NULL) scd_vdp_release(m);
  memset(m, 0, sizeof *m);
  m->bios = bios;
  m->bios_size = sizeof bios;
  m->cpu[SCD_MAIN].cycle_ticks = SCD_MAIN_CYCLE_TICKS;
  if (scd_vdp_init(m) != 0) exit(2);
  port(0xC00004U, 2U, 0x8000U | reg0);
  port(0xC00004U, 2U, 0x8144U); /* display on, Mode 5 */
  port(0xC00004U, 2U, 0x8701U); /* backdrop: palette 0 entry 1 */
  port(0xC00004U, 2U, 0x8C00U | reg12);
  port(0xC00004U, 2U, 0x8F02U);
  port(0xC00004U, 4U, 0xC0020000U); /* CRAM entry 1 */
  port(0xC00000U, 2U, 0x0A64U);
}

static void run_until(uint64_t limit) {
  for (;;) {
    const uint64_t t = scd_devices_next_event(m);
    if (t > limit) break;
    m->now = t;
    scd_devices_advance(m, t);
  }
  m->now = limit;
}

static char *report(void) {
  static char text[4096];
  FILE *f = tmpfile();
  size_t n;
  if (f == NULL) exit(2);
  scd_vdp_capture_report(m, f);
  rewind(f);
  n = fread(text, 1U, sizeof text - 1U, f);
  text[n] = '\0';
  fclose(f);
  return text;
}

static int load(const char *name, uint64_t onset, uint8_t *out, size_t size, size_t *got) {
  char path[1024];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s_%llu.bin", dir, name, (unsigned long long)onset);
  f = fopen(path, "rb");
  if (f == NULL) return 0;
  *got = fread(out, 1U, size, f);
  fclose(f);
  return 1;
}

int main(int argc, char **argv) {
  static uint8_t canon[4U + 2U * 320U * 448U];
  size_t n = 0U;
  if (argc != 2) return 2;
  dir = argv[1];
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (m == NULL) return 2;

  /* the frame whose display ends at onset 3: frame index 2, line L sampled at 2 * FRAME + L * LINE */
  fresh(0x04U, 0x00U);
  row(scd_vdp_capture_request(m, 3U, dir) == 0, "capture request accepted (onset 3)");
  row(scd_vdp_capture_request(m, 1U, dir) == -1, "onset 1 is refused (no complete frame before it)");
  run_until(2U * FRAME + 100U * LINE - 1U);
  port(0xC00004U, 4U, 0xC0020000U);
  port(0xC00000U, 2U, 0x0E00U); /* just before line 100 begins */
  run_until(3U * FRAME);
  row(strstr(report(), "\"onset\":3,\"status\":\"written\",\"width\":256,\"height\":224,\"vram_display_changes\":0") != NULL,
      "report: onset 3 written, 256 x 224, no VRAM change during the display");
  if (load("canon", 3U, canon, sizeof canon, &n)) {
    int rows_ok = 1;
    unsigned y, x;
    row(n == 4U + 2U * 256U * 224U && canon[0] == 0x00U && canon[1] == 0x01U && canon[2] == 0xE0U && canon[3] == 0x00U,
        "canonical file: u16 LE width 256, height 224, then 57344 pixels");
    for (y = 0; y < 224U && n == 4U + 2U * 256U * 224U; ++y)
      for (x = 0; x < 256U; ++x) {
        const size_t k = 4U + 2U * ((size_t)y * 256U + x);
        const unsigned v = (unsigned)canon[k] | ((unsigned)canon[k + 1U] << 8);
        if (v != (y < 100U ? 0x035AU : 0x03C0U)) rows_ok = 0;
      }
    row(rows_ok, "pixels: rows 0-99 = $035A (backdrop $0A64, normal), rows 100-223 = $03C0 (the CRAM change before line 100)");
  } else {
    row(0, "canonical file written");
  }
  {
    static uint8_t vdp[48U + 128U + 80U + 65536U];
    size_t k = 0U;
    row(load("vdp", 3U, vdp, sizeof vdp, &k) && k == sizeof vdp && vdp[2U * 12U + 1U] == 0x00U && vdp[48U + 2U] == 0x0EU && vdp[48U + 3U] == 0x00U,
        "VDP state file: registers (big-endian words), CRAM entry 1 = $0E00 at the onset, VSRAM, VRAM");
  }

  /* refusals */
  fresh(0x04U, 0x08U); /* shadow/highlight */
  scd_vdp_capture_request(m, 3U, dir);
  run_until(3U * FRAME);
  row(strstr(report(), "\"status\":\"vdp_state_unsupported\"") != NULL, "shadow/highlight on the captured lines: vdp_state_unsupported");
  fresh(0x00U, 0x00U); /* register #0 bit 2 clear: palette-select mode */
  scd_vdp_capture_request(m, 3U, dir);
  run_until(3U * FRAME);
  row(strstr(report(), "\"status\":\"vdp_state_unsupported\"") != NULL, "palette-select mode: vdp_state_unsupported");
  fresh(0x04U, 0x00U);
  run_until(2U * FRAME + 10U * LINE);
  scd_vdp_capture_request(m, 3U, dir); /* its frame began before the request */
  run_until(3U * FRAME);
  row(strstr(report(), "\"status\":\"lines_incomplete\"") != NULL, "a frame already in progress at the request: lines_incomplete");

  scd_vdp_release(m);
  free(m);
  printf("%d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
