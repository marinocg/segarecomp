#include "genesis_snapshot.h"

#include <stdlib.h>
#include <string.h>

#ifdef SEGARECOMP_GENESIS_SOUND
#include "genesis_audio.h"
#include "z80_machine.h"
#include "z80_registry.h"
#endif

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Format constants (docs/architecture/genesis-save-states-persistence-and-rewind.md section 4).                                      */

#define HEADER_FIXED_BYTES 156U
#define SECTION_ENTRY_BYTES 12U
#define DIGEST_BYTES 32U

enum {
  SEC_CPU = 1, SEC_RAM = 2, SEC_VDP = 3, SEC_IO = 4, SEC_CART = 5, SEC_Z80 = 6, SEC_PSG = 7, SEC_YM = 8, SEC_MIXER = 9, SEC_AUDIO = 10,
  SEC_LAST = SEC_AUDIO
};
#define SECTION_FLAG_REQUIRED 1U

/* Fixed per-section maxima: an allocation or read is never sized by a file length. */
static const uint32_t k_section_max[SEC_LAST + 1] = {0U, 256U, 65536U, 70000U, 16384U, 65536U + 64U, 512U, 64U, 16384U, 80000U, 256U};

static const char k_exec_tag[] = "segarecomp.genesis.sgstate.exec.v1";

const char *genesis_snapshot_status_text(GenesisSnapshotStatus status) {
  switch (status) {
    case GENESIS_SNAPSHOT_OK: return "ok";
    case GENESIS_SNAPSHOT_BAD_ARGUMENT: return "invalid request";
    case GENESIS_SNAPSHOT_NO_MEMORY: return "not enough memory";
    case GENESIS_SNAPSHOT_TOO_LARGE: return "the state file is too large";
    case GENESIS_SNAPSHOT_TRUNCATED: return "the state file is truncated";
    case GENESIS_SNAPSHOT_BAD_MAGIC: return "this is not a save state";
    case GENESIS_SNAPSHOT_UNSUPPORTED_VERSION: return "the state was written by an incompatible version";
    case GENESIS_SNAPSHOT_BAD_LAYOUT: return "the state file is damaged";
    case GENESIS_SNAPSHOT_UNKNOWN_SECTION: return "the state has unsupported or missing parts";
    case GENESIS_SNAPSHOT_INTEGRITY: return "the state file is corrupt";
    case GENESIS_SNAPSHOT_WRONG_CARTRIDGE: return "the state belongs to a different game or version";
    case GENESIS_SNAPSHOT_INCOMPATIBLE: return "the state does not match this build of the game";
    case GENESIS_SNAPSHOT_INVALID_STATE: return "the state holds values the console cannot have";
    case GENESIS_SNAPSHOT_IMAGE_MISSING: return "the state needs sound code this build does not contain";
    case GENESIS_SNAPSHOT_NOT_CAPTURABLE: return "the game is in a state that cannot be saved";
  }
  return "unknown error";
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Little-endian writer (growable, bounded) and reader (bounded, sticky error).                                                       */

typedef struct Writer {
  uint8_t *data;
  uint32_t size, capacity;
  int error;
} Writer;

static void w_reserve(Writer *w, uint32_t extra) {
  uint32_t wanted;
  if (w->error) return;
  if (extra > GENESIS_SNAPSHOT_MAX_FILE_BYTES || w->size > GENESIS_SNAPSHOT_MAX_FILE_BYTES - extra) { w->error = 1; return; }
  wanted = w->size + extra;
  if (wanted > w->capacity) {
    uint32_t capacity = w->capacity == 0U ? 4096U : w->capacity;
    uint8_t *grown;
    while (capacity < wanted) capacity *= 2U;
    if (capacity > GENESIS_SNAPSHOT_MAX_FILE_BYTES) capacity = GENESIS_SNAPSHOT_MAX_FILE_BYTES;
    grown = (uint8_t *)realloc(w->data, capacity);
    if (grown == NULL) { w->error = 1; return; }
    w->data = grown;
    w->capacity = capacity;
  }
}

static void w_bytes(Writer *w, const void *src, uint32_t count) {
  w_reserve(w, count);
  if (w->error) return;
  if (count != 0U) memcpy(w->data + w->size, src, count);
  w->size += count;
}
static void w_u8(Writer *w, uint8_t v) { w_bytes(w, &v, 1U); }
static void w_u16(Writer *w, uint16_t v) { uint8_t b[2]; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); w_bytes(w, b, 2U); }
static void w_u32(Writer *w, uint32_t v) { uint8_t b[4]; unsigned i; for (i = 0; i < 4U; ++i) b[i] = (uint8_t)(v >> (8U * i)); w_bytes(w, b, 4U); }
static void w_u64(Writer *w, uint64_t v) { uint8_t b[8]; unsigned i; for (i = 0; i < 8U; ++i) b[i] = (uint8_t)(v >> (8U * i)); w_bytes(w, b, 8U); }

typedef struct Reader {
  const uint8_t *p;
  uint32_t size, off;
  int error;
} Reader;

static const uint8_t *r_take(Reader *r, uint32_t count) {
  const uint8_t *at;
  if (r->error || count > r->size - r->off) { r->error = 1; return NULL; }
  at = r->p + r->off;
  r->off += count;
  return at;
}
static uint8_t r_u8(Reader *r) { const uint8_t *b = r_take(r, 1U); return b == NULL ? 0U : b[0]; }
static uint16_t r_u16(Reader *r) { const uint8_t *b = r_take(r, 2U); return b == NULL ? 0U : (uint16_t)(b[0] | ((unsigned)b[1] << 8)); }
static uint32_t r_u32(Reader *r) { const uint8_t *b = r_take(r, 4U); uint32_t v = 0U; unsigned i; if (b == NULL) return 0U; for (i = 0; i < 4U; ++i) v |= (uint32_t)b[i] << (8U * i); return v; }
static uint64_t r_u64(Reader *r) { const uint8_t *b = r_take(r, 8U); uint64_t v = 0U; unsigned i; if (b == NULL) return 0U; for (i = 0; i < 8U; ++i) v |= (uint64_t)b[i] << (8U * i); return v; }
static void r_bytes(Reader *r, void *dst, uint32_t count) { const uint8_t *b = r_take(r, count); if (b != NULL && count != 0U) memcpy(dst, b, count); }
static int r_done(const Reader *r) { return !r->error && r->off == r->size; }

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | ((unsigned)b[1] << 8)); }
static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint64_t le64(const uint8_t *b) { return (uint64_t)le32(b) | ((uint64_t)le32(b + 4) << 32); }
static void put_le16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *b, uint32_t v) { unsigned i; for (i = 0; i < 4U; ++i) b[i] = (uint8_t)(v >> (8U * i)); }

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Machine profile and execution fingerprint.                                                                                         */

void genesis_snapshot_buffer_free(GenesisSnapshotBuffer *buffer) {
  if (buffer == NULL) return;
  free(buffer->data);
  buffer->data = NULL;
  buffer->size = 0U;
}

uint16_t genesis_snapshot_machine_flags(const GenesisSnapshotMachine *machine) {
  uint16_t flags = 0U;
  if (machine == NULL || machine->runtime == NULL) return 0U;
  if (machine->z80 != NULL && machine->audio != NULL) flags |= GENESIS_SNAPSHOT_FLAG_SOUND;
  if (machine->runtime->cartridge_sram != NULL && machine->runtime->cartridge_sram->layout_supported != 0U &&
      machine->runtime->cartridge_sram_storage != NULL && machine->runtime->cartridge_sram->storage_bytes != 0U) {
    flags |= GENESIS_SNAPSHOT_FLAG_SRAM;
    if (machine->runtime->cartridge_sram->kind == SEGARECOMP_GENESIS_CARTRIDGE_KIND_EEPROM_X24C01) flags |= GENESIS_SNAPSHOT_FLAG_EEPROM;
  }
  return flags;
}

