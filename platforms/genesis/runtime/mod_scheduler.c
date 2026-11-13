/* SEG-138-T001/T002 (ADR 0297): the Genesis mod phase scheduler. See mod_scheduler.h. Strict C11; no allocation; no wall clock, host RNG or host pointers are ever offered to a mod. */
#include "mod_scheduler.h"

#include <stdio.h>
#include <string.h>

/* The ABI 2 service functions carry no context pointer, so the scheduler whose callback is running is reachable through this single pointer, set only for the duration of a callback. */
static GenesisModScheduler *g_current = NULL;

#define PHASE_BIT(p) (UINT32_C(1) << (p))
#define VALID_PHASES (PHASE_BIT(SEGA_MOD_PHASE_RESET) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | \
                      PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | PHASE_BIT(SEGA_MOD_PHASE_RENDER))
#define READ_PHASES (PHASE_BIT(SEGA_MOD_PHASE_RESET) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_END))
#define WRITE_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_END))
#define INPUT_READ_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT))

static uint64_t cap_bit(unsigned id) { return UINT64_C(1) << (id % 64U); }

void genesis_mod_scheduler_supported(uint64_t out[2]) {
  out[0] = cap_bit(SEGA_MOD_CAP_HOST_LOG) | cap_bit(SEGA_MOD_CAP_FRAME_WORK_RAM) | cap_bit(SEGA_MOD_CAP_MEMORY_READ) | cap_bit(SEGA_MOD_CAP_MEMORY_WRITE) | cap_bit(SEGA_MOD_CAP_INPUT_READ) |
           cap_bit(SEGA_MOD_CAP_INPUT_INTERCEPT) |
           cap_bit(SEGA_MOD_CAP_OVERLAY_RGBA); /* presentation output of `render`: collected by the provider adapter and presented by the host, not a scheduler service */
  out[1] = 0;
}

const char *genesis_mod_phase_name(uint32_t phase) {
  switch (phase) {
    case SEGA_MOD_PHASE_RESET: return "reset";
    case SEGA_MOD_PHASE_FRAME_BEGIN: return "frame-begin";
    case SEGA_MOD_PHASE_INPUT: return "input";
    case SEGA_MOD_PHASE_POST_INPUT: return "post-input";
    case SEGA_MOD_PHASE_HOOK_ENTRY: return "hook-entry";
    case SEGA_MOD_PHASE_HOOK_EXIT: return "hook-exit";
    case SEGA_MOD_PHASE_FRAME_END: return "frame-end";
    case SEGA_MOD_PHASE_RENDER: return "render";
    default: break;
  }
  return "";
}

const char *genesis_mod_status_cause(int status) {
  switch (status) {
    case SEGA_MOD_E_CAPABILITY: return "capability";
    case SEGA_MOD_E_SPACE: return "space";
    case SEGA_MOD_E_RANGE: return "range";
    case SEGA_MOD_E_ALIGNMENT: return "alignment";
    case SEGA_MOD_E_READONLY: return "readonly";
    case SEGA_MOD_E_NOT_OWNER: return "owner";
    case SEGA_MOD_E_BUDGET: return "budget";
    case SEGA_MOD_E_PHASE: return "phase";
    default: break;
  }
  return "";
}

void genesis_mod_fault_current(const char *cause) {
  GenesisModScheduler *s = g_current;
  if (s != NULL && s->in_callback && s->external_cause == NULL && cause != NULL) s->external_cause = cause;
}

static int granted(const GenesisModScheduler *s, unsigned id) { return sega_mod_is_granted(&s->host, id); }

/* A violation: the FIRST one decides the fault; every later call in the same callback keeps answering with its own status. */
static int violation(GenesisModScheduler *s, int status) {
  if (s->fault_status == SEGA_MOD_OK) s->fault_status = status;
  return status;
}

static int width_ok(uint32_t width) { return width == 1U || width == 2U || width == 4U; }

/* Is [offset, offset+width) inside one declared range that allows `mode`? */
static int in_declared_range(const GenesisModScheduler *s, uint32_t space, uint32_t offset, uint32_t width, uint32_t mode) {
  uint32_t i;
  for (i = 0; i < s->config.range_count; ++i) {
    const GenesisModRange *r = &s->ranges[i];
    if (r->space == space && (r->modes & mode) == mode && offset >= r->offset && (uint64_t)offset + width <= (uint64_t)r->offset + r->length) return 1;
  }
  return 0;
}

static int known_read_only_space(uint32_t space) {
  return space == SEGA_GEN_SPACE_M68K_ROM || space == SEGA_GEN_SPACE_Z80_RAM || space == SEGA_GEN_SPACE_VDP_VRAM || space == SEGA_GEN_SPACE_VDP_CRAM || space == SEGA_GEN_SPACE_VDP_VSRAM;
}

