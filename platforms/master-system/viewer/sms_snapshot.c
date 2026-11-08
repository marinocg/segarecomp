#include "sms_snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "sv_state_codec.h"

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Format constants (docs/architecture/sms-sega-cd-save-states-inventory.md; the container conventions are the Genesis ones).         */

#define HEADER_FIXED_BYTES 156U
#define SECTION_ENTRY_BYTES 12U
#define DIGEST_BYTES 32U
#define PLATFORM_ID 2U   /* 1 = Genesis (SGSTATE), 2 = Master System */

enum { SEC_CPU = 1, SEC_MEM = 2, SEC_MACHINE = 3, SEC_VDP = 4, SEC_RENDER = 5, SEC_PSG = 6, SEC_PAD = 7, SEC_LAST = SEC_PAD };
#define SECTION_FLAG_REQUIRED 1U

/* Fixed per-section maxima: an allocation or read is never sized by a file length. */
#define CPU_BYTES 64U
#define MEM_BYTES (SMS_RAM_SIZE + SMS_CART_RAM_SIZE + SMS_MAPPER_REG_COUNT + 1U)
#define MACHINE_BYTES (1U + 8U + 1U + 3U + 1U + 4U + 32U)
#define VDP_BYTES (SMS_VDP_VRAM_SIZE + SMS_VDP_CRAM_SIZE + SMS_VDP_REGISTERS + 2U + 9U + 2U + 4U + 8U + 8U)
#define RENDER_BYTES (4U + 4U + 8U + 1U + 1U + 8U + 2U * SMS_FB_MAX_BYTES)
#define PSG_BYTES (SN76489_STATE_BYTES + 8U + 8U + 8U + 4U + 4U + 8U + 4U + 2U * SN76489_PCM_BATCH)
#define PAD_BYTES 5U
static const uint32_t k_section_len[SEC_LAST + 1] = {0U, CPU_BYTES, MEM_BYTES, MACHINE_BYTES, VDP_BYTES, RENDER_BYTES, PSG_BYTES, PAD_BYTES};

static const char k_exec_tag[] = "segarecomp.sms.smsstate.exec.v1;profile=sms2_ntsc_export";

#define CYCLES_LIMIT (UINT64_C(1) << 50)   /* ~ 10^15 T-states: any value above is not a state this machine can have reached */

const char *sms_snapshot_status_text(SmsSnapshotStatus status) {
  switch (status) {
    case SMS_SNAPSHOT_OK: return "ok";
    case SMS_SNAPSHOT_BAD_ARGUMENT: return "invalid request";
    case SMS_SNAPSHOT_NO_MEMORY: return "not enough memory";
    case SMS_SNAPSHOT_TOO_LARGE: return "the state file is too large";
    case SMS_SNAPSHOT_TRUNCATED: return "the state file is truncated";
    case SMS_SNAPSHOT_BAD_MAGIC: return "this is not a save state";
    case SMS_SNAPSHOT_UNSUPPORTED_VERSION: return "the state was written by an incompatible version";
    case SMS_SNAPSHOT_BAD_LAYOUT: return "the state file is damaged";
    case SMS_SNAPSHOT_UNKNOWN_SECTION: return "the state has unsupported or missing parts";
    case SMS_SNAPSHOT_INTEGRITY: return "the state file is corrupt";
    case SMS_SNAPSHOT_WRONG_CARTRIDGE: return "the state belongs to a different game or version";
    case SMS_SNAPSHOT_INCOMPATIBLE: return "the state does not match this game's mapper or build";
    case SMS_SNAPSHOT_INVALID_STATE: return "the state holds values the console cannot have";
    case SMS_SNAPSHOT_NOT_CAPTURABLE: return "the game is in a state that cannot be saved";
  }
  return "unknown error";
}

void sms_snapshot_buffer_free(SmsSnapshotBuffer *buffer) {
  if (buffer == NULL) return;
  free(buffer->data);
  buffer->data = NULL;
  buffer->size = 0U;
}

