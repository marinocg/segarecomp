/* SEG-138-T001/T002 (ADR 0297): the Genesis mod phase scheduler. See mod_scheduler.h. Strict C11; no allocation; no wall clock, host RNG or host pointers are ever offered to a mod. */
#include "mod_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ABI 2 service functions carry no context pointer, so the scheduler whose callback is running is reachable through this single pointer, set only for the duration of a callback. */
static GenesisModScheduler *g_current = NULL;

#define PHASE_BIT(p) (UINT32_C(1) << (p))
#define HOOK_PHASES (PHASE_BIT(SEGA_MOD_PHASE_HOOK_ENTRY) | PHASE_BIT(SEGA_MOD_PHASE_HOOK_EXIT))
#define VALID_PHASES (PHASE_BIT(SEGA_MOD_PHASE_RESET) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | \
                      PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | PHASE_BIT(SEGA_MOD_PHASE_RENDER) | HOOK_PHASES)
#define READ_PHASES (PHASE_BIT(SEGA_MOD_PHASE_RESET) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | HOOK_PHASES)
#define WRITE_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | HOOK_PHASES)
#define INPUT_READ_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT))

static uint64_t cap_bit(unsigned id) { return UINT64_C(1) << (id % 64U); }

void genesis_mod_scheduler_supported(uint64_t out[2]) {
  out[0] = cap_bit(SEGA_MOD_CAP_HOST_LOG) | cap_bit(SEGA_MOD_CAP_FRAME_WORK_RAM) | cap_bit(SEGA_MOD_CAP_MEMORY_READ) | cap_bit(SEGA_MOD_CAP_MEMORY_WRITE) | cap_bit(SEGA_MOD_CAP_INPUT_READ) |
           cap_bit(SEGA_MOD_CAP_INPUT_INTERCEPT) | cap_bit(SEGA_MOD_CAP_HOOK_FUNCTION) | cap_bit(SEGA_MOD_CAP_STATE_MOD) |
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

/* ---- the register view of the function hook in flight (space SEGA_GEN_SPACE_M68K_HOOK_REGS, mod-execution-phases.md 5.3) ---- */
static int in_hook_phase(const GenesisModScheduler *s) { return (HOOK_PHASES & PHASE_BIT(s->phase)) != 0U && s->hook_site != NULL; }

static int service_reg_read(GenesisModScheduler *s, uint32_t slot, uint32_t width, uint32_t *value) {
  if (value == NULL || width != 4U || slot > 31U) return violation(s, SEGA_MOD_E_RANGE);
  if (slot == GENESIS_HOOK_REG_CYCLES) { /* the deterministic M68K cycle counter: guest time, visible in every live phase (never in render, which is outside guest time) */
    if (s->phase == SEGA_MOD_PHASE_RENDER) return violation(s, SEGA_MOD_E_PHASE);
    *value = (uint32_t)(s->runtime->scheduler.master_ticks / 7U);
    return SEGA_MOD_OK;
  }
  if (!granted(s, SEGA_MOD_CAP_HOOK_FUNCTION)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (!in_hook_phase(s)) return violation(s, SEGA_MOD_E_PHASE);
  if (((s->hook_site->reads >> slot) & 1U) == 0U) return violation(s, SEGA_MOD_E_RANGE); /* only the slots the declaration named */
  if (s->staged_reg_set >> slot & 1U) { *value = s->staged_reg[slot]; return SEGA_MOD_OK; } /* reads observe this callback's staged results */
  if (!genesis_hook_reg_read(s->runtime, s->hook_entry_sp, slot, value)) return violation(s, SEGA_MOD_E_RANGE);
  return SEGA_MOD_OK;
}

static int service_reg_write(GenesisModScheduler *s, uint32_t slot, uint32_t width, uint32_t value) {
  if (width != 4U || slot > 31U) return violation(s, SEGA_MOD_E_RANGE);
  if (!granted(s, SEGA_MOD_CAP_HOOK_FUNCTION)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (!in_hook_phase(s) || s->phase != SEGA_MOD_PHASE_HOOK_ENTRY) return violation(s, SEGA_MOD_E_PHASE);
  if ((s->hook_site->mode & GENESIS_HOOK_MODE_REPLACE) == 0U) return violation(s, SEGA_MOD_E_READONLY); /* only a replacement produces results */
  if (((s->hook_site->writes >> slot) & 1U) == 0U || ((GENESIS_HOOK_WRITABLE_MASK >> slot) & 1U) == 0U) return violation(s, SEGA_MOD_E_RANGE);
  s->staged_reg[slot] = value;
  s->staged_reg_set |= UINT32_C(1) << slot;
  return SEGA_MOD_OK;
}

static int service_read(uint32_t space, uint32_t offset, uint32_t width, uint32_t *value) {
  GenesisModScheduler *s = g_current;
  const int render = s != NULL && s->phase == SEGA_MOD_PHASE_RENDER;
  uint32_t i, result = 0;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (space == SEGA_GEN_SPACE_M68K_HOOK_REGS) return service_reg_read(s, offset, width, value);
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
  if (space == SEGA_GEN_SPACE_M68K_HOOK_REGS) return service_reg_write(s, offset, width, value);
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
  s->staged_reg_set = 0U;
  if ((HOOK_PHASES & PHASE_BIT(phase)) == 0U) s->hook_site = NULL;
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
  if (s->hook_site != NULL && (HOOK_PHASES & PHASE_BIT(s->phase)) != 0U)
    (void)snprintf(s->fault.text, sizeof(s->fault.text), "E_MOD_FAULT cause=%s mod=%s phase=%s frame=%llu hook=%s", cause, s->config.mod_id, genesis_mod_phase_name(s->phase), (unsigned long long)s->frame,
                   s->hook_site->id);
  else
    (void)snprintf(s->fault.text, sizeof(s->fault.text), "E_MOD_FAULT cause=%s mod=%s phase=%s frame=%llu", cause, s->config.mod_id, genesis_mod_phase_name(s->phase), (unsigned long long)s->frame);
  emit(s, s->fault.text, (uint32_t)strlen(s->fault.text));
  if (s->runtime != NULL) s->runtime->mod_pad_active = 0U; /* the guest goes on with the physical pad */
}

static void sha_u32(GenesisSha256 *state, uint32_t v) {
  const uint8_t b[4] = {(uint8_t)(v >> 24U), (uint8_t)(v >> 16U), (uint8_t)(v >> 8U), (uint8_t)v};
  genesis_sha256_update(state, b, 4U);
}

static void stream_write(GenesisModScheduler *s, uint64_t frame, uint32_t phase, uint32_t space, const GenesisModStagedWrite *w) {
  sha_u32(&s->write_stream, (uint32_t)(frame >> 32U));
  sha_u32(&s->write_stream, (uint32_t)frame);
  sha_u32(&s->write_stream, phase);
  sha_u32(&s->write_stream, space);
  sha_u32(&s->write_stream, w->offset);
  sha_u32(&s->write_stream, w->width);
  sha_u32(&s->write_stream, w->value);
  ++s->write_total;
}

static void log_commit(GenesisModScheduler *s, uint32_t space, const GenesisModStagedWrite *w) {
  stream_write(s, s->frame, s->phase, space, w);
  if (s->write_log_count < GENESIS_MOD_WRITE_LOG_MAX) {
    GenesisModWriteRecord *r = &s->write_log[s->write_log_count++];
    r->frame = s->frame;
    r->phase = s->phase;
    r->space = space;
    r->offset = w->offset;
    r->width = w->width;
    r->value = w->value;
  } else {
    ++s->write_log_dropped;
  }
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
    log_commit(s, SEGA_GEN_SPACE_M68K_WORK_RAM, w);
  }
  if (s->phase == SEGA_MOD_PHASE_HOOK_ENTRY && s->hook_replaced) { /* a committed replacement: its register results, in slot order */
    for (i = 0; i < 32U; ++i) {
      GenesisModStagedWrite w;
      if (((s->staged_reg_set >> i) & 1U) == 0U) continue;
      (void)genesis_hook_reg_write(s->runtime, i, s->staged_reg[i]);
      w.offset = i;
      w.width = 4U;
      w.value = s->staged_reg[i];
      log_commit(s, SEGA_GEN_SPACE_M68K_HOOK_REGS, &w);
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

/* ---- static function hooks (ADR 0298): dispatch-boundary callbacks installed in GenesisRuntime::function_hooks ---- */
static const GenesisModHookBinding *find_binding(const GenesisModScheduler *s, const GenesisHookSiteInfo *site) {
  uint32_t i;
  for (i = 0; i < s->config.hook_count; ++i)
    if (strcmp(s->bindings[i].id, site->id) == 0) return &s->bindings[i];
  return NULL;
}

static int hook_enter(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t entry_sp) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  const GenesisModHookBinding *binding;
  int result, replaced_requested;
  if (!s->started || s->disabled || (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_HOOK_ENTRY)) == 0U) return 0;
  binding = find_binding(s, site);
  if (binding == NULL) return 0;
  begin_callback(s, SEGA_MOD_PHASE_HOOK_ENTRY, rt->virtual_frame_count);
  s->hook_site = site;
  s->hook_entry_sp = entry_sp;
  s->hook_replaced = 0U;
  ++s->hook_calls[0];
  result = s->config.provider->hook != NULL ? s->config.provider->hook(s->config.provider_state, &s->host, binding->hook_id) : 0;
  replaced_requested = result == 1;
  if (result != 0 && result != 1) s->external_cause = s->external_cause != NULL ? s->external_cause : "value";                                  /* only 0 (proceed) and 1 (replaced) exist */
  else if (replaced_requested && (site->mode & GENESIS_HOOK_MODE_REPLACE) == 0U) s->external_cause = s->external_cause != NULL ? s->external_cause : "value";   /* before/after cannot replace */
  else if (replaced_requested && (s->staged_reg_set & site->writes) != site->writes) s->external_cause = s->external_cause != NULL ? s->external_cause : "contract"; /* every declared result must be produced */
  s->hook_replaced = replaced_requested ? 1U : 0U;
  if ((site->mode & GENESIS_HOOK_MODE_REPLACE) != 0U && result == 0) { /* a declined replacement has no effects (ADR 0298): the original runs, and nothing the callback staged is committed */
    s->staged_count = 0U;
    s->staged_bytes = 0U;
    s->staged_log_count = 0U;
    s->staged_reg_set = 0U;
  }
  finish_callback(s, result != 0 && result != 1 ? 1 : 0);
  return s->disabled ? 0 : replaced_requested;   /* a faulted replacement never committed: the ORIGINAL function runs */
}

static void hook_leave(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t entry_sp) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  const GenesisModHookBinding *binding;
  int result;
  if (!s->started || s->disabled || (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_HOOK_EXIT)) == 0U) return;
  binding = find_binding(s, site);
  if (binding == NULL) return;
  begin_callback(s, SEGA_MOD_PHASE_HOOK_EXIT, rt->virtual_frame_count);
  s->hook_site = site;
  s->hook_entry_sp = entry_sp;
  s->hook_replaced = 0U;
  ++s->hook_calls[1];
  result = s->config.provider->hook != NULL ? s->config.provider->hook(s->config.provider_state, &s->host, binding->hook_id) : 0;
  if (result != 0) s->external_cause = s->external_cause != NULL ? s->external_cause : "value";
  finish_callback(s, result != 0 ? 1 : 0);
}

static void hook_skipped(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t kind) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  static const char *const names[4] = {"", "E_HOOK_FRAME_ABANDONED", "E_HOOK_UNPROVEN_CALLER", "E_HOOK_FRAME_OVERFLOW"};
  char line[160];
  if (kind < 1U || kind > 3U) return;
  ++s->hook_skips[kind];
  if (s->disabled) return; /* a disabled mod is inert: counters keep counting, no further diagnostic */
  if ((s->hook_skip_logged >> kind & 1U) != 0U) return; /* the counters count every occurrence; one diagnostic line per class keeps the log bounded */
  s->hook_skip_logged |= UINT32_C(1) << kind;
  (void)snprintf(line, sizeof line, "%s mod=%s hook=%s frame=%llu", names[kind], s->config.mod_id, site->id, (unsigned long long)rt->virtual_frame_count);
  emit(s, line, (uint32_t)strlen(line));
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
  if (config->phase_mask == 0U || (config->phase_mask & ~VALID_PHASES) != 0U) { /* none, unknown or bit 0 */
    *rejection = "E_PHASE_UNSUPPORTED";
    return 1;
  }
  if ((config->phase_mask & HOOK_PHASES) != 0U) { /* ADR 0298: hook phases need the capability and at least one binding; bindings need a hook phase */
    if (!sega_mod_is_granted(&s->host, SEGA_MOD_CAP_HOOK_FUNCTION)) {
      *rejection = "E_CAPABILITY_UNSUPPORTED";
      return 1;
    }
    if (config->hook_count == 0U || config->hooks == NULL || config->provider->hook == NULL) {
      *rejection = "E_PHASE_UNSUPPORTED";
      return 1;
    }
  }
  if (config->hook_count != 0U && ((config->phase_mask & HOOK_PHASES) == 0U || config->hooks == NULL || config->hook_count > GENESIS_MOD_HOOKS_MAX)) {
    *rejection = "E_HOOK_BINDING";
    return 1;
  }
  for (i = 0; i < config->hook_count; ++i) {
    uint32_t j;
    const char *id = config->hooks[i].id;
    if (id == NULL || id[0] == '\0' || strlen(id) > 64U) {
      *rejection = "E_HOOK_BINDING";
      return 1;
    }
    for (j = 0; j < i; ++j)
      if (strcmp(config->hooks[j].id, id) == 0 || config->hooks[j].hook_id == config->hooks[i].hook_id) {
        *rejection = "E_HOOK_BINDING";
        return 1;
      }
    s->bindings[i] = config->hooks[i];
  }
  if (sega_mod_is_granted(&s->host, SEGA_MOD_CAP_STATE_MOD)) { /* ADR 0299: a mod with state must supply both entries and declare its schema */
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
  if (config->site_count != 0U && config->sites == NULL) {
    *rejection = "E_HOOK_BINDING";
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
  {
    uint32_t any = 0U;
    for (i = 0; i < 32U; ++i) any |= config->identity_sha256[i];
    if (any != 0U) {
      memcpy(s->identity, config->identity_sha256, 32U);
    } else { /* a hand-built mod: the identity is the digest of its id, never empty */
      GenesisSha256 sha;
      static const char tag[] = "segarecomp.genesis.mod.identity.v0:";
      genesis_sha256_init(&sha);
      genesis_sha256_update(&sha, (const uint8_t *)tag, (uint32_t)(sizeof tag - 1U));
      genesis_sha256_update(&sha, (const uint8_t *)config->mod_id, (uint32_t)strlen(config->mod_id));
      genesis_sha256_final(&sha, s->identity);
    }
  }
  s->config.hooks = s->bindings; /* the scheduler owns copies: the caller's array need not outlive attach */
  if ((config->phase_mask & HOOK_PHASES) != 0U) {
    s->function_hooks.ctx = s;
    s->function_hooks.enter = hook_enter;
    s->function_hooks.leave = hook_leave;
    s->function_hooks.skipped = hook_skipped;
    rt->function_hooks = &s->function_hooks;
  }
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
  if (s->runtime->function_hooks == &s->function_hooks) s->runtime->function_hooks = NULL;
  s->runtime->mod_pad_active = 0U;
  s->runtime = NULL;
}

void genesis_mod_write_log_digest(const GenesisModScheduler *s, uint8_t out[32]) {
  GenesisSha256 sha = s->write_stream; /* finalize a copy: the stream keeps running */
  sha_u32(&sha, (uint32_t)(s->write_total >> 32U));
  sha_u32(&sha, (uint32_t)s->write_total);
  genesis_sha256_final(&sha, out);
}

/* ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * SEG-138-T006 (ADR 0299): mod state in the Genesis save-state / rewind container. The container (genesis_snapshot.c) owns identity, schema and framing; this is the body codec.
 * Body, little-endian: flags u8 x4 (disabled, started, faulted, pad_active) | pads u8 x8 (sampled, final, previous, mod_pad) | callbacks_run u64 | hook_calls u64 x2 | hook_skips u64 x4 |
 * hook_skip_logged u32 | [fault: phase u32, frame u64, cause_len u8, cause, text_len u16, text] | write stream (h u32 x8, length u64, used u32, block x64) | write_total u64 |
 * hook depth u32, abandoned/unproven/overflowed u64 x3, frames (site index, return pc, entry sp) u32 x3 each | blob_len u32 | blob.
 * Not saved: the retained diagnostic write log (the streamed digest is exact and continues), staged data (none exists at the barrier), the render copy.
 * ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct StateOut {
  uint8_t *p;
  uint32_t cap, n;
  int error;
} StateOut;
static void so_bytes(StateOut *o, const void *src, uint32_t count) {
  if (o->error || count > o->cap - o->n) { o->error = 1; return; }
  if (count != 0U) memcpy(o->p + o->n, src, count);
  o->n += count;
}
static void so_u8(StateOut *o, uint8_t v) { so_bytes(o, &v, 1U); }
static void so_u16(StateOut *o, uint16_t v) { const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8U)}; so_bytes(o, b, 2U); }
static void so_u32(StateOut *o, uint32_t v) { uint8_t b[4]; unsigned i; for (i = 0; i < 4U; ++i) b[i] = (uint8_t)(v >> (8U * i)); so_bytes(o, b, 4U); }
static void so_u64(StateOut *o, uint64_t v) { uint8_t b[8]; unsigned i; for (i = 0; i < 8U; ++i) b[i] = (uint8_t)(v >> (8U * i)); so_bytes(o, b, 8U); }

typedef struct StateIn {
  const uint8_t *p;
  uint32_t size, off;
  int error;
} StateIn;
static const uint8_t *si_take(StateIn *in, uint32_t count) {
  const uint8_t *at;
  if (in->error || count > in->size - in->off) { in->error = 1; return NULL; }
  at = in->p + in->off;
  in->off += count;
  return at;
}
static uint8_t si_u8(StateIn *in) { const uint8_t *b = si_take(in, 1U); return b != NULL ? b[0] : 0U; }
static uint16_t si_u16(StateIn *in) { const uint8_t *b = si_take(in, 2U); return b != NULL ? (uint16_t)(b[0] | ((unsigned)b[1] << 8U)) : 0U; }
static uint32_t si_u32(StateIn *in) { const uint8_t *b = si_take(in, 4U); uint32_t v = 0U; unsigned i; if (b == NULL) return 0U; for (i = 0; i < 4U; ++i) v |= (uint32_t)b[i] << (8U * i); return v; }
static uint64_t si_u64(StateIn *in) { const uint8_t *b = si_take(in, 8U); uint64_t v = 0U; unsigned i; if (b == NULL) return 0U; for (i = 0; i < 8U; ++i) v |= (uint64_t)b[i] << (8U * i); return v; }

typedef struct StagedModState {
  uint8_t disabled, started, faulted, pad_active;
  uint8_t sampled[2], final_pad[2], previous_final[2], mod_pad[2];
  uint64_t callbacks_run, hook_calls[2], hook_skips[4];
  uint32_t hook_skip_logged;
  uint32_t fault_phase;
  uint64_t fault_frame;
  char fault_cause[32];
  char fault_text[192];
  GenesisSha256 stream;
  uint64_t write_total;
  uint32_t depth;
  uint64_t abandoned, unproven, overflowed;
  struct { uint32_t site, return_pc, entry_sp; } frames[GENESIS_HOOK_FRAMES_MAX];
  uint32_t blob_len;
  uint8_t blob[GENESIS_SNAPSHOT_MOD_BLOB_MAX];
} StagedModState;

static int guest_affecting(const GenesisModScheduler *s) {
  return granted(s, SEGA_MOD_CAP_MEMORY_WRITE) || granted(s, SEGA_MOD_CAP_INPUT_INTERCEPT) || granted(s, SEGA_MOD_CAP_HOOK_FUNCTION) || granted(s, SEGA_MOD_CAP_AUDIO_COMMAND_SEND);
}

static uint32_t state_put(void *ctx, uint8_t *out, uint32_t cap, GenesisSnapshotStatus *status) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  StateOut o;
  uint32_t i, depth = 0U, blob_len = 0U, blob_at;
  const uint32_t fail = UINT32_MAX;
  o.p = out; o.cap = cap; o.n = 0U; o.error = 0;
  *status = GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  if (s->runtime == NULL || s->in_callback) return fail;
  if (guest_affecting(s) && !granted(s, SEGA_MOD_CAP_STATE_MOD)) { *status = GENESIS_SNAPSHOT_MOD_UNAVAILABLE; return fail; } /* E_STATE_UNAVAILABLE: never a state that silently drops the mod's effect */
  if (s->runtime->function_hooks == &s->function_hooks) depth = s->function_hooks.depth;
  if (depth > GENESIS_HOOK_FRAMES_MAX || (depth != 0U && s->config.site_count == 0U)) return fail;
  so_u8(&o, s->disabled); so_u8(&o, s->started); so_u8(&o, s->fault.faulted); so_u8(&o, s->runtime->mod_pad_active);
  so_u8(&o, s->sampled[0]); so_u8(&o, s->sampled[1]); so_u8(&o, s->final_pad[0]); so_u8(&o, s->final_pad[1]);
  so_u8(&o, s->previous_final[0]); so_u8(&o, s->previous_final[1]);
  so_u8(&o, s->runtime->mod_pad_active ? s->runtime->mod_pad1 : 0U); so_u8(&o, s->runtime->mod_pad_active ? s->runtime->mod_pad2 : 0U); /* a stale latch behind an inactive interception is not state */
  so_u64(&o, s->callbacks_run);
  for (i = 0; i < 2U; ++i) so_u64(&o, s->hook_calls[i]);
  for (i = 0; i < 4U; ++i) so_u64(&o, s->hook_skips[i]);
  so_u32(&o, s->hook_skip_logged);
  if (s->fault.faulted) {
    const uint32_t cause_len = s->fault.cause != NULL ? (uint32_t)strlen(s->fault.cause) : 0U, text_len = (uint32_t)strlen(s->fault.text);
    if (cause_len == 0U || cause_len > 31U || text_len > 191U) return fail;
    so_u32(&o, s->fault.phase);
    so_u64(&o, s->fault.frame);
    so_u8(&o, (uint8_t)cause_len);
    so_bytes(&o, s->fault.cause, cause_len);
    so_u16(&o, (uint16_t)text_len);
    so_bytes(&o, s->fault.text, text_len);
  }
  for (i = 0; i < 8U; ++i) so_u32(&o, s->write_stream.h[i]);
  so_u64(&o, s->write_stream.length);
  so_u32(&o, s->write_stream.used);
  so_bytes(&o, s->write_stream.block, 64U);
  so_u64(&o, s->write_total);
  so_u32(&o, depth);
  so_u64(&o, s->function_hooks.abandoned); so_u64(&o, s->function_hooks.unproven_callers); so_u64(&o, s->function_hooks.overflowed);
  for (i = 0; i < depth; ++i) {
    const GenesisHookFrame *f = &s->function_hooks.frames[i];
    if (f->site < s->config.sites || f->site >= s->config.sites + s->config.site_count) return fail;
    so_u32(&o, (uint32_t)(f->site - s->config.sites));
    so_u32(&o, f->return_pc);
    so_u32(&o, f->entry_sp);
  }
  /* the mod's own blob: only an enabled mod with `state.mod` has one (a disabled mod is inert; its flag is the whole state) */
  if (granted(s, SEGA_MOD_CAP_STATE_MOD) && !s->disabled) {
    uint32_t len = 0U;
    if (o.error || o.cap - o.n < GENESIS_SNAPSHOT_MOD_BLOB_MAX + 4U) return fail;
    blob_at = o.n + 4U;
    if (s->config.provider->save_state(s->config.provider_state, o.p + blob_at, GENESIS_SNAPSHOT_MOD_BLOB_MAX, &len) != 0 || len > GENESIS_SNAPSHOT_MOD_BLOB_MAX) { *status = GENESIS_SNAPSHOT_MOD_UNAVAILABLE; return fail; }
    blob_len = len;
    so_u32(&o, blob_len);
    o.n += blob_len;
  } else {
    so_u32(&o, 0U);
  }
  if (o.error) return fail;
  *status = GENESIS_SNAPSHOT_OK;
  return o.n;
}

static void *state_stage(void *ctx, const uint8_t *body, uint32_t length) {
  const GenesisModScheduler *s = (const GenesisModScheduler *)ctx;
  StagedModState *st;
  StateIn in;
  uint32_t i, cause_len, text_len;
  const uint8_t *text;
  if (s->runtime == NULL || s->in_callback) return NULL;
  st = (StagedModState *)malloc(sizeof *st);
  if (st == NULL) return NULL;
  memset(st, 0, sizeof *st);
  in.p = body; in.size = length; in.off = 0U; in.error = 0;
  st->disabled = si_u8(&in); st->started = si_u8(&in); st->faulted = si_u8(&in); st->pad_active = si_u8(&in);
  for (i = 0; i < 2U; ++i) st->sampled[i] = si_u8(&in);
  for (i = 0; i < 2U; ++i) st->final_pad[i] = si_u8(&in);
  for (i = 0; i < 2U; ++i) st->previous_final[i] = si_u8(&in);
  for (i = 0; i < 2U; ++i) st->mod_pad[i] = si_u8(&in);
  st->callbacks_run = si_u64(&in);
  for (i = 0; i < 2U; ++i) st->hook_calls[i] = si_u64(&in);
  for (i = 0; i < 4U; ++i) st->hook_skips[i] = si_u64(&in);
  st->hook_skip_logged = si_u32(&in);
  if (in.error || st->disabled > 1U || st->started > 1U || st->faulted > 1U || st->pad_active > 1U || st->disabled != st->faulted || (st->faulted != 0U && st->started == 0U) ||
      (st->hook_skip_logged & ~0xEU) != 0U || st->hook_skips[0] != 0U)
    goto bad;
  if (st->pad_active != 0U && (st->disabled != 0U || st->started == 0U || (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_INPUT)) == 0U)) goto bad; /* only a live `input` mod latches the pad */
  if (st->pad_active == 0U && (st->mod_pad[0] != 0U || st->mod_pad[1] != 0U)) goto bad;
  if (st->started == 0U && (st->callbacks_run != 0U || st->pad_active != 0U)) goto bad;
  if (st->faulted != 0U) {
    st->fault_phase = si_u32(&in);
    st->fault_frame = si_u64(&in);
    cause_len = si_u8(&in);
    if (in.error || cause_len == 0U || cause_len > 31U || genesis_mod_phase_name(st->fault_phase)[0] == '\0') goto bad;
    text = si_take(&in, cause_len);
    if (text == NULL) goto bad;
    memcpy(st->fault_cause, text, cause_len);
    text_len = si_u16(&in);
    if (in.error || text_len == 0U || text_len > 191U) goto bad;
    text = si_take(&in, text_len);
    if (text == NULL) goto bad;
    memcpy(st->fault_text, text, text_len);
    for (i = 0; i < cause_len; ++i) if ((uint8_t)st->fault_cause[i] < 0x21U || (uint8_t)st->fault_cause[i] > 0x7EU) goto bad;
    for (i = 0; i < text_len; ++i) if ((uint8_t)st->fault_text[i] < 0x20U || (uint8_t)st->fault_text[i] > 0x7EU) goto bad;
    if (strncmp(st->fault_text, "E_MOD_FAULT cause=", 18U) != 0) goto bad;
  }
  for (i = 0; i < 8U; ++i) st->stream.h[i] = si_u32(&in);
  st->stream.length = si_u64(&in);
  st->stream.used = si_u32(&in);
  { const uint8_t *b = si_take(&in, 64U); if (b != NULL) memcpy(st->stream.block, b, 64U); }
  st->write_total = si_u64(&in);
  if (in.error || st->stream.used >= 64U || (st->stream.length & 63U) != st->stream.used || st->stream.length != st->write_total * 28U) goto bad; /* 7 big-endian u32 per committed write */
  st->depth = si_u32(&in);
  st->abandoned = si_u64(&in); st->unproven = si_u64(&in); st->overflowed = si_u64(&in);
  if (in.error || st->depth > GENESIS_HOOK_FRAMES_MAX) goto bad;
  if (s->runtime->function_hooks != &s->function_hooks && (st->depth != 0U || st->abandoned != 0U || st->unproven != 0U || st->overflowed != 0U)) goto bad; /* no hook phases: no frames, no counters */
  for (i = 0; i < st->depth; ++i) {
    st->frames[i].site = si_u32(&in); st->frames[i].return_pc = si_u32(&in); st->frames[i].entry_sp = si_u32(&in);
    if (in.error || st->frames[i].site >= s->config.site_count || (s->config.sites[st->frames[i].site].mode & GENESIS_HOOK_MODE_AFTER) == 0U) goto bad; /* only `after` sites keep a frame */
  }
  st->blob_len = si_u32(&in);
  if (in.error || st->blob_len > GENESIS_SNAPSHOT_MOD_BLOB_MAX || st->blob_len != in.size - in.off) goto bad;     /* exact: no trailing bytes */
  if (st->blob_len != 0U && (!granted(s, SEGA_MOD_CAP_STATE_MOD) || st->disabled != 0U)) goto bad;               /* a mod without state, or a disabled one, has none */
  if (st->blob_len != 0U) memcpy(st->blob, in.p + in.off, st->blob_len);
  return st;
bad:
  free(st);
  return NULL;
}