static uint8_t byte_at(const GenesisModScheduler *s, const uint8_t *base, uint32_t offset) {
  uint8_t value = base[offset];
  uint32_t i;
  for (i = 0; i < s->staged_count; ++i) { /* reads observe this callback's staged writes, in order */
    const GenesisModStagedWrite *w = &s->staged[i];
    if (offset >= w->offset && offset < w->offset + w->width) value = (uint8_t)(w->value >> (8U * (w->width - 1U - (offset - w->offset))));
  }
  return value;
}

static int service_read(uint32_t space, uint32_t offset, uint32_t width, uint32_t *value) {
  GenesisModScheduler *s = g_current;
  const int render = s != NULL && s->phase == SEGA_MOD_PHASE_RENDER;
  uint32_t i, result = 0;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (render ? !granted(s, SEGA_MOD_CAP_FRAME_WORK_RAM) : !granted(s, SEGA_MOD_CAP_MEMORY_READ)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (!render && (READ_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (value == NULL || !width_ok(width)) return violation(s, SEGA_MOD_E_RANGE);
  if (space != SEGA_GEN_SPACE_M68K_WORK_RAM) return violation(s, SEGA_MOD_E_SPACE);
  if (width > 1U && (offset & 1U) != 0U) return violation(s, SEGA_MOD_E_ALIGNMENT); /* the M68K faults on an odd word/long access */
  if ((uint64_t)offset + width > GENESIS_MOD_WORK_RAM_BYTES) return violation(s, SEGA_MOD_E_RANGE);
  /* render: `frame.work_ram` grants the complete 64 KiB frame copy by contract (native-sdk-abi-2.md section 4: "copy of the platform's work RAM"), independent of declared ranges; the copy bound,
     width and alignment still apply. Declared ranges bound the LIVE memory a mod may read or write in the other phases. */
  if (!render && !in_declared_range(s, space, offset, width, GENESIS_MOD_ACCESS_READ)) return violation(s, SEGA_MOD_E_RANGE);
  if (render && !s->render_copy_valid) return violation(s, SEGA_MOD_E_PHASE);
  for (i = 0; i < width; ++i) result = (result << 8U) | (render ? s->render_copy[offset + i] : byte_at(s, s->runtime->work_ram, offset + i));
  *value = result;
  return SEGA_MOD_OK;
}

static int service_write(uint32_t space, uint32_t offset, uint32_t width, uint32_t value) {
  GenesisModScheduler *s = g_current;
  GenesisModStagedWrite *w;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_MEMORY_WRITE)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if ((WRITE_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (known_read_only_space(space)) return violation(s, SEGA_MOD_E_READONLY);
  if (space != SEGA_GEN_SPACE_M68K_WORK_RAM) return violation(s, SEGA_MOD_E_SPACE);
  if (!width_ok(width) || (width < 4U && value >> (8U * width)) != 0U) return violation(s, SEGA_MOD_E_RANGE);
  if (width > 1U && (offset & 1U) != 0U) return violation(s, SEGA_MOD_E_ALIGNMENT);
  if ((uint64_t)offset + width > GENESIS_MOD_WORK_RAM_BYTES || !in_declared_range(s, space, offset, width, GENESIS_MOD_ACCESS_WRITE)) return violation(s, SEGA_MOD_E_RANGE);
  if (s->staged_count >= GENESIS_MOD_WRITE_OPS_MAX || s->staged_bytes + width > GENESIS_MOD_WRITE_BYTES_MAX) return violation(s, SEGA_MOD_E_BUDGET);
  w = &s->staged[s->staged_count++];
  w->offset = offset;
  w->width = width;
  w->value = value;
  s->staged_bytes += width;
  return SEGA_MOD_OK;
}

static int service_input_get(uint32_t port, uint32_t *buttons) {
  GenesisModScheduler *s = g_current;
  uint32_t index;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_INPUT_READ)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if ((INPUT_READ_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (buttons == NULL || port < 1U || port > 2U) return violation(s, SEGA_MOD_E_RANGE);
  index = port - 1U;
  /* frame-begin: the previous frame's final input; input: the sample with this callback's staged changes; post-input: the final input. */
  if (s->phase == SEGA_MOD_PHASE_FRAME_BEGIN) *buttons = s->previous_final[index];
  else if (s->phase == SEGA_MOD_PHASE_INPUT) *buttons = s->staged_pad_set[index] ? s->staged_pad[index] : s->final_pad[index];
  else *buttons = s->final_pad[index];
  return SEGA_MOD_OK;
}

static int service_input_set(uint32_t port, uint32_t buttons) {
  GenesisModScheduler *s = g_current;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_INPUT_INTERCEPT)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (s->phase != SEGA_MOD_PHASE_INPUT) return violation(s, SEGA_MOD_E_PHASE);
  if (port < 1U || port > 2U || buttons > 0xFFU) return violation(s, SEGA_MOD_E_RANGE); /* button bits only */
  s->staged_pad[port - 1U] = (uint8_t)buttons;
  s->staged_pad_set[port - 1U] = 1U;
  return SEGA_MOD_OK;
}

static void service_log(const char *ascii, uint32_t length) {
  GenesisModScheduler *s = g_current;
  uint32_t i;
  char *line;
  if (s == NULL || !s->in_callback) return;
  if (!granted(s, SEGA_MOD_CAP_HOST_LOG)) {
    (void)violation(s, SEGA_MOD_E_CAPABILITY);
    return;
  }
  if (s->staged_log_count >= GENESIS_MOD_LOG_LINES_MAX) {
    (void)violation(s, SEGA_MOD_E_BUDGET);
    return;
  }
  if (ascii == NULL) length = 0U;
  if (length > GENESIS_MOD_LOG_LINE_BYTES) length = GENESIS_MOD_LOG_LINE_BYTES; /* a long line is cut, not a fault */
  line = s->staged_log[s->staged_log_count++];
  for (i = 0; i < length; ++i) line[i] = (ascii[i] >= 0x20 && ascii[i] <= 0x7E) ? ascii[i] : '?';
  line[length] = '\0';
}

static void emit(const GenesisModScheduler *s, const char *line, uint32_t length) {
  if (s->config.log_sink != NULL) s->config.log_sink(s->config.log_ctx, line, length);
}

static void begin_callback(GenesisModScheduler *s, uint32_t phase, uint64_t frame) {
  s->phase = phase;
  s->frame = frame;
  s->host.phase = phase;
  s->host.frame = frame;
  s->staged_count = 0U;
  s->staged_bytes = 0U;
  s->staged_log_count = 0U;
  s->staged_pad_set[0] = s->staged_pad_set[1] = 0U;
  s->fault_status = SEGA_MOD_OK;
  s->external_cause = NULL;
  s->in_callback = 1U;
  g_current = s;
}

static void record_fault(GenesisModScheduler *s, const char *cause) {
  s->disabled = 1U;
  s->fault.faulted = 1U;
  s->fault.cause = cause;
  s->fault.phase = s->phase;
  s->fault.frame = s->frame;
  (void)snprintf(s->fault.text, sizeof(s->fault.text), "E_MOD_FAULT cause=%s mod=%s phase=%s frame=%llu", cause, s->config.mod_id, genesis_mod_phase_name(s->phase), (unsigned long long)s->frame);
  emit(s, s->fault.text, (uint32_t)strlen(s->fault.text));
  if (s->runtime != NULL) s->runtime->mod_pad_active = 0U; /* the guest goes on with the physical pad */
}

static void sha_u32(GenesisSha256 *state, uint32_t v) {
  const uint8_t b[4] = {(uint8_t)(v >> 24U), (uint8_t)(v >> 16U), (uint8_t)(v >> 8U), (uint8_t)v};
  genesis_sha256_update(state, b, 4U);
}

static void stream_write(GenesisModScheduler *s, uint64_t frame, uint32_t phase, const GenesisModStagedWrite *w) {
  sha_u32(&s->write_stream, (uint32_t)(frame >> 32U));
  sha_u32(&s->write_stream, (uint32_t)frame);
  sha_u32(&s->write_stream, phase);
  sha_u32(&s->write_stream, SEGA_GEN_SPACE_M68K_WORK_RAM);
  sha_u32(&s->write_stream, w->offset);
  sha_u32(&s->write_stream, w->width);
  sha_u32(&s->write_stream, w->value);
  ++s->write_total;
}

static void finish_callback(GenesisModScheduler *s, int provider_failed) {
  uint32_t i;
  s->in_callback = 0U;
  g_current = NULL;
  ++s->callbacks_run;
  if (s->fault_status != SEGA_MOD_OK || s->external_cause != NULL || provider_failed) { /* staged writes, input and log are discarded; the mod is disabled for the session */
    record_fault(s, s->fault_status != SEGA_MOD_OK ? genesis_mod_status_cause(s->fault_status) : s->external_cause != NULL ? s->external_cause : "provider");
    return;
  }
  for (i = 0; i < s->staged_count; ++i) { /* atomic commit at the end of the callback, in staging order */
    const GenesisModStagedWrite *w = &s->staged[i];
    uint32_t b;
    for (b = 0; b < w->width; ++b) s->runtime->work_ram[w->offset + b] = (uint8_t)(w->value >> (8U * (w->width - 1U - b)));
    stream_write(s, s->frame, s->phase, w);
    if (s->write_log_count < GENESIS_MOD_WRITE_LOG_MAX) {
      GenesisModWriteRecord *r = &s->write_log[s->write_log_count++];
      r->frame = s->frame;
      r->phase = s->phase;
      r->space = SEGA_GEN_SPACE_M68K_WORK_RAM;
      r->offset = w->offset;
      r->width = w->width;
      r->value = w->value;
    } else {
      ++s->write_log_dropped;
    }
  }
  for (i = 0; i < 2U; ++i)
    if (s->staged_pad_set[i]) s->final_pad[i] = s->staged_pad[i];
  for (i = 0; i < s->staged_log_count; ++i) emit(s, s->staged_log[i], (uint32_t)strlen(s->staged_log[i]));
}

static void run_phase(GenesisModScheduler *s, uint32_t phase, uint64_t frame) {
  if (s->disabled || (s->config.phase_mask & PHASE_BIT(phase)) == 0U) return;
  begin_callback(s, phase, frame);
  s->config.provider->phase(s->config.provider_state, &s->host);
  finish_callback(s, 0);
}

static void open_frame(GenesisModScheduler *s, GenesisRuntime *rt) {
  const uint64_t frame = rt->virtual_frame_count;
  s->previous_final[0] = s->final_pad[0];
  s->previous_final[1] = s->final_pad[1];
  run_phase(s, SEGA_MOD_PHASE_FRAME_BEGIN, frame);
  s->sampled[0] = rt->pad1; /* one sample per frame; the physical state stays in pad1/pad2 for recordings */
  s->sampled[1] = rt->pad2;
  s->final_pad[0] = s->sampled[0];
  s->final_pad[1] = s->sampled[1];
  run_phase(s, SEGA_MOD_PHASE_INPUT, frame);
  if (!s->disabled && (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_INPUT)) != 0U) {
    rt->mod_pad_active = 1U;
    rt->mod_pad1 = s->final_pad[0];
    rt->mod_pad2 = s->final_pad[1];
  } else {
    rt->mod_pad_active = 0U;
  }
  run_phase(s, SEGA_MOD_PHASE_POST_INPUT, frame);
}

static void hook_start(void *ctx, GenesisRuntime *rt) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  if (s->started) return;
  s->started = 1U;
  if (s->config.provider->init != NULL) {
    int failed;
    begin_callback(s, SEGA_MOD_PHASE_RESET, rt->virtual_frame_count);
    failed = s->config.provider->init(s->config.provider_state, &s->host) != 0;
    finish_callback(s, failed);
  }
  run_phase(s, SEGA_MOD_PHASE_RESET, rt->virtual_frame_count);
  open_frame(s, rt);
}

static void hook_frame_end(void *ctx, GenesisRuntime *rt) {
  run_phase((GenesisModScheduler *)ctx, SEGA_MOD_PHASE_FRAME_END, rt->virtual_frame_count);
}

static void hook_after_snapshot(void *ctx, GenesisRuntime *rt) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  const uint64_t ended = rt->virtual_frame_count - 1U;
  if (!s->disabled && (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_RENDER)) != 0U) {
    s->render_copy_valid = 0U;
    if (granted(s, SEGA_MOD_CAP_FRAME_WORK_RAM)) { /* render reads copies taken after frame-end, never live memory */
      memcpy(s->render_copy, rt->work_ram, GENESIS_MOD_WORK_RAM_BYTES);
      s->render_copy_valid = 1U;
    }
    run_phase(s, SEGA_MOD_PHASE_RENDER, ended);
    s->render_copy_valid = 0U;
  }
  open_frame(s, rt);
}