uint64_t sms_snapshot_cycles_to_ms(uint64_t cycles) {
  /* T = cycles x 11 / 39,375,000 s; split to stay overflow-free for any 64-bit count */
  const uint64_t whole = cycles / UINT64_C(39375000), rest = cycles % UINT64_C(39375000);
  return whole * 11U * 1000U + rest * 11U * 1000U / UINT64_C(39375000);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Execution fingerprint: the state schema and every size/constant the sections depend on, the mapper declaration and the PSG variant.  */

static void exec_fingerprint(const SmsSnapshotMachine *m, uint16_t flags, uint8_t out[32]) {
  SmsSha256 sha;
  const Sn76489Config *cfg = &m->psg->chip.config;
  const Sn76489PcmConfig *pcm = &m->psg->pcm.config;
  uint32_t values[24];
  unsigned i, n = 0;
  values[n++] = SMS_SNAPSHOT_STATE_SCHEMA;
  values[n++] = flags;
  values[n++] = (uint32_t)m->machine->mem.family;
  values[n++] = m->machine->mem.rom_size;
  values[n++] = SMS_RAM_SIZE;
  values[n++] = SMS_CART_RAM_SIZE;
  values[n++] = SMS_VDP_VRAM_SIZE;
  values[n++] = SMS_VDP_CRAM_SIZE;
  values[n++] = SMS_VDP_REGISTERS;
  values[n++] = SMS_FB_WIDTH;
  values[n++] = SMS_FB_MAX_HEIGHT;
  values[n++] = SN76489_STATE_BYTES;
  values[n++] = SN76489_PCM_BATCH;
  values[n++] = (uint32_t)SMS_CYCLES_PER_FRAME;
  values[n++] = cfg->noise_taps;
  values[n++] = cfg->lfsr_bits;
  values[n++] = cfg->divider;
  values[n++] = (uint32_t)pcm->rate_num;
  values[n++] = (uint32_t)(pcm->rate_num >> 32);
  values[n++] = (uint32_t)pcm->rate_den;
  values[n++] = (uint32_t)(pcm->rate_den >> 32);
  values[n++] = pcm->full_scale;
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, k_exec_tag, (uint32_t)(sizeof k_exec_tag - 1U));
  for (i = 0; i < n; ++i) sms_sha256_update_u32(&sha, values[i]);
  for (i = 0; i < 16U; ++i) sms_sha256_update_u16(&sha, cfg->levels != NULL ? cfg->levels[i] : 0U);
  sms_sha256_final(&sha, out);
}

static uint16_t flags_of(const SmsSnapshotMachine *m) { return m->script_input ? SMS_SNAPSHOT_FLAG_SCRIPT_INPUT : 0U; }

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Section codecs. Each `put_*` reads the machine; each `get_*` decodes into a STAGED copy only and validates.                        */

static void digest_input_prefix(const SmsMachine *m, uint32_t count, uint8_t out[32]) {
  SmsSha256 sha;
  uint32_t i;
  sms_sha256_init(&sha);
  for (i = 0; i < count; ++i) {
    sms_sha256_update_u64(&sha, m->input_events[i].frame);
    sms_sha256_update_u8(&sha, m->input_events[i].p1);
    sms_sha256_update_u8(&sha, m->input_events[i].p2);
    sms_sha256_update_u8(&sha, m->input_events[i].pause);
  }
  sms_sha256_final(&sha, out);
}

static void put_cpu(SvcWriter *w, const SmsMachine *m) {
  const Z80State *s = &m->rt.state;
  const uint8_t regs[16] = {s->a, s->f, s->b, s->c, s->d, s->e, s->h, s->l, s->a2, s->f2, s->b2, s->c2, s->d2, s->e2, s->h2, s->l2};
  svc_w_bytes(w, regs, sizeof regs);
  svc_w_u16(w, s->ix); svc_w_u16(w, s->iy); svc_w_u16(w, s->sp); svc_w_u16(w, s->pc); svc_w_u16(w, s->wz);
  svc_w_u8(w, s->i); svc_w_u8(w, s->r); svc_w_u8(w, s->im); svc_w_u8(w, s->iff1); svc_w_u8(w, s->iff2); svc_w_u8(w, s->q);
  svc_w_u8(w, s->halted); svc_w_u8(w, s->int_deferral); svc_w_u8(w, s->ld_a_ir); svc_w_u8(w, s->in_prefix_run);
  svc_w_u8(w, s->int_line); svc_w_u8(w, s->nmi_pending); svc_w_u8(w, s->nmi_reject);
  svc_w_u64(w, s->cycles);
  svc_w_u8(w, (uint8_t)m->rt.outcome);
}

static int get_cpu(SvcReader *r, SmsMachine *m) {
  Z80State *s = &m->rt.state;
  uint8_t regs[16], outcome;
  svc_r_bytes(r, regs, sizeof regs);
  s->a = regs[0]; s->f = regs[1]; s->b = regs[2]; s->c = regs[3]; s->d = regs[4]; s->e = regs[5]; s->h = regs[6]; s->l = regs[7];
  s->a2 = regs[8]; s->f2 = regs[9]; s->b2 = regs[10]; s->c2 = regs[11]; s->d2 = regs[12]; s->e2 = regs[13]; s->h2 = regs[14]; s->l2 = regs[15];
  s->ix = svc_r_u16(r); s->iy = svc_r_u16(r); s->sp = svc_r_u16(r); s->pc = svc_r_u16(r); s->wz = svc_r_u16(r);
  s->i = svc_r_u8(r); s->r = svc_r_u8(r); s->im = svc_r_u8(r); s->iff1 = svc_r_u8(r); s->iff2 = svc_r_u8(r); s->q = svc_r_u8(r);
  s->halted = svc_r_u8(r); s->int_deferral = svc_r_u8(r); s->ld_a_ir = svc_r_u8(r); s->in_prefix_run = svc_r_u8(r);
  s->int_line = svc_r_u8(r); s->nmi_pending = svc_r_u8(r); s->nmi_reject = svc_r_u8(r);
  s->cycles = svc_r_u64(r);
  outcome = svc_r_u8(r);
  m->rt.outcome = (Z80Outcome)outcome;
  if (!svc_r_done(r)) return 0;
  if (s->im > 2U || s->iff1 > 1U || s->iff2 > 1U || s->halted > 1U || s->int_deferral > 1U || s->ld_a_ir > 1U || s->in_prefix_run > 1U || s->int_line > 1U ||
      s->nmi_pending > 1U || s->nmi_reject > 1U)
    return 0;
  if (s->cycles >= CYCLES_LIMIT) return 0;
  return outcome == (uint8_t)Z80_OUTCOME_NONE || z80_outcome_is_resumable((Z80Outcome)outcome);   /* an error outcome is a latched stop: never a state */
}