/* The execution-compatibility fingerprint: independent of build directories, compiler paths and emitter versions. It binds the state schema
 * and the sizes/constants the serialized sections depend on, so a runtime whose state layout would not accept the data is refused. */
static void exec_fingerprint(uint16_t flags, uint8_t out[32]) {
  GenesisSha256 sha;
  uint8_t word[8];
  const uint32_t values[] = {GENESIS_SNAPSHOT_STATE_SCHEMA, flags, 65536U, GENESIS_VDP_VRAM_BYTES, GENESIS_VDP_CRAM_BYTES, GENESIS_VDP_VSRAM_BYTES,
                             GENESIS_VDP_REGISTER_COUNT, GENESIS_Z80_RAM_BYTES, GENESIS_EXCEPTION_FRAME_ORIGIN_BYTES,
                             GENESIS_M68K_SOFTWARE_EXCEPTION_VECTOR_LIMIT};
  unsigned i;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, (const uint8_t *)k_exec_tag, (uint32_t)(sizeof k_exec_tag - 1U));
  for (i = 0; i < sizeof values / sizeof values[0]; ++i) {
    put_le32(word, values[i]);
    genesis_sha256_update(&sha, word, 4U);
  }
  genesis_sha256_final(&sha, out);
}


/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Section codecs. Each `put_*` reads the machine; each `get_*` decodes into the STAGED copy only and validates.                        */

static void put_cpu(Writer *w, const GenesisRuntime *rt) {
  unsigned i;
  for (i = 0; i < 8U; ++i) w_u32(w, rt->d[i]);
  for (i = 0; i < 8U; ++i) w_u32(w, rt->a[i]);
  w_u32(w, rt->usp);
  w_u16(w, rt->sr);
  w_u32(w, rt->pc);
  w_u8(w, rt->m68k_interrupt.sampled_level);
  w_u8(w, rt->m68k_interrupt.level7_edge_pending);
  w_u8(w, rt->m68k_interrupt.stopped);
}

static int get_cpu(Reader *r, GenesisRuntime *rt) {
  unsigned i;
  for (i = 0; i < 8U; ++i) rt->d[i] = r_u32(r);
  for (i = 0; i < 8U; ++i) rt->a[i] = r_u32(r);
  rt->usp = r_u32(r);
  rt->sr = r_u16(r);
  rt->pc = r_u32(r);
  rt->m68k_interrupt.sampled_level = r_u8(r);
  rt->m68k_interrupt.level7_edge_pending = r_u8(r);
  rt->m68k_interrupt.stopped = r_u8(r);
  if (!r_done(r)) return 0;
  return rt->m68k_interrupt.sampled_level <= 7U && rt->m68k_interrupt.level7_edge_pending <= 1U && rt->m68k_interrupt.stopped <= 1U;
}

static void put_vdp(Writer *w, const GenesisVdpState *v) {
  unsigned i;
  for (i = 0; i < GENESIS_VDP_REGISTER_COUNT; ++i) w_u16(w, v->registers[i]);
  w_u8(w, v->control_port_awaiting_second_word);
  w_u16(w, v->control_port_first_word);
  w_u32(w, v->addressed_pointer);
  w_u16(w, v->auto_increment_value);
  w_u16(w, v->status_register);
  w_u8(w, v->data_port_transfer_code);
  w_u8(w, v->data_port_transfer_code_valid);
  w_u8(w, (uint8_t)v->dma.phase);
  w_u8(w, (uint8_t)v->dma.kind);
  w_u32(w, v->dma.source_address);
  w_u32(w, v->dma.remaining_length);
  w_u32(w, v->dma.fill_byte_count);
  w_u32(w, v->dma.transfer_access_count);
  w_u8(w, v->dma.write_target_code);
  w_bytes(w, v->vram, GENESIS_VDP_VRAM_BYTES);
  w_bytes(w, v->cram, GENESIS_VDP_CRAM_BYTES);
  w_bytes(w, v->vsram, GENESIS_VDP_VSRAM_BYTES);
}

static int get_vdp(Reader *r, GenesisVdpState *v) {
  unsigned i;
  uint8_t phase, kind;
  for (i = 0; i < GENESIS_VDP_REGISTER_COUNT; ++i) v->registers[i] = r_u16(r);
  v->control_port_awaiting_second_word = r_u8(r);
  v->control_port_first_word = r_u16(r);
  v->addressed_pointer = r_u32(r);
  v->auto_increment_value = r_u16(r);
  v->status_register = r_u16(r);
  v->data_port_transfer_code = r_u8(r);
  v->data_port_transfer_code_valid = r_u8(r);
  phase = r_u8(r);
  kind = r_u8(r);
  v->dma.source_address = r_u32(r);
  v->dma.remaining_length = r_u32(r);
  v->dma.fill_byte_count = r_u32(r);
  v->dma.transfer_access_count = r_u32(r);
  v->dma.write_target_code = r_u8(r);
  r_bytes(r, v->vram, GENESIS_VDP_VRAM_BYTES);
  r_bytes(r, v->cram, GENESIS_VDP_CRAM_BYTES);
  r_bytes(r, v->vsram, GENESIS_VDP_VSRAM_BYTES);
  if (!r_done(r)) return 0;
  if (phase > (uint8_t)GENESIS_VDP_DMA_BUSY || kind > (uint8_t)GENESIS_VDP_DMA_VRAM_FILL) return 0;
  v->dma.phase = (GenesisVdpDmaPhase)phase;
  v->dma.kind = (GenesisVdpDmaKind)kind;
  if (v->control_port_awaiting_second_word > 1U || v->data_port_transfer_code_valid > 1U || v->data_port_transfer_code > 0x3FU) return 0;
  if (v->addressed_pointer > 0xFFFFU || v->dma.source_address > 0xFFFFFFU || v->dma.remaining_length > 65536U || v->dma.fill_byte_count > 131072U ||
      v->dma.write_target_code > 0x3FU)
    return 0;
  return 1;
}

static void put_io(Writer *w, const GenesisRuntime *rt) {
  const GenesisZ80BusState *bus = &rt->devices.z80_bus;
  unsigned i;
  w_u8(w, bus->bus_requested);
  w_u8(w, bus->bus_granted);
  w_u8(w, bus->reset_released);
  w_u16(w, bus->bank);
  w_bytes(w, bus->z80_ram, GENESIS_Z80_RAM_BYTES);
  w_u32(w, rt->devices.psg.write_count);
  w_u32(w, rt->devices.psg.write_digest);
  for (i = 0; i < 3U; ++i) w_u8(w, rt->devices.controller_io.data[i]);
  for (i = 0; i < 3U; ++i) w_u8(w, rt->devices.controller_io.ctrl[i]);
  w_u8(w, rt->devices.interrupt.vblank_pending);
  w_u32(w, rt->devices.interrupt.vblank_status_read_count);
  w_u32(w, rt->devices.interrupt.vblank_transition_count);
  w_u8(w, rt->devices.interrupt.checkpoint_entered);
  w_u32(w, rt->devices.interrupt.vblank_transition_count_at_checkpoint_entry);
  w_u64(w, rt->scheduler.master_ticks);
  w_u8(w, rt->raster.initialized);
  w_u64(w, rt->raster.last_line);
  w_u64(w, rt->raster.next_line_tick);
  w_u8(w, rt->raster.hint_counter);
  w_u8(w, rt->raster.hint_pending);
  w_u64(w, rt->raster.hint_undelivered);
  w_u64(w, rt->virtual_frame_count);
  w_bytes(w, rt->exception_frame_irq6_origin, GENESIS_EXCEPTION_FRAME_ORIGIN_BYTES);
  w_u32(w, rt->z80_epoch.epoch_count);
  w_u8(w, rt->z80_epoch.executed);
  w_bytes(w, rt->z80_epoch.written, (uint32_t)sizeof rt->z80_epoch.written);
  w_u64(w, rt->z80_synced_ticks);
}

