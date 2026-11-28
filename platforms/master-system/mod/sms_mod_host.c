/* SEG-147-T004/T006 (ADR 0325): see sms_mod_host.h. Strict C11; static storage; no clock, no host RNG. */
#include "sms_mod_host.h"

#include <stdlib.h>
#include <string.h>

#include "sms_mod_scheduler.h"

/* The build's generated artifact (sms_mod_artifact.c). */
extern const char segarecomp_sms_mod_identity[];         /* package identity, 64 hex */
extern const char segarecomp_sms_mod_effective_sha256[]; /* the effective cartridge image, 64 hex */
extern const char segarecomp_sms_mod_id[];
extern const char segarecomp_sms_mod_capabilities[];     /* granted catalogue names, comma separated */
extern const unsigned long long segarecomp_sms_mod_caps_required[2], segarecomp_sms_mod_caps_optional[2];
extern const unsigned long segarecomp_sms_mod_state_schema;
extern const unsigned long segarecomp_sms_mod_range_count;
extern const unsigned long segarecomp_sms_mod_ranges[][4]; /* space, offset, length, modes */
/* Defined by the artifact (native: every phase) or by the Luau adapter (the callbacks the script defines). Valid after `segarecomp_mod_provider`. */
extern uint32_t segarecomp_sms_mod_phase_mask(void);
extern int segarecomp_sms_mod_nondeterministic(void);

typedef struct Region { uint32_t offset, length; } Region;

static SmsModScheduler scheduler;
static SmsModState state;
static SmsRenderer *frame_renderer;
static SmsMachine *frame_machine;
static int attached;

static int observing;
static Region regions[SMS_MOD_OBSERVE_REGIONS];
static uint32_t region_count, region_bytes, sample_count;
static uint8_t samples[SMS_MOD_OBSERVE_FRAMES][SMS_MOD_OBSERVE_BYTES];
static uint8_t frame_digests[SMS_MOD_OBSERVE_FRAMES][32];
static SmsSha256 stream;
static uint64_t presented_frames, active_frames;
static uint8_t presented[SMS_MOD_RGB_MAX_BYTES];
static uint32_t presented_height;
static uint8_t overlay_frame[SMS_MOD_RGB_MAX_BYTES];