static void put_mem(SvcWriter *w, const SmsMemory *mem) {
  svc_w_bytes(w, mem->ram, sizeof mem->ram);
  svc_w_bytes(w, mem->cart_ram, sizeof mem->cart_ram);
  svc_w_bytes(w, mem->mapper_regs, sizeof mem->mapper_regs);
  svc_w_u8(w, mem->memory_control);
}

static int get_mem(SvcReader *r, SmsMemory *mem) {
  svc_r_bytes(r, mem->ram, sizeof mem->ram);
  svc_r_bytes(r, mem->cart_ram, sizeof mem->cart_ram);
  svc_r_bytes(r, mem->mapper_regs, sizeof mem->mapper_regs);
  mem->memory_control = svc_r_u8(r);
  if (!svc_r_done(r)) return 0;
  mem->error = SMS_OK;
  mem->error_address = 0U; mem->error_value = 0U; mem->error_cycles = 0U;
  /* only values the guest can have written: port $3E accepts cartridge + RAM enabled with BIOS off; $FFFC rejects the overlay and bank-shift bits */
  if ((mem->memory_control & 0x58U) != 0x08U) return 0;
  if ((mem->mapper_regs[0] & (SMS_CTL_CART_RAM_OVERLAY | SMS_CTL_BANK_SHIFT_MASK)) != 0U) return 0;
  return 1;
}

static void put_machine(SvcWriter *w, const SmsMachine *m, int script) {
  uint8_t digest[32];
  svc_w_u8(w, m->io_control);
  svc_w_u64(w, m->next_line);
  svc_w_u8(w, m->irq_mask);
  svc_w_u8(w, m->input.p1); svc_w_u8(w, m->input.p2); svc_w_u8(w, m->input.pause);
  svc_w_u8(w, script ? 1U : 0U);
  svc_w_u32(w, script ? m->input_next : 0U);
  if (script) digest_input_prefix(m, m->input_next, digest); else memset(digest, 0, sizeof digest);
  svc_w_bytes(w, digest, 32U);
}

/* Decodes into the staged machine. `script`: the live machine's mode (the state's must equal it). */
static int get_machine(SvcReader *r, SmsMachine *stage, const SmsMachine *live, int script) {
  uint8_t digest[32], mode;
  uint32_t input_next;
  stage->io_control = svc_r_u8(r);
  stage->next_line = svc_r_u64(r);
  stage->irq_mask = svc_r_u8(r);
  stage->input.p1 = svc_r_u8(r); stage->input.p2 = svc_r_u8(r); stage->input.pause = svc_r_u8(r);
  mode = svc_r_u8(r);
  input_next = svc_r_u32(r);
  svc_r_bytes(r, digest, 32U);
  if (!svc_r_done(r)) return 0;
  if ((stage->irq_mask & ~(uint8_t)(SMS_IRQ_FRAME | SMS_IRQ_LINE)) != 0U || stage->input.p1 > 0x3FU || stage->input.p2 > 0x3FU || stage->input.pause > 1U) return 0;
  if (mode != (script ? 1U : 0U)) return 0;
  if (stage->next_line >= (CYCLES_LIMIT / SMS_CYCLES_PER_LINE)) return 0;
  if (script) {
    uint8_t expect[32];
    if (input_next > live->input_count || live->input_events == NULL) return 0;
    digest_input_prefix(live, input_next, expect);
    if (memcmp(expect, digest, 32U) != 0) return 0;   /* a different schedule consumed so far: the cursor means nothing here */
    stage->input_next = input_next;
  } else {
    if (input_next != 0U) return 0;
    stage->input_next = live->input_count;   /* recorded events of the abandoned timeline are not pending */
  }
  stage->dead = 0;
  memset(&stage->dead_stop, 0, sizeof stage->dead_stop);
  return 1;
}