static int get_io(Reader *r, GenesisRuntime *rt) {
  GenesisZ80BusState *bus = &rt->devices.z80_bus;
  unsigned i;
  bus->bus_requested = r_u8(r);
  bus->bus_granted = r_u8(r);
  bus->reset_released = r_u8(r);
  bus->bank = r_u16(r);
  r_bytes(r, bus->z80_ram, GENESIS_Z80_RAM_BYTES);
  rt->devices.psg.write_count = r_u32(r);
  rt->devices.psg.write_digest = r_u32(r);
  for (i = 0; i < 3U; ++i) rt->devices.controller_io.data[i] = r_u8(r);
  for (i = 0; i < 3U; ++i) rt->devices.controller_io.ctrl[i] = r_u8(r);
  rt->devices.interrupt.vblank_pending = r_u8(r);
  rt->devices.interrupt.vblank_status_read_count = r_u32(r);
  rt->devices.interrupt.vblank_transition_count = r_u32(r);
  rt->devices.interrupt.checkpoint_entered = r_u8(r);
  rt->devices.interrupt.vblank_transition_count_at_checkpoint_entry = r_u32(r);
  rt->scheduler.master_ticks = r_u64(r);
  rt->raster.initialized = r_u8(r);
  rt->raster.last_line = r_u64(r);
  rt->raster.next_line_tick = r_u64(r);
  rt->raster.hint_counter = r_u8(r);
  rt->raster.hint_pending = r_u8(r);
  rt->raster.hint_undelivered = r_u64(r);
  rt->virtual_frame_count = r_u64(r);
  r_bytes(r, rt->exception_frame_irq6_origin, GENESIS_EXCEPTION_FRAME_ORIGIN_BYTES);
  rt->z80_epoch.epoch_count = r_u32(r);
  rt->z80_epoch.executed = r_u8(r);
  r_bytes(r, rt->z80_epoch.written, (uint32_t)sizeof rt->z80_epoch.written);
  rt->z80_synced_ticks = r_u64(r);
  if (!r_done(r)) return 0;
  if (bus->bus_requested > 1U || bus->bus_granted > 1U || bus->reset_released > 1U || bus->bank > 0x1FFU) return 0;
  if (rt->devices.interrupt.vblank_pending > 1U || rt->devices.interrupt.checkpoint_entered > 1U) return 0;
  if (rt->raster.initialized > 1U || rt->raster.hint_pending > 1U || rt->z80_epoch.executed > 1U) return 0;
  if (rt->z80_synced_ticks > rt->scheduler.master_ticks) return 0;
  return 1;
}

static void put_cart(Writer *w, const GenesisRuntime *rt, uint16_t flags) {
  w_u8(w, (flags & GENESIS_SNAPSHOT_FLAG_SRAM) != 0U ? 1U : 0U);
  if ((flags & GENESIS_SNAPSHOT_FLAG_SRAM) == 0U) return;
  w_u32(w, rt->cartridge_sram->storage_bytes);
  w_u8(w, rt->cartridge_sram->kind);
  w_u8(w, rt->cartridge_sram_control);
  w_u8(w, rt->cartridge_eeprom.sda);
  w_u8(w, rt->cartridge_eeprom.scl);
  w_u8(w, rt->cartridge_eeprom.old_sda);
  w_u8(w, rt->cartridge_eeprom.old_scl);
  w_u8(w, rt->cartridge_eeprom.phase);
  w_u8(w, rt->cartridge_eeprom.cycles);
  w_u8(w, rt->cartridge_eeprom.read);
  w_u8(w, rt->cartridge_eeprom.address);
  w_u8(w, rt->cartridge_eeprom.buffer);
  w_bytes(w, rt->cartridge_sram_storage, rt->cartridge_sram->storage_bytes);
}

/* The SRAM bytes are staged into `storage_stage` (a copy of the live array) and committed with the rest. */
static int get_cart(Reader *r, GenesisRuntime *rt, uint8_t *storage_stage, uint16_t flags) {
  const uint8_t present = r_u8(r);
  if (present != ((flags & GENESIS_SNAPSHOT_FLAG_SRAM) != 0U ? 1U : 0U)) return 0;
  if (present == 0U) return r_done(r);
  {
    const uint32_t bytes = r_u32(r);
    const uint8_t kind = r_u8(r);
    rt->cartridge_sram_control = r_u8(r);
    rt->cartridge_eeprom.sda = r_u8(r);
    rt->cartridge_eeprom.scl = r_u8(r);
    rt->cartridge_eeprom.old_sda = r_u8(r);
    rt->cartridge_eeprom.old_scl = r_u8(r);
    rt->cartridge_eeprom.phase = r_u8(r);
    rt->cartridge_eeprom.cycles = r_u8(r);
    rt->cartridge_eeprom.read = r_u8(r);
    rt->cartridge_eeprom.address = r_u8(r);
    rt->cartridge_eeprom.buffer = r_u8(r);
    if (r->error || bytes != rt->cartridge_sram->storage_bytes || kind != rt->cartridge_sram->kind) return 0;
    r_bytes(r, storage_stage, bytes);
  }
  if (!r_done(r)) return 0;
  if (rt->cartridge_eeprom.sda > 1U || rt->cartridge_eeprom.scl > 1U || rt->cartridge_eeprom.old_sda > 1U || rt->cartridge_eeprom.old_scl > 1U ||
      rt->cartridge_eeprom.read > 1U || rt->cartridge_eeprom.cycles > 9U || rt->cartridge_eeprom.address > 0x7FU ||
      rt->cartridge_eeprom.phase > (uint8_t)GENESIS_EEPROM_PHASE_LAST)
    return 0;
  if (rt->cartridge_sram->kind != SEGARECOMP_GENESIS_CARTRIDGE_KIND_EEPROM_X24C01 &&
      (rt->cartridge_eeprom.phase != 0U || rt->cartridge_eeprom.cycles != 0U))
    return 0;
  return 1;
}

#ifdef SEGARECOMP_GENESIS_SOUND

static void w_i32(Writer *w, int32_t v) { w_u32(w, (uint32_t)v); }
static int32_t r_i32(Reader *r) { return (int32_t)r_u32(r); }
static int all_zero(const uint8_t *p, size_t n) { size_t i; for (i = 0; i < n; ++i) if (p[i] != 0U) return 0; return 1; }

static int image_hash_of(uint32_t ordinal, uint8_t out[32]) {
  if (ordinal == 0U) { memset(out, 0, 32U); return 1; }
  return genesis_z80_image_content_hash(ordinal, out);
}