/* The only fallible step: the mod's own load. A mod that refuses (or a Luau VM whose load faults) must not leave a half-restored session: the previous blob is put back when the mod can take it,
 * otherwise the mod is disabled with cause `state` -- in both cases the guest state is not touched, because the container runs this before replacing anything. */
static int state_apply(void *ctx, void *staged) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  const StagedModState *st = (const StagedModState *)staged;
  uint8_t *prior;
  uint32_t prior_len = 0U;
  int have_prior = 0, ok;
  if (!granted(s, SEGA_MOD_CAP_STATE_MOD) || st->disabled != 0U) return 0;
  prior = (uint8_t *)malloc(GENESIS_SNAPSHOT_MOD_BLOB_MAX);
  if (prior == NULL) return 1;
  if (!s->disabled && s->config.provider->save_state(s->config.provider_state, prior, GENESIS_SNAPSHOT_MOD_BLOB_MAX, &prior_len) == 0 && prior_len <= GENESIS_SNAPSHOT_MOD_BLOB_MAX) have_prior = 1;
  ok = s->config.provider->load_state(s->config.provider_state, st->blob, st->blob_len) == 0;
  if (!ok) {
    if (!have_prior || s->config.provider->load_state(s->config.provider_state, prior, prior_len) != 0) {
      s->phase = SEGA_MOD_PHASE_RESET; /* the diagnostic names the restore moment: the guest frame the machine is at */
      s->frame = s->runtime->virtual_frame_count;
      record_fault(s, "state");
    }
  }
  free(prior);
  return ok ? 0 : 1;
}