static void put_vdp(SvcWriter *w, const SmsVdp *v) {
  svc_w_bytes(w, v->vram, sizeof v->vram);
  svc_w_bytes(w, v->cram, sizeof v->cram);
  svc_w_bytes(w, v->reg, sizeof v->reg);
  svc_w_u16(w, v->address);
  svc_w_u8(w, v->code); svc_w_u8(w, v->latch_set); svc_w_u8(w, v->latch_byte); svc_w_u8(w, v->read_buffer);
  svc_w_u8(w, v->frame_pending); svc_w_u8(w, v->sprite_overflow); svc_w_u8(w, v->sprite_collision); svc_w_u8(w, v->line_pending); svc_w_u8(w, v->line_counter);
  svc_w_u8(w, v->hcounter_latch); svc_w_u8(w, v->hcounter_valid);
  svc_w_u32(w, v->line);
  svc_w_u64(w, v->frame);
  svc_w_u32(w, v->vram_writes); svc_w_u32(w, v->cram_writes);
}

static int get_vdp(SvcReader *r, SmsVdp *v) {
  unsigned i;
  svc_r_bytes(r, v->vram, sizeof v->vram);
  svc_r_bytes(r, v->cram, sizeof v->cram);
  svc_r_bytes(r, v->reg, sizeof v->reg);
  v->address = svc_r_u16(r);
  v->code = svc_r_u8(r); v->latch_set = svc_r_u8(r); v->latch_byte = svc_r_u8(r); v->read_buffer = svc_r_u8(r);
  v->frame_pending = svc_r_u8(r); v->sprite_overflow = svc_r_u8(r); v->sprite_collision = svc_r_u8(r); v->line_pending = svc_r_u8(r); v->line_counter = svc_r_u8(r);
  v->hcounter_latch = svc_r_u8(r); v->hcounter_valid = svc_r_u8(r);
  v->line = svc_r_u32(r);
  v->frame = svc_r_u64(r);
  v->vram_writes = svc_r_u32(r); v->cram_writes = svc_r_u32(r);
  if (!svc_r_done(r)) return 0;
  if (v->address >= SMS_VDP_VRAM_SIZE || v->code > 3U || v->latch_set > 1U || v->frame_pending > 1U || v->sprite_overflow > 1U || v->sprite_collision > 1U ||
      v->line_pending > 1U || v->hcounter_valid > 1U || v->line >= SMS_LINES_PER_FRAME || v->frame >= CYCLES_LIMIT / SMS_CYCLES_PER_FRAME)
    return 0;
  for (i = 0; i < SMS_VDP_CRAM_SIZE; ++i)
    if ((v->cram[i] & 0xC0U) != 0U) return 0;   /* CRAM stores six bits */
  return 1;
}

static void put_render(SvcWriter *w, const SmsRenderer *r) {
  svc_w_u32(w, r->height); svc_w_u32(w, r->last_height);
  svc_w_u64(w, r->last_frame);
  svc_w_u8(w, r->vscroll); svc_w_u8(w, r->in_frame);
  svc_w_u64(w, r->frames_completed);
  svc_w_bytes(w, r->pixels, sizeof r->pixels);
  svc_w_bytes(w, r->last, sizeof r->last);
}

static int height_ok(uint32_t h) { return h == 0U || h == 192U || h == 224U; }

static int get_render(SvcReader *rd, SmsRenderer *r) {
  r->height = svc_r_u32(rd); r->last_height = svc_r_u32(rd);
  r->last_frame = svc_r_u64(rd);
  r->vscroll = svc_r_u8(rd); r->in_frame = svc_r_u8(rd);
  r->frames_completed = svc_r_u64(rd);
  svc_r_bytes(rd, r->pixels, sizeof r->pixels);
  svc_r_bytes(rd, r->last, sizeof r->last);
  if (!svc_r_done(rd)) return 0;
  if (!height_ok(r->height) || !height_ok(r->last_height) || r->in_frame > 1U || (r->in_frame == 1U && r->height == 0U)) return 0;
  if (r->frames_completed >= CYCLES_LIMIT / SMS_CYCLES_PER_FRAME || (r->frames_completed == 0U) != (r->last_height == 0U)) return 0;
  return 1;
}

static void put_psg(SvcWriter *w, const SmsPsg *p) {
  uint8_t bytes[SN76489_STATE_BYTES];
  uint32_t i;
  sn76489_state_bytes(&p->chip, bytes);
  svc_w_bytes(w, bytes, sizeof bytes);
  svc_w_u64(w, p->pcm.index); svc_w_u64(w, p->pcm.boundary); svc_w_u64(w, p->pcm.sum);
  svc_w_u32(w, p->pcm.count); svc_w_u32(w, p->pcm.last_mean);
  svc_w_u64(w, p->pcm.batch_first); svc_w_u32(w, p->pcm.batch_count);
  for (i = 0; i < p->pcm.batch_count; ++i) svc_w_u16(w, (uint16_t)p->pcm.batch[i]);
}