/* 1 and the ordinal for a stored hash (0 = none); 0 when this program has no such image. */
static int ordinal_of_hash(const uint8_t hash[32], uint32_t *ordinal) {
  uint32_t i;
  uint8_t candidate[32];
  if (all_zero(hash, 32U)) { *ordinal = 0U; return 1; }
  for (i = 1U; i <= genesis_z80_image_count; ++i)
    if (genesis_z80_image_content_hash(i, candidate) && memcmp(candidate, hash, 32U) == 0) { *ordinal = i; return 1; }
  return 0;
}

static GenesisSnapshotStatus put_z80(Writer *w, const GenesisZ80Machine *m) {
  const Z80State *s = &m->cpu.state;
  uint8_t hash[32];
  if (m->view_stop != (GenesisDiagnosticCategory)0) return GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  w_u8(w, s->a); w_u8(w, s->f); w_u8(w, s->b); w_u8(w, s->c); w_u8(w, s->d); w_u8(w, s->e); w_u8(w, s->h); w_u8(w, s->l);
  w_u8(w, s->a2); w_u8(w, s->f2); w_u8(w, s->b2); w_u8(w, s->c2); w_u8(w, s->d2); w_u8(w, s->e2); w_u8(w, s->h2); w_u8(w, s->l2);
  w_u16(w, s->ix); w_u16(w, s->iy); w_u16(w, s->sp); w_u16(w, s->pc); w_u16(w, s->wz);
  w_u8(w, s->i); w_u8(w, s->r); w_u8(w, s->im); w_u8(w, s->iff1); w_u8(w, s->iff2); w_u8(w, s->q);
  w_u8(w, s->halted); w_u8(w, s->int_deferral); w_u8(w, s->ld_a_ir); w_u8(w, s->in_prefix_run); w_u8(w, s->int_line);
  w_u8(w, s->nmi_pending); w_u8(w, s->nmi_reject);
  w_u64(w, s->cycles);
  w_u64(w, m->cycle_base_master_ticks);
  if (!image_hash_of(m->bound_ordinal, hash)) return GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  w_bytes(w, hash, 32U);
  if (!image_hash_of(m->last_bound, hash)) return GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  w_bytes(w, hash, 32U);
  w_u32(w, (uint32_t)m->sound_fault);
  w_u32(w, m->sound_fault_epoch);
  w_u64(w, m->sound_fault_master_ticks);
  w_u32(w, m->sound_recoveries);
  return GENESIS_SNAPSHOT_OK;
}

static GenesisSnapshotStatus get_z80(Reader *r, GenesisZ80Machine *m) {
  Z80State *s = &m->cpu.state;
  uint8_t bound[32], last[32];
  uint32_t fault;
  s->a = r_u8(r); s->f = r_u8(r); s->b = r_u8(r); s->c = r_u8(r); s->d = r_u8(r); s->e = r_u8(r); s->h = r_u8(r); s->l = r_u8(r);
  s->a2 = r_u8(r); s->f2 = r_u8(r); s->b2 = r_u8(r); s->c2 = r_u8(r); s->d2 = r_u8(r); s->e2 = r_u8(r); s->h2 = r_u8(r); s->l2 = r_u8(r);
  s->ix = r_u16(r); s->iy = r_u16(r); s->sp = r_u16(r); s->pc = r_u16(r); s->wz = r_u16(r);
  s->i = r_u8(r); s->r = r_u8(r); s->im = r_u8(r); s->iff1 = r_u8(r); s->iff2 = r_u8(r); s->q = r_u8(r);
  s->halted = r_u8(r); s->int_deferral = r_u8(r); s->ld_a_ir = r_u8(r); s->in_prefix_run = r_u8(r); s->int_line = r_u8(r);
  s->nmi_pending = r_u8(r); s->nmi_reject = r_u8(r);
  s->cycles = r_u64(r);
  m->cycle_base_master_ticks = r_u64(r);
  r_bytes(r, bound, 32U);
  r_bytes(r, last, 32U);
  fault = r_u32(r);
  m->sound_fault_epoch = r_u32(r);
  m->sound_fault_master_ticks = r_u64(r);
  m->sound_recoveries = r_u32(r);
  if (!r_done(r)) return GENESIS_SNAPSHOT_BAD_LAYOUT;
  if (s->im > 2U || s->iff1 > 1U || s->iff2 > 1U || s->halted > 1U || s->int_deferral > 1U || s->ld_a_ir > 1U || s->in_prefix_run > 1U ||
      s->int_line > 1U || s->nmi_pending > 1U || s->nmi_reject > 1U)
    return GENESIS_SNAPSHOT_INVALID_STATE;
  if (!ordinal_of_hash(bound, &m->bound_ordinal) || !ordinal_of_hash(last, &m->last_bound)) return GENESIS_SNAPSHOT_IMAGE_MISSING;
  if (fault > 255U || (fault != 0U && !all_zero(bound, 32U))) return GENESIS_SNAPSHOT_INVALID_STATE;   /* a faulted sound CPU has no bound image */
  m->sound_fault = (GenesisDiagnosticCategory)fault;
  m->view_stop = (GenesisDiagnosticCategory)0;
  s->deadline = 0U;
  return GENESIS_SNAPSHOT_OK;
}

static void put_psg(Writer *w, const GenesisAudio *a) {
  uint8_t bytes[SN76489_STATE_BYTES];
  sn76489_state_bytes(&a->psg, bytes);
  w_bytes(w, bytes, SN76489_STATE_BYTES);
}

static int get_psg(Reader *r, GenesisAudio *a) {
  uint8_t bytes[SN76489_STATE_BYTES];
  r_bytes(r, bytes, SN76489_STATE_BYTES);
  if (!r_done(r)) return 0;
  return sn76489_load_state_bytes(&a->psg, bytes) == 0;
}

static void put_audio_counters(Writer *w, const GenesisAudio *a) {
  w_u64(w, a->psg_writes);
  w_u64(w, a->psg_data_before_latch);
  w_u64(w, a->ym_writes);
  w_u64(w, a->ym_class_dac); w_u64(w, a->ym_class_dac_enable); w_u64(w, a->ym_class_key); w_u64(w, a->ym_class_timer);
  w_u64(w, a->ym_class_global); w_u64(w, a->ym_class_operator); w_u64(w, a->ym_class_address);
  w_u8(w, a->ym_latched[0]);
  w_u8(w, a->ym_latched[1]);
  w_u64(w, a->ym_samples);
  w_u64(w, a->ym_sample_fnv);
}

static int get_audio_counters(Reader *r, GenesisAudio *a) {
  a->psg_writes = r_u64(r);
  a->psg_data_before_latch = r_u64(r);
  a->ym_writes = r_u64(r);
  a->ym_class_dac = r_u64(r); a->ym_class_dac_enable = r_u64(r); a->ym_class_key = r_u64(r); a->ym_class_timer = r_u64(r);
  a->ym_class_global = r_u64(r); a->ym_class_operator = r_u64(r); a->ym_class_address = r_u64(r);
  a->ym_latched[0] = r_u8(r);
  a->ym_latched[1] = r_u8(r);
  a->ym_samples = r_u64(r);
  a->ym_sample_fnv = r_u64(r);
  /* the bounded diagnostic traces are not guest state: the restored session starts them empty */
  a->trace_count = 0U;
  a->ym_trace_count = 0U;
  return r_done(r);
}

static void put_sha(Writer *w, const GenesisSha256 *sha) {
  unsigned i;
  for (i = 0; i < 8U; ++i) w_u32(w, sha->h[i]);
  w_u64(w, sha->length);
  w_bytes(w, sha->block, 64U);
  w_u32(w, sha->used);
}

