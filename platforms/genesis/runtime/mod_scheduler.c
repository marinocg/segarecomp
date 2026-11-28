/* SEG-138-T001/T002 (ADR 0297): the Genesis mod phase scheduler. See mod_scheduler.h. Strict C11; no allocation; no wall clock, host RNG or host pointers are ever offered to a mod. */
#include "mod_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ABI 2 service functions carry no context pointer, so the scheduler whose callback is running is reachable through this single pointer, set only for the duration of a callback. */
static GenesisModScheduler *g_current = NULL;

/* ADR 0320 decision 4: the view holds every raster/field line the capture holds (interlace mode 2 has 448 output rows but 224 captured lines; nothing is dropped) and its line has the layout of the capture's. */
_Static_assert(SEGA_GEN_FRAME_MAX_LINES == GENESIS_RASTER_MAX_LINES, "the exact-frame view holds every captured raster line");

#define PHASE_BIT(p) (UINT32_C(1) << (p))
#define HOOK_PHASES (PHASE_BIT(SEGA_MOD_PHASE_HOOK_ENTRY) | PHASE_BIT(SEGA_MOD_PHASE_HOOK_EXIT))
#define VALID_PHASES (PHASE_BIT(SEGA_MOD_PHASE_RESET) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | \
                      PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | PHASE_BIT(SEGA_MOD_PHASE_RENDER) | HOOK_PHASES)
#define READ_PHASES (PHASE_BIT(SEGA_MOD_PHASE_RESET) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | HOOK_PHASES)
#define WRITE_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | HOOK_PHASES)
#define AUDIO_COMMAND_PHASES WRITE_PHASES /* audio.command.send: the phases that may write guest memory (modding-audio-architecture.md section 5) */
#define AUDIO_EVENT_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_END) | PHASE_BIT(SEGA_MOD_PHASE_RENDER))
#define INPUT_READ_PHASES (PHASE_BIT(SEGA_MOD_PHASE_FRAME_BEGIN) | PHASE_BIT(SEGA_MOD_PHASE_INPUT) | PHASE_BIT(SEGA_MOD_PHASE_POST_INPUT))

static uint64_t cap_bit(unsigned id) { return UINT64_C(1) << (id % 64U); }

