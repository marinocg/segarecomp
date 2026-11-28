/* SEG-147-T004/T005/T006 (ADR 0325): see sms_mod_scheduler.h. Strict C11. */
#include "sms_mod_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ABI 2 service functions carry no context pointer: the scheduler whose callback is running, set only for the duration of that callback. */
static SmsModScheduler *g_current = NULL;

#define PHASE_BIT(p) (UINT32_C(1) << (p))
#define VALID_PHASES (PHASE_BIT(SEGA_MOD_PHASE_RESET) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | \
                      PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | PHASE_BIT(SEGA_MOD_PHASE_RENDER))
#define READ_PHASES (VALID_PHASES & ~PHASE_BIT(SEGA_MOD_PHASE_RENDER))
#define WRITE_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_END))
#define INPUT_READ_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT))

static uint64_t cap_bit(unsigned id) { return UINT64_C(1) << (id % 64U); }

void sms_mod_scheduler_supported(uint64_t out[2]) {
  out[0] = cap_bit(SEGA_MOD_CAP_HOST_LOG) | cap_bit(SEGA_MOD_CAP_FRAME_WORK_RAM) | cap_bit(SEGA_MOD_CAP_FRAME_SMS_VDP) | cap_bit(SEGA_MOD_CAP_OVERLAY_RGBA) | cap_bit(SEGA_MOD_CAP_MEMORY_READ) |
           cap_bit(SEGA_MOD_CAP_MEMORY_WRITE) | cap_bit(SEGA_MOD_CAP_INPUT_READ) | cap_bit(SEGA_MOD_CAP_INPUT_INTERCEPT) | cap_bit(SEGA_MOD_CAP_STATE_MOD) | cap_bit(SEGA_MOD_CAP_PRESENT_TRANSFORM);
  out[1] = 0;
}

static const char *phase_name(uint32_t phase) {
  switch (phase) {
    case SEGA_MOD_PHASE_RESET: return "reset";
    case SEGA_MOD_PHASE_FRAME_BEGIN: return "frame-begin";
    case SEGA_MOD_PHASE_INPUT: return "input";
    case SEGA_MOD_PHASE_POST_INPUT: return "post-input";
    case SEGA_MOD_PHASE_FRAME_END: return "frame-end";
    case SEGA_MOD_PHASE_RENDER: return "render";
    default: break;
  }
  return "";
}