static int get_sha(Reader *r, GenesisSha256 *sha) {
  unsigned i;
  for (i = 0; i < 8U; ++i) sha->h[i] = r_u32(r);
  sha->length = r_u64(r);
  r_bytes(r, sha->block, 64U);
  sha->used = r_u32(r);
  return sha->used < 64U && (sha->length % 64U) == sha->used;
}

static GenesisSnapshotStatus put_source(Writer *w, const GenesisMixerSource *s) {
  uint32_t i;
  if (s->fifo_count > GENESIS_MIXER_FIFO_WINDOWS || s->fifo_head >= GENESIS_MIXER_FIFO_WINDOWS) return GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  w_u64(w, s->window);
  w_u64(w, s->window_end);
  w_u64(w, s->watermark);
  w_u64(w, (uint64_t)s->sum[0]);
  w_u64(w, (uint64_t)s->sum[1]);
  w_u32(w, s->count);
  w_i32(w, s->last[0]);
  w_i32(w, s->last[1]);
  w_u32(w, s->fifo_count);
  for (i = 0U; i < s->fifo_count; ++i) {  /* canonical: oldest first, the head is normalized to zero on restore */
    const uint32_t at = (s->fifo_head + i) % GENESIS_MIXER_FIFO_WINDOWS;
    w_i32(w, s->fifo[at][0]);
    w_i32(w, s->fifo[at][1]);
  }
  return GENESIS_SNAPSHOT_OK;
}

static int get_source(Reader *r, GenesisMixerSource *s) {
  uint32_t i;
  s->window = r_u64(r);
  s->window_end = r_u64(r);
  s->watermark = r_u64(r);
  s->sum[0] = (int64_t)r_u64(r);
  s->sum[1] = (int64_t)r_u64(r);
  s->count = r_u32(r);
  s->last[0] = r_i32(r);
  s->last[1] = r_i32(r);
  s->fifo_count = r_u32(r);
  if (r->error || s->fifo_count > GENESIS_MIXER_FIFO_WINDOWS) return 0;
  s->fifo_head = 0U;
  for (i = 0U; i < s->fifo_count; ++i) {
    s->fifo[i][0] = r_i32(r);
    s->fifo[i][1] = r_i32(r);
  }
  if (!r->error && (s->count > 4096U || s->sum[0] > (int64_t)s->count * 65536 || s->sum[0] < -(int64_t)s->count * 65536 || s->sum[1] > (int64_t)s->count * 65536 || s->sum[1] < -(int64_t)s->count * 65536)) return 0;
  return !r->error && s->window_end == genesis_mixer_window_start(s->window + 1U) && s->window < UINT64_MAX / GENESIS_NTSC_MASTER_TICKS_PER_SECOND - 4U;
}

static GenesisSnapshotStatus put_mixer(Writer *w, const GenesisMixer *m) {
  GenesisSnapshotStatus status;
  if (m->fault != GENESIS_MIXER_FAULT_NONE) return GENESIS_SNAPSHOT_NOT_CAPTURABLE;
  status = put_source(w, &m->ym);
  if (status != GENESIS_SNAPSHOT_OK) return status;
  status = put_source(w, &m->psg);
  if (status != GENESIS_SNAPSHOT_OK) return status;
  w_u64(w, m->frames);
  w_u64(w, m->clipped);
  w_u64(w, m->changes);
  w_u16(w, (uint16_t)m->min_sample);
  w_u16(w, (uint16_t)m->max_sample);
  w_u16(w, (uint16_t)m->prev[0]);
  w_u16(w, (uint16_t)m->prev[1]);
  w_u8(w, m->have_prev);
  w_u8(w, (uint8_t)m->fault);
  put_sha(w, &m->sha);
  put_sha(w, &m->range_sha);
  w_u64(w, m->range_first);
  w_u64(w, m->range_count);
  w_u64(w, m->range_frames);
  return GENESIS_SNAPSHOT_OK;
}

static int get_mixer(Reader *r, GenesisMixer *m) {
  uint8_t fault;
  if (!get_source(r, &m->ym) || !get_source(r, &m->psg)) return 0;
  m->frames = r_u64(r);
  m->clipped = r_u64(r);
  m->changes = r_u64(r);
  m->min_sample = (int16_t)r_u16(r);
  m->max_sample = (int16_t)r_u16(r);
  m->prev[0] = (int16_t)r_u16(r);
  m->prev[1] = (int16_t)r_u16(r);
  m->have_prev = r_u8(r);
  fault = r_u8(r);
  if (!get_sha(r, &m->sha) || !get_sha(r, &m->range_sha)) return 0;
  m->range_first = r_u64(r);
  m->range_count = r_u64(r);
  m->range_frames = r_u64(r);
  if (!r_done(r)) return 0;
  /* host-only playback ring: the frames already produced belong to the abandoned timeline; the ring is emptied, its counters continue */
  m->ring_read = m->ring_write;
  m->fault = GENESIS_MIXER_FAULT_NONE;
  return fault == 0U && m->have_prev <= 1U;
}

#endif /* SEGARECOMP_GENESIS_SOUND */

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Capture                                                                                                                            */

static void section_begin(Writer *w, uint32_t *start) { *start = w->size; }

