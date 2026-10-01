/* Device wiring with the T004 VDP attached (PSG and pad still fail closed): installs the SMS VDP and writes its
 * artifacts: `vdp.trace` (register writes, status reads, flag events, per-frame VRAM/CRAM write hashes), `vram.bin`,
 * `cram.bin` and `vdp.json` (registers, address/code, latch, buffer, flags, line counter, last line). T005 attaches the
 * deterministic renderer (sms_render.h) to the VDP line hook and adds the frame artifacts: `frames.txt` (one line per
 * completed frame: frame height vdp_trace_entries sha256), `framebuffer.bin` (the last completed frame, 256 x height CRAM
 * bytes, row-major) and `frame.json`. T006/T007 extend or replace this unit. */
#include <stdio.h>
#include <string.h>

#include "sms_render.h"
#include "sms_vdp.h"

#define VDP_TRACE_CAPACITY 65536u

static SmsVdp vdp;
static SmsVdpTraceEntry vdp_trace[VDP_TRACE_CAPACITY];
#define FRAME_RECORD_CAPACITY 4096u
static SmsRenderer renderer;
static SmsFrameRecord frame_records[FRAME_RECORD_CAPACITY];

void sms_install_devices(SmsMachine *machine) {
  sms_vdp_set_trace(&vdp, vdp_trace, VDP_TRACE_CAPACITY);
  sms_vdp_install(machine, &vdp);
  sms_renderer_init(&renderer, &vdp, frame_records, FRAME_RECORD_CAPACITY);
  sms_renderer_attach(&renderer);
}

static void hex32(const uint8_t *bytes, char *out) {
  static const char digits[] = "0123456789abcdef";
  unsigned i;
  for (i = 0; i < 32u; ++i) {
    out[2u * i] = digits[bytes[i] >> 4];
    out[2u * i + 1u] = digits[bytes[i] & 15u];
  }
  out[64] = '\0';
}

static int put(const char *dir, const char *name, const void *data, size_t size) {
  char path[1024];
  FILE *f;
  int ok;
  if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return 0;
  f = fopen(path, "wb");
  if (f == NULL) return 0;
  ok = size == 0 || fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

int sms_write_device_artifacts(SmsMachine *machine, const char *dir) {
  static char text[VDP_TRACE_CAPACITY * 64u];
  static const char *const kinds[6] = {"reg", "status", "flag_frame", "flag_line", "vram", "cram"};
  size_t used = 0;
  uint32_t i;
  int ok = 1;
  char json[768];
  sms_vdp_flush_trace(&vdp, machine->rt.state.cycles);
  for (i = 0; i < vdp.trace_count; ++i) {
    const SmsVdpTraceEntry *e = &vdp_trace[i];
    used += (size_t)snprintf(text + used, sizeof text - used, "%llu %s %u %02X %016llx\n", (unsigned long long)e->cycles,
                             kinds[e->kind % 6u], (unsigned)e->arg, (unsigned)e->byte, (unsigned long long)e->data);
  }
  ok &= put(dir, "vdp.trace", text, used);
  ok &= put(dir, "vram.bin", vdp.vram, sizeof vdp.vram);
  ok &= put(dir, "cram.bin", vdp.cram, sizeof vdp.cram);
  snprintf(json, sizeof json,
           "{\"regs\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],\"address\":%u,\"code\":%u,\"latch\":%u,\"buffer\":%u,"
           "\"frame_pending\":%u,\"sprite_overflow\":%u,\"sprite_collision\":%u,\"line_pending\":%u,"
           "\"line_counter\":%u,\"line\":%u,\"frame\":%llu,\"trace_entries\":%u,\"trace_dropped\":%u}\n",
           vdp.reg[0], vdp.reg[1], vdp.reg[2], vdp.reg[3], vdp.reg[4], vdp.reg[5], vdp.reg[6], vdp.reg[7], vdp.reg[8],
           vdp.reg[9], vdp.reg[10], (unsigned)vdp.address, (unsigned)vdp.code, (unsigned)vdp.latch_set,
           (unsigned)vdp.read_buffer, (unsigned)vdp.frame_pending, (unsigned)vdp.sprite_overflow,
           (unsigned)vdp.sprite_collision, (unsigned)vdp.line_pending, (unsigned)vdp.line_counter, (unsigned)vdp.line,
           (unsigned long long)vdp.frame, vdp.trace_count, vdp.trace_dropped);
  ok &= put(dir, "vdp.json", json, strlen(json));
  {
    static char frames_text[FRAME_RECORD_CAPACITY * 112u];
    char sha[65];
    size_t n = 0;
    for (i = 0; i < renderer.record_count; ++i) {
      const SmsFrameRecord *f = &frame_records[i];
      hex32(f->sha256, sha);
      n += (size_t)snprintf(frames_text + n, sizeof frames_text - n, "%llu %u %u %s\n", (unsigned long long)f->frame, f->height,
                            f->trace_count, sha);
    }
    ok &= put(dir, "frames.txt", frames_text, n);
    ok &= put(dir, "framebuffer.bin", renderer.last, (size_t)SMS_FB_WIDTH * renderer.last_height);
    {
      uint8_t last_sha[32];
      char meta[512];
      sms_renderer_last_sha256(&renderer, last_sha);
      hex32(last_sha, sha);
      snprintf(meta, sizeof meta,
               "{\"width\":%u,\"height\":%u,\"frames_completed\":%llu,\"last_frame\":%llu,\"sha256\":\"%s\","
               "\"records\":%u,\"records_dropped\":%u}\n",
               (unsigned)SMS_FB_WIDTH, (unsigned)renderer.last_height, (unsigned long long)renderer.frames_completed,
               (unsigned long long)renderer.last_frame, renderer.last_height == 0u ? "" : sha, renderer.record_count,
               renderer.record_dropped);
      ok &= put(dir, "frame.json", meta, strlen(meta));
    }
  }
  return ok;
}