static int get_psg(SvcReader *r, SmsPsg *p, uint64_t cycles) {
  uint8_t bytes[SN76489_STATE_BYTES];
  uint32_t i;
  svc_r_bytes(r, bytes, sizeof bytes);
  p->pcm.index = svc_r_u64(r); p->pcm.boundary = svc_r_u64(r); p->pcm.sum = svc_r_u64(r);
  p->pcm.count = svc_r_u32(r); p->pcm.last_mean = svc_r_u32(r);
  p->pcm.batch_first = svc_r_u64(r); p->pcm.batch_count = svc_r_u32(r);
  if (r->error || p->pcm.batch_count > SN76489_PCM_BATCH) return 0;
  for (i = 0; i < p->pcm.batch_count; ++i) p->pcm.batch[i] = (int16_t)svc_r_u16(r);
  if (!svc_r_done(r)) return 0;
  if (sn76489_load_state_bytes(&p->chip, bytes) != 0) return 0;   /* paired API: validates every field and leaves the (staged) chip untouched on failure */
  if (p->pcm.index >= (UINT64_C(1) << 40) || p->pcm.count > 4U * SN76489_PCM_BATCH || p->pcm.last_mean > SN76489_FULL_SCALE || p->pcm.sum > (uint64_t)p->pcm.count * SN76489_FULL_SCALE) return 0;
  if (p->pcm.boundary != sn76489_pcm_sample_start(&p->pcm.config, p->pcm.index + 1U)) return 0;   /* T_(index+1): the sampling grid is a function of the index */
  if (p->chip.ticks > cycles / p->chip.config.divider + 1U) return 0;   /* the chip never runs ahead of the guest clock */
  return 1;
}