GenesisSnapshotStatus genesis_snapshot_capture(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity,
                                               const GenesisSnapshotThumbnail *thumbnail, GenesisSnapshotBuffer *out) {
  Writer w;
  uint16_t flags;
  uint32_t i, table_at, sections = 0U, starts[SEC_LAST + 2U], ids[SEC_LAST + 1U];
  uint32_t thumb_bytes = 0U, payload_start;
  uint8_t fp[32];
  GenesisSnapshotStatus status = GENESIS_SNAPSHOT_OK;
  const GenesisRuntime *rt;
  GenesisSha256 sha;
  uint8_t digest[32];

  if (out != NULL) { out->data = NULL; out->size = 0U; }
  if (machine == NULL || machine->runtime == NULL || identity == NULL || out == NULL) return GENESIS_SNAPSHOT_BAD_ARGUMENT;
  if ((machine->z80 == NULL) != (machine->audio == NULL)) return GENESIS_SNAPSHOT_BAD_ARGUMENT;
#ifndef SEGARECOMP_GENESIS_SOUND
  if (machine->z80 != NULL) return GENESIS_SNAPSHOT_BAD_ARGUMENT;
#endif
  if (thumbnail != NULL && thumbnail->rgb565 != NULL) {
    if (thumbnail->width == 0U || thumbnail->height == 0U || thumbnail->width > GENESIS_SNAPSHOT_MAX_THUMB_W || thumbnail->height > GENESIS_SNAPSHOT_MAX_THUMB_H)
      return GENESIS_SNAPSHOT_BAD_ARGUMENT;
    thumb_bytes = (uint32_t)thumbnail->width * (uint32_t)thumbnail->height * 2U;
  }
  rt = machine->runtime;
  flags = genesis_snapshot_machine_flags(machine);
  exec_fingerprint(flags, fp);

  memset(&w, 0, sizeof w);
  /* header (the variable fields are patched after the layout is known) */
  w_bytes(&w, "SGSTATE", 8U);
  w_u16(&w, GENESIS_SNAPSHOT_CONTAINER_VERSION);
  w_u16(&w, GENESIS_SNAPSHOT_STATE_SCHEMA);
  w_u16(&w, flags);
  w_u16(&w, 1U);  /* platform: Genesis */
  w_u32(&w, rt->region_profile);
  w_u32(&w, HEADER_FIXED_BYTES);
  w_u32(&w, 0U);  /* total bytes: patched */
  w_u32(&w, 0U);  /* section count: patched */
  w_bytes(&w, identity->source_sha256, 32U);
  w_bytes(&w, identity->variant_sha256, 32U);
  w_bytes(&w, fp, 32U);
  w_u64(&w, rt->virtual_frame_count);
  w_u64(&w, rt->scheduler.master_ticks);
  w_u16(&w, thumb_bytes != 0U ? thumbnail->width : 0U);
  w_u16(&w, thumb_bytes != 0U ? thumbnail->height : 0U);
  w_u32(&w, thumb_bytes);
  w_u32(&w, 0U);  /* reserved */

  /* section table: reserve for the maximum, shrink by writing only `sections` entries below */
  {
    const uint32_t wanted = (flags & GENESIS_SNAPSHOT_FLAG_SOUND) != 0U ? SEC_LAST : SEC_CART;
    for (i = 1U; i <= wanted; ++i) ids[sections++] = i;
  }
  table_at = w.size;
  for (i = 0U; i < sections; ++i) { w_u16(&w, 0U); w_u16(&w, 0U); w_u32(&w, 0U); w_u32(&w, 0U); }
  if (thumb_bytes != 0U) w_bytes(&w, thumbnail->rgb565, thumb_bytes);
  payload_start = w.size;
  (void)payload_start;

  for (i = 0U; i < sections && status == GENESIS_SNAPSHOT_OK && !w.error; ++i) {
    section_begin(&w, &starts[i]);
    switch (ids[i]) {
      case SEC_CPU: put_cpu(&w, rt); break;
      case SEC_RAM: w_bytes(&w, rt->work_ram, 65536U); break;
      case SEC_VDP: put_vdp(&w, &rt->devices.vdp); break;
      case SEC_IO: put_io(&w, rt); break;
      case SEC_CART: put_cart(&w, rt, flags); break;
#ifdef SEGARECOMP_GENESIS_SOUND
      case SEC_Z80: status = put_z80(&w, machine->z80); break;
      case SEC_PSG: put_psg(&w, machine->audio); break;
      case SEC_YM: {
        const uint32_t bytes = machine->audio->ym == NULL ? 0U : ym2612_snapshot_bytes(machine->audio->ym);
        if (bytes == 0U || bytes > YM2612_SNAPSHOT_MAX_BYTES) { status = GENESIS_SNAPSHOT_NOT_CAPTURABLE; break; }
        w_reserve(&w, bytes);
        if (!w.error) {
          if (ym2612_save(machine->audio->ym, w.data + w.size, bytes) != 0) { status = GENESIS_SNAPSHOT_NOT_CAPTURABLE; break; }
          w.size += bytes;
        }
        break;
      }
      case SEC_MIXER: status = put_mixer(&w, &machine->audio->mixer); break;
      case SEC_AUDIO: put_audio_counters(&w, machine->audio); break;
#endif
      default: status = GENESIS_SNAPSHOT_BAD_ARGUMENT; break;
    }
  }
  starts[sections] = w.size;
  if (w.error) { free(w.data); return GENESIS_SNAPSHOT_NO_MEMORY; }
  if (status != GENESIS_SNAPSHOT_OK) { free(w.data); return status; }
  for (i = 0U; i < sections; ++i) {
    const uint32_t length = starts[i + 1U] - starts[i];
    if (length > k_section_max[ids[i]]) { free(w.data); return GENESIS_SNAPSHOT_TOO_LARGE; }
    put_le16(w.data + table_at + i * SECTION_ENTRY_BYTES, (uint16_t)ids[i]);
    put_le16(w.data + table_at + i * SECTION_ENTRY_BYTES + 2U, (uint16_t)SECTION_FLAG_REQUIRED);
    put_le32(w.data + table_at + i * SECTION_ENTRY_BYTES + 4U, starts[i]);
    put_le32(w.data + table_at + i * SECTION_ENTRY_BYTES + 8U, length);
  }
  w_reserve(&w, DIGEST_BYTES);
  if (w.error) { free(w.data); return GENESIS_SNAPSHOT_TOO_LARGE; }
  put_le32(w.data + 24U, w.size + DIGEST_BYTES);
  put_le32(w.data + 28U, sections);
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, w.data, w.size);
  genesis_sha256_final(&sha, digest);
  memcpy(w.data + w.size, digest, DIGEST_BYTES);
  w.size += DIGEST_BYTES;
  {  /* exactly sized */
    uint8_t *fit = (uint8_t *)realloc(w.data, w.size);
    if (fit != NULL) w.data = fit;
  }
  out->data = w.data;
  out->size = w.size;
  return GENESIS_SNAPSHOT_OK;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Inspect (structure + integrity), shared by restore                                                                                 */

typedef struct SectionView {
  const uint8_t *data;
  uint32_t length;
  int present;
} SectionView;