static void state_commit(void *ctx, void *staged, GenesisRuntime *runtime) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  StagedModState *st = (StagedModState *)staged;
  uint32_t i;
  s->started = st->started;
  s->disabled = st->disabled;
  s->callbacks_run = st->callbacks_run;
  memcpy(s->sampled, st->sampled, 2U);
  memcpy(s->final_pad, st->final_pad, 2U);
  memcpy(s->previous_final, st->previous_final, 2U);
  runtime->mod_pad_active = st->pad_active;
  runtime->mod_pad1 = st->mod_pad[0];
  runtime->mod_pad2 = st->mod_pad[1];
  memcpy(s->hook_calls, st->hook_calls, sizeof s->hook_calls);
  memcpy(s->hook_skips, st->hook_skips, sizeof s->hook_skips);
  s->hook_skip_logged = st->hook_skip_logged;
  memset(&s->fault, 0, sizeof s->fault);
  s->fault_cause_store[0] = '\0';
  if (st->faulted != 0U) {
    s->fault.faulted = 1U;
    memcpy(s->fault_cause_store, st->fault_cause, sizeof s->fault_cause_store);
    s->fault.cause = s->fault_cause_store;
    s->fault.phase = st->fault_phase;
    s->fault.frame = st->fault_frame;
    memcpy(s->fault.text, st->fault_text, sizeof s->fault.text);
  }
  s->write_stream = st->stream;
  s->write_total = st->write_total;
  s->write_log_count = 0U; /* the retained diagnostic log restarts empty; every earlier write counts as not retained (the digest above is exact) */
  s->write_log_dropped = st->write_total;
  s->render_copy_valid = 0U;
  s->hook_site = NULL;
  if (runtime->function_hooks == &s->function_hooks) {
    s->function_hooks.depth = st->depth;
    s->function_hooks.abandoned = st->abandoned;
    s->function_hooks.unproven_callers = st->unproven;
    s->function_hooks.overflowed = st->overflowed;
    for (i = 0; i < st->depth; ++i) {
      s->function_hooks.frames[i].site = &s->config.sites[st->frames[i].site];
      s->function_hooks.frames[i].return_pc = st->frames[i].return_pc;
      s->function_hooks.frames[i].entry_sp = st->frames[i].entry_sp;
    }
  }
  free(st);
}