static int hex_digit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int parse_sha256(const char *text, uint8_t out[32]) {
  unsigned i;
  for (i = 0U; i < 32U; ++i) {
    const int hi = hex_digit(text[2U * i]), lo = hi < 0 ? -1 : hex_digit(text[2U * i + 1U]);
    if (hi < 0 || lo < 0) return 0;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return text[64] == '\0';
}

static void put_hex(FILE *out, const uint8_t *bytes, uint32_t n) {
  uint32_t i;
  for (i = 0U; i < n; ++i) fprintf(out, "%02x", (unsigned)bytes[i]);
}

static void put_json_text(FILE *out, const char *text, uint32_t n) {
  uint32_t i;
  for (i = 0U; i < n; ++i) {
    if (text[i] == '"' || text[i] == '\\') fputc('\\', out);
    fputc(text[i] >= 0x20 && text[i] <= 0x7E ? text[i] : '?', out);
  }
}

static void log_sink(void *context, const char *line, uint32_t length) {
  (void)context;
  fputs("MOD_LOG {\"line\":\"", stderr);
  put_json_text(stderr, line, length);
  fputs("\"}\n", stderr);
}

static void refuse(const char *cls) { fprintf(stderr, "MOD_REFUSED {\"class\":\"%s\"}\n", cls); }

static void frame_done(void *context, uint64_t frame) {
  const SmsModPresentation *p;
  uint32_t height, i, at = 0U;
  uint8_t sha_digest[32];
  SmsSha256 sha;
  (void)context;
  if (!observing) return;
  if (sample_count < SMS_MOD_OBSERVE_FRAMES && frame == sample_count) {
    for (i = 0U; i < region_count; ++i) {
      memcpy(&samples[sample_count][at], &frame_machine->mem.ram[regions[i].offset], regions[i].length);
      at += regions[i].length;
    }
  }
  height = frame_renderer->last_height;
  memset(sha_digest, 0, sizeof sha_digest);
  if (height != 0U) {
    p = sms_mod_scheduler_presentation(&scheduler);
    if (p != NULL && !sms_mod_present_is_empty(p)) ++active_frames;
    sms_mod_present_compose(p, frame_renderer->last, height, presented);
    presented_height = height;
    sms_sha256_init(&sha);
    sms_sha256_update(&sha, presented, (size_t)SMS_FB_WIDTH * height * 3U);
    sms_sha256_final(&sha, sha_digest);
    sms_sha256_update_u64(&stream, frame);
    sms_sha256_update(&stream, sha_digest, sizeof sha_digest);
    ++presented_frames;
  }
  if (sample_count < SMS_MOD_OBSERVE_FRAMES && frame == sample_count) {
    memcpy(frame_digests[sample_count], sha_digest, 32U);
    ++sample_count;
  }
}

int sms_mod_host_attach(SmsMachine *machine, SmsPad *pad, SmsVdp *vdp, SmsRenderer *renderer) {
  static SmsModRange ranges[SMS_MOD_RANGES_MAX];
  SmsModConfig config;
  const char *rejection = "";
  void *provider_state = NULL;
  uint32_t i;
  memset(&config, 0, sizeof config);
  if (!parse_sha256(segarecomp_sms_mod_identity, config.identity_sha256) || segarecomp_sms_mod_range_count > SMS_MOD_RANGES_MAX || renderer == NULL) {
    refuse("E_PROVIDER_ENTRY");
    return 1;
  }
  config.provider = segarecomp_mod_provider(&provider_state);
  if (config.provider == NULL) {
    refuse("E_ABI_ENTRY_MISSING");
    return 1;
  }
  config.provider_state = provider_state;
  config.phase_mask = segarecomp_sms_mod_phase_mask();
  config.mod_id = segarecomp_sms_mod_id;
  for (i = 0U; i < 2U; ++i) {
    config.required[i] = segarecomp_sms_mod_caps_required[i];
    config.optional[i] = segarecomp_sms_mod_caps_optional[i];
  }
  for (i = 0U; i < segarecomp_sms_mod_range_count; ++i) {
    ranges[i].space = (uint32_t)segarecomp_sms_mod_ranges[i][0];
    ranges[i].offset = (uint32_t)segarecomp_sms_mod_ranges[i][1];
    ranges[i].length = (uint32_t)segarecomp_sms_mod_ranges[i][2];
    ranges[i].modes = (uint32_t)segarecomp_sms_mod_ranges[i][3];
  }
  config.ranges = ranges;
  config.range_count = (uint32_t)segarecomp_sms_mod_range_count;
  config.state_schema = (uint32_t)segarecomp_sms_mod_state_schema;
  config.log_sink = log_sink;
  config.frame_done = frame_done;
  frame_renderer = renderer;
  frame_machine = machine;
  if (sms_mod_scheduler_attach(&scheduler, machine, pad, vdp, &config, &rejection) != 0) {
    refuse(rejection);
    return 1;
  }
  sms_mod_scheduler_state(&scheduler, &state);
  sms_sha256_init(&stream);
  attached = 1;
  fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"package_identity\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu}\n", scheduler.mod_id, segarecomp_sms_mod_identity, segarecomp_sms_mod_capabilities,
          segarecomp_sms_mod_range_count);
  return 0;
}

const SmsModState *sms_mod_host_state(void) { return attached ? &state : NULL; }

void sms_mod_host_identity(uint8_t source_sha256[32], uint8_t variant_sha256[32]) {
  if (!parse_sha256(segarecomp_sms_mod_effective_sha256, source_sha256)) memset(source_sha256, 0, 32U);
  if (!parse_sha256(segarecomp_sms_mod_identity, variant_sha256)) memset(variant_sha256, 0, 32U);
}

int sms_mod_host_compose(const uint8_t *framebuffer, uint32_t height, uint8_t *rgb) {
  const SmsModPresentation *p = attached ? sms_mod_scheduler_presentation(&scheduler) : NULL;
  if (p == NULL || sms_mod_present_is_empty(p) || framebuffer == NULL || height == 0U || height > SEGA_SMS_SCREEN_MAX_HEIGHT) return 0;
  sms_mod_present_compose(p, framebuffer, height, overlay_frame);
  memcpy(rgb, overlay_frame, (size_t)SMS_FB_WIDTH * height * 3U);
  return 1;
}