static GenesisSnapshotStatus parse_container(const uint8_t *data, size_t size, GenesisSnapshotInfo *info, SectionView views[SEC_LAST + 1U]) {
  uint32_t header_bytes, total, count, i, thumb_bytes, cursor;
  uint16_t flags, platform;
  GenesisSha256 sha;
  uint8_t digest[32];
  if (data == NULL || info == NULL) return GENESIS_SNAPSHOT_BAD_ARGUMENT;
  memset(info, 0, sizeof *info);
  if (size > GENESIS_SNAPSHOT_MAX_FILE_BYTES) return GENESIS_SNAPSHOT_TOO_LARGE;
  if (size < 8U) return GENESIS_SNAPSHOT_TRUNCATED;
  if (memcmp(data, "SGSTATE", 8U) != 0) return GENESIS_SNAPSHOT_BAD_MAGIC;
  if (size < HEADER_FIXED_BYTES + DIGEST_BYTES) return GENESIS_SNAPSHOT_TRUNCATED;
  info->container_version = le16(data + 8);
  info->state_schema = le16(data + 10);
  if (info->container_version != GENESIS_SNAPSHOT_CONTAINER_VERSION || info->state_schema != GENESIS_SNAPSHOT_STATE_SCHEMA) return GENESIS_SNAPSHOT_UNSUPPORTED_VERSION;
  flags = le16(data + 12);
  platform = le16(data + 14);
  header_bytes = le32(data + 20);
  total = le32(data + 24);
  count = le32(data + 28);
  if (platform != 1U || header_bytes != HEADER_FIXED_BYTES || (flags & ~(GENESIS_SNAPSHOT_FLAG_SOUND | GENESIS_SNAPSHOT_FLAG_SRAM | GENESIS_SNAPSHOT_FLAG_EEPROM)) != 0U ||
      ((flags & GENESIS_SNAPSHOT_FLAG_EEPROM) != 0U && (flags & GENESIS_SNAPSHOT_FLAG_SRAM) == 0U))
    return GENESIS_SNAPSHOT_BAD_LAYOUT;
  if (total != size) return total > size ? GENESIS_SNAPSHOT_TRUNCATED : GENESIS_SNAPSHOT_BAD_LAYOUT;
  if (count == 0U || count > SEC_LAST) return GENESIS_SNAPSHOT_BAD_LAYOUT;
  info->machine_flags = flags;
  info->region_profile = le32(data + 16);
  memcpy(info->source_sha256, data + 32, 32U);
  memcpy(info->variant_sha256, data + 64, 32U);
  memcpy(info->exec_fingerprint, data + 96, 32U);
  info->guest_frame = le64(data + 128);
  info->guest_master_ticks = le64(data + 136);
  info->thumb_width = le16(data + 144);
  info->thumb_height = le16(data + 146);
  thumb_bytes = le32(data + 148);
  if (le32(data + 152) != 0U) return GENESIS_SNAPSHOT_BAD_LAYOUT;
  if ((thumb_bytes == 0U) != (info->thumb_width == 0U && info->thumb_height == 0U) ||
      (thumb_bytes != 0U && (info->thumb_width > GENESIS_SNAPSHOT_MAX_THUMB_W || info->thumb_height > GENESIS_SNAPSHOT_MAX_THUMB_H ||
                             thumb_bytes != (uint32_t)info->thumb_width * (uint32_t)info->thumb_height * 2U)))
    return GENESIS_SNAPSHOT_BAD_LAYOUT;
  /* the digest first: nothing below trusts a byte of a file that is not intact */
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, data, (uint32_t)(size - DIGEST_BYTES));
  genesis_sha256_final(&sha, digest);
  if (memcmp(digest, data + size - DIGEST_BYTES, DIGEST_BYTES) != 0) return GENESIS_SNAPSHOT_INTEGRITY;

  if (HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES + thumb_bytes + DIGEST_BYTES > size) return GENESIS_SNAPSHOT_TRUNCATED;
  cursor = HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES + thumb_bytes;
  if (thumb_bytes != 0U) info->thumb_rgb565 = data + HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES;
  if (views != NULL) memset(views, 0, sizeof(SectionView) * (SEC_LAST + 1U));
  for (i = 0U; i < count; ++i) {
    const uint8_t *entry = data + HEADER_FIXED_BYTES + i * SECTION_ENTRY_BYTES;
    const uint32_t id = le16(entry), sflags = le16(entry + 2), offset = le32(entry + 4), length = le32(entry + 8);
    if (id == 0U || id > SEC_LAST) return GENESIS_SNAPSHOT_UNKNOWN_SECTION;
    if ((sflags & SECTION_FLAG_REQUIRED) == 0U || (sflags & ~SECTION_FLAG_REQUIRED) != 0U) return GENESIS_SNAPSHOT_UNKNOWN_SECTION;
    if (views != NULL && views[id].present) return GENESIS_SNAPSHOT_UNKNOWN_SECTION;  /* duplicated */
    if (length > k_section_max[id]) return GENESIS_SNAPSHOT_BAD_LAYOUT;
    if (offset != cursor || length > size - DIGEST_BYTES - cursor) return GENESIS_SNAPSHOT_BAD_LAYOUT;  /* contiguous, in order, inside the file */
    if (views != NULL) { views[id].data = data + offset; views[id].length = length; views[id].present = 1; }
    cursor += length;
  }
  if (cursor != size - DIGEST_BYTES) return GENESIS_SNAPSHOT_BAD_LAYOUT;  /* no gaps or trailing bytes */
  info->size = (uint32_t)size;
  if (views != NULL) {
    const uint32_t wanted = (flags & GENESIS_SNAPSHOT_FLAG_SOUND) != 0U ? SEC_LAST : SEC_CART;
    for (i = 1U; i <= SEC_LAST; ++i)
      if (views[i].present != (i <= wanted)) return GENESIS_SNAPSHOT_UNKNOWN_SECTION;  /* a required section missing, or one the profile does not carry */
  }
  return GENESIS_SNAPSHOT_OK;
}

