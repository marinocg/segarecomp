/* SEG-147-T004/T006 (ADR 0325): see sms_mod_host.h. Strict C11; static storage; no clock, no host RNG. */
#include "sms_mod_host.h"

#include <stdlib.h>
#include <string.h>

#include "sms_mod_scheduler.h"
#include "sms_mod_set_artifact.h"

/* The build's generated artifact. */
#ifdef SEGARECOMP_SMS_MOD_SET
extern const char segarecomp_sms_mod_effective_sha256[]; /* the composed cartridge image, 64 hex */
#else
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
#endif

typedef struct Region { uint32_t offset, length; } Region;

#ifdef SEGARECOMP_SMS_MOD_SET
#define MEMBERS_MAX SMS_MOD_SET_MEMBERS_MAX
static SmsModSet set;
#else
#define MEMBERS_MAX 1U
#endif
static SmsModScheduler schedulers[MEMBERS_MAX];
static SegarecompSmsModMemberRow rows[MEMBERS_MAX];
static void *provider_states[MEMBERS_MAX];
static uint32_t member_count, lead; /* `lead`: the one member that draws (a build refuses a set with two) */
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
    p = sms_mod_scheduler_presentation(&schedulers[lead]);
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

/* The build identity a save state is bound to: one mod's package identity, or the set's composite identity. */
static const char *variant_text(void) {
#ifdef SEGARECOMP_SMS_MOD_SET
  return segarecomp_sms_mod_set_identity;
#else
  return segarecomp_sms_mod_identity;
#endif
}

static int draws(const char *capabilities) { return strstr(capabilities, "overlay.rgba") != NULL || strstr(capabilities, "present.transform") != NULL; }

static int load_rows(void) {
#ifdef SEGARECOMP_SMS_MOD_SET
  uint32_t i;
  if (segarecomp_sms_mod_set_count == 0UL || segarecomp_sms_mod_set_count > MEMBERS_MAX) return 0;
  member_count = (uint32_t)segarecomp_sms_mod_set_count;
  for (i = 0U; i < member_count; ++i) rows[i] = segarecomp_sms_mod_set_members[i];
#else
  member_count = 1U;
  memset(&rows[0], 0, sizeof rows[0]);
  rows[0].id = segarecomp_sms_mod_id;
  rows[0].identity = segarecomp_sms_mod_identity;
  rows[0].capabilities = segarecomp_sms_mod_capabilities;
  rows[0].required[0] = segarecomp_sms_mod_caps_required[0]; rows[0].required[1] = segarecomp_sms_mod_caps_required[1];
  rows[0].optional[0] = segarecomp_sms_mod_caps_optional[0]; rows[0].optional[1] = segarecomp_sms_mod_caps_optional[1];
  rows[0].state_schema = segarecomp_sms_mod_state_schema;
  rows[0].range_count = segarecomp_sms_mod_range_count;
  rows[0].ranges = segarecomp_sms_mod_ranges;
#endif
  return 1;
}

static const SegaModProvider *open_provider(uint32_t i, uint32_t *phase_mask) {
  const SegaModProvider *provider;
#ifdef SEGARECOMP_SMS_MOD_SET
  if (rows[i].native_entry != NULL) {
    provider = rows[i].native_entry(&provider_states[i]);
    *phase_mask = (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4) | (1U << 7) | (1U << 8); /* a native provider is called in every phase */
  } else {
    provider = segarecomp_sms_mod_luau_open(&rows[i], &provider_states[i], phase_mask);
  }
#else
  provider = segarecomp_mod_provider(&provider_states[i]);
  *phase_mask = segarecomp_sms_mod_phase_mask();
#endif
  return provider;
}