static const char *status_cause(int status) {
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

void sms_mod_fault_current(const char *cause) {
  SmsModScheduler *s = g_current;
  if (s != NULL && s->in_callback && s->external_cause == NULL && cause != NULL) s->external_cause = cause;
}

static int granted(const SmsModScheduler *s, unsigned id) { return sega_mod_is_granted(&s->host, id); }

/* A violation: the FIRST one decides the fault; every later call in the same callback keeps answering with its own status. */
static int violation(SmsModScheduler *s, int status) {
  if (s->fault_status == SEGA_MOD_OK) s->fault_status = status;
  return status;
}

static int in_declared_range(const SmsModScheduler *s, uint32_t space, uint32_t offset, uint32_t width, uint32_t mode) {
  uint32_t i;
  for (i = 0U; i < s->config.range_count; ++i) {
    const SmsModRange *r = &s->ranges[i];
    if (r->space == space && (r->modes & mode) == mode && offset >= r->offset && (uint64_t)offset + width <= (uint64_t)r->offset + r->length) return 1;
  }
  return 0;
}

static uint32_t space_bytes(uint32_t space) { return space == SEGA_SMS_SPACE_Z80_WORK_RAM ? SMS_RAM_SIZE : space == SEGA_SMS_SPACE_Z80_CART_RAM ? SMS_CART_RAM_SIZE : 0U; }

/* Live memory with this callback's staged writes applied in order. */
static uint8_t byte_at(const SmsModScheduler *s, uint32_t space, uint32_t offset) {
  uint8_t value = space == SEGA_SMS_SPACE_Z80_CART_RAM ? s->machine->mem.cart_ram[offset] : s->machine->mem.ram[offset];
  uint32_t i;
  for (i = 0U; i < s->staged_count; ++i) {
    const SmsModStagedWrite *w = &s->staged[i];
    if (w->space == space && offset >= w->offset && offset < w->offset + w->width) value = (uint8_t)(w->value >> (8U * (offset - w->offset))); /* little endian */
  }
  return value;
}

static int service_read(uint32_t space, uint32_t offset, uint32_t width, uint32_t *value) {
  SmsModScheduler *s = g_current;
  uint32_t i, result = 0U;
  int render;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  render = s->phase == SEGA_MOD_PHASE_RENDER;
  if (render ? !granted(s, SEGA_MOD_CAP_FRAME_WORK_RAM) : !granted(s, SEGA_MOD_CAP_MEMORY_READ)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (!render && (READ_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (value == NULL || (width != 1U && width != 2U)) return violation(s, SEGA_MOD_E_RANGE);
  if (space_bytes(space) == 0U || (render && space != SEGA_SMS_SPACE_Z80_WORK_RAM)) return violation(s, SEGA_MOD_E_SPACE);
  if ((uint64_t)offset + width > space_bytes(space)) return violation(s, SEGA_MOD_E_RANGE);
  if (!render && !in_declared_range(s, space, offset, width, SMS_MOD_ACCESS_READ)) return violation(s, SEGA_MOD_E_RANGE);
  if (render && !s->render_ram_valid) return violation(s, SEGA_MOD_E_PHASE);
  for (i = 0U; i < width; ++i) result |= (uint32_t)(render ? s->render_ram[offset + i] : byte_at(s, space, offset + i)) << (8U * i);
  *value = result;
  return SEGA_MOD_OK;
}

static int service_write(uint32_t space, uint32_t offset, uint32_t width, uint32_t value) {
  SmsModScheduler *s = g_current;
  SmsModStagedWrite *w;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_MEMORY_WRITE)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if ((WRITE_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (space == SEGA_SMS_SPACE_Z80_CART_RAM) return violation(s, SEGA_MOD_E_READONLY); /* persistent saves are not a mod-writable surface */
  if (space != SEGA_SMS_SPACE_Z80_WORK_RAM) return violation(s, SEGA_MOD_E_SPACE);
  if ((width != 1U && width != 2U) || value >> (8U * width) != 0U) return violation(s, SEGA_MOD_E_RANGE);
  if ((uint64_t)offset + width > SMS_RAM_SIZE || !in_declared_range(s, space, offset, width, SMS_MOD_ACCESS_WRITE)) return violation(s, SEGA_MOD_E_RANGE);
  if (s->staged_count >= SMS_MOD_WRITE_OPS_MAX || s->staged_bytes + width > SMS_MOD_WRITE_BYTES_MAX) return violation(s, SEGA_MOD_E_BUDGET);
  w = &s->staged[s->staged_count++];
  w->space = space;
  w->offset = offset;
  w->width = width;
  w->value = value;
  s->staged_bytes += width;
  return SEGA_MOD_OK;
}

static int service_input_get(uint32_t port, uint32_t *buttons) {
  SmsModScheduler *s = g_current;
  uint32_t i;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_INPUT_READ)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if ((INPUT_READ_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (buttons == NULL || port < 1U || port > 2U) return violation(s, SEGA_MOD_E_RANGE);
  i = port - 1U;
  /* frame-begin: the previous frame's final input; input: the sample with this callback's staged change; post-input: the final input. */
  if (s->phase == SEGA_MOD_PHASE_FRAME_BEGIN) *buttons = s->previous_final[i];
  else if (s->phase == SEGA_MOD_PHASE_INPUT) *buttons = s->staged_pad_set[i] ? s->staged_pad[i] : s->final_pad[i];
  else *buttons = s->final_pad[i];
  return SEGA_MOD_OK;
}

static int service_input_set(uint32_t port, uint32_t buttons) {
  SmsModScheduler *s = g_current;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_INPUT_INTERCEPT)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (s->phase != SEGA_MOD_PHASE_INPUT) return violation(s, SEGA_MOD_E_PHASE);
  if (port < 1U || port > 2U || buttons > 0x3FU) return violation(s, SEGA_MOD_E_RANGE);
  s->staged_pad[port - 1U] = (uint8_t)buttons;
  s->staged_pad_set[port - 1U] = 1U;
  return SEGA_MOD_OK;
}

static void service_log(const char *ascii, uint32_t length) {
  SmsModScheduler *s = g_current;
  uint32_t i;
  char *line;
  if (s == NULL || !s->in_callback) return;
  if (!granted(s, SEGA_MOD_CAP_HOST_LOG)) { (void)violation(s, SEGA_MOD_E_CAPABILITY); return; }
  if (s->staged_log_count >= SMS_MOD_LOG_LINES_MAX) { (void)violation(s, SEGA_MOD_E_BUDGET); return; }
  if (ascii == NULL) length = 0U;
  if (length > SMS_MOD_LOG_LINE_BYTES) length = SMS_MOD_LOG_LINE_BYTES;
  line = s->staged_log[s->staged_log_count++];
  for (i = 0U; i < length; ++i) line[i] = (ascii[i] >= 0x20 && ascii[i] <= 0x7E) ? ascii[i] : '?';
  line[length] = '\0';
}

/* ---- the render API: capability and phase here, argument rules in sms_mod_present.c ---- */
static int render_gate(unsigned capability, SmsModScheduler **out) {
  SmsModScheduler *s = g_current;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, capability)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (s->phase != SEGA_MOD_PHASE_RENDER) return violation(s, SEGA_MOD_E_PHASE);
  *out = s;
  return SEGA_MOD_OK;
}
#define RENDER_CALL(capability, call)                  \
  do {                                                 \
    SmsModScheduler *s = NULL;                         \
    int status = render_gate((capability), &s);        \
    if (status != SEGA_MOD_OK) return status;          \
    status = (call);                                   \
    return status == SEGA_MOD_OK ? status : violation(s, status); \
  } while (0)

static int api_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgba) { RENDER_CALL(SEGA_MOD_CAP_OVERLAY_RGBA, sms_mod_present_rect(&s->staged_presentation, x, y, w, h, rgba)); }
static int api_text(int32_t x, int32_t y, const char *a, uint32_t n, uint32_t rgba) { RENDER_CALL(SEGA_MOD_CAP_OVERLAY_RGBA, sms_mod_present_text(&s->staged_presentation, x, y, a, n, rgba)); }
static int api_palette(uint32_t v, uint32_t rgb) { RENDER_CALL(SEGA_MOD_CAP_PRESENT_TRANSFORM, sms_mod_present_palette(&s->staged_presentation, v, rgb)); }
static int api_scroll(int32_t dx, int32_t dy) { RENDER_CALL(SEGA_MOD_CAP_PRESENT_TRANSFORM, sms_mod_present_scroll(&s->staged_presentation, dx, dy)); }
static int api_pattern(uint32_t i, const uint8_t rows[32]) { RENDER_CALL(SEGA_MOD_CAP_OVERLAY_RGBA, sms_mod_present_sprite_pattern(&s->staged_presentation, i, rows)); }
static int api_colors(const uint8_t cram[16]) { RENDER_CALL(SEGA_MOD_CAP_OVERLAY_RGBA, sms_mod_present_sprite_colors(&s->staged_presentation, cram)); }
static int api_sprite(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t tile, uint32_t flags) {
  RENDER_CALL(SEGA_MOD_CAP_OVERLAY_RGBA, sms_mod_present_sprite_add(&s->staged_presentation, x, y, w, h, tile, flags));
}

/* ---- callback lifecycle ---- */
static void emit(const SmsModScheduler *s, const char *line) {
  if (s->config.log_sink != NULL) s->config.log_sink(s->config.log_context, line, (uint32_t)strlen(line));
}

static void stop_intercepting(SmsModScheduler *s) {
  s->intercepting = 0U;
  if (s->pad != NULL) s->pad->intercept = NULL;
}

static void record_fault(SmsModScheduler *s, const char *cause) {
  s->disabled = 1U;
  s->fault.faulted = 1U;
  s->fault.phase = s->phase;
  s->fault.frame = s->frame;
  (void)snprintf(s->fault.cause, sizeof s->fault.cause, "%s", cause);
  (void)snprintf(s->fault.text, sizeof s->fault.text, "E_MOD_FAULT cause=%s mod=%s phase=%s frame=%llu", s->fault.cause, s->mod_id, phase_name(s->phase), (unsigned long long)s->frame);
  s->presentation_valid = 0U;
  if (!s->in_set) stop_intercepting(s); /* the guest goes on with the physical pad (in a set another member may still own the interception: the set decides) */
  emit(s, s->fault.text);
}

static void begin_callback(SmsModScheduler *s, uint32_t phase, uint64_t frame) {
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

static void stream_u64(SmsSha256 *sha, uint64_t v) { sms_sha256_update_u64(sha, v); }

static void finish_callback(SmsModScheduler *s, int provider_failed) {
  uint32_t i, b;
  s->in_callback = 0U;
  g_current = NULL;
  ++s->callbacks_run;
  if (s->fault_status != SEGA_MOD_OK || s->external_cause != NULL || provider_failed) { /* every staged effect is discarded; the mod is disabled for the session */
    record_fault(s, s->fault_status != SEGA_MOD_OK ? status_cause(s->fault_status) : s->external_cause != NULL ? s->external_cause : "provider");
    return;
  }
  for (i = 0U; i < s->staged_count; ++i) { /* atomic commit in staging order */
    const SmsModStagedWrite *w = &s->staged[i];
    for (b = 0U; b < w->width; ++b) s->machine->mem.ram[w->offset + b] = (uint8_t)(w->value >> (8U * b));
    stream_u64(&s->write_stream, s->frame);
    sms_sha256_update_u32(&s->write_stream, s->phase);
    sms_sha256_update_u32(&s->write_stream, w->space);
    sms_sha256_update_u32(&s->write_stream, w->offset);
    sms_sha256_update_u32(&s->write_stream, w->width);
    sms_sha256_update_u32(&s->write_stream, w->value);
    ++s->write_total;
  }
  for (i = 0U; i < 2U; ++i)
    if (s->staged_pad_set[i]) s->final_pad[i] = s->staged_pad[i];
  for (i = 0U; i < s->staged_log_count; ++i) emit(s, s->staged_log[i]);
}

static int registered(const SmsModScheduler *s, uint32_t phase) { return !s->disabled && (s->config.phase_mask & PHASE_BIT(phase)) != 0U; }

static void run_phase(SmsModScheduler *s, uint32_t phase, uint64_t frame) {
  if (!registered(s, phase)) return;
  begin_callback(s, phase, frame);
  s->config.provider->phase(s->config.provider_state, &s->host);
  finish_callback(s, 0);
}

static void run_render(SmsModScheduler *s, uint64_t frame) {
  s->presentation_valid = 0U;
  if (!registered(s, SEGA_MOD_PHASE_RENDER)) return;
  sms_mod_present_clear(&s->staged_presentation);
  s->render_ram_valid = 0U;
  if (granted(s, SEGA_MOD_CAP_FRAME_WORK_RAM)) { /* render reads copies taken after frame-end, never live memory */
    memcpy(s->render_ram, s->machine->mem.ram, SMS_RAM_SIZE);
    s->render_ram_valid = 1U;
  }
  s->features.vdp = NULL;
  if (granted(s, SEGA_MOD_CAP_FRAME_SMS_VDP) && s->vdp != NULL) {
    memcpy(s->view_vram, s->vdp->vram, sizeof s->view_vram);
    memcpy(s->view_cram, s->vdp->cram, sizeof s->view_cram);
    memcpy(s->view_registers, s->vdp->reg, sizeof s->view_registers);
    s->view.size = (uint32_t)sizeof s->view;
    s->view.active_lines = sms_vdp_active_lines(sms_vdp_mode(s->vdp));
    s->view.vram = s->view_vram;
    s->view.cram = s->view_cram;
    s->view.registers = s->view_registers;
    s->view.sprite_table = (uint32_t)(s->view_registers[5] & 0x7EU) << 7U;
    s->view.sprite_pattern_base = (s->view_registers[6] & 0x04U) != 0U ? 0x2000U : 0U;
    s->view.sprite_height = (s->view_registers[1] & 0x02U) != 0U ? 16U : 8U;
    s->features.vdp = &s->view;
  }
  run_phase(s, SEGA_MOD_PHASE_RENDER, frame);
  s->features.vdp = NULL;
  s->render_ram_valid = 0U;
  if (!s->disabled) { /* committed only by a callback that did not fault */
    memcpy(&s->presentation, &s->staged_presentation, sizeof s->presentation);
    s->presentation_valid = 1U;
    s->presentation_frame = frame;
  }
}

static void reset_session(SmsModScheduler *s, uint64_t frame) {
  s->started = 1U;
  if (!s->disabled && s->config.provider->init != NULL) {
    int failed;
    begin_callback(s, SEGA_MOD_PHASE_RESET, frame);
    failed = s->config.provider->init(s->config.provider_state, &s->host) != 0;
    finish_callback(s, failed);
  }
  run_phase(s, SEGA_MOD_PHASE_RESET, frame);
}

static void hook_boundary(void *context, SmsMachine *machine, uint64_t frame) {
  SmsModScheduler *s = (SmsModScheduler *)context;
  (void)machine;
  if (!s->started) {
    reset_session(s, frame);
  } else if (frame != 0U) {
    run_phase(s, SEGA_MOD_PHASE_FRAME_END, frame - 1U);
    run_render(s, frame - 1U);
    if (s->config.frame_done != NULL) s->config.frame_done(s->config.frame_done_context, frame - 1U);
  }
  s->previous_final[0] = s->final_pad[0];
  s->previous_final[1] = s->final_pad[1];
  run_phase(s, SEGA_MOD_PHASE_FRAME_BEGIN, frame);
}

static void hook_sampled(void *context, SmsMachine *machine, uint64_t frame) {
  SmsModScheduler *s = (SmsModScheduler *)context;
  s->sampled[0] = machine->input.p1;
  s->sampled[1] = machine->input.p2;
  s->final_pad[0] = s->sampled[0];
  s->final_pad[1] = s->sampled[1];
  run_phase(s, SEGA_MOD_PHASE_INPUT, frame);
  if (registered(s, SEGA_MOD_PHASE_INPUT) && s->pad != NULL) {
    s->guest_input.p1 = s->final_pad[0];
    s->guest_input.p2 = s->final_pad[1];
    s->guest_input.pause = machine->input.pause;
    s->intercepting = 1U;
    s->pad->intercept = &s->guest_input;
  } else {
    stop_intercepting(s);
  }
  run_phase(s, SEGA_MOD_PHASE_POST_INPUT, frame);
}

/* A machine reset starts the guest session over: `reset` runs again before the next frame. A faulted mod stays disabled (phases section 6.1). */
static void forget_session(SmsModScheduler *s) {
  s->started = 0U;
  s->presentation_valid = 0U;
  /* the replay witness describes the run since the last reset, as the viewer's input recording does */
  sms_sha256_init(&s->write_stream);
  s->write_total = 0U;
  s->callbacks_run = 0U;
  memset(s->final_pad, 0, sizeof s->final_pad);
  memset(s->previous_final, 0, sizeof s->previous_final);
  memset(s->sampled, 0, sizeof s->sampled);
}

static void hook_reset(void *context, SmsMachine *machine) {
  SmsModScheduler *s = (SmsModScheduler *)context;
  (void)machine;
  forget_session(s);
  stop_intercepting(s);
}

int sms_mod_scheduler_attach(SmsModScheduler *s, SmsMachine *machine, SmsPad *pad, const SmsVdp *vdp, const SmsModConfig *config, const char **rejection) {
  uint64_t supported[2];
  const char *dummy = NULL;
  SegaModNegotiation n;
  uint32_t i, id_length;
  if (rejection == NULL) rejection = &dummy;
  *rejection = "";
  if (s == NULL || machine == NULL || pad == NULL || config == NULL || config->mod_id == NULL || (config->range_count != 0U && config->ranges == NULL)) {
    *rejection = "E_PROVIDER_ENTRY";
    return 1;
  }
  for (id_length = 0U; id_length <= 64U && config->mod_id[id_length] != '\0'; ++id_length)
    if (config->mod_id[id_length] < 0x21 || config->mod_id[id_length] > 0x7E) break;
  if (id_length == 0U || id_length > 64U || config->mod_id[id_length] != '\0') {
    *rejection = "E_PROVIDER_ENTRY";
    return 1;
  }
  if (machine->frame_hooks != NULL) {
    *rejection = "E_SCHEDULER_BUSY";
    return 1;
  }
  memset(s, 0, sizeof *s);
  sms_sha256_init(&s->write_stream);
  s->config = *config;
  memcpy(s->mod_id, config->mod_id, id_length);
  sms_mod_scheduler_supported(supported);
  s->host.size = (uint32_t)sizeof(SegaModHost);
  s->host.abi_major = SEGA_MOD_ABI_MAJOR;
  s->host.abi_minor = SEGA_MOD_ABI_MINOR;
  s->host.platform = SEGA_MOD_PLATFORM_MASTER_SYSTEM;
  s->host.granted[0] = (config->required[0] | config->optional[0]) & supported[0];
  s->host.granted[1] = (config->required[1] | config->optional[1]) & supported[1];
  if (vdp == NULL) s->host.granted[0] &= ~cap_bit(SEGA_MOD_CAP_FRAME_SMS_VDP);
  n = sega_mod_negotiate(config->provider, &s->host, config->required);
  if (n == SEGA_MOD_NEG_OK && ((config->required[0] & ~supported[0]) | config->required[1]) != 0U) n = SEGA_MOD_NEG_E_CAPABILITY_UNSUPPORTED;
  if (n != SEGA_MOD_NEG_OK) {
    *rejection = sega_mod_negotiation_class(n);
    return 1;
  }
  if (config->provider->phase == NULL) {
    *rejection = "E_PROVIDER_ENTRY";
    return 1;
  }
  if (config->phase_mask == 0U || (config->phase_mask & ~VALID_PHASES) != 0U) {
    *rejection = "E_PHASE_UNSUPPORTED";
    return 1;
  }
  if (granted(s, SEGA_MOD_CAP_STATE_MOD)) {
    if (config->provider->save_state == NULL || config->provider->load_state == NULL) {
      *rejection = "E_PROVIDER_ENTRY";
      return 1;
    }
    if (config->state_schema == 0U || config->state_schema > 2147483647U) {
      *rejection = "E_STATE_SCHEMA";
      return 1;
    }
  } else if (config->state_schema != 0U) {
    *rejection = "E_STATE_SCHEMA";
    return 1;
  }
  if (config->range_count > SMS_MOD_RANGES_MAX) {
    *rejection = "E_RANGE_DECLARATION";
    return 1;
  }
  for (i = 0U; i < config->range_count; ++i) {
    const SmsModRange *r = &config->ranges[i];
    const uint32_t bytes = space_bytes(r->space);
    /* mapper awareness: only the Sega mapper has cartridge RAM, and cartridge RAM is never mod-writable */
    if (bytes == 0U || r->length == 0U || (uint64_t)r->offset + r->length > bytes || r->modes == 0U || (r->modes & ~(SMS_MOD_ACCESS_READ | SMS_MOD_ACCESS_WRITE)) != 0U ||
        (r->space == SEGA_SMS_SPACE_Z80_CART_RAM && (r->modes != SMS_MOD_ACCESS_READ || machine->mem.family != SMS_MAPPER_SEGA)) ||
        ((r->modes & SMS_MOD_ACCESS_READ) != 0U && !granted(s, SEGA_MOD_CAP_MEMORY_READ)) || ((r->modes & SMS_MOD_ACCESS_WRITE) != 0U && !granted(s, SEGA_MOD_CAP_MEMORY_WRITE))) {
      *rejection = "E_RANGE_DECLARATION";
      return 1;
    }
    s->ranges[i] = *r;
  }
  s->config.ranges = s->ranges;
  s->config.mod_id = s->mod_id;
  s->render_api.size = (uint32_t)sizeof(SegaSmsRenderApi);
  s->render_api.overlay_rect = api_rect;
  s->render_api.overlay_text = api_text;
  s->render_api.palette_color = api_palette;
  s->render_api.scroll = api_scroll;
  s->render_api.sprite_pattern = api_pattern;
  s->render_api.sprite_colors = api_colors;
  s->render_api.sprite_add = api_sprite;
  s->features.size = (uint32_t)sizeof(SegaMasterSystemFeatures);
  s->features.work_ram_bytes = SMS_RAM_SIZE;
  s->features.cart_ram_bytes = machine->mem.family == SMS_MAPPER_SEGA ? SMS_CART_RAM_SIZE : 0U;
  s->features.vram_bytes = SMS_VDP_VRAM_SIZE;
  s->features.cram_bytes = SMS_VDP_CRAM_SIZE;
  s->features.mapper = machine->mem.family == SMS_MAPPER_SEGA ? SEGA_SMS_MAPPER_SEGA : SEGA_SMS_MAPPER_ROM_ONLY;
  s->features.render = granted(s, SEGA_MOD_CAP_OVERLAY_RGBA) || granted(s, SEGA_MOD_CAP_PRESENT_TRANSFORM) ? &s->render_api : NULL;
  s->host.platform_features = &s->features;
  s->host.platform_features_size = (uint32_t)sizeof(SegaMasterSystemFeatures);
  s->host.log = service_log;
  s->host.read = service_read;
  s->host.write = service_write;
  s->host.input_get = service_input_get;
  s->host.input_set = service_input_set;
  s->machine = machine;
  s->pad = pad;
  s->vdp = vdp;
  s->hooks.context = s;
  s->hooks.reset = hook_reset;
  s->hooks.boundary = hook_boundary;
  s->hooks.sampled = hook_sampled;
  machine->frame_hooks = &s->hooks;
  return 0;
}

void sms_mod_scheduler_detach(SmsModScheduler *s) {
  if (s == NULL || s->machine == NULL) return;
  if (s->machine->frame_hooks == &s->hooks) s->machine->frame_hooks = NULL;
  stop_intercepting(s);
  s->machine = NULL;
}

const SmsModPresentation *sms_mod_scheduler_presentation(const SmsModScheduler *s) { return s != NULL && s->presentation_valid && !s->disabled ? &s->presentation : NULL; }

void sms_mod_write_log_digest(const SmsModScheduler *s, uint8_t out[32]) {
  SmsSha256 sha = s->write_stream; /* finalize a copy: the stream keeps running */
  sms_sha256_update_u64(&sha, s->write_total);
  sms_sha256_final(&sha, out);
}

/* ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * SEG-147-T005: the MODSTATE body, little endian: flags u8 x4 (disabled, started, faulted, intercepting) | pads u8 x8 (sampled, final, previous final, guest) | callbacks_run u64 |
 * [fault: phase u32, frame u64, cause char[32], text char[192]] | write stream (h u32 x8, total_bytes u64, block_used u32, block x64) | write_total u64 | blob_len u32 | blob.
 * Not saved: staged data (none exists at the barrier), the render copies and the presentation (the next render derives it again).
 * ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct Out { uint8_t *p; uint32_t cap, n; int error; } Out;
static void out_bytes(Out *o, const void *src, uint32_t count) {
  if (o->error || count > o->cap - o->n) { o->error = 1; return; }
  if (count != 0U) memcpy(o->p + o->n, src, count);
  o->n += count;
}
static void out_u8(Out *o, uint8_t v) { out_bytes(o, &v, 1U); }
static void out_u32(Out *o, uint32_t v) { uint8_t b[4]; unsigned i; for (i = 0U; i < 4U; ++i) b[i] = (uint8_t)(v >> (8U * i)); out_bytes(o, b, 4U); }
static void out_u64(Out *o, uint64_t v) { uint8_t b[8]; unsigned i; for (i = 0U; i < 8U; ++i) b[i] = (uint8_t)(v >> (8U * i)); out_bytes(o, b, 8U); }

typedef struct In { const uint8_t *p; uint32_t size, off; int error; } In;
static void in_bytes(In *in, void *dst, uint32_t count) {
  if (in->error || count > in->size - in->off) { in->error = 1; memset(dst, 0, count); return; }
  if (count != 0U) memcpy(dst, in->p + in->off, count);
  in->off += count;
}
static uint8_t in_u8(In *in) { uint8_t v = 0U; in_bytes(in, &v, 1U); return v; }
static uint32_t in_u32(In *in) { uint8_t b[4]; uint32_t v = 0U; unsigned i; in_bytes(in, b, 4U); for (i = 0U; i < 4U; ++i) v |= (uint32_t)b[i] << (8U * i); return v; }
static uint64_t in_u64(In *in) { uint8_t b[8]; uint64_t v = 0U; unsigned i; in_bytes(in, b, 8U); for (i = 0U; i < 8U; ++i) v |= (uint64_t)b[i] << (8U * i); return v; }

typedef struct Staged {
  uint8_t disabled, started, faulted, intercepting;
  uint8_t sampled[2], final_pad[2], previous_final[2], guest[2];
  uint64_t callbacks_run, write_total;
  SmsModFault fault;
  SmsSha256 stream;
  uint32_t blob_len;
  uint8_t blob[SMS_MOD_BLOB_MAX];
} Staged;

#define STREAM_RECORD_BYTES 28U /* frame u64 + phase, space, offset, width, value u32 */

static int live_input(const SmsModScheduler *s) { return !s->disabled && s->started && (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_INPUT)) != 0U; }

static int guest_affecting(const SmsModScheduler *s) { return granted(s, SEGA_MOD_CAP_MEMORY_WRITE) || granted(s, SEGA_MOD_CAP_INPUT_INTERCEPT); }

static uint32_t state_put(void *context, uint8_t *out, uint32_t cap, SmsModStateStatus *status) {
  SmsModScheduler *s = (SmsModScheduler *)context;
  Out o;
  unsigned i;
  uint8_t latched;
  const uint32_t fail = UINT32_MAX;
  o.p = out; o.cap = cap; o.n = 0U; o.error = 0;
  *status = SMS_MOD_STATE_NOT_CAPTURABLE;
  if (s->machine == NULL || s->in_callback) return fail;
  if (guest_affecting(s) && !granted(s, SEGA_MOD_CAP_STATE_MOD)) { *status = SMS_MOD_STATE_UNAVAILABLE; return fail; } /* never a state that silently drops the mod's effect */
  latched = (uint8_t)(s->intercepting && (!s->in_set || live_input(s))); /* in a set only a member that takes part in the interception saves it */
  out_u8(&o, s->disabled); out_u8(&o, s->started); out_u8(&o, (uint8_t)s->fault.faulted); out_u8(&o, latched);
  out_bytes(&o, s->sampled, 2U); out_bytes(&o, s->final_pad, 2U); out_bytes(&o, s->previous_final, 2U);
  out_u8(&o, latched ? s->guest_input.p1 : 0U); out_u8(&o, latched ? s->guest_input.p2 : 0U);
  out_u64(&o, s->callbacks_run);
  if (s->fault.faulted) {
    out_u32(&o, s->fault.phase);
    out_u64(&o, s->fault.frame);
    out_bytes(&o, s->fault.cause, (uint32_t)sizeof s->fault.cause);
    out_bytes(&o, s->fault.text, (uint32_t)sizeof s->fault.text);
  }
  for (i = 0U; i < 8U; ++i) out_u32(&o, s->write_stream.h[i]);
  out_u64(&o, s->write_stream.total_bytes);
  out_u32(&o, s->write_stream.block_used);
  out_bytes(&o, s->write_stream.block, 64U);
  out_u64(&o, s->write_total);
  if (granted(s, SEGA_MOD_CAP_STATE_MOD) && !s->disabled) { /* a disabled mod is inert: its flag is the whole state */
    uint32_t len = 0U, at;
    if (o.error || o.cap - o.n < SMS_MOD_BLOB_MAX + 4U) return fail;
    at = o.n + 4U;
    if (s->config.provider->save_state(s->config.provider_state, o.p + at, SMS_MOD_BLOB_MAX, &len) != 0 || len > SMS_MOD_BLOB_MAX) { *status = SMS_MOD_STATE_UNAVAILABLE; return fail; }
    out_u32(&o, len);
    o.n += len;
  } else {
    out_u32(&o, 0U);
  }
  if (o.error) return fail;
  *status = SMS_MOD_STATE_OK;
  return o.n;
}

static void *state_stage(void *context, const uint8_t *body, uint32_t length) {
  const SmsModScheduler *s = (const SmsModScheduler *)context;
  Staged *st;
  In in;
  unsigned i;
  if (s->machine == NULL || s->in_callback) return NULL;
  st = (Staged *)calloc(1U, sizeof *st);
  if (st == NULL) return NULL;
  in.p = body; in.size = length; in.off = 0U; in.error = 0;
  st->disabled = in_u8(&in); st->started = in_u8(&in); st->faulted = in_u8(&in); st->intercepting = in_u8(&in);
  in_bytes(&in, st->sampled, 2U); in_bytes(&in, st->final_pad, 2U); in_bytes(&in, st->previous_final, 2U); in_bytes(&in, st->guest, 2U);
  st->callbacks_run = in_u64(&in);
  if (in.error || st->disabled > 1U || st->started > 1U || st->faulted > 1U || st->intercepting > 1U || st->disabled != st->faulted) goto bad;
  for (i = 0U; i < 2U; ++i)
    if (st->sampled[i] > 0x3FU || st->final_pad[i] > 0x3FU || st->previous_final[i] > 0x3FU || st->guest[i] > 0x3FU) goto bad;
  if (st->intercepting != 0U && (st->disabled != 0U || st->started == 0U || (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_INPUT)) == 0U)) goto bad;
  if (st->intercepting == 0U && (st->guest[0] != 0U || st->guest[1] != 0U)) goto bad;
  if (st->started == 0U && st->callbacks_run != 0U) goto bad; /* before the first frame after a reset nothing has run (a fault from before the reset stays) */
  if (st->faulted != 0U) {
    st->fault.faulted = 1U;
    st->fault.phase = in_u32(&in);
    st->fault.frame = in_u64(&in);
    in_bytes(&in, st->fault.cause, (uint32_t)sizeof st->fault.cause);
    in_bytes(&in, st->fault.text, (uint32_t)sizeof st->fault.text);
    if (in.error || phase_name(st->fault.phase)[0] == '\0' || st->fault.cause[sizeof st->fault.cause - 1U] != '\0' || st->fault.text[sizeof st->fault.text - 1U] != '\0' ||
        strncmp(st->fault.text, "E_MOD_FAULT cause=", 18U) != 0)
      goto bad;
    for (i = 0U; st->fault.cause[i] != '\0'; ++i) if (st->fault.cause[i] < 0x21 || st->fault.cause[i] > 0x7E) goto bad;
    for (i = 0U; st->fault.text[i] != '\0'; ++i) if (st->fault.text[i] < 0x20 || st->fault.text[i] > 0x7E) goto bad;
  }
  for (i = 0U; i < 8U; ++i) st->stream.h[i] = in_u32(&in);
  st->stream.total_bytes = in_u64(&in);
  st->stream.block_used = in_u32(&in);
  in_bytes(&in, st->stream.block, 64U);
  st->write_total = in_u64(&in);
  if (in.error || st->stream.block_used >= 64U || (st->stream.total_bytes & 63U) != st->stream.block_used || st->write_total > UINT64_MAX / STREAM_RECORD_BYTES ||
      st->stream.total_bytes != st->write_total * STREAM_RECORD_BYTES)
    goto bad;
  st->blob_len = in_u32(&in);
  if (in.error || st->blob_len > SMS_MOD_BLOB_MAX || st->blob_len != in.size - in.off) goto bad; /* exact: no trailing bytes */
  if (st->blob_len != 0U && (!granted(s, SEGA_MOD_CAP_STATE_MOD) || st->disabled != 0U)) goto bad;
  if (st->blob_len != 0U) memcpy(st->blob, in.p + in.off, st->blob_len);
  return st;
bad:
  free(st);
  return NULL;
}

/* The only fallible step: the mod's own load. A refusal puts the previous blob back when the mod can take it; otherwise the mod is disabled with cause `state`. The guest is not touched either way. */
static int state_apply(void *context, void *staged) {
  SmsModScheduler *s = (SmsModScheduler *)context;
  const Staged *st = (const Staged *)staged;
  uint8_t *prior;
  uint32_t prior_len = 0U;
  int have_prior = 0, ok;
  if (!granted(s, SEGA_MOD_CAP_STATE_MOD) || st->disabled != 0U) return 0;
  prior = (uint8_t *)malloc(SMS_MOD_BLOB_MAX);
  if (prior == NULL) return 1;
  if (!s->disabled && s->config.provider->save_state(s->config.provider_state, prior, SMS_MOD_BLOB_MAX, &prior_len) == 0 && prior_len <= SMS_MOD_BLOB_MAX) have_prior = 1;
  ok = s->config.provider->load_state(s->config.provider_state, st->blob, st->blob_len) == 0;
  /* an already disabled mod keeps its first diagnostic: a refused restore changes nothing about it */
  if (!ok && !s->disabled && (!have_prior || s->config.provider->load_state(s->config.provider_state, prior, prior_len) != 0)) {
    s->phase = SEGA_MOD_PHASE_RESET;
    s->frame = s->machine->rt.state.cycles / SMS_CYCLES_PER_FRAME;
    record_fault(s, "state");
  }
  free(prior);
  return ok ? 0 : 1;
}

static void state_commit(void *context, void *staged, SmsMachine *machine, SmsPad *pad) {
  SmsModScheduler *s = (SmsModScheduler *)context;
  Staged *st = (Staged *)staged;
  s->started = st->started;
  s->disabled = st->disabled;
  memcpy(s->sampled, st->sampled, 2U);
  memcpy(s->final_pad, st->final_pad, 2U);
  memcpy(s->previous_final, st->previous_final, 2U);
  s->callbacks_run = st->callbacks_run;
  s->fault = st->fault;
  s->write_stream = st->stream;
  s->write_total = st->write_total;
  s->presentation_valid = 0U;
  s->render_ram_valid = 0U;
  s->intercepting = st->intercepting;
  s->guest_input.p1 = st->guest[0];
  s->guest_input.p2 = st->guest[1];
  s->guest_input.pause = machine->input.pause;
  pad->intercept = st->intercepting ? &s->guest_input : NULL;
  free(st);
}

static void state_discard(void *context, void *staged) {
  (void)context;
  free(staged);
}

/* Everything besides identity and schema that decides whether a state is usable. */
static void compat_digest(const SmsModScheduler *s, uint8_t out[32]) {
  static const char tag[] = "segarecomp.sms.mod.compat.v1";
  SmsSha256 sha;
  uint32_t i;
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, tag, sizeof tag - 1U);
  sms_sha256_update_u32(&sha, s->host.abi_major);
  sms_sha256_update_u64(&sha, s->host.granted[0]);
  sms_sha256_update_u64(&sha, s->host.granted[1]);
  sms_sha256_update_u32(&sha, s->config.phase_mask);
  sms_sha256_update_u32(&sha, s->config.provider->implementation_version);
  sms_sha256_update_u32(&sha, s->config.range_count);
  for (i = 0U; i < s->config.range_count; ++i) {
    sms_sha256_update_u32(&sha, s->ranges[i].space);
    sms_sha256_update_u32(&sha, s->ranges[i].offset);
    sms_sha256_update_u32(&sha, s->ranges[i].length);
    sms_sha256_update_u32(&sha, s->ranges[i].modes);
  }
  sms_sha256_final(&sha, out);
}

void sms_mod_scheduler_state(SmsModScheduler *s, SmsModState *out) {
  memset(out, 0, sizeof *out);
  out->context = s;
  memcpy(out->identity, s->config.identity_sha256, 32U);
  out->state_schema = s->config.state_schema;
  compat_digest(s, out->compat);
  out->put = state_put;
  out->stage = state_stage;
  out->apply = state_apply;
  out->commit = state_commit;
  out->discard = state_discard;
}

/* ---- SEG-141-T007 (ADR 0327): the set. It calls the single-mod runner above; nothing here grants, checks or commits anything itself. ---- */
static void set_run_all(SmsModSet *set, uint32_t phase, uint64_t frame) {
  uint32_t i;
  for (i = 0U; i < set->count; ++i) run_phase(set->members[i], phase, frame);
}

static void set_release_pad(SmsModSet *set) {
  uint32_t i;
  for (i = 0U; i < set->count; ++i)
    if (live_input(set->members[i])) return;
  if (set->pad != NULL) set->pad->intercept = NULL;
}

static void set_boundary(void *context, SmsMachine *machine, uint64_t frame) {
  SmsModSet *set = (SmsModSet *)context;
  uint32_t i;
  (void)machine;
  if (!set->members[0]->started) {
    for (i = 0U; i < set->count; ++i) reset_session(set->members[i], frame);
  } else if (frame != 0U) {
    set_run_all(set, SEGA_MOD_PHASE_FRAME_END, frame - 1U);
    for (i = 0U; i < set->count; ++i) run_render(set->members[i], frame - 1U);
    if (set->frame_done != NULL) set->frame_done(set->frame_done_context, frame - 1U);
  }
  for (i = 0U; i < set->count; ++i) {
    set->members[i]->previous_final[0] = set->members[i]->final_pad[0];
    set->members[i]->previous_final[1] = set->members[i]->final_pad[1];
  }
  set_run_all(set, SEGA_MOD_PHASE_FRAME_BEGIN, frame);
  set_release_pad(set);
}

static void set_sampled(void *context, SmsMachine *machine, uint64_t frame) {
  SmsModSet *set = (SmsModSet *)context;
  uint8_t chain[2];
  uint32_t i;
  int latch = 0;
  chain[0] = machine->input.p1;
  chain[1] = machine->input.p2;
  for (i = 0U; i < set->count; ++i) {
    SmsModScheduler *s = set->members[i];
    s->sampled[0] = machine->input.p1;
    s->sampled[1] = machine->input.p2;
    s->final_pad[0] = chain[0];
    s->final_pad[1] = chain[1];
    run_phase(s, SEGA_MOD_PHASE_INPUT, frame); /* a faulted callback leaves final_pad as it was */
    chain[0] = s->final_pad[0];
    chain[1] = s->final_pad[1];
  }
  set->guest_input.p1 = chain[0];
  set->guest_input.p2 = chain[1];
  set->guest_input.pause = machine->input.pause;
  for (i = 0U; i < set->count; ++i) {
    SmsModScheduler *s = set->members[i];
    s->final_pad[0] = chain[0];
    s->final_pad[1] = chain[1];
    s->guest_input = set->guest_input;
    s->intercepting = (uint8_t)(live_input(s) && set->pad != NULL);
    latch = latch || s->intercepting != 0U;
  }
  if (set->pad != NULL) set->pad->intercept = latch ? &set->guest_input : NULL;
  set_run_all(set, SEGA_MOD_PHASE_POST_INPUT, frame);
  set_release_pad(set);
}

static void set_reset(void *context, SmsMachine *machine) {
  SmsModSet *set = (SmsModSet *)context;
  uint32_t i;
  (void)machine;
  for (i = 0U; i < set->count; ++i) {
    forget_session(set->members[i]);
    set->members[i]->intercepting = 0U;
  }
  if (set->pad != NULL) set->pad->intercept = NULL;
}

void sms_mod_set_detach(SmsModSet *set) {
  uint32_t i;
  if (set == NULL || set->machine == NULL) return;
  if (set->machine->frame_hooks == &set->hooks) set->machine->frame_hooks = NULL;
  if (set->pad != NULL) set->pad->intercept = NULL;
  for (i = 0U; i < set->count; ++i) {
    set->members[i]->intercepting = 0U;
    set->members[i]->machine = NULL;
  }
  set->machine = NULL;
  set->count = 0U;
}

int sms_mod_set_attach(SmsModSet *set, SmsMachine *machine, SmsPad *pad, const SmsVdp *vdp, SmsModScheduler *const *schedulers, const SmsModConfig *configs, uint32_t count, const uint8_t identity[32],
                       void (*frame_done)(void *context, uint64_t frame), void *frame_done_context, const char **rejection, uint32_t *member) {
  const char *dummy = NULL;
  uint32_t dummy_member = 0U, i, j, any = 0U;
  if (rejection == NULL) rejection = &dummy;
  if (member == NULL) member = &dummy_member;
  *rejection = "";
  *member = count;
  if (set == NULL || machine == NULL || pad == NULL || schedulers == NULL || configs == NULL || identity == NULL || count == 0U || count > SMS_MOD_SET_MEMBERS_MAX) {
    *rejection = "E_SET_SIZE";
    return 1;
  }
  for (i = 0U; i < 32U; ++i) any |= identity[i];
  if (any == 0U) {
    *rejection = "E_SET_IDENTITY";
    return 1;
  }
  if (machine->frame_hooks != NULL) {
    *rejection = "E_SCHEDULER_BUSY";
    return 1;
  }
  memset(set, 0, sizeof *set);
  memcpy(set->identity, identity, 32U);
  set->machine = machine;
  set->pad = pad;
  for (i = 0U; i < count; ++i) {
    SmsModScheduler *s = schedulers[i];
    *member = i;
    for (j = 0U; j < i; ++j)
      if (schedulers[j] == s) {
        *rejection = "E_PROVIDER_ENTRY";
        goto refused;
      }
    if (sms_mod_scheduler_attach(s, machine, pad, vdp, &configs[i], rejection) != 0) goto refused;
    machine->frame_hooks = NULL; /* the set installs its own once every member is accepted */
    s->in_set = 1U;
    set->members[set->count++] = s;
    for (j = 0U; j < i; ++j)
      if (strcmp(set->members[j]->mod_id, s->mod_id) == 0) {
        *rejection = "E_SET_DUPLICATE_MOD";
        goto refused;
      }
  }
  set->frame_done = frame_done;
  set->frame_done_context = frame_done_context;
  set->hooks.context = set;
  set->hooks.reset = set_reset;
  set->hooks.boundary = set_boundary;
  set->hooks.sampled = set_sampled;
  machine->frame_hooks = &set->hooks;
  *member = count;
  return 0;
refused:
  sms_mod_set_detach(set);
  return 1;
}

/* body: member_count u32 | per member (length u32, the member's own body) */
static uint32_t set_state_put(void *context, uint8_t *out, uint32_t cap, SmsModStateStatus *status) {
  SmsModSet *set = (SmsModSet *)context;
  Out o;
  uint8_t *scratch;
  uint32_t i;
  o.p = out; o.cap = cap; o.n = 0U; o.error = 0;
  *status = SMS_MOD_STATE_NOT_CAPTURABLE;
  if (set->machine == NULL) return UINT32_MAX;
  scratch = (uint8_t *)malloc(SMS_MOD_BODY_MAX);
  if (scratch == NULL) return UINT32_MAX;
  out_u32(&o, set->count);
  for (i = 0U; i < set->count; ++i) {
    const uint32_t n = state_put(set->members[i], scratch, SMS_MOD_BODY_MAX, status);
    if (n > SMS_MOD_BODY_MAX) {
      free(scratch);
      return UINT32_MAX;
    }
    out_u32(&o, n);
    out_bytes(&o, scratch, n);
  }
  free(scratch);
  if (o.error) {
    *status = SMS_MOD_STATE_UNAVAILABLE;
    return UINT32_MAX;
  }
  *status = SMS_MOD_STATE_OK;
  return o.n;
}

typedef struct StagedSet { void *members[SMS_MOD_SET_MEMBERS_MAX]; } StagedSet;

static void set_state_discard(void *context, void *staged) {
  StagedSet *st = (StagedSet *)staged;
  uint32_t i;
  (void)context;
  if (st == NULL) return;
  for (i = 0U; i < SMS_MOD_SET_MEMBERS_MAX; ++i) free(st->members[i]);
  free(st);
}

static void *set_state_stage(void *context, const uint8_t *body, uint32_t length) {
  SmsModSet *set = (SmsModSet *)context;
  StagedSet *st;
  In in;
  uint32_t i;
  if (set->machine == NULL) return NULL;
  st = (StagedSet *)calloc(1U, sizeof *st);
  if (st == NULL) return NULL;
  in.p = body; in.size = length; in.off = 0U; in.error = 0;
  if (in_u32(&in) != set->count || in.error) goto bad;
  for (i = 0U; i < set->count; ++i) {
    const uint32_t n = in_u32(&in);
    if (in.error || n > in.size - in.off) goto bad;
    st->members[i] = state_stage(set->members[i], in.p + in.off, n);
    if (st->members[i] == NULL || ((const Staged *)st->members[i])->started != ((const Staged *)st->members[0])->started) goto bad;
    in.off += n;
  }
  if (in.off != in.size) goto bad;
  return st;
bad:
  set_state_discard(context, st);
  return NULL;
}

static int set_state_apply(void *context, void *staged) {
  SmsModSet *set = (SmsModSet *)context;
  StagedSet *st = (StagedSet *)staged;
  uint8_t *prior[SMS_MOD_SET_MEMBERS_MAX] = {NULL};
  uint32_t prior_len[SMS_MOD_SET_MEMBERS_MAX] = {0U};
  uint32_t i, j;
  int failed = 0;
  for (i = 0U; !failed && i < set->count; ++i) { /* the running members' blobs, taken before any of them changes */
    SmsModScheduler *s = set->members[i];
    if (!granted(s, SEGA_MOD_CAP_STATE_MOD) || s->disabled) continue;
    prior[i] = (uint8_t *)malloc(SMS_MOD_BLOB_MAX);
    if (prior[i] == NULL) failed = 1;
    else if (s->config.provider->save_state(s->config.provider_state, prior[i], SMS_MOD_BLOB_MAX, &prior_len[i]) != 0 || prior_len[i] > SMS_MOD_BLOB_MAX) {
      free(prior[i]);
      prior[i] = NULL;
    }
  }
  for (i = 0U; !failed && i < set->count; ++i) {
    if (state_apply(set->members[i], st->members[i]) == 0) continue;
    failed = 1;
    for (j = 0U; j < i; ++j) { /* put back the members that had already loaded the other state */
      SmsModScheduler *s = set->members[j];
      if (!granted(s, SEGA_MOD_CAP_STATE_MOD) || ((const Staged *)st->members[j])->disabled != 0U || s->disabled) continue;
      if (prior[j] == NULL || s->config.provider->load_state(s->config.provider_state, prior[j], prior_len[j]) != 0) {
        s->phase = SEGA_MOD_PHASE_RESET;
        s->frame = set->machine->rt.state.cycles / SMS_CYCLES_PER_FRAME;
        record_fault(s, "state");
      }
    }
  }
  for (i = 0U; i < set->count; ++i) free(prior[i]);
  if (failed) set_release_pad(set);
  return failed;
}

static void set_state_commit(void *context, void *staged, SmsMachine *machine, SmsPad *pad) {
  SmsModSet *set = (SmsModSet *)context;
  StagedSet *st = (StagedSet *)staged;
  uint8_t active = 0U, p1 = 0U, p2 = 0U;
  uint32_t i;
  for (i = 0U; i < set->count; ++i) {
    const Staged *m = (const Staged *)st->members[i];
    if (m->intercepting != 0U) {
      active = 1U;
      p1 = m->guest[0];
      p2 = m->guest[1];
    }
    state_commit(set->members[i], st->members[i], machine, pad); /* frees the member's staged state */
    st->members[i] = NULL;
  }
  set->guest_input.p1 = p1;
  set->guest_input.p2 = p2;
  set->guest_input.pause = machine->input.pause;
  pad->intercept = active ? &set->guest_input : NULL;
  free(st);
}

void sms_mod_set_state(SmsModSet *set, SmsModState *out) {
  static const char tag[] = "segarecomp.sms.mod.set.compat.v1";
  SmsSha256 sha;
  uint32_t i;
  memset(out, 0, sizeof *out);
  out->context = set;
  memcpy(out->identity, set->identity, 32U);
  out->state_schema = 1U; /* the set body format; each member's declared schema is part of `compat` */
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, tag, sizeof tag - 1U);
  sms_sha256_update_u32(&sha, set->count);
  for (i = 0U; i < set->count; ++i) {
    uint8_t member_compat[32];
    compat_digest(set->members[i], member_compat);
    sms_sha256_update(&sha, set->members[i]->config.identity_sha256, 32U);
    sms_sha256_update_u32(&sha, set->members[i]->config.state_schema);
    sms_sha256_update(&sha, member_compat, 32U);
  }
  sms_sha256_final(&sha, out->compat);
  out->put = set_state_put;
  out->stage = set_state_stage;
  out->apply = set_state_apply;
  out->commit = set_state_commit;
  out->discard = set_state_discard;
}
