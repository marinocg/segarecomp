/* SEG-009-T005: state-injection driver for the SMS renderer (tests/sms_render_test.py).
 *
 *   sms_render_dump <scenario> <result>
 *
 * Scenario (little endian): VRAM[16384], CRAM[32], registers[11], u32 event count, then events {u16 line, u8 register, u8 value}
 * in non-decreasing line order. Every event is applied before the render of its line (the per-line latching seam of the
 * renderer: a write during line L reaches line L + 1, so the scenario generator emits it for line L + 1).
 * Result: u16 height, u8 overflow, u8 collision, then 256 x height framebuffer bytes; height 0 when nothing was rendered. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_render.h"

static SmsVdp vdp;
static SmsRenderer renderer;
static SmsFrameRecord records[4];

int main(int argc, char **argv) {
  static uint8_t data[SMS_VDP_VRAM_SIZE + SMS_VDP_CRAM_SIZE + 11u + 4u + 4u * 4096u];
  FILE *f;
  size_t size;
  uint32_t count, i, line, next = 0;
  uint8_t header[4];
  const size_t fixed = SMS_VDP_VRAM_SIZE + SMS_VDP_CRAM_SIZE + 11u + 4u;
  if (argc != 3) return 64;
  f = fopen(argv[1], "rb");
  if (f == NULL) return 64;
  size = fread(data, 1, sizeof data, f);
  fclose(f);
  if (size < fixed) return 64;
  count = (uint32_t)data[fixed - 4] | ((uint32_t)data[fixed - 3] << 8) | ((uint32_t)data[fixed - 2] << 16) | ((uint32_t)data[fixed - 1] << 24);
  if (count > 4096u || size != fixed + 4u * count) return 64;
  sms_vdp_reset(&vdp);
  memcpy(vdp.vram, data, SMS_VDP_VRAM_SIZE);
  memcpy(vdp.cram, data + SMS_VDP_VRAM_SIZE, SMS_VDP_CRAM_SIZE);
  memcpy(vdp.reg, data + SMS_VDP_VRAM_SIZE + SMS_VDP_CRAM_SIZE, 11u);
  sms_renderer_init(&renderer, &vdp, records, 4u);
  for (line = 0; line < 224u; ++line) {
    while (next < count) {
      const uint8_t *e = data + fixed + 4u * next;
      if ((uint32_t)(e[0] | (e[1] << 8)) != line) break;
      if (e[2] < SMS_VDP_REGISTERS) vdp.reg[e[2]] = e[3];
      ++next;
    }
    if (sms_vdp_mode(&vdp) == SMS_VDP_MODE_UNSUPPORTED) break;
    sms_render_line(&renderer, &vdp, line, 0);
    if (renderer.frames_completed != 0u) break;
  }
  f = fopen(argv[2], "wb");
  if (f == NULL) return 64;
  header[0] = (uint8_t)(renderer.last_height & 255u);
  header[1] = (uint8_t)(renderer.last_height >> 8);
  header[2] = vdp.sprite_overflow;
  header[3] = vdp.sprite_collision;
  fwrite(header, 1, 4, f);
  if (renderer.last_height != 0u) fwrite(renderer.last, 1, (size_t)SMS_FB_WIDTH * renderer.last_height, f);
  (void)i;
  return fclose(f) == 0 ? 0 : 64;
}