void genesis_mod_scheduler_supported(uint64_t out[2]) {
  out[0] = cap_bit(SEGA_MOD_CAP_HOST_LOG) | cap_bit(SEGA_MOD_CAP_FRAME_WORK_RAM) | cap_bit(SEGA_MOD_CAP_MEMORY_READ) | cap_bit(SEGA_MOD_CAP_MEMORY_WRITE) | cap_bit(SEGA_MOD_CAP_INPUT_READ) |
           cap_bit(SEGA_MOD_CAP_INPUT_INTERCEPT) | cap_bit(SEGA_MOD_CAP_HOOK_FUNCTION) | cap_bit(SEGA_MOD_CAP_STATE_MOD) |
           cap_bit(SEGA_MOD_CAP_AUDIO_COMMAND_SEND) | cap_bit(SEGA_MOD_CAP_AUDIO_EVENT_READ) |
           cap_bit(SEGA_MOD_CAP_SCENERY_WORDS) | cap_bit(SEGA_MOD_CAP_OVERLAY_RGBA) | cap_bit(SEGA_MOD_CAP_FRAME_VDP); /* frame.vdp (ADR 0320): REQUIRED-only, it selects the exact-frame callback; scenery.words (SEG-140-T011) is output of the same kind: */ /* presentation output of `render`: collected by the provider adapter and presented by the host, not a scheduler service */
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
    case SEGA_MOD_E_AUDIO_TARGET: return "audio-target";
    case SEGA_MOD_E_AUDIO_NAME: return "audio-name";
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

/* SEG-152-T018 (ADR 0321): a read of the materialised ROM ranges. Check order: capability, phase, width, alignment, range (one declared range), budget. */
static int service_rom_read(GenesisModScheduler *s, uint32_t offset, uint32_t width, uint32_t *value) {
  uint32_t i, k;
  if (!granted(s, SEGA_MOD_CAP_MEMORY_READ)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if (s->phase != SEGA_MOD_PHASE_RENDER) return violation(s, SEGA_MOD_E_PHASE);
  if (value == NULL || !width_ok(width)) return violation(s, SEGA_MOD_E_RANGE);
  if (width > 1U && (offset & 1U) != 0U) return violation(s, SEGA_MOD_E_ALIGNMENT);
  for (i = 0; i < s->config.range_count; ++i) {
    const GenesisModRange *r = &s->ranges[i];
    if (r->space == SEGA_GEN_SPACE_M68K_ROM && offset >= r->offset && (uint64_t)offset + width <= (uint64_t)r->offset + r->length) break;
  }
  if (i == s->config.range_count) return violation(s, SEGA_MOD_E_RANGE);
  if (s->rom_reads >= GENESIS_MOD_ROM_READS_MAX || s->rom_read_bytes + width > GENESIS_MOD_ROM_READ_BYTES_MAX) return violation(s, SEGA_MOD_E_BUDGET);
  ++s->rom_reads;
  s->rom_read_bytes += width;
  *value = 0U;
  for (k = 0; k < width; ++k) *value = (*value << 8U) | s->config.rom_data[s->rom_base[i] + (offset - s->ranges[i].offset) + k];
  return SEGA_MOD_OK;
}

static int service_read(uint32_t space, uint32_t offset, uint32_t width, uint32_t *value) {
  GenesisModScheduler *s = g_current;
  const int render = s != NULL && s->phase == SEGA_MOD_PHASE_RENDER;
  uint32_t i, result = 0;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (space == SEGA_GEN_SPACE_M68K_HOOK_REGS) return service_reg_read(s, offset, width, value);
  if (space == SEGA_GEN_SPACE_M68K_ROM) return service_rom_read(s, offset, width, value);
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

static int stage_write(GenesisModScheduler *s, uint32_t offset, uint32_t width, uint32_t value) {
  GenesisModStagedWrite *w;
  if (s->staged_count >= GENESIS_MOD_WRITE_OPS_MAX || s->staged_bytes + width > GENESIS_MOD_WRITE_BYTES_MAX) return violation(s, SEGA_MOD_E_BUDGET);
  w = &s->staged[s->staged_count++];
  w->offset = offset;
  w->width = width;
  w->value = value;
  s->staged_bytes += width;
  return SEGA_MOD_OK;
}

static int service_write(uint32_t space, uint32_t offset, uint32_t width, uint32_t value) {
  GenesisModScheduler *s = g_current;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (space == SEGA_GEN_SPACE_M68K_HOOK_REGS) return service_reg_write(s, offset, width, value);
  if (!granted(s, SEGA_MOD_CAP_MEMORY_WRITE)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if ((WRITE_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (known_read_only_space(space)) return violation(s, SEGA_MOD_E_READONLY);
  if (space != SEGA_GEN_SPACE_M68K_WORK_RAM) return violation(s, SEGA_MOD_E_SPACE);
  if (!width_ok(width) || (width < 4U && value >> (8U * width)) != 0U) return violation(s, SEGA_MOD_E_RANGE);
  if (width > 1U && (offset & 1U) != 0U) return violation(s, SEGA_MOD_E_ALIGNMENT);
  if ((uint64_t)offset + width > GENESIS_MOD_WORK_RAM_BYTES || !in_declared_range(s, space, offset, width, GENESIS_MOD_ACCESS_WRITE)) return violation(s, SEGA_MOD_E_RANGE);
  return stage_write(s, offset, width, value);
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

/* ---- the declared audio interface (SEG-145-T008). Names resolve only through the rows the package declared: no id, chip or driver protocol is known here. ---- */
static int audio_name_is(const char *declared, const char *asked) { return strncmp(declared, asked, SEGA_MOD_AUDIO_NAME_BYTES) == 0; } /* declared names are NUL-terminated within the bound, so a longer `asked` never matches */

static int audio_command(const char *mapping, const char *name) {
  GenesisModScheduler *s = g_current;
  uint32_t i;
  int declared = 0;
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_AUDIO_COMMAND_SEND)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if ((AUDIO_COMMAND_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (name == NULL) return violation(s, SEGA_MOD_E_RANGE);
  for (i = 0; i < s->audio_count; ++i) {
    const GenesisModAudioRow *row = &s->audio[i];
    if (row->kind != GENESIS_MOD_AUDIO_COMMAND || !audio_name_is(row->mapping, mapping)) continue;
    declared = 1;
    if (audio_name_is(row->name, name)) return stage_write(s, row->offset, row->width, row->value);
  }
  return violation(s, declared ? SEGA_MOD_E_AUDIO_NAME : SEGA_MOD_E_AUDIO_TARGET);
}

static int service_audio_sfx(const char *name) { return audio_command("sfx", name); }
static int service_audio_music(const char *name) { return audio_command("music", name); }
static int service_audio_stop(void) { return audio_command("music", "stop"); }

static int service_audio_event(const char *mapping, char name_out[SEGA_MOD_AUDIO_NAME_BYTES]) {
  GenesisModScheduler *s = g_current;
  uint32_t i, k;
  int declared = 0, render;
  if (name_out != NULL) memset(name_out, 0, SEGA_MOD_AUDIO_NAME_BYTES);
  if (s == NULL || !s->in_callback) return SEGA_MOD_E_PHASE;
  if (!granted(s, SEGA_MOD_CAP_AUDIO_EVENT_READ)) return violation(s, SEGA_MOD_E_CAPABILITY);
  if ((AUDIO_EVENT_PHASES & PHASE_BIT(s->phase)) == 0U) return violation(s, SEGA_MOD_E_PHASE);
  if (mapping == NULL || name_out == NULL) return violation(s, SEGA_MOD_E_RANGE);
  render = s->phase == SEGA_MOD_PHASE_RENDER;
  /* An exact-frame render observes the producer's previous-onset copy, which the host hands over only under frame.work_ram: without that grant there is nothing to observe, and answering "no event" would lie. */
  if (render && s->exact_frame && !granted(s, SEGA_MOD_CAP_FRAME_WORK_RAM)) return violation(s, SEGA_MOD_E_CAPABILITY);
  for (i = 0; i < s->audio_count; ++i) {
    const GenesisModAudioRow *row = &s->audio[i];
    uint32_t value = 0U;
    if (row->kind != GENESIS_MOD_AUDIO_EVENT || !audio_name_is(row->mapping, mapping)) continue;
    declared = 1;
    if (render) {
      if (!s->audio_observed_valid) continue; /* an exact frame without a previous-onset copy: nothing observed, no event */
      value = s->audio_observed[i];
    } else {
      for (k = 0; k < row->width; ++k) value = (value << 8U) | byte_at(s, s->runtime->work_ram, row->offset + k); /* sees this callback's staged writes */
    }
    if (value == row->value) {
      memcpy(name_out, row->name, SEGA_MOD_AUDIO_NAME_BYTES);
      return SEGA_MOD_OK;
    }
  }
  return declared ? SEGA_MOD_OK : violation(s, SEGA_MOD_E_AUDIO_TARGET);
}

/* `render` observes a copy: the declared event locations only, taken from `work_ram` (the after-frame-end image). */
static void observe_audio(GenesisModScheduler *s, const uint8_t *work_ram) {
  uint32_t i, k;
  s->audio_observed_valid = 0U;
  if (work_ram == NULL || !granted(s, SEGA_MOD_CAP_AUDIO_EVENT_READ)) return;
  for (i = 0; i < s->audio_count; ++i) {
    uint32_t value = 0U;
    if (s->audio[i].kind != GENESIS_MOD_AUDIO_EVENT) continue;
    for (k = 0; k < s->audio[i].width; ++k) value = (value << 8U) | work_ram[s->audio[i].offset + k];
    s->audio_observed[i] = value;
  }
  s->audio_observed_valid = 1U;
}

static int bounded_name(const char *name) {
  uint32_t i;
  if (name == NULL) return 0;
  for (i = 0; i < SEGA_MOD_AUDIO_NAME_BYTES; ++i) {
    if (name[i] == '\0') return i != 0U;
    if (name[i] < 0x21 || name[i] > 0x7E) return 0;
  }
  return 0;
}

/* Validates and copies the declared interface (defence in depth: the package layer checked the same). A required audio capability without a usable mapping is refused (E_AUDIO_TARGET); an optional one is
 * simply not granted: the capability is ABSENT, never a guess. */
static const char *attach_audio(GenesisModScheduler *s, const GenesisModConfig *config) {
  uint32_t i, j, kinds = 0U;
  if (config->audio_count > GENESIS_MOD_AUDIO_ENTRIES_MAX || (config->audio_count != 0U && config->audio == NULL)) return "E_AUDIO_MAPPING";
  for (i = 0; i < config->audio_count; ++i) {
    const GenesisModAudioEntry *e = &config->audio[i];
    GenesisModAudioRow *row = &s->audio[s->audio_count];
    if (!bounded_name(e->mapping) || !bounded_name(e->name) || (e->kind != GENESIS_MOD_AUDIO_COMMAND && e->kind != GENESIS_MOD_AUDIO_EVENT) || !width_ok(e->width) ||
        (e->width > 1U && (e->offset % e->width) != 0U) || (uint64_t)e->offset + e->width > GENESIS_MOD_WORK_RAM_BYTES || (e->width < 4U && e->value >> (8U * e->width)) != 0U)
      return "E_AUDIO_MAPPING";
    if (e->kind == GENESIS_MOD_AUDIO_COMMAND && strcmp(e->mapping, "sfx") != 0 && strcmp(e->mapping, "music") != 0) return "E_AUDIO_MAPPING";
    for (j = 0; j < s->audio_count; ++j) {
      const GenesisModAudioRow *o = &s->audio[j];
      if (strcmp(o->mapping, e->mapping) != 0) continue;
      if (o->kind != e->kind || o->offset != e->offset || o->width != e->width || strcmp(o->name, e->name) == 0) return "E_AUDIO_MAPPING"; /* one mapping, one location, unique names */
    }
    if (!granted(s, e->kind == GENESIS_MOD_AUDIO_COMMAND ? SEGA_MOD_CAP_AUDIO_COMMAND_SEND : SEGA_MOD_CAP_AUDIO_EVENT_READ)) return "E_AUDIO_MAPPING";
    memcpy(row->mapping, e->mapping, strlen(e->mapping) + 1U);
    memcpy(row->name, e->name, strlen(e->name) + 1U);
    row->kind = e->kind;
    row->offset = e->offset;
    row->width = e->width;
    row->value = e->value;
    kinds |= e->kind;
    ++s->audio_count;
  }
  {
    const struct { uint32_t cap, kind; } needs[2] = {{SEGA_MOD_CAP_AUDIO_COMMAND_SEND, GENESIS_MOD_AUDIO_COMMAND}, {SEGA_MOD_CAP_AUDIO_EVENT_READ, GENESIS_MOD_AUDIO_EVENT}};
    for (i = 0; i < 2U; ++i) {
      if ((kinds & needs[i].kind) != 0U || !granted(s, needs[i].cap)) continue;
      if ((config->required[0] & cap_bit(needs[i].cap)) != 0U) return "E_AUDIO_TARGET";
      s->host.granted[0] &= ~cap_bit(needs[i].cap);
    }
  }
  return NULL;
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
  s->rom_reads = 0U;
  s->rom_read_bytes = 0U;
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
  if (s->runtime != NULL && !s->in_set) s->runtime->mod_pad_active = 0U; /* the guest goes on with the physical pad (in a set another member may still own the latch: the set decides) */
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

static void reset_session(GenesisModScheduler *s, GenesisRuntime *rt) {
  s->started = 1U;
  if (s->config.provider->init != NULL) {
    int failed;
    begin_callback(s, SEGA_MOD_PHASE_RESET, rt->virtual_frame_count);
    failed = s->config.provider->init(s->config.provider_state, &s->host) != 0;
    finish_callback(s, failed);
  }
  run_phase(s, SEGA_MOD_PHASE_RESET, rt->virtual_frame_count);
}

static void hook_start(void *ctx, GenesisRuntime *rt) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  if (s->started) return;
  reset_session(s, rt);
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

static void render_frame(GenesisModScheduler *s, GenesisRuntime *rt) {
  const uint64_t ended = rt->virtual_frame_count - 1U;
  if (!s->disabled && !s->exact_frame && (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_RENDER)) != 0U) {   /* an exact-frame provider's render runs at the producer seam instead (genesis_mod_scheduler_present) */
    s->render_copy_valid = 0U;
    if (granted(s, SEGA_MOD_CAP_FRAME_WORK_RAM)) { /* render reads copies taken after frame-end, never live memory */
      memcpy(s->render_copy, rt->work_ram, GENESIS_MOD_WORK_RAM_BYTES);
      s->render_copy_valid = 1U;
    }
    observe_audio(s, rt->work_ram);
    run_phase(s, SEGA_MOD_PHASE_RENDER, ended);
    s->render_copy_valid = 0U;
    s->audio_observed_valid = 0U;
  }
}

static void hook_after_snapshot(void *ctx, GenesisRuntime *rt) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  render_frame(s, rt);
  open_frame(s, rt);
}

int genesis_mod_scheduler_present(GenesisModScheduler *s, const GenesisModExactFrame *f) {
  uint32_t lines;
  if (s == NULL || f == NULL || !s->exact_frame || s->disabled || s->runtime == NULL || s->in_callback || (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_RENDER)) == 0U) return 0;
  if (f->vram == NULL || (f->line_count != 0U && f->lines == NULL)) return 0;
  lines = f->line_count < SEGA_GEN_FRAME_MAX_LINES ? f->line_count : SEGA_GEN_FRAME_MAX_LINES;
  s->render_copy_valid = 0U;
  if (granted(s, SEGA_MOD_CAP_FRAME_WORK_RAM) && f->work_ram != NULL) {   /* the previous-onset copy, re-copied so the provider can never alias the producer's buffer */
    memcpy(s->render_copy, f->work_ram, GENESIS_MOD_WORK_RAM_BYTES);
    s->render_copy_valid = 1U;
  }
  memcpy(s->view_vram, f->vram, GENESIS_MOD_WORK_RAM_BYTES);
  if (lines != 0U) memcpy(s->view_lines, f->lines, (size_t)lines * sizeof(SegaGenesisFrameLine));
  memset(&s->view, 0, sizeof s->view);
  s->view.size = (uint32_t)sizeof s->view;
  s->view.width = f->width;
  s->view.height = f->height;
  s->view.side = f->side;
  s->view.requested = f->requested;
  s->view.work_ram = s->render_copy_valid ? s->render_copy : NULL;
  s->view.flags = f->vram_frame_start ? SEGA_GEN_FRAME_VRAM_FRAME_START : 0U;
  s->view.vram = s->view_vram;
  s->view.lines = s->view_lines;
  s->view.line_count = lines;
  s->features.frame = &s->view;
  ++s->exact_calls;
  observe_audio(s, f->work_ram);
  run_phase(s, SEGA_MOD_PHASE_RENDER, s->runtime->virtual_frame_count - 1U);
  s->audio_observed_valid = 0U;
  s->features.frame = NULL;
  s->render_copy_valid = 0U;
  return s->disabled ? 0 : 1;
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
  s->host.granted[0] &= ~(cap_bit(SEGA_MOD_CAP_FRAME_VDP) & ~config->required[0]); /* ADR 0320 decision 2: frame.vdp selects the callback timing, so it is granted ONLY when required; an optional declaration is never granted (the package layer refuses it with E_CAPABILITY_INCONSISTENT) and never changes timing */
  s->exact_frame = (config->required[0] & cap_bit(SEGA_MOD_CAP_FRAME_VDP)) != 0U ? 1U : 0U;
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
  {
    const char *audio_rejection = attach_audio(s, config);
    if (audio_rejection != NULL) {
      *rejection = audio_rejection;
      return 1;
    }
  }
  s->config.audio = NULL; /* the scheduler owns copies in s->audio: the caller's array and strings need not outlive attach */
  if (sega_mod_is_granted(&s->host, SEGA_MOD_CAP_FRAME_VDP) && config->vdp_helpers == NULL) { /* ADR 0320 decisions 2 and 5: a granted frame.vdp promises the view AND the helper table; a host that has no producer seam (the headless capture) supplies neither, so the requirement is refused, exactly as before frame.vdp became grantable */
    *rejection = "E_CAPABILITY_UNSUPPORTED";
    return 1;
  }
  if (config->phase_mask == 0U || (config->phase_mask & ~VALID_PHASES) != 0U) { /* none, unknown or bit 0 */
    *rejection = "E_PHASE_UNSUPPORTED";
    return 1;
  }
  if (s->exact_frame && (config->phase_mask & PHASE_BIT(SEGA_MOD_PHASE_RENDER)) == 0U) { /* an exact-frame provider is its render callback */
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
  if (config->site_count > GENESIS_MOD_HOOKS_MAX || (config->site_count != 0U && config->sites == NULL)) { /* the existing static hook limit bounds the generated site table too */
    *rejection = "E_HOOK_BINDING";
    return 1;
  }
  if (config->range_count > GENESIS_MOD_RANGES_MAX) {
    *rejection = "E_RANGE_DECLARATION";
    return 1;
  }
  {
    uint32_t rom_total = 0U;
    for (i = 0; i < config->range_count; ++i) {
      const GenesisModRange *r = &config->ranges[i];
      if (r->space != SEGA_GEN_SPACE_M68K_ROM) continue;
      if (r->length == 0U || r->length > GENESIS_MOD_ROM_BYTES_MAX || r->modes != GENESIS_MOD_ACCESS_READ || !sega_mod_is_granted(&s->host, SEGA_MOD_CAP_MEMORY_READ)) {
        *rejection = "E_RANGE_DECLARATION";
        return 1;
      }
      s->rom_base[i] = rom_total;
      rom_total += r->length;
      if (rom_total > GENESIS_MOD_ROM_BYTES_MAX) {
        *rejection = "E_RANGE_DECLARATION";
        return 1;
      }
    }
    if (rom_total != config->rom_data_bytes || (rom_total != 0U && config->rom_data == NULL)) {
      *rejection = "E_RANGE_DECLARATION";
      return 1;
    }
  }
  for (i = 0; i < config->range_count; ++i) {
    const GenesisModRange *r = &config->ranges[i];
    const uint32_t modes = r->modes;
    if (r->space == SEGA_GEN_SPACE_M68K_ROM) {
      s->ranges[i] = *r;
      continue;
    }
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
  s->features.render = sega_mod_is_granted(&s->host, SEGA_MOD_CAP_OVERLAY_RGBA) || sega_mod_is_granted(&s->host, SEGA_MOD_CAP_SCENERY_WORDS) ? config->render_api : NULL;   /* SEG-140-T011: scenery.words is render output too */
  s->features.frame = NULL;   /* non-NULL only inside the exact-frame render callback */
  s->features.vdp = sega_mod_is_granted(&s->host, SEGA_MOD_CAP_FRAME_VDP) ? config->vdp_helpers : NULL;   /* ADR 0320 decision 5: the host's own VDP functions, supplied by the adapter (the scheduler never links the VDP renderer) */
  s->audio_api.size = (uint32_t)sizeof(SegaModAudioApi);
  s->audio_api.play_sfx = service_audio_sfx;
  s->audio_api.play_music = service_audio_music;
  s->audio_api.stop_music = service_audio_stop;
  s->audio_api.read_event = service_audio_event;
  s->features.audio = granted(s, SEGA_MOD_CAP_AUDIO_COMMAND_SEND) || granted(s, SEGA_MOD_CAP_AUDIO_EVENT_READ) ? &s->audio_api : NULL;
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

static int live_input(const GenesisModScheduler *s) { return !s->disabled && s->started && (s->config.phase_mask & PHASE_BIT(SEGA_MOD_PHASE_INPUT)) != 0U; }

static int guest_affecting(const GenesisModScheduler *s) {
  return granted(s, SEGA_MOD_CAP_MEMORY_WRITE) || granted(s, SEGA_MOD_CAP_INPUT_INTERCEPT) || granted(s, SEGA_MOD_CAP_HOOK_FUNCTION) || granted(s, SEGA_MOD_CAP_AUDIO_COMMAND_SEND);
}

static uint32_t state_put(void *ctx, uint8_t *out, uint32_t cap, GenesisSnapshotStatus *status) {
  GenesisModScheduler *s = (GenesisModScheduler *)ctx;
  StateOut o;
  uint32_t i, depth = 0U, blob_len = 0U, blob_at;
  uint8_t latched;
  const uint32_t fail = UINT32_MAX;
  o.p = out; o.cap = cap; o.n = 0U; o.error = 0;
  *status = GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  if (s->runtime == NULL || s->in_callback) return fail;
  if (guest_affecting(s) && !granted(s, SEGA_MOD_CAP_STATE_MOD)) { *status = GENESIS_SNAPSHOT_MOD_UNAVAILABLE; return fail; } /* E_STATE_UNAVAILABLE: never a state that silently drops the mod's effect */
  if (s->runtime->function_hooks == &s->function_hooks) depth = s->function_hooks.depth;
  if (depth > GENESIS_HOOK_FRAMES_MAX || (depth != 0U && s->config.site_count == 0U)) return fail;
  latched = (uint8_t)(s->runtime->mod_pad_active && (!s->in_set || live_input(s))); /* in a set the latch is shared: only a member that takes part in it saves it */
  so_u8(&o, s->disabled); so_u8(&o, s->started); so_u8(&o, s->fault.faulted); so_u8(&o, latched);
  so_u8(&o, s->sampled[0]); so_u8(&o, s->sampled[1]); so_u8(&o, s->final_pad[0]); so_u8(&o, s->final_pad[1]);
  so_u8(&o, s->previous_final[0]); so_u8(&o, s->previous_final[1]);
  so_u8(&o, latched ? s->runtime->mod_pad1 : 0U); so_u8(&o, latched ? s->runtime->mod_pad2 : 0U); /* a stale latch behind an inactive interception is not state */
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
    uint32_t index = 0U;
    while (index < s->config.site_count && f->site != &s->config.sites[index]) ++index; /* pointer equality only: no relational comparison of unrelated objects */
    if (index == s->config.site_count) return fail;
    so_u32(&o, index);
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
  if (in.error || st->stream.used >= 64U || (st->stream.length & 63U) != st->stream.used || st->write_total > UINT64_MAX / 28U || st->stream.length != st->write_total * 28U) goto bad; /* 7 big-endian u32 per committed write */
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
  uint32_t i, site_count;
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
  if (s->audio_count != 0U) { /* the declared audio interface decides which guest bytes a command writes (only when one exists, so other digests are unchanged) */
    sha_u32(&sha, s->audio_count);
    for (i = 0; i < s->audio_count; ++i) {
      genesis_sha256_update(&sha, (const uint8_t *)s->audio[i].mapping, SEGA_MOD_AUDIO_NAME_BYTES);
      genesis_sha256_update(&sha, (const uint8_t *)s->audio[i].name, SEGA_MOD_AUDIO_NAME_BYTES);
      sha_u32(&sha, s->audio[i].kind); sha_u32(&sha, s->audio[i].offset); sha_u32(&sha, s->audio[i].width); sha_u32(&sha, s->audio[i].value);
    }
  }
  if (s->config.rom_data_bytes != 0U) { /* the materialised bytes are part of the configuration (only when ROM ranges exist, so other digests are unchanged) */
    sha_u32(&sha, s->config.rom_data_bytes);
    genesis_sha256_update(&sha, s->config.rom_data, s->config.rom_data_bytes);
  }
  sha_u32(&sha, s->config.hook_count);
  for (i = 0; i < s->config.hook_count; ++i) {
    const uint32_t n = (uint32_t)strlen(s->bindings[i].id);
    sha_u32(&sha, n);
    genesis_sha256_update(&sha, (const uint8_t *)s->bindings[i].id, n);
    sha_u32(&sha, s->bindings[i].hook_id);
  }
  /* The generated hook site table decides how saved hook-frame indices are read: its size and, in table order, each site's entry, id and mode. */
  site_count = s->config.sites != NULL && s->config.site_count <= GENESIS_MOD_HOOKS_MAX ? s->config.site_count : 0U; /* an unattached/rejected config contributes nothing */
  sha_u32(&sha, site_count);
  for (i = 0; i < site_count; ++i) {
    const GenesisHookSiteInfo *site = &s->config.sites[i];
    const uint32_t n = site->id != NULL ? (uint32_t)strlen(site->id) : 0U;
    sha_u32(&sha, site->entry);
    sha_u32(&sha, site->mode);
    sha_u32(&sha, n);
    if (n != 0U) genesis_sha256_update(&sha, (const uint8_t *)site->id, n);
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

/* ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * SEG-141-T003 (ADR 0327): the set. It calls the single-mod phase runner above; nothing here grants, checks or commits anything itself.
 * ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ */
static void set_run_all(GenesisModSet *set, uint32_t phase, uint64_t frame) {
  uint32_t i;
  for (i = 0; i < set->count; ++i) run_phase(set->members[i], phase, frame);
}

/* A latch no live `input` member stands behind is released: the guest reads the physical pad. */
static void set_release_pad(GenesisModSet *set) {
  uint32_t i;
  for (i = 0; i < set->count; ++i)
    if (live_input(set->members[i])) return;
  set->runtime->mod_pad_active = 0U;
}

static void set_open_frame(GenesisModSet *set, GenesisRuntime *rt) {
  const uint64_t frame = rt->virtual_frame_count;
  uint8_t chain[2];
  uint32_t i;
  int latch = 0;
  for (i = 0; i < set->count; ++i) {
    set->members[i]->previous_final[0] = set->members[i]->final_pad[0];
    set->members[i]->previous_final[1] = set->members[i]->final_pad[1];
  }
  set_run_all(set, SEGA_MOD_PHASE_FRAME_BEGIN, frame);
  chain[0] = rt->pad1;
  chain[1] = rt->pad2;
  for (i = 0; i < set->count; ++i) {
    GenesisModScheduler *s = set->members[i];
    s->sampled[0] = rt->pad1;
    s->sampled[1] = rt->pad2;
    s->final_pad[0] = chain[0];
    s->final_pad[1] = chain[1];
    run_phase(s, SEGA_MOD_PHASE_INPUT, frame); /* a faulted callback leaves final_pad as it was: the chain is unchanged */
    chain[0] = s->final_pad[0];
    chain[1] = s->final_pad[1];
  }
  for (i = 0; i < set->count; ++i) {
    set->members[i]->final_pad[0] = chain[0]; /* every member sees the composed pad from here on */
    set->members[i]->final_pad[1] = chain[1];
    latch = latch || live_input(set->members[i]);
  }
  rt->mod_pad_active = latch ? 1U : 0U;
  if (latch) {
    rt->mod_pad1 = chain[0];
    rt->mod_pad2 = chain[1];
  }
  set_run_all(set, SEGA_MOD_PHASE_POST_INPUT, frame);
  set_release_pad(set);
}

static void set_start(void *ctx, GenesisRuntime *rt) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  uint32_t i;
  if (set->members[0]->started) return;
  for (i = 0; i < set->count; ++i) reset_session(set->members[i], rt);
  set_open_frame(set, rt);
}

static void set_frame_end(void *ctx, GenesisRuntime *rt) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  set_run_all(set, SEGA_MOD_PHASE_FRAME_END, rt->virtual_frame_count);
  set_release_pad(set);
}

static void set_after_snapshot(void *ctx, GenesisRuntime *rt) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  uint32_t i;
  for (i = 0; i < set->count; ++i) render_frame(set->members[i], rt);
  set_release_pad(set);
  set_open_frame(set, rt);
}

static int set_hook_enter(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t entry_sp) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  uint32_t i;
  int replaced = 0;
  for (i = 0; i < set->count; ++i)
    if (hook_enter(set->members[i], rt, site, entry_sp) != 0) replaced = 1; /* only a replace site can answer 1, and it has one owner */
  set_release_pad(set);
  return replaced;
}

static void set_hook_leave(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t entry_sp) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  uint32_t i;
  for (i = 0; i < set->count; ++i) hook_leave(set->members[i], rt, site, entry_sp);
  set_release_pad(set);
}

static void set_hook_skipped(void *ctx, GenesisRuntime *rt, const GenesisHookSiteInfo *site, uint32_t kind) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  uint32_t i;
  for (i = 0; i < set->count; ++i)
    if (find_binding(set->members[i], site) != NULL) hook_skipped(set->members[i], rt, site, kind);
}

void genesis_mod_set_detach(GenesisModSet *set) {
  uint32_t i;
  if (set == NULL || set->runtime == NULL) return;
  if (set->runtime->frame_hooks == &set->hooks) set->runtime->frame_hooks = NULL;
  if (set->runtime->function_hooks == &set->function_hooks) set->runtime->function_hooks = NULL;
  set->runtime->mod_pad_active = 0U;
  for (i = 0; i < set->count; ++i) set->members[i]->runtime = NULL;
  set->runtime = NULL;
  set->count = 0U;
}

int genesis_mod_set_attach(GenesisModSet *set, GenesisRuntime *rt, GenesisModScheduler *const *schedulers, const GenesisModConfig *configs, uint32_t count, const uint8_t identity[32],
                           const char **rejection, uint32_t *member) {
  const char *dummy = NULL;
  uint32_t dummy_member = 0U, i, j, k, any = 0U;
  int hooked = 0;
  if (rejection == NULL) rejection = &dummy;
  if (member == NULL) member = &dummy_member;
  *rejection = "";
  *member = count;
  if (set == NULL || rt == NULL || schedulers == NULL || configs == NULL || identity == NULL || count == 0U || count > GENESIS_MOD_SET_MEMBERS_MAX) {
    *rejection = "E_SET_SIZE";
    return 1;
  }
  for (i = 0; i < 32U; ++i) any |= identity[i];
  if (any == 0U) {
    *rejection = "E_SET_IDENTITY";
    return 1;
  }
  if (rt->frame_hooks != NULL || rt->function_hooks != NULL) {
    *rejection = "E_SCHEDULER_BUSY";
    return 1;
  }
  memset(set, 0, sizeof(*set));
  memcpy(set->identity, identity, 32U);
  set->runtime = rt;
  for (i = 0; i < count; ++i) {
    GenesisModScheduler *s = schedulers[i];
    *member = i;
    for (j = 0; j < i; ++j)
      if (schedulers[j] == s) {
        *rejection = "E_PROVIDER_ENTRY";
        goto refused;
      }
    if (genesis_mod_scheduler_attach(s, rt, &configs[i], rejection) != 0) goto refused;
    rt->frame_hooks = NULL; /* the set installs its own once every member is accepted */
    rt->function_hooks = NULL;
    s->in_set = 1U;
    set->members[set->count++] = s;
    for (j = 0; j < i; ++j)
      if (strcmp(set->members[j]->config.mod_id, s->config.mod_id) == 0) {
        *rejection = "E_SET_DUPLICATE_MOD";
        goto refused;
      }
    if ((s->config.phase_mask & HOOK_PHASES) != 0U) {
      if (s->config.site_count == 0U || s->config.sites == NULL || (hooked && (set->sites != s->config.sites || set->site_count != s->config.site_count))) {
        *rejection = "E_SET_HOOK_SITES";
        goto refused;
      }
      hooked = 1;
      set->sites = s->config.sites;
      set->site_count = s->config.site_count;
    }
  }
  for (k = 0; k < set->site_count; ++k) { /* exclusive ownership of a replace-capable site */
    uint32_t owners = 0U;
    if ((set->sites[k].mode & GENESIS_HOOK_MODE_REPLACE) == 0U) continue;
    for (i = 0; i < set->count; ++i)
      if (find_binding(set->members[i], &set->sites[k]) != NULL && ++owners > 1U) {
        *member = i;
        *rejection = "E_SET_HOOK_OWNER";
        goto refused;
      }
  }
  set->hooks.ctx = set;
  set->hooks.start = set_start;
  set->hooks.frame_end = set_frame_end;
  set->hooks.after_snapshot = set_after_snapshot;
  rt->frame_hooks = &set->hooks;
  if (hooked) {
    set->function_hooks.ctx = set;
    set->function_hooks.enter = set_hook_enter;
    set->function_hooks.leave = set_hook_leave;
    set->function_hooks.skipped = set_hook_skipped;
    rt->function_hooks = &set->function_hooks;
  }
  *member = count;
  return 0;
refused:
  genesis_mod_set_detach(set);
  return 1;
}

/* ---- the set's state body: member_count u32 | per member (length u32, the member's own body) | hook depth u32, abandoned/unproven/overflowed u64 x3, frames (site index, return pc, entry sp) u32 x3 each ---- */
static uint32_t set_state_put(void *ctx, uint8_t *out, uint32_t cap, GenesisSnapshotStatus *status) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  StateOut o;
  uint8_t *scratch;
  uint32_t i, depth;
  o.p = out; o.cap = cap; o.n = 0U; o.error = 0;
  *status = GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  if (set->runtime == NULL) return UINT32_MAX;
  scratch = (uint8_t *)malloc(GENESIS_SNAPSHOT_MOD_BODY_MAX);
  if (scratch == NULL) return UINT32_MAX;
  so_u32(&o, set->count);
  for (i = 0; i < set->count; ++i) {
    const uint32_t n = state_put(set->members[i], scratch, GENESIS_SNAPSHOT_MOD_BODY_MAX, status);
    if (n > GENESIS_SNAPSHOT_MOD_BODY_MAX) {
      free(scratch);
      return UINT32_MAX;
    }
    so_u32(&o, n);
    so_bytes(&o, scratch, n);
  }
  free(scratch);
  depth = set->function_hooks.depth;
  if (depth > GENESIS_HOOK_FRAMES_MAX) o.error = 1;
  so_u32(&o, o.error ? 0U : depth);
  so_u64(&o, set->function_hooks.abandoned); so_u64(&o, set->function_hooks.unproven_callers); so_u64(&o, set->function_hooks.overflowed);
  for (i = 0; !o.error && i < depth; ++i) {
    const GenesisHookFrame *f = &set->function_hooks.frames[i];
    uint32_t index = 0U;
    while (index < set->site_count && f->site != &set->sites[index]) ++index;
    if (index == set->site_count) o.error = 1;
    so_u32(&o, index);
    so_u32(&o, f->return_pc);
    so_u32(&o, f->entry_sp);
  }
  if (o.error) { /* the members' blobs together exceed the section's bound: no partial state */
    *status = GENESIS_SNAPSHOT_MOD_UNAVAILABLE;
    return UINT32_MAX;
  }
  *status = GENESIS_SNAPSHOT_OK;
  return o.n;
}

typedef struct StagedSetState {
  void *members[GENESIS_MOD_SET_MEMBERS_MAX];
  uint32_t count, depth;
  uint64_t abandoned, unproven, overflowed;
  struct { uint32_t site, return_pc, entry_sp; } frames[GENESIS_HOOK_FRAMES_MAX];
} StagedSetState;

static void set_state_discard(void *ctx, void *staged) {
  StagedSetState *st = (StagedSetState *)staged;
  uint32_t i;
  (void)ctx;
  if (st == NULL) return;
  for (i = 0; i < GENESIS_MOD_SET_MEMBERS_MAX; ++i) free(st->members[i]);
  free(st);
}

static void *set_state_stage(void *ctx, const uint8_t *body, uint32_t length) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  StagedSetState *st;
  StateIn in;
  uint32_t i;
  if (set->runtime == NULL) return NULL;
  st = (StagedSetState *)malloc(sizeof *st);
  if (st == NULL) return NULL;
  memset(st, 0, sizeof *st);
  in.p = body; in.size = length; in.off = 0U; in.error = 0;
  st->count = si_u32(&in);
  if (in.error || st->count != set->count) goto bad;
  for (i = 0; i < st->count; ++i) {
    const uint32_t n = si_u32(&in);
    const uint8_t *at = si_take(&in, n);
    if (at == NULL) goto bad;
    st->members[i] = state_stage(set->members[i], at, n);
    if (st->members[i] == NULL) goto bad;
    {
      const StagedModState *m = (const StagedModState *)st->members[i], *first = (const StagedModState *)st->members[0];
      if (m->started != first->started || m->depth != 0U) goto bad; /* members start together and keep no call frame of their own */
    }
  }
  st->depth = si_u32(&in);
  st->abandoned = si_u64(&in); st->unproven = si_u64(&in); st->overflowed = si_u64(&in);
  if (in.error || st->depth > GENESIS_HOOK_FRAMES_MAX) goto bad;
  if (set->site_count == 0U && (st->depth != 0U || st->abandoned != 0U || st->unproven != 0U || st->overflowed != 0U)) goto bad;
  for (i = 0; i < st->depth; ++i) {
    st->frames[i].site = si_u32(&in); st->frames[i].return_pc = si_u32(&in); st->frames[i].entry_sp = si_u32(&in);
    if (in.error || st->frames[i].site >= set->site_count || (set->sites[st->frames[i].site].mode & GENESIS_HOOK_MODE_AFTER) == 0U) goto bad;
  }
  if (in.off != in.size) goto bad;
  return st;
bad:
  set_state_discard(ctx, st);
  return NULL;
}

static int set_state_apply(void *ctx, void *staged) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  StagedSetState *st = (StagedSetState *)staged;
  uint8_t *prior[GENESIS_MOD_SET_MEMBERS_MAX] = {NULL};
  uint32_t prior_len[GENESIS_MOD_SET_MEMBERS_MAX] = {0U};
  uint32_t i, j;
  int failed = 0;
  for (i = 0; !failed && i < set->count; ++i) { /* the blobs of the running members, taken before any of them changes */
    GenesisModScheduler *s = set->members[i];
    if (!granted(s, SEGA_MOD_CAP_STATE_MOD) || s->disabled) continue;
    prior[i] = (uint8_t *)malloc(GENESIS_SNAPSHOT_MOD_BLOB_MAX);
    if (prior[i] == NULL) failed = 1;
    else if (s->config.provider->save_state(s->config.provider_state, prior[i], GENESIS_SNAPSHOT_MOD_BLOB_MAX, &prior_len[i]) != 0 || prior_len[i] > GENESIS_SNAPSHOT_MOD_BLOB_MAX) {
      free(prior[i]);
      prior[i] = NULL;
    }
  }
  for (i = 0; !failed && i < set->count; ++i) {
    if (state_apply(set->members[i], st->members[i]) == 0) continue;
    failed = 1;
    for (j = 0; j < i; ++j) { /* put back the members that had already loaded the other state */
      GenesisModScheduler *s = set->members[j];
      const StagedModState *m = (const StagedModState *)st->members[j];
      if (!granted(s, SEGA_MOD_CAP_STATE_MOD) || m->disabled != 0U || s->disabled) continue; /* it loaded nothing, or is inert */
      if (prior[j] == NULL || s->config.provider->load_state(s->config.provider_state, prior[j], prior_len[j]) != 0) {
        s->phase = SEGA_MOD_PHASE_RESET;
        s->frame = set->runtime->virtual_frame_count;
        record_fault(s, "state");
      }
    }
  }
  for (i = 0; i < set->count; ++i) free(prior[i]);
  if (failed) set_release_pad(set);
  return failed;
}

static void set_state_commit(void *ctx, void *staged, GenesisRuntime *runtime) {
  GenesisModSet *set = (GenesisModSet *)ctx;
  StagedSetState *st = (StagedSetState *)staged;
  uint8_t active = 0U, pad1 = 0U, pad2 = 0U;
  uint32_t i;
  for (i = 0; i < set->count; ++i) {
    const StagedModState *m = (const StagedModState *)st->members[i];
    if (m->pad_active != 0U) {
      active = 1U;
      pad1 = m->mod_pad[0];
      pad2 = m->mod_pad[1];
    }
    state_commit(set->members[i], st->members[i], runtime); /* frees the member's staged state */
    st->members[i] = NULL;
  }
  runtime->mod_pad_active = active;
  runtime->mod_pad1 = pad1;
  runtime->mod_pad2 = pad2;
  if (runtime->function_hooks == &set->function_hooks) {
    set->function_hooks.depth = st->depth;
    set->function_hooks.abandoned = st->abandoned;
    set->function_hooks.unproven_callers = st->unproven;
    set->function_hooks.overflowed = st->overflowed;
    for (i = 0; i < st->depth; ++i) {
      set->function_hooks.frames[i].site = &set->sites[st->frames[i].site];
      set->function_hooks.frames[i].return_pc = st->frames[i].return_pc;
      set->function_hooks.frames[i].entry_sp = st->frames[i].entry_sp;
    }
  }
  free(st);
}

void genesis_mod_set_snapshot_state(GenesisModSet *set, GenesisSnapshotModState *out) {
  GenesisSha256 sha;
  uint32_t i;
  static const char tag[] = "segarecomp.genesis.mod.set.compat.v1";
  memset(out, 0, sizeof *out);
  out->ctx = set;
  memcpy(out->identity, set->identity, 32U);
  out->state_schema = 1U; /* the set body format; each member's declared schema is part of `compat` */
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, (const uint8_t *)tag, (uint32_t)(sizeof tag - 1U));
  sha_u32(&sha, set->count);
  for (i = 0; i < set->count; ++i) {
    uint8_t member_compat[32];
    compat_digest(set->members[i], member_compat);
    genesis_sha256_update(&sha, set->members[i]->identity, 32U);
    sha_u32(&sha, set->members[i]->config.state_schema);
    genesis_sha256_update(&sha, member_compat, 32U);
  }
  genesis_sha256_final(&sha, out->compat);
  out->put = set_state_put;
  out->stage = set_state_stage;
  out->apply = set_state_apply;
  out->commit = set_state_commit;
  out->discard = set_state_discard;
}