GenesisSnapshotStatus genesis_snapshot_inspect(const uint8_t *data, size_t size, GenesisSnapshotInfo *info) {
  SectionView views[SEC_LAST + 1U];
  return parse_container(data, size, info, views);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Restore: stage everything, validate everything, then commit with plain copies.                                                     */

/* Everything about a container that can be decided without staging a single device: structure, integrity, identity, region, machine profile and
 * execution fingerprint. Shared by restore and by the slot lists (which must tell a usable state from a foreign one without mutating anything). */
static GenesisSnapshotStatus precheck(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity, const uint8_t *data, size_t size,
                                      GenesisSnapshotInfo *info, SectionView views[SEC_LAST + 1U]) {
  GenesisSnapshotStatus status;
  uint8_t fp[32];
  uint16_t flags;
  if (machine == NULL || machine->runtime == NULL || identity == NULL || data == NULL) return GENESIS_SNAPSHOT_BAD_ARGUMENT;
  if ((machine->z80 == NULL) != (machine->audio == NULL)) return GENESIS_SNAPSHOT_BAD_ARGUMENT;
#ifndef SEGARECOMP_GENESIS_SOUND
  if (machine->z80 != NULL) return GENESIS_SNAPSHOT_BAD_ARGUMENT;
#endif
  status = parse_container(data, size, info, views);
  if (status != GENESIS_SNAPSHOT_OK) return status;
  if (memcmp(info->source_sha256, identity->source_sha256, 32U) != 0 || memcmp(info->variant_sha256, identity->variant_sha256, 32U) != 0)
    return GENESIS_SNAPSHOT_WRONG_CARTRIDGE;
  flags = genesis_snapshot_machine_flags(machine);
  exec_fingerprint(flags, fp);
  if (info->machine_flags != flags || info->region_profile != machine->runtime->region_profile || memcmp(info->exec_fingerprint, fp, 32U) != 0)
    return GENESIS_SNAPSHOT_INCOMPATIBLE;
  if (info->state_schema != GENESIS_SNAPSHOT_STATE_SCHEMA) return GENESIS_SNAPSHOT_INCOMPATIBLE;
  return GENESIS_SNAPSHOT_OK;
}

static GenesisSnapshotStatus restore_impl(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity, const uint8_t *data, size_t size, int commit);

GenesisSnapshotStatus genesis_snapshot_validate(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity, const uint8_t *data, size_t size,
                                                GenesisSnapshotInfo *info) {
  GenesisSnapshotInfo local;
  SectionView views[SEC_LAST + 1U];
  const GenesisSnapshotStatus st = precheck(machine, identity, data, size, info != NULL ? info : &local, views);
  if (st != GENESIS_SNAPSHOT_OK) return st;
  return restore_impl(machine, identity, data, size, 0);   /* the same staged validation restore runs, without committing */
}

static GenesisSnapshotStatus restore_impl(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity, const uint8_t *data, size_t size, int commit) {
  GenesisSnapshotInfo info;
  SectionView views[SEC_LAST + 1U];
  GenesisSnapshotStatus status;
  GenesisRuntime *stage_rt = NULL;
  uint8_t *stage_storage = NULL;
  uint16_t flags;
  Reader r;
#ifdef SEGARECOMP_GENESIS_SOUND
  GenesisZ80Machine *stage_z80 = NULL;
  GenesisAudio *stage_audio = NULL;
#endif

  status = precheck(machine, identity, data, size, &info, views);
  if (status != GENESIS_SNAPSHOT_OK) return status;
  flags = genesis_snapshot_machine_flags(machine);

  stage_rt = (GenesisRuntime *)malloc(sizeof *stage_rt);
  if (stage_rt == NULL) return GENESIS_SNAPSHOT_NO_MEMORY;
  memcpy(stage_rt, machine->runtime, sizeof *stage_rt);  /* shallow clone: every rebind/diagnostic field keeps its live value */
  if ((flags & GENESIS_SNAPSHOT_FLAG_SRAM) != 0U) {
    stage_storage = (uint8_t *)malloc(machine->runtime->cartridge_sram->storage_bytes);
    if (stage_storage == NULL) { free(stage_rt); return GENESIS_SNAPSHOT_NO_MEMORY; }
    memcpy(stage_storage, machine->runtime->cartridge_sram_storage, machine->runtime->cartridge_sram->storage_bytes);
  }
#ifdef SEGARECOMP_GENESIS_SOUND
  if (machine->z80 != NULL) {
    stage_z80 = (GenesisZ80Machine *)malloc(sizeof *stage_z80);
    stage_audio = (GenesisAudio *)malloc(sizeof *stage_audio);
    if (stage_z80 == NULL || stage_audio == NULL) { free(stage_z80); free(stage_audio); free(stage_storage); free(stage_rt); return GENESIS_SNAPSHOT_NO_MEMORY; }
    memcpy(stage_z80, machine->z80, sizeof *stage_z80);
    memcpy(stage_audio, machine->audio, sizeof *stage_audio);
  }
#endif

#define SECTION_READER(id) (r.p = views[id].data, r.size = views[id].length, r.off = 0U, r.error = 0)
  status = GENESIS_SNAPSHOT_INVALID_STATE;
  SECTION_READER(SEC_CPU);
  if (!get_cpu(&r, stage_rt)) goto done;
  if (views[SEC_RAM].length != 65536U) goto done;
  memcpy(stage_rt->work_ram, views[SEC_RAM].data, 65536U);
  SECTION_READER(SEC_VDP);
  if (!get_vdp(&r, &stage_rt->devices.vdp)) goto done;
  SECTION_READER(SEC_IO);
  if (!get_io(&r, stage_rt)) goto done;
  SECTION_READER(SEC_CART);
  if (!get_cart(&r, stage_rt, stage_storage, flags)) goto done;
  if (stage_rt->virtual_frame_count != info.guest_frame || stage_rt->scheduler.master_ticks != info.guest_master_ticks) { status = GENESIS_SNAPSHOT_BAD_LAYOUT; goto done; }
#ifdef SEGARECOMP_GENESIS_SOUND
  if (machine->z80 != NULL) {
    SECTION_READER(SEC_Z80);
    status = get_z80(&r, stage_z80);
    if (status != GENESIS_SNAPSHOT_OK) goto done;
    status = GENESIS_SNAPSHOT_INVALID_STATE;
    SECTION_READER(SEC_PSG);
    if (!get_psg(&r, stage_audio)) goto done;
    if (machine->audio->ym == NULL || ym2612_check(machine->audio->ym, views[SEC_YM].data, views[SEC_YM].length) != 0) goto done;
    SECTION_READER(SEC_MIXER);
    if (!get_mixer(&r, &stage_audio->mixer)) goto done;
    SECTION_READER(SEC_AUDIO);
    if (!get_audio_counters(&r, stage_audio)) goto done;
    if (stage_audio->ym_samples != 0U && stage_audio->ym_samples > UINT64_MAX / 2U) goto done;
  }
#endif
  {  /* Cross-component clocks. Every comparison is a division or a subtraction of already-bounded values, so no crafted extreme can overflow, and a gap between a component and the
        guest clock (which the next catch-up loop would have to run through) is bounded to a few frames. */
    const uint64_t master = stage_rt->scheduler.master_ticks;
#ifdef SEGARECOMP_GENESIS_SOUND
    const uint64_t slack = 4U * GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
#endif
    if (master > (UINT64_C(1) << 62)) goto done;
    if (stage_rt->raster.initialized) {   /* the scheduler's own invariant: next_line_tick is the start of the line after last_line, which is at most a few lines behind the clock */
      const uint64_t line_now = master / GENESIS_NTSC_MASTER_TICKS_PER_LINE;
      if (stage_rt->raster.last_line > line_now || line_now - stage_rt->raster.last_line > 4U ||
          stage_rt->raster.next_line_tick != (stage_rt->raster.last_line + 1U) * GENESIS_NTSC_MASTER_TICKS_PER_LINE)
        goto done;
    }
#ifdef SEGARECOMP_GENESIS_SOUND
    if (machine->z80 != NULL) {
      const uint64_t base = stage_z80->cycle_base_master_ticks, cycles = stage_z80->cpu.state.cycles;
      const uint64_t ym = stage_audio->ym_samples, psg = stage_audio->psg.ticks;
      const int runnable = stage_z80->bound_ordinal != 0U && stage_rt->devices.z80_bus.reset_released && !stage_rt->devices.z80_bus.bus_requested;
      if (base > master + slack || cycles > (master + slack - base) / GENESIS_Z80_CLOCK_DIVIDER) goto done;
      if (runnable && master > slack + base && cycles < (master - slack - base) / GENESIS_Z80_CLOCK_DIVIDER) goto done;
      if (ym > (master + slack) / YM2612_MASTER_TICKS_PER_SAMPLE || (master > slack && ym < (master - slack) / YM2612_MASTER_TICKS_PER_SAMPLE)) goto done;
      if (psg > (master + slack) / GENESIS_MIXER_PSG_TICK_MASTER || (master > slack && psg < (master - slack) / GENESIS_MIXER_PSG_TICK_MASTER)) goto done;
      {  /* the YM2612's own clock and every mixer source sit at the guest clock (within the slack) from BOTH sides: a far-behind clock would make the next catch-up run for ages */
        const uint8_t *ymb = views[SEC_YM].data;
        const uint64_t ym_now = le64(ymb), ym_chip_samples = le64(ymb + 40);
        const GenesisMixerSource *src[2];
        unsigned k;
        if (ym_chip_samples != ym || ym_now > (master + slack) / YM2612_MASTER_TICKS_PER_CLOCK || (master > slack && ym_now < (master - slack) / YM2612_MASTER_TICKS_PER_CLOCK)) goto done;
        src[0] = &stage_audio->mixer.ym;
        src[1] = &stage_audio->mixer.psg;
        for (k = 0U; k < 2U; ++k)
          if (src[k]->watermark > master + slack || src[k]->watermark + slack < master || src[k]->window_end > src[k]->watermark + slack || src[k]->window_end + slack < src[k]->watermark) goto done;
      }
    }
#endif
  }
#undef SECTION_READER

  if (!commit) { status = GENESIS_SNAPSHOT_OK; goto done; }   /* validation only: the staged copies are dropped, the machine is never touched */
  /* ---- commit: everything above passed; the copies below cannot fail ---- */
  memcpy(machine->runtime, stage_rt, sizeof *stage_rt);
  if (stage_storage != NULL) memcpy(machine->runtime->cartridge_sram_storage, stage_storage, machine->runtime->cartridge_sram->storage_bytes);
#ifdef SEGARECOMP_GENESIS_SOUND
  if (machine->z80 != NULL) {
    memcpy(machine->z80, stage_z80, sizeof *stage_z80);
    memcpy(machine->audio, stage_audio, sizeof *stage_audio);
    (void)ym2612_load(machine->audio->ym, views[SEC_YM].data, views[SEC_YM].length);
  }
#endif
  status = GENESIS_SNAPSHOT_OK;

done:
#ifdef SEGARECOMP_GENESIS_SOUND
  free(stage_z80);
  free(stage_audio);
#endif
  free(stage_storage);
  free(stage_rt);
  return status;
}

GenesisSnapshotStatus genesis_snapshot_restore(const GenesisSnapshotMachine *machine, const GenesisSnapshotIdentity *identity, const uint8_t *data, size_t size) {
  return restore_impl(machine, identity, data, size, 1);
}