static void put_pad(SvcWriter *w, const SmsPad *p) { svc_w_u8(w, p->io_control); svc_w_u32(w, p->h_latches); }
static int get_pad(SvcReader *r, SmsPad *p) {
  p->io_control = svc_r_u8(r);
  p->h_latches = svc_r_u32(r);
  return svc_r_done(r);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Capture                                                                                                                            */

static int machine_ok(const SmsSnapshotMachine *m) {
  if (m == NULL || m->machine == NULL || m->vdp == NULL || m->renderer == NULL || m->psg == NULL || m->pad == NULL) return 0;
  return m->machine->vdp.port.context == m->vdp && m->machine->psg.context == m->psg && m->machine->pad.context == m->pad && m->renderer->vdp == m->vdp &&
         m->psg->machine == m->machine && m->pad->vdp == m->vdp && m->pad->input == &m->machine->input;
}

SmsSnapshotStatus sms_snapshot_capture(const SmsSnapshotMachine *machine, const SmsSnapshotIdentity *identity, const SmsSnapshotThumbnail *thumbnail,
                                       SmsSnapshotBuffer *out) {
  SvcWriter w;
  uint32_t sections[SEC_LAST], starts[SEC_LAST + 1U];
  uint32_t i, table_at, thumb_bytes = 0U;
  int script;
  uint16_t flags;
  uint8_t fp[32], digest[32];
  SmsSha256 sha;
  if (out != NULL) { out->data = NULL; out->size = 0U; }
  if (!machine_ok(machine) || identity == NULL || out == NULL) return SMS_SNAPSHOT_BAD_ARGUMENT;
  script = machine->script_input != 0;
  flags = flags_of(machine);
  if (machine->machine->dead || machine->machine->mem.error != SMS_OK || z80_outcome_is_error(machine->machine->rt.outcome)) return SMS_SNAPSHOT_NOT_CAPTURABLE;
  if (thumbnail != NULL && thumbnail->rgb565 != NULL) {
    if (thumbnail->width == 0U || thumbnail->height == 0U || thumbnail->width > SMS_SNAPSHOT_MAX_THUMB_W || thumbnail->height > SMS_SNAPSHOT_MAX_THUMB_H) return SMS_SNAPSHOT_BAD_ARGUMENT;
    thumb_bytes = (uint32_t)thumbnail->width * thumbnail->height * 2U;
  }
  exec_fingerprint(machine, flags, fp);
  for (i = 0; i < SEC_LAST; ++i) sections[i] = i + 1U;
  svc_w_init(&w, SMS_SNAPSHOT_MAX_FILE_BYTES);
  svc_w_bytes(&w, SMS_SNAPSHOT_MAGIC, 8U);
  svc_w_u16(&w, SMS_SNAPSHOT_CONTAINER_VERSION);
  svc_w_u16(&w, SMS_SNAPSHOT_STATE_SCHEMA);
  svc_w_u16(&w, flags);
  svc_w_u16(&w, PLATFORM_ID);
  svc_w_u32(&w, (uint32_t)machine->machine->mem.family);
  svc_w_u32(&w, HEADER_FIXED_BYTES);
  svc_w_u32(&w, 0U);                     /* total size: patched below */
  svc_w_u32(&w, SEC_LAST);
  svc_w_bytes(&w, identity->source_sha256, 32U);
  svc_w_bytes(&w, identity->variant_sha256, 32U);
  svc_w_bytes(&w, fp, 32U);
  svc_w_u64(&w, machine->machine->rt.state.cycles / SMS_CYCLES_PER_FRAME);
  svc_w_u64(&w, machine->machine->rt.state.cycles);
  svc_w_u16(&w, thumb_bytes != 0U ? thumbnail->width : 0U);
  svc_w_u16(&w, thumb_bytes != 0U ? thumbnail->height : 0U);
  svc_w_u32(&w, thumb_bytes);
  svc_w_u32(&w, 0U);
  table_at = w.size;
  for (i = 0; i < SEC_LAST * SECTION_ENTRY_BYTES; ++i) svc_w_u8(&w, 0U);
  if (thumb_bytes != 0U) svc_w_bytes(&w, thumbnail->rgb565, thumb_bytes);
  for (i = 0; i < SEC_LAST && !w.error; ++i) {
    starts[i] = w.size;
    switch (sections[i]) {
      case SEC_CPU: put_cpu(&w, machine->machine); break;
      case SEC_MEM: put_mem(&w, &machine->machine->mem); break;
      case SEC_MACHINE: put_machine(&w, machine->machine, script); break;
      case SEC_VDP: put_vdp(&w, machine->vdp); break;
      case SEC_RENDER: put_render(&w, machine->renderer); break;
      case SEC_PSG: put_psg(&w, machine->psg); break;
      case SEC_PAD: put_pad(&w, machine->pad); break;
      default: break;
    }
  }
  starts[SEC_LAST] = w.size;
  if (w.error) { free(w.data); return SMS_SNAPSHOT_NO_MEMORY; }
  for (i = 0; i < SEC_LAST; ++i) {
    const uint32_t length = starts[i + 1U] - starts[i];
    if (length > k_section_len[sections[i]]) { free(w.data); return SMS_SNAPSHOT_TOO_LARGE; }
    svc_put_le16(w.data + table_at + i * SECTION_ENTRY_BYTES, (uint16_t)sections[i]);
    svc_put_le16(w.data + table_at + i * SECTION_ENTRY_BYTES + 2U, (uint16_t)SECTION_FLAG_REQUIRED);
    svc_put_le32(w.data + table_at + i * SECTION_ENTRY_BYTES + 4U, starts[i]);
    svc_put_le32(w.data + table_at + i * SECTION_ENTRY_BYTES + 8U, length);
  }
  svc_w_reserve(&w, DIGEST_BYTES);
  if (w.error) { free(w.data); return SMS_SNAPSHOT_TOO_LARGE; }
  svc_put_le32(w.data + 24U, w.size + DIGEST_BYTES);
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, w.data, w.size);
  sms_sha256_final(&sha, digest);
  memcpy(w.data + w.size, digest, DIGEST_BYTES);
  w.size += DIGEST_BYTES;
  {
    uint8_t *fit = (uint8_t *)realloc(w.data, w.size);
    if (fit != NULL) w.data = fit;
  }
  out->data = w.data;
  out->size = w.size;
  return SMS_SNAPSHOT_OK;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Inspect (structure + integrity)                                                                                                    */

typedef struct SectionView {
  const uint8_t *data;
  uint32_t length;
  int present;
} SectionView;

static SmsSnapshotStatus parse_container(const uint8_t *data, size_t size, SmsSnapshotInfo *info, SectionView views[SEC_LAST + 1U]) {
  uint32_t total, count, i, thumb_bytes, cursor;
  uint16_t flags, platform;
  SmsSha256 sha;
  uint8_t digest[32];
  if (data == NULL || info == NULL) return SMS_SNAPSHOT_BAD_ARGUMENT;
  memset(info, 0, sizeof *info);
  if (size > SMS_SNAPSHOT_MAX_FILE_BYTES) return SMS_SNAPSHOT_TOO_LARGE;
  if (size < 8U) return SMS_SNAPSHOT_TRUNCATED;
  if (memcmp(data, SMS_SNAPSHOT_MAGIC, 8U) != 0) return SMS_SNAPSHOT_BAD_MAGIC;
  if (size < HEADER_FIXED_BYTES + DIGEST_BYTES) return SMS_SNAPSHOT_TRUNCATED;
  info->container_version = svc_le16(data + 8);
  info->state_schema = svc_le16(data + 10);
  if (info->container_version != SMS_SNAPSHOT_CONTAINER_VERSION || info->state_schema != SMS_SNAPSHOT_STATE_SCHEMA) return SMS_SNAPSHOT_UNSUPPORTED_VERSION;
  flags = svc_le16(data + 12);
  platform = svc_le16(data + 14);
  total = svc_le32(data + 24);
  count = svc_le32(data + 28);
  if (platform != PLATFORM_ID || svc_le32(data + 20) != HEADER_FIXED_BYTES || (flags & ~SMS_SNAPSHOT_FLAG_SCRIPT_INPUT) != 0U) return SMS_SNAPSHOT_BAD_LAYOUT;
  if (total != size) return total > size ? SMS_SNAPSHOT_TRUNCATED : SMS_SNAPSHOT_BAD_LAYOUT;
  if (count != SEC_LAST) return SMS_SNAPSHOT_BAD_LAYOUT;
  info->machine_flags = flags;
  info->mapper_family = svc_le32(data + 16);
  memcpy(info->source_sha256, data + 32, 32U);
  memcpy(info->variant_sha256, data + 64, 32U);
  memcpy(info->exec_fingerprint, data + 96, 32U);
  info->guest_frame = svc_le64(data + 128);
  info->guest_cycles = svc_le64(data + 136);
  info->thumb_width = svc_le16(data + 144);
  info->thumb_height = svc_le16(data + 146);
  thumb_bytes = svc_le32(data + 148);
  if (svc_le32(data + 152) != 0U) return SMS_SNAPSHOT_BAD_LAYOUT;
  if ((thumb_bytes == 0U) != (info->thumb_width == 0U && info->thumb_height == 0U) ||
      (thumb_bytes != 0U && (info->thumb_width > SMS_SNAPSHOT_MAX_THUMB_W || info->thumb_height > SMS_SNAPSHOT_MAX_THUMB_H ||
                             thumb_bytes != (uint32_t)info->thumb_width * (uint32_t)info->thumb_height * 2U)))
    return SMS_SNAPSHOT_BAD_LAYOUT;
  /* the digest first: nothing below trusts a byte of a file that is not intact */
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, data, size - DIGEST_BYTES);
  sms_sha256_final(&sha, digest);
  if (memcmp(digest, data + size - DIGEST_BYTES, DIGEST_BYTES) != 0) return SMS_SNAPSHOT_INTEGRITY;

  if (HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES + thumb_bytes + DIGEST_BYTES > size) return SMS_SNAPSHOT_TRUNCATED;
  cursor = HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES + thumb_bytes;
  if (thumb_bytes != 0U) info->thumb_rgb565 = data + HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES;
  memset(views, 0, sizeof(SectionView) * (SEC_LAST + 1U));
  for (i = 0U; i < count; ++i) {
    const uint8_t *entry = data + HEADER_FIXED_BYTES + i * SECTION_ENTRY_BYTES;
    const uint32_t id = svc_le16(entry), sflags = svc_le16(entry + 2), offset = svc_le32(entry + 4), length = svc_le32(entry + 8);
    if (id == 0U || id > SEC_LAST) return SMS_SNAPSHOT_UNKNOWN_SECTION;
    if ((sflags & SECTION_FLAG_REQUIRED) == 0U || (sflags & ~SECTION_FLAG_REQUIRED) != 0U) return SMS_SNAPSHOT_UNKNOWN_SECTION;
    if (views[id].present) return SMS_SNAPSHOT_UNKNOWN_SECTION;   /* duplicated */
    if (length > k_section_len[id]) return SMS_SNAPSHOT_BAD_LAYOUT;
    if (offset != cursor || length > size - DIGEST_BYTES - cursor) return SMS_SNAPSHOT_BAD_LAYOUT;   /* contiguous, in order, inside the file */
    views[id].data = data + offset;
    views[id].length = length;
    views[id].present = 1;
    cursor += length;
  }
  if (cursor != size - DIGEST_BYTES) return SMS_SNAPSHOT_BAD_LAYOUT;   /* no gaps or trailing bytes */
  for (i = 1U; i <= SEC_LAST; ++i)
    if (!views[i].present) return SMS_SNAPSHOT_UNKNOWN_SECTION;
  info->size = (uint32_t)size;
  return SMS_SNAPSHOT_OK;
}