int sms_mod_host_attach(SmsMachine *machine, SmsPad *pad, SmsVdp *vdp, SmsRenderer *renderer) {
  static SmsModRange ranges[MEMBERS_MAX][SMS_MOD_RANGES_MAX];
  static SmsModConfig configs[MEMBERS_MAX];
  const char *rejection = "";
  uint32_t i, k;
  if (!load_rows() || renderer == NULL) {
    refuse("E_PROVIDER_ENTRY");
    return 1;
  }
  lead = 0U;
  for (i = 0U; i < member_count; ++i)
    if (draws(rows[i].capabilities)) {
      lead = i;
      break;
    }
  for (i = 0U; i < member_count; ++i) {
    SmsModConfig *config = &configs[i];
    memset(config, 0, sizeof *config);
    if (!parse_sha256(rows[i].identity, config->identity_sha256) || rows[i].range_count > SMS_MOD_RANGES_MAX) {
      refuse("E_PROVIDER_ENTRY");
      return 1;
    }
    config->provider = open_provider(i, &config->phase_mask);
    if (config->provider == NULL) {
      refuse("E_ABI_ENTRY_MISSING");
      return 1;
    }
    config->provider_state = provider_states[i];
    config->mod_id = rows[i].id;
    for (k = 0U; k < 2U; ++k) {
      config->required[k] = rows[i].required[k];
      config->optional[k] = rows[i].optional[k];
    }
    for (k = 0U; k < rows[i].range_count; ++k) {
      ranges[i][k].space = (uint32_t)rows[i].ranges[k][0];
      ranges[i][k].offset = (uint32_t)rows[i].ranges[k][1];
      ranges[i][k].length = (uint32_t)rows[i].ranges[k][2];
      ranges[i][k].modes = (uint32_t)rows[i].ranges[k][3];
    }
    config->ranges = ranges[i];
    config->range_count = (uint32_t)rows[i].range_count;
    config->state_schema = (uint32_t)rows[i].state_schema;
    config->log_sink = log_sink;
  }
  frame_renderer = renderer;
  frame_machine = machine;
#ifdef SEGARECOMP_SMS_MOD_SET
  {
    SmsModScheduler *pointers[MEMBERS_MAX];
    uint8_t identity[32];
    uint32_t member = 0U;
    for (i = 0U; i < member_count; ++i) pointers[i] = &schedulers[i];
    if (!parse_sha256(segarecomp_sms_mod_set_identity, identity) ||
        sms_mod_set_attach(&set, machine, pad, vdp, pointers, configs, member_count, identity, frame_done, NULL, &rejection, &member) != 0) {
      refuse(rejection[0] != '\0' ? rejection : "E_SET_IDENTITY");
      return 1;
    }
    sms_mod_set_state(&set, &state);
  }
#else
  configs[0].frame_done = frame_done;
  if (sms_mod_scheduler_attach(&schedulers[0], machine, pad, vdp, &configs[0], &rejection) != 0) {
    refuse(rejection);
    return 1;
  }
  sms_mod_scheduler_state(&schedulers[0], &state);
#endif
  sms_sha256_init(&stream);
  attached = 1;
  for (i = 0U; i < member_count; ++i)
    fprintf(stderr, "MOD_LOADED {\"mod\":\"%s\",\"package_identity\":\"%s\",\"capabilities\":\"%s\",\"ranges\":%lu}\n", schedulers[i].mod_id, rows[i].identity, rows[i].capabilities, rows[i].range_count);
#ifdef SEGARECOMP_SMS_MOD_SET
  fprintf(stderr, "MOD_SET_LOADED {\"set_identity\":\"%s\",\"mods\":%u}\n", segarecomp_sms_mod_set_identity, (unsigned)member_count);
#endif
  return 0;
}

const SmsModState *sms_mod_host_state(void) { return attached ? &state : NULL; }

void sms_mod_host_identity(uint8_t source_sha256[32], uint8_t variant_sha256[32]) {
  if (!parse_sha256(segarecomp_sms_mod_effective_sha256, source_sha256)) memset(source_sha256, 0, 32U);
  if (!parse_sha256(variant_text(), variant_sha256)) memset(variant_sha256, 0, 32U);
}

int sms_mod_host_compose(const uint8_t *framebuffer, uint32_t height, uint8_t *rgb) {
  const SmsModPresentation *p = attached ? sms_mod_scheduler_presentation(&schedulers[lead]) : NULL;
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

/* One mod's report: the fields of a single-mod MOD_SUMMARY that belong to the mod itself. */
static void put_member(FILE *out, uint32_t i) {
  const SmsModScheduler *s = &schedulers[i];
  uint8_t digest[32];
  int nondeterministic;
#ifdef SEGARECOMP_SMS_MOD_SET
  nondeterministic = rows[i].native_entry == NULL && segarecomp_sms_mod_luau_nondeterministic(provider_states[i]);
#else
  nondeterministic = segarecomp_sms_mod_nondeterministic();
#endif
  fprintf(out, "\"mod\":\"%s\",\"faulted\":%s,\"fault\":\"", s->mod_id, s->fault.faulted ? "true" : "false");
  put_json_text(out, s->fault.text, (uint32_t)strlen(s->fault.text));
  sms_mod_write_log_digest(s, digest);
  fprintf(out, "\",\"callbacks\":%llu,\"committed_writes\":%llu,\"write_digest\":\"", (unsigned long long)s->callbacks_run, (unsigned long long)s->write_total);
  put_hex(out, digest, 32U);
  fprintf(out, "\",\"nondeterministic\":%s", nondeterministic ? "true" : "false");
}

void sms_mod_host_summary(FILE *out, const char *extra) {
  uint8_t digest[32];
  SmsSha256 sha;
  uint32_t i;
  if (!attached) return;
#ifdef SEGARECOMP_SMS_MOD_SET
  fprintf(out, "MOD_SET_SUMMARY {\"set_identity\":\"%s\",\"members\":[", segarecomp_sms_mod_set_identity);
  for (i = 0U; i < member_count; ++i) {
    fputs(i != 0U ? ",{" : "{", out);
    put_member(out, i);
    fputc('}', out);
  }
  fputc(']', out);
#else
  fputs("MOD_SUMMARY {", out);
  put_member(out, 0U);
#endif
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