static int parse_number(const char **at, uint32_t *out) {
  uint32_t v = 0U;
  const char *p = *at;
  if (*p < '0' || *p > '9') return 0;
  for (; *p >= '0' && *p <= '9'; ++p) {
    if (v > 100000U) return 0;
    v = v * 10U + (uint32_t)(*p - '0');
  }
  *at = p;
  *out = v;
  return 1;
}

int sms_mod_host_observe(const char *list) {
  region_count = 0U;
  region_bytes = 0U;
  while (list != NULL && *list != '\0') {
    Region r;
    if (region_count >= SMS_MOD_OBSERVE_REGIONS || !parse_number(&list, &r.offset) || *list++ != ':' || !parse_number(&list, &r.length) || r.length == 0U || r.offset > SMS_RAM_SIZE ||
        r.length > SMS_RAM_SIZE - r.offset || r.length > SMS_MOD_OBSERVE_BYTES - region_bytes)
      return 0;
    regions[region_count++] = r;
    region_bytes += r.length;
    if (*list == ',') ++list;
    else if (*list != '\0') return 0;
  }
  observing = 1;
  return 1;
}

const uint8_t *sms_mod_host_last_presented(uint32_t *height) {
  if (height != NULL) *height = presented_height;
  return presented_height != 0U ? presented : NULL;
}

void sms_mod_host_summary(FILE *out, const char *extra) {
  uint8_t digest[32];
  SmsSha256 sha;
  uint32_t i;
  if (!attached) return;
  fprintf(out, "MOD_SUMMARY {\"mod\":\"%s\",\"faulted\":%s,\"fault\":\"", scheduler.mod_id, scheduler.fault.faulted ? "true" : "false");
  put_json_text(out, scheduler.fault.text, (uint32_t)strlen(scheduler.fault.text));
  sms_mod_write_log_digest(&scheduler, digest);
  fprintf(out, "\",\"callbacks\":%llu,\"committed_writes\":%llu,\"write_digest\":\"", (unsigned long long)scheduler.callbacks_run, (unsigned long long)scheduler.write_total);
  put_hex(out, digest, 32U);
  fprintf(out, "\",\"nondeterministic\":%s", segarecomp_sms_mod_nondeterministic() ? "true" : "false");
  if (observing) {
    sha = stream; /* finalize a copy */
    sms_sha256_final(&sha, digest);
    fprintf(out, ",\"presented_frames\":%llu,\"active_frames\":%llu,\"presentation_stream_sha256\":\"", (unsigned long long)presented_frames, (unsigned long long)active_frames);
    put_hex(out, digest, 32U);
    sms_sha256_init(&sha);
    sms_sha256_update(&sha, presented, (size_t)SMS_FB_WIDTH * presented_height * 3U);
    sms_sha256_final(&sha, digest);
    fprintf(out, "\",\"last_presented_sha256\":\"");
    put_hex(out, digest, 32U);
    sms_sha256_init(&sha);
    sms_sha256_update(&sha, frame_machine->mem.ram, SMS_RAM_SIZE);
    sms_sha256_final(&sha, digest);
    fprintf(out, "\",\"final_work_ram_sha256\":\"");
    put_hex(out, digest, 32U);
    fprintf(out, "\",\"digests\":[");
    for (i = 0U; i < sample_count; ++i) {
      fprintf(out, "%s\"", i != 0U ? "," : "");
      put_hex(out, frame_digests[i], 32U);
      fputc('"', out);
    }
    fprintf(out, "],\"ram_frames\":[");
    for (i = 0U; region_bytes != 0U && i < sample_count; ++i) {
      fprintf(out, "%s\"", i != 0U ? "," : "");
      put_hex(out, samples[i], region_bytes);
      fputc('"', out);
    }
    fputc(']', out);
  }
  fprintf(out, "%s}\n", extra);
}