SmsSnapshotStatus sms_snapshot_inspect(const uint8_t *data, size_t size, SmsSnapshotInfo *info) {
  SectionView views[SEC_LAST + 1U];
  return parse_container(data, size, info, views);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Restore: stage everything, validate everything, then commit with plain copies.                                                     */

typedef struct Stage {
  SmsMachine machine;
  SmsVdp vdp;
  SmsRenderer renderer;
  SmsPsg psg;
  SmsPad pad;
} Stage;

static SmsSnapshotStatus restore_impl(const SmsSnapshotMachine *machine, const SmsSnapshotIdentity *identity, const uint8_t *data, size_t size, int commit,
                                      SmsSnapshotInfo *info_out) {
  SmsSnapshotInfo info;
  SectionView views[SEC_LAST + 1U];
  SmsSnapshotStatus status;
  Stage *st;
  SvcReader r;
  uint8_t fp[32];
  uint16_t flags;
  int script;
  if (!machine_ok(machine) || identity == NULL || data == NULL) return SMS_SNAPSHOT_BAD_ARGUMENT;
  script = machine->script_input != 0;
  status = parse_container(data, size, &info, views);
  if (info_out != NULL) *info_out = info;
  if (status != SMS_SNAPSHOT_OK) return status;
  if (memcmp(info.source_sha256, identity->source_sha256, 32U) != 0 || memcmp(info.variant_sha256, identity->variant_sha256, 32U) != 0) return SMS_SNAPSHOT_WRONG_CARTRIDGE;
  flags = flags_of(machine);
  exec_fingerprint(machine, flags, fp);
  if (info.machine_flags != flags || info.mapper_family != (uint32_t)machine->machine->mem.family || memcmp(info.exec_fingerprint, fp, 32U) != 0) return SMS_SNAPSHOT_INCOMPATIBLE;

  st = (Stage *)malloc(sizeof *st);
  if (st == NULL) return SMS_SNAPSHOT_NO_MEMORY;
  memcpy(&st->machine, machine->machine, sizeof st->machine);   /* shallow clones: every rebind/diagnostic field keeps its live value */
  memcpy(&st->vdp, machine->vdp, sizeof st->vdp);
  memcpy(&st->renderer, machine->renderer, sizeof st->renderer);
  memcpy(&st->psg, machine->psg, sizeof st->psg);
  memcpy(&st->pad, machine->pad, sizeof st->pad);

#define SECTION_READER(id) svc_r_init(&r, views[id].data, views[id].length)
  status = SMS_SNAPSHOT_INVALID_STATE;
  SECTION_READER(SEC_CPU);     if (!get_cpu(&r, &st->machine)) goto done;
  SECTION_READER(SEC_MEM);     if (!get_mem(&r, &st->machine.mem)) goto done;
  SECTION_READER(SEC_MACHINE); if (!get_machine(&r, &st->machine, machine->machine, script)) goto done;
  SECTION_READER(SEC_VDP);     if (!get_vdp(&r, &st->vdp)) goto done;
  SECTION_READER(SEC_RENDER);  if (!get_render(&r, &st->renderer)) goto done;
  SECTION_READER(SEC_PSG);     if (!get_psg(&r, &st->psg, st->machine.rt.state.cycles)) goto done;
  SECTION_READER(SEC_PAD);     if (!get_pad(&r, &st->pad)) goto done;
#undef SECTION_READER
  {  /* cross-component invariants: the scheduler position, the VDP's last event, the interrupt level and the header must tell one story */
    const SmsMachine *m = &st->machine;
    const uint64_t cycles = m->rt.state.cycles;
    if (info.guest_cycles != cycles || info.guest_frame != cycles / SMS_CYCLES_PER_FRAME) { status = SMS_SNAPSHOT_BAD_LAYOUT; goto done; }
    if (m->rt.state.int_line != (m->irq_mask != 0U ? 1U : 0U)) goto done;
    if (m->next_line != 0U) {
      const uint64_t last = m->next_line - 1U;
      if (last * SMS_CYCLES_PER_LINE > cycles) goto done;                           /* an event was applied that lies in the future */
      if (m->next_line * SMS_CYCLES_PER_LINE > cycles + SMS_CYCLES_PER_LINE) goto done;   /* an event overdue by more than the barrier allows */
      if (st->vdp.line != (uint32_t)(last % SMS_LINES_PER_FRAME) || st->vdp.frame != last / SMS_LINES_PER_FRAME) goto done;
    } else if (cycles >= SMS_CYCLES_PER_LINE) {
      goto done;
    }
    if (st->renderer.last_frame > st->vdp.frame) goto done;
    if (st->pad.io_control != m->io_control) goto done;                               /* the machine's mirror and the pad's agree by construction */
  }
  if (!commit) { status = SMS_SNAPSHOT_OK; goto done; }   /* validation only: the staged copies are dropped, the machine is never touched */
  /* ---- commit: everything above passed; the copies below cannot fail ---- */
  memcpy(machine->machine, &st->machine, sizeof st->machine);
  memcpy(machine->vdp, &st->vdp, sizeof st->vdp);
  memcpy(machine->renderer, &st->renderer, sizeof st->renderer);
  memcpy(machine->psg, &st->psg, sizeof st->psg);
  memcpy(machine->pad, &st->pad, sizeof st->pad);
  status = SMS_SNAPSHOT_OK;
done:
  free(st);
  return status;
}

SmsSnapshotStatus sms_snapshot_validate(const SmsSnapshotMachine *machine, const SmsSnapshotIdentity *identity, const uint8_t *data, size_t size,
                                        SmsSnapshotInfo *info) {
  SmsSnapshotInfo local;
  return restore_impl(machine, identity, data, size, 0, info != NULL ? info : &local);
}

SmsSnapshotStatus sms_snapshot_restore(const SmsSnapshotMachine *machine, const SmsSnapshotIdentity *identity, const uint8_t *data, size_t size) {
  return restore_impl(machine, identity, data, size, 1, NULL);
}