int genesis_mod_scheduler_attach(GenesisModScheduler *s, GenesisRuntime *rt, const GenesisModConfig *config, const char **rejection) {
  uint64_t supported[2], required_bad;
  uint32_t i;
  SegaModNegotiation n;
  const char *dummy = NULL;
  if (rejection == NULL) rejection = &dummy;
  *rejection = "";
  if (s == NULL || rt == NULL || config == NULL || config->mod_id == NULL || strlen(config->mod_id) == 0U || strlen(config->mod_id) > 64U ||
      (config->range_count != 0U && config->ranges == NULL)) {
    *rejection = "E_PROVIDER_ENTRY";
    return 1;
  }
  if (rt->frame_hooks != NULL) {
    *rejection = "E_SCHEDULER_BUSY";
    return 1;
  }
  for (i = 0; config->mod_id[i] != '\0'; ++i)
    if (config->mod_id[i] < 0x21 || config->mod_id[i] > 0x7E) {
      *rejection = "E_PROVIDER_ENTRY";
      return 1;
    }
  memset(s, 0, sizeof(*s));
  genesis_sha256_init(&s->write_stream);
  s->config = *config;
  genesis_mod_scheduler_supported(supported);
  s->host.size = (uint32_t)sizeof(SegaModHost);
  s->host.abi_major = SEGA_MOD_ABI_MAJOR;
  s->host.abi_minor = SEGA_MOD_ABI_MINOR;
  s->host.platform = SEGA_MOD_PLATFORM_GENESIS;
  s->host.granted[0] = (config->required[0] | config->optional[0]) & supported[0];
  s->host.granted[1] = (config->required[1] | config->optional[1]) & supported[1];
  n = sega_mod_negotiate(config->provider, &s->host, config->required);
  required_bad = (config->required[0] & ~supported[0]) | config->required[1];
  if (n == SEGA_MOD_NEG_OK && required_bad != 0U) n = SEGA_MOD_NEG_E_CAPABILITY_UNSUPPORTED;
  if (n != SEGA_MOD_NEG_OK) {
    *rejection = sega_mod_negotiation_class(n);
    return 1;
  }
  if (config->provider->phase == NULL) {
    *rejection = "E_PROVIDER_ENTRY";
    return 1;
  }
  if (config->phase_mask == 0U || (config->phase_mask & ~VALID_PHASES) != 0U) { /* none, unknown, bit 0 or a hook phase (SEG-138-T004) */
    *rejection = "E_PHASE_UNSUPPORTED";
    return 1;
  }
  if (config->range_count > GENESIS_MOD_RANGES_MAX) {
    *rejection = "E_RANGE_DECLARATION";
    return 1;
  }
  for (i = 0; i < config->range_count; ++i) {
    const GenesisModRange *r = &config->ranges[i];
    const uint32_t modes = r->modes;
    if (r->space != SEGA_GEN_SPACE_M68K_WORK_RAM || r->length == 0U || (uint64_t)r->offset + r->length > GENESIS_MOD_WORK_RAM_BYTES || modes == 0U ||
        (modes & ~(GENESIS_MOD_ACCESS_READ | GENESIS_MOD_ACCESS_WRITE)) != 0U ||
        ((modes & GENESIS_MOD_ACCESS_READ) != 0U && !sega_mod_is_granted(&s->host, SEGA_MOD_CAP_MEMORY_READ)) ||
        ((modes & GENESIS_MOD_ACCESS_WRITE) != 0U && !sega_mod_is_granted(&s->host, SEGA_MOD_CAP_MEMORY_WRITE))) {
      *rejection = "E_RANGE_DECLARATION";
      return 1;
    }
    s->ranges[i] = *r;
  }
  s->features.size = (uint32_t)sizeof(SegaGenesisFeatures);
  s->features.work_ram_bytes = 65536U;
  s->features.vram_bytes = 65536U;
  s->features.vdp_registers = 24U;
  s->features.cram_bytes = 128U;
  s->features.vsram_bytes = 80U;
  s->features.visible_lines = 224U;
  s->host.platform_features = &s->features;
  s->host.platform_features_size = (uint32_t)sizeof(SegaGenesisFeatures);
  s->host.log = service_log;
  s->host.read = service_read;
  s->host.write = service_write;
  s->host.input_get = service_input_get;
  s->host.input_set = service_input_set;
  s->runtime = rt;
  s->hooks.ctx = s;
  s->hooks.start = hook_start;
  s->hooks.frame_end = hook_frame_end;
  s->hooks.after_snapshot = hook_after_snapshot;
  rt->frame_hooks = &s->hooks;
  return 0;
}

void genesis_mod_scheduler_detach(GenesisModScheduler *s) {
  if (s == NULL || s->runtime == NULL) return;
  if (s->runtime->frame_hooks == &s->hooks) s->runtime->frame_hooks = NULL;
  s->runtime->mod_pad_active = 0U;
  s->runtime = NULL;
}

void genesis_mod_write_log_digest(const GenesisModScheduler *s, uint8_t out[32]) {
  GenesisSha256 sha = s->write_stream; /* finalize a copy: the stream keeps running */
  sha_u32(&sha, (uint32_t)(s->write_total >> 32U));
  sha_u32(&sha, (uint32_t)s->write_total);
  genesis_sha256_final(&sha, out);
}