static void state_discard(void *ctx, void *staged) {
  (void)ctx;
  free(staged);
}

static void compat_digest(const GenesisModScheduler *s, uint8_t out[32]) {
  GenesisSha256 sha;
  uint32_t i;
  static const char tag[] = "segarecomp.genesis.mod.compat.v1";
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, (const uint8_t *)tag, (uint32_t)(sizeof tag - 1U));
  sha_u32(&sha, s->host.abi_major);
  sha_u32(&sha, (uint32_t)(s->host.granted[0] >> 32U)); sha_u32(&sha, (uint32_t)s->host.granted[0]);
  sha_u32(&sha, (uint32_t)(s->host.granted[1] >> 32U)); sha_u32(&sha, (uint32_t)s->host.granted[1]);
  sha_u32(&sha, s->config.phase_mask);
  sha_u32(&sha, s->config.provider != NULL ? s->config.provider->implementation_version : 0U);
  sha_u32(&sha, s->config.range_count);
  for (i = 0; i < s->config.range_count; ++i) { sha_u32(&sha, s->ranges[i].space); sha_u32(&sha, s->ranges[i].offset); sha_u32(&sha, s->ranges[i].length); sha_u32(&sha, s->ranges[i].modes); }
  sha_u32(&sha, s->config.hook_count);
  for (i = 0; i < s->config.hook_count; ++i) {
    const uint32_t n = (uint32_t)strlen(s->bindings[i].id);
    sha_u32(&sha, n);
    genesis_sha256_update(&sha, (const uint8_t *)s->bindings[i].id, n);
    sha_u32(&sha, s->bindings[i].hook_id);
  }
  genesis_sha256_final(&sha, out);
}

void genesis_mod_snapshot_state(GenesisModScheduler *s, GenesisSnapshotModState *out) {
  memset(out, 0, sizeof *out);
  out->ctx = s;
  memcpy(out->identity, s->identity, 32U);
  out->state_schema = s->config.state_schema;
  compat_digest(s, out->compat);
  out->put = state_put;
  out->stage = state_stage;
  out->apply = state_apply;
  out->commit = state_commit;
  out->discard = state_discard;
}
