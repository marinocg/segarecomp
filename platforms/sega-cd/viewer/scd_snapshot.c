#include "scd_snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "checkpoint_evidence.h"
#include "runtime.h"
#include "sv_state_codec.h"
#ifdef SEGARECOMP_GENESIS_SOUND
#include "genesis_audio.h"
#include "genesis_snapshot.h"
#include "segarecomp/device/sega/ym2612/ym2612.h"
#include "z80_machine.h"
#else
#include "genesis_snapshot.h"
#endif

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Format constants (docs/architecture/sms-sega-cd-save-states-inventory.md; the container conventions are the Genesis and SMS ones).  */

#define HEADER_FIXED_BYTES 156U
#define SECTION_ENTRY_BYTES 12U
#define DIGEST_BYTES 32U
#define PLATFORM_ID 3U   /* 1 = Genesis (SGSTATE), 2 = Master System (SMSSTAT), 3 = Sega CD (SCDSTAT) */

enum {
  SEC_CPU = 1, SEC_CORE, SEC_PRG, SEC_WORD, SEC_WORK, SEC_GATE, SEC_CDC, SEC_CDD, SEC_PCM, SEC_GFX, SEC_AUDIO, SEC_BACKUP, SEC_INPUT, SEC_OVERLAY, SEC_DYNAMIC, SEC_LEDGER, SEC_HOST,
  SEC_Z80, SEC_LAST = SEC_Z80
};
#define SECTION_FLAG_REQUIRED 1U

#define FLAG_BACKUP 0x0001U    /* backup RAM installed */
#define FLAG_INPUT 0x0002U     /* a live input is installed */
#define FLAG_OVERLAYS 0x0004U
#define FLAG_DYNAMIC 0x0008U
#define FLAG_Z80 0x0010U       /* the main-side Z80 is attached (with its sound devices) */
#define FLAG_MASK (FLAG_BACKUP | FLAG_INPUT | FLAG_OVERLAYS | FLAG_DYNAMIC | FLAG_Z80)

/* Fixed per-section maxima: an allocation or read is never sized by a file length. */
#define OVERLAY_ENTRY_BYTES (4U + 4U + 1U + 1U + 4U + 4U + SCD_OVERLAY_MAX_GRANULES / 8U)
#define DYN_BIND_BYTES (1U + 4U * 7U + 8U + 1U + 2U * (SCD_DYN_GRANULES / 8U + 2U))
#define RASTER_BYTES (GENESIS_RASTER_MAX_LINES * (GENESIS_VDP_REGISTER_COUNT * 2U + GENESIS_VDP_CRAM_BYTES + GENESIS_VDP_VSRAM_BYTES))
static const uint32_t k_section_max[SEC_LAST + 1] = {
  0U, 512U, 1024U, SCD_PRG_RAM_BYTES, SCD_WORD_RAM_BYTES, SCD_WORK_RAM_BYTES, 1024U, SCD_CDC_BUFFER_BYTES + 2352U + 256U, 256U, 66000U, 256U,
  80000U, SCD_BACKUP_RAM_BYTES + 16U, 128U, 4U + SCD_OVERLAY_MAX_GROUPS * OVERLAY_ENTRY_BYTES, 16U + 2U * SCD_DYN_MAX_BINDINGS * DYN_BIND_BYTES, 8U + SCD_LEDGER_MAX * 8U,
  GENESIS_SNAPSHOT_PART_VDP_MAX + GENESIS_SNAPSHOT_PART_IO_MAX + 512U + RASTER_BYTES + GENESIS_VDP_VRAM_BYTES + 64U, 110000U};

static const char k_exec_tag[] = "segarecomp.scd.scdstate.exec.v1";
static const char k_disc_tag[] = "segarecomp.scd.disc.v1";
static const char k_bios_tag[] = "segarecomp.scd.bios.v1";

#define TICKS_LIMIT (UINT64_C(1) << 60)
#define SCD_RING_LAG 2U   /* output frames the YM2612/PSG part trails in scd_z80.c (SCD_Z80_FM_LAG): the oldest ring frame the mix still needs is (mix frames - lag) */
#define FRAME_TICKS (scd_frame_ticks())

const char *scd_snapshot_status_text(ScdSnapshotStatus status) {
  switch (status) {
    case SCD_SNAPSHOT_OK: return "ok";
    case SCD_SNAPSHOT_BAD_ARGUMENT: return "invalid request";
    case SCD_SNAPSHOT_NO_MEMORY: return "not enough memory";
    case SCD_SNAPSHOT_TOO_LARGE: return "the state file is too large";
    case SCD_SNAPSHOT_TRUNCATED: return "the state file is truncated";
    case SCD_SNAPSHOT_BAD_MAGIC: return "this is not a save state";
    case SCD_SNAPSHOT_UNSUPPORTED_VERSION: return "the state was written by an incompatible version";
    case SCD_SNAPSHOT_BAD_LAYOUT: return "the state file is damaged";
    case SCD_SNAPSHOT_UNKNOWN_SECTION: return "the state has unsupported or missing parts";
    case SCD_SNAPSHOT_INTEGRITY: return "the state file is corrupt";
    case SCD_SNAPSHOT_WRONG_DISC: return "the state belongs to a different disc or BIOS";
    case SCD_SNAPSHOT_INCOMPATIBLE: return "the state does not match this build of the game";
    case SCD_SNAPSHOT_INVALID_STATE: return "the state holds values the console cannot have";
    case SCD_SNAPSHOT_IMAGE_MISSING: return "the state needs sound code this build does not contain";
    case SCD_SNAPSHOT_NOT_CAPTURABLE: return "the game is in a state that cannot be saved";
  }
  return "unknown error";
}

void scd_snapshot_buffer_free(ScdSnapshotBuffer *buffer) {
  if (buffer == NULL) return;
  free(buffer->data);
  buffer->data = NULL;
  buffer->size = 0U;
}

uint64_t scd_snapshot_ticks_to_ms(uint64_t ticks) {
  const uint64_t per_ms = SCD_TICKS_PER_SECOND / 1000U;
  return ticks / per_ms;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Capturability and identity                                                                                                         */

static void z80_view(ScdMachine *m, ScdZ80SnapshotView *view, int *have) {
  const ScdZ80Ops *ops = scd_z80_registered_ops();
  *have = 0;
  memset(view, 0, sizeof *view);
  if (m->z80 == NULL) return;
  *have = 1;
  if (ops == NULL || ops->snapshot_parts == NULL || ops->snapshot_parts(m, view) != 0) view->refuse = "the main-side Z80 cannot be saved by this build";
}

const char *scd_snapshot_refusal(const ScdMachine *machine) {
  ScdZ80SnapshotView view;
  int have;
  ScdMachine *m = (ScdMachine *)machine;   /* z80_view only reads */
  void *host, *raster, *line0;
  size_t host_bytes, raster_bytes, line0_bytes;
  ScdVdpHostFields fields;
  if (machine == NULL) return "no machine";
  if (m->bios == NULL || m->authentic == NULL) return "only the BIOS-image profile can be saved";
  if (m->program == NULL || m->program->disc.identity == NULL || m->authentic->bios_sha256 == NULL) return "the disc or BIOS has no verified identity";
  if (m->stop.stop_class != SCD_STOP_NONE) return "the machine is in a typed stop";
  if (m->materialize || m->verify_only || m->capture_kind != 0U || m->reloc != NULL) return "a build pass is running";
  if (m->access_trace != NULL || m->write_log != NULL || m->pcm_trace != NULL || m->event_log != NULL || m->coverage != NULL) return "a diagnostic observer is attached";
  if (m->sub_wait_request != 0U) return "the machine is inside an instruction";
  if (m->input != NULL && (!m->input->live || m->input->recording || m->input->next != m->input->count)) return "input is scripted or being recorded";
  if (scd_vdp_host_access(m, &host, &host_bytes, &raster, &raster_bytes, &line0, &line0_bytes, &fields) != 0) return "the video device host is missing";
  if (fields.capture_count != 0U) return "a diagnostic frame capture is requested";
  if (fields.dma_armed != 0U) return "the machine is inside a video transfer";
  z80_view(m, &view, &have);
  if (have) {
    if (view.refuse != NULL) return view.refuse;
#ifdef SEGARECOMP_GENESIS_SOUND
    if (view.audio == NULL) return "the main-side Z80 is attached without its sound devices";
#else
    return "this build saves a machine without the main-side Z80 only";
#endif
  }
  return NULL;
}

static void sha_str(const char *tag, const char *text, uint8_t out[32]) {
  GenesisSha256 sha;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, (const uint8_t *)tag, (uint32_t)strlen(tag) + 1U);
  genesis_sha256_update(&sha, (const uint8_t *)text, (uint32_t)strlen(text));
  genesis_sha256_final(&sha, out);
}

static void u32_update(GenesisSha256 *sha, uint32_t v) {
  uint8_t b[4];
  svc_put_le32(b, v);
  genesis_sha256_update(sha, b, 4U);
}
static void str_update(GenesisSha256 *sha, const char *text) {
  const char *t = text != NULL ? text : "";
  u32_update(sha, (uint32_t)strlen(t));
  genesis_sha256_update(sha, (const uint8_t *)t, (uint32_t)strlen(t));
}

static uint16_t machine_flags(const ScdMachine *m) {
  uint16_t flags = 0U;
  if (m->backup_ram_present) flags |= FLAG_BACKUP;
  if (m->input != NULL) flags |= FLAG_INPUT;
  if (m->overlays != NULL) flags |= FLAG_OVERLAYS;
  if (m->dyn.enabled) flags |= FLAG_DYNAMIC;
  if (m->z80 != NULL) flags |= FLAG_Z80;
  return flags;
}

/* The executable-image contract: what the generated code was built to execute, independent of build directories, compiler, generator version and compiled-entry counts. */
static void exec_fingerprint(const ScdMachine *m, uint16_t flags, uint8_t out[32]) {
  GenesisSha256 sha;
  const ScdAuthentic *a = m->authentic;
  uint32_t i, k;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, (const uint8_t *)k_exec_tag, (uint32_t)(sizeof k_exec_tag - 1U));
  u32_update(&sha, SCD_SNAPSHOT_STATE_SCHEMA);
  u32_update(&sha, flags);
  u32_update(&sha, SCD_RUNTIME_ABI);
  u32_update(&sha, SCD_PRG_RAM_BYTES); u32_update(&sha, SCD_WORD_RAM_BYTES); u32_update(&sha, SCD_WORK_RAM_BYTES); u32_update(&sha, SCD_CDC_BUFFER_BYTES);
  u32_update(&sha, SCD_BACKUP_RAM_BYTES); u32_update(&sha, GENESIS_VDP_VRAM_BYTES); u32_update(&sha, GENESIS_VDP_REGISTER_COUNT);
  u32_update(&sha, SCD_MIX_QUEUE_FRAMES); u32_update(&sha, SCD_DYN_MAX_BINDINGS); u32_update(&sha, SCD_OVERLAY_MAX_GROUPS);
  u32_update(&sha, m->bios_size);
  u32_update(&sha, a->ip_entry);
  u32_update(&sha, a->sub_image_count);
  for (i = 0U; i < a->sub_image_count; ++i) { u32_update(&sha, a->sub_images[i].length); str_update(&sha, a->sub_images[i].sha256); }
  for (k = 0U; k < 2U; ++k) {
    const ScdProcessorProgram *p = &m->program->processors[k];
    u32_update(&sha, p->entry_pc); u32_update(&sha, p->image_base); u32_update(&sha, p->image_disc_offset); u32_update(&sha, p->image_length);
  }
  u32_update(&sha, m->program->main_initial_ssp);
  if (m->overlays != NULL) {
    str_update(&sha, m->overlays->manifest_identity);
    u32_update(&sha, m->overlays->group_count);
    for (i = 0U; i < m->overlays->group_count; ++i) {
      const ScdOverlayGroup *g = &m->overlays->groups[i];
      u32_update(&sha, g->cpu); u32_update(&sha, g->start); u32_update(&sha, g->end); u32_update(&sha, g->candidate_count);
    }
  }
  if (m->dyn.enabled) {
    const ScdDynamicProgram *d = scd_dynamic_program(m);
    u32_update(&sha, d != NULL ? d->image_count : 0U);
    for (i = 0U; d != NULL && i < d->image_count; ++i) { u32_update(&sha, d->images[i].id); u32_update(&sha, d->images[i].first_lba); u32_update(&sha, d->images[i].length); str_update(&sha, d->images[i].sha256); }
  }
  genesis_sha256_final(&sha, out);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Section codecs. Each `put_*` reads the machine; each `get_*` decodes into the STAGED copy only and validates.                       */

#define PU8(v) svc_w_u8(w, (uint8_t)(v))
#define PU16(v) svc_w_u16(w, (uint16_t)(v))
#define PU32(v) svc_w_u32(w, (uint32_t)(v))
#define PU64(v) svc_w_u64(w, (uint64_t)(v))
#define GU8(v) ((v) = svc_r_u8(r))
#define GU16(v) ((v) = svc_r_u16(r))
#define GU32(v) ((v) = svc_r_u32(r))
#define GU64(v) ((v) = svc_r_u64(r))
#define GI32(v) ((v) = (int32_t)svc_r_u32(r))
#define GI64(v) ((v) = (int64_t)svc_r_u64(r))
#define REQ(cond) do { if (!(cond)) return 0; } while (0)

static uint64_t absdiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }
static int near_tick(uint64_t value, uint64_t now, uint64_t frames) { return absdiff(value, now) <= frames * FRAME_TICKS; }
/* An instant the machine only ever COMPARES with its clock and never catches up to (a deadline that has passed stays in the register until the next one replaces it): only the future is bounded. */
static int not_far_ahead(uint64_t value, uint64_t now, uint64_t frames) { return value <= now + frames * FRAME_TICKS; }

static void put_cpu(SvcWriter *w, const ScdMachine *m) {
  unsigned k, i;
  for (k = 0U; k < 2U; ++k) {
    const ScdCpu *c = &m->cpu[k];
    for (i = 0U; i < 8U; ++i) PU32(c->d[i]);
    for (i = 0U; i < 8U; ++i) PU32(c->a[i]);
    PU32(c->usp); PU32(c->pc); PU16(c->sr);
    PU8(c->irq.sampled_level); PU8(c->irq.level7_edge_pending); PU8(c->irq.stopped);
    PU64(c->next); PU64(c->retired); PU64(c->cycles); PU64(c->trace); PU32(c->prefetch);
  }
}

static int get_cpu(SvcReader *r, ScdMachine *s, const ScdMachine *live) {
  unsigned k, i;
  for (k = 0U; k < 2U; ++k) {
    ScdCpu *c = &s->cpu[k];
    for (i = 0U; i < 8U; ++i) GU32(c->d[i]);
    for (i = 0U; i < 8U; ++i) GU32(c->a[i]);
    GU32(c->usp); GU32(c->pc); GU16(c->sr);
    GU8(c->irq.sampled_level); GU8(c->irq.level7_edge_pending); GU8(c->irq.stopped);
    GU64(c->next); GU64(c->retired); GU64(c->cycles); GU64(c->trace); GU32(c->prefetch);
    if (r->error) return 0;
    c->cycle_ticks = live->cpu[k].cycle_ticks;   /* a function of the profile, not of the file */
    REQ(c->irq.sampled_level <= 7U && c->irq.level7_edge_pending <= 1U && c->irq.stopped <= 1U);
    REQ(c->next < TICKS_LIMIT && c->retired < TICKS_LIMIT && c->cycles < TICKS_LIMIT);
    /* the program counter is any 32-bit value the generated code produced; a target the program has no compiled entry for is a typed stop, never a guess */
  }
  return svc_r_done(r);
}

static void put_core(SvcWriter *w, const ScdMachine *m) {
  PU64(m->now); PU64(m->vdp_onset); PU64(m->vdp_frames); PU64(m->device_log);
  PU8(m->sub_pristine); PU32(m->epochs); PU32(m->epoch_image); PU64(m->epoch_tick);
  PU32(m->main_refresh_hold); PU32(m->main_irq_delay); PU64(m->main_extra_ticks); PU32(m->refresh_skip_cycles); PU64(m->main_refresh_due);
  PU64(m->vint_at); PU8(m->vint_pending); PU8(m->hint_counter); PU8(m->hint_pending); PU64(m->hint_next); PU64(m->hint_at); PU16(m->hint_vector);
  PU8(m->z80_ran);
  PU64(m->main_dma_until); PU8(m->main_dma_multi); PU64(m->main_dma_bank_until);
  PU8(m->sub_wait_request);
}

static int get_core(SvcReader *r, ScdMachine *s) {
  uint32_t epoch_image;
  GU64(s->now); GU64(s->vdp_onset); GU64(s->vdp_frames); GU64(s->device_log);
  GU8(s->sub_pristine); GU32(s->epochs); GU32(epoch_image); GU64(s->epoch_tick);
  s->epoch_image = (int32_t)epoch_image;
  GU32(s->main_refresh_hold); GU32(s->main_irq_delay); GU64(s->main_extra_ticks); GU32(s->refresh_skip_cycles); GU64(s->main_refresh_due);
  GU64(s->vint_at); GU8(s->vint_pending); GU8(s->hint_counter); GU8(s->hint_pending); GU64(s->hint_next); GU64(s->hint_at); GU16(s->hint_vector);
  GU8(s->z80_ran);
  GU64(s->main_dma_until); GU8(s->main_dma_multi); GU64(s->main_dma_bank_until);
  GU8(s->sub_wait_request);
  REQ(svc_r_done(r));
  REQ(s->now < TICKS_LIMIT && s->vdp_onset < TICKS_LIMIT && s->vdp_frames < TICKS_LIMIT / FRAME_TICKS);
  REQ(s->sub_pristine <= 1U && s->vint_pending <= 1U && s->hint_pending <= 1U && s->z80_ran <= 1U && s->main_dma_multi <= 1U && s->sub_wait_request == 0U);
  REQ(near_tick(s->vdp_onset, s->now, 4U));
  REQ(s->epoch_image >= -1 && (s->epoch_image < 0 || (uint32_t)s->epoch_image < s->authentic->sub_image_count));
  REQ(s->epoch_tick <= s->now);
  REQ(s->main_extra_ticks < FRAME_TICKS && s->main_irq_delay < 64U && s->main_refresh_hold <= 1U);
  REQ(not_far_ahead(s->vint_at, s->now, 4U));
  REQ(not_far_ahead(s->hint_at, s->now, 4U));
  REQ(not_far_ahead(s->main_dma_until, s->now, 4U));
  REQ(not_far_ahead(s->main_dma_bank_until, s->now, 4U));
  REQ(near_tick(s->main_refresh_due, s->now, 2U));
  REQ(s->hint_next < TICKS_LIMIT / SCD_LINE_TICKS_MACHINE);
  return 1;
}

static void put_gate(SvcWriter *w, const ScdMachine *m) {
  const ScdGateArray *g = &m->ga;
  unsigned i;
  PU8(g->ctl); PU8(g->sub_halted); PU8(g->sub_wait); PU8(g->prg_window); PU8(g->ifl2); PU8(g->ien2_mirror); PU8(g->led); PU8(g->write_protect); PU8(g->prg_bank); PU8(g->mode);
  PU8(g->dmna_int); PU8(g->main_flag); PU8(g->sub_flag);
  for (i = 0U; i < 8U; ++i) PU16(g->comcmd[i]);
  for (i = 0U; i < 8U; ++i) PU16(g->comstat[i]);
  PU8(g->interrupt_enable); PU8(g->reg32_hi); PU8(g->pending); PU8(g->timer_reload); PU64(g->timer_next); PU8(g->cdd_ctrl_hi); PU8(g->cdd_ctrl_lo);
  svc_w_bytes(w, g->scratch, sizeof g->scratch);
  PU64(g->stopwatch_origin); PU32(g->stopwatch_base);
}

static int get_gate(SvcReader *r, ScdMachine *s) {
  ScdGateArray *g = &s->ga;
  unsigned i;
  GU8(g->ctl); GU8(g->sub_halted); GU8(g->sub_wait); GU8(g->prg_window); GU8(g->ifl2); GU8(g->ien2_mirror); GU8(g->led); GU8(g->write_protect); GU8(g->prg_bank); GU8(g->mode);
  GU8(g->dmna_int); GU8(g->main_flag); GU8(g->sub_flag);
  for (i = 0U; i < 8U; ++i) GU16(g->comcmd[i]);
  for (i = 0U; i < 8U; ++i) GU16(g->comstat[i]);
  GU8(g->interrupt_enable); GU8(g->reg32_hi); GU8(g->pending); GU8(g->timer_reload); GU64(g->timer_next); GU8(g->cdd_ctrl_hi); GU8(g->cdd_ctrl_lo);
  svc_r_bytes(r, g->scratch, sizeof g->scratch);
  GU64(g->stopwatch_origin); GU32(g->stopwatch_base);
  REQ(svc_r_done(r));
  REQ(g->sub_halted <= 1U && g->sub_wait <= 1U && g->prg_window <= 1U && g->ifl2 <= 1U && g->dmna_int <= 1U && g->prg_bank <= 3U && g->mode <= 0x1FU);
  REQ(g->timer_next == 0U || near_tick(g->timer_next, s->now, 8U));
  REQ(g->stopwatch_origin <= s->now && g->stopwatch_base < 0x1000U);
  return 1;
}

static void put_cdc(SvcWriter *w, const ScdMachine *m) {
  const ScdCdc *c = &m->cdc;
  unsigned i, j;
  PU8(c->address); PU8(c->destination); PU8(c->edt); PU8(c->dsr); PU16(c->dma_address); PU8(c->ifstat); PU8(c->ifctrl); PU8(c->ctrl0); PU8(c->ctrl1); PU8(c->irq);
  PU16(c->dbc); PU16(c->dac); PU16(c->pt); PU16(c->wa);
  for (i = 0U; i < 2U; ++i) for (j = 0U; j < 4U; ++j) PU8(c->head[i][j]);
  PU8(c->stat0); PU8(c->stat2); PU8(c->stat3); PU64(c->decoder_end); PU16(c->host_word);
  PU8(c->dma_state); PU8(c->dma_target); PU64(c->dma_base);
  svc_w_bytes(w, c->buffer, sizeof c->buffer);
}

static int get_cdc(SvcReader *r, ScdMachine *s) {
  ScdCdc *c = &s->cdc;
  unsigned i, j;
  GU8(c->address); GU8(c->destination); GU8(c->edt); GU8(c->dsr); GU16(c->dma_address); GU8(c->ifstat); GU8(c->ifctrl); GU8(c->ctrl0); GU8(c->ctrl1); GU8(c->irq);
  GU16(c->dbc); GU16(c->dac); GU16(c->pt); GU16(c->wa);
  for (i = 0U; i < 2U; ++i) for (j = 0U; j < 4U; ++j) GU8(c->head[i][j]);
  GU8(c->stat0); GU8(c->stat2); GU8(c->stat3); GU64(c->decoder_end); GU16(c->host_word);
  GU8(c->dma_state); GU8(c->dma_target); GU64(c->dma_base);
  svc_r_bytes(r, c->buffer, sizeof c->buffer);
  REQ(svc_r_done(r));
  REQ(c->dma_state <= SCD_DMA_HALTED && c->dma_target <= SCD_DMA_TARGET_BANK1 && c->edt <= 1U && c->dsr <= 1U && c->destination <= 7U);
  REQ(c->decoder_end < TICKS_LIMIT && c->dma_base <= s->now + FRAME_TICKS * 4U);
  return 1;
}

static void put_cdd(SvcWriter *w, const ScdMachine *m) {
  const ScdCdd *d = &m->cdd;
  svc_w_bytes(w, d->tx, sizeof d->tx);
  svc_w_bytes(w, d->rx, sizeof d->rx);
  PU8(d->status); PU8(d->pending); PU32(d->latency); PU32(d->lba); PU32(d->index); PU64(d->anchor); PU64(d->grid_line0); PU64(d->grid_acc0); PU64(d->next_tick);
}

static int get_cdd(SvcReader *r, ScdMachine *s) {
  ScdCdd *d = &s->cdd;
  svc_r_bytes(r, d->tx, sizeof d->tx);
  svc_r_bytes(r, d->rx, sizeof d->rx);
  GU8(d->status); GU8(d->pending); GU32(d->latency);
  GI32(d->lba); GU32(d->index); GU64(d->anchor); GU64(d->grid_line0); GI64(d->grid_acc0); GU64(d->next_tick);
  REQ(svc_r_done(r));
  REQ(s->disc != NULL && d->index <= s->disc->track_count);
  REQ(d->lba >= -(int32_t)(1 << 20) && d->lba < (int32_t)(1 << 22));
  REQ(d->latency < TICKS_LIMIT / 2U && d->anchor <= s->now && d->grid_line0 < TICKS_LIMIT / SCD_LINE_TICKS_MACHINE);
  REQ(d->next_tick == 0U || near_tick(d->next_tick, s->now, 8U));
  return 1;
}

static void put_pcm(SvcWriter *w, const ScdMachine *m) {
  const ScdPcm *p = &m->pcm;
  unsigned i;
  svc_w_bytes(w, p->env, 8U); svc_w_bytes(w, p->pan, 8U); svc_w_bytes(w, p->st, 8U);
  for (i = 0U; i < 8U; ++i) PU16(p->fd[i]);
  for (i = 0U; i < 8U; ++i) PU16(p->ls[i]);
  PU8(p->index); PU8(p->bank); PU8(p->enabled); PU8(p->onoff); PU8(p->running);
  for (i = 0U; i < 8U; ++i) PU32(p->addr[i]);
  PU64(p->origin); PU64(p->samples);
  svc_w_bytes(w, p->ram, sizeof p->ram);
}

static int get_pcm(SvcReader *r, ScdMachine *s) {
  ScdPcm *p = &s->pcm;
  unsigned i;
  svc_r_bytes(r, p->env, 8U); svc_r_bytes(r, p->pan, 8U); svc_r_bytes(r, p->st, 8U);
  for (i = 0U; i < 8U; ++i) GU16(p->fd[i]);
  for (i = 0U; i < 8U; ++i) GU16(p->ls[i]);
  GU8(p->index); GU8(p->bank); GU8(p->enabled); GU8(p->onoff); GU8(p->running);
  for (i = 0U; i < 8U; ++i) GU32(p->addr[i]);
  GU64(p->origin); GU64(p->samples);
  svc_r_bytes(r, p->ram, sizeof p->ram);
  REQ(svc_r_done(r));
  REQ(p->index < 8U && p->bank < 16U && p->enabled <= 1U);
  for (i = 0U; i < 8U; ++i) REQ(p->addr[i] < (1U << 27));
  REQ(p->origin <= s->now && p->samples < TICKS_LIMIT / 2048U);
  return 1;
}

static void put_gfx(SvcWriter *w, const ScdMachine *m) {
  const ScdGfx *g = &m->gfx;
  PU8(g->busy); PU8(g->ctl);
  PU16(g->map_base); PU16(g->cell_v); PU16(g->buffer_origin); PU16(g->offset); PU16(g->h_dots); PU16(g->trace_base);
  PU32(g->trace_ptr); PU32(g->map_ptr); PU32(g->dot_mask); PU32(g->buffer_offset); PU32(g->buffer_start);
  PU8(g->stamp_shift); PU8(g->map_shift); PU8(g->lines_left); PU64(g->base); PU64(g->line_ticks);
}

static int get_gfx(SvcReader *r, ScdMachine *s) {
  ScdGfx *g = &s->gfx;
  GU8(g->busy); GU8(g->ctl);
  GU16(g->map_base); GU16(g->cell_v); GU16(g->buffer_origin); GU16(g->offset); GU16(g->h_dots); GU16(g->trace_base);
  GU32(g->trace_ptr); GU32(g->map_ptr); GU32(g->dot_mask); GU32(g->buffer_offset); GU32(g->buffer_start);
  GU8(g->stamp_shift); GU8(g->map_shift); GU8(g->lines_left); GU64(g->base); GU64(g->line_ticks);
  REQ(svc_r_done(r));
  REQ(g->busy <= 1U && g->stamp_shift <= 32U && g->map_shift <= 32U);
  REQ(g->base < TICKS_LIMIT && g->line_ticks < FRAME_TICKS * 4U && (g->busy == 0U || g->base <= s->now + FRAME_TICKS * 4U));
  return 1;
}

static void put_audio(SvcWriter *w, const ScdMachine *m) {
  const ScdCdda *c = &m->cdda;
  const ScdMix *x = &m->mix;
  uint32_t i;
  PU8(c->playing); PU32(c->next_lba); PU32(c->last_index); PU16(c->fader_register); PU16(c->fader_target); PU16(c->fader_volume);
  PU64(x->tick); PU32(x->rem); PU64(x->frames); PU16(x->held[0]); PU16(x->held[1]);
  PU8(x->cd_active); PU32(x->cd_next); PU32(x->cd_end); PU16(x->fader_volume);
  PU32(x->q_count);
  for (i = 0U; i < x->q_count; ++i) {   /* canonical: oldest first; the head is normalized to zero on restore */
    const uint32_t at = (x->q_head + i) % SCD_MIX_QUEUE_FRAMES;
    PU16(x->queue[2U * at]); PU16(x->queue[2U * at + 1U]);
  }
  PU32(x->chunk_count);
  for (i = 0U; i < 2U * x->chunk_count; ++i) PU16(x->chunk[i]);
}

static int get_audio(SvcReader *r, ScdMachine *s) {
  ScdCdda *c = &s->cdda;
  ScdMix *x = &s->mix;
  uint32_t i, held0, held1;
  GU8(c->playing); GI32(c->next_lba); GU32(c->last_index); GU16(c->fader_register); GU16(c->fader_target); GU16(c->fader_volume);
  GU64(x->tick); GU32(x->rem); GU64(x->frames); held0 = svc_r_u16(r); held1 = svc_r_u16(r);
  GU8(x->cd_active); GI32(x->cd_next); GI32(x->cd_end); GU16(x->fader_volume);
  GU32(x->q_count);
  REQ(!r->error && x->q_count <= SCD_MIX_QUEUE_FRAMES);
  x->q_head = 0U;
  for (i = 0U; i < x->q_count; ++i) { x->queue[2U * i] = (int16_t)svc_r_u16(r); x->queue[2U * i + 1U] = (int16_t)svc_r_u16(r); }
  GU32(x->chunk_count);
  REQ(!r->error && x->chunk_count <= SCD_MIX_CHUNK_FRAMES);
  for (i = 0U; i < 2U * x->chunk_count; ++i) x->chunk[i] = (int16_t)svc_r_u16(r);
  REQ(svc_r_done(r));
  x->held[0] = (int16_t)held0; x->held[1] = (int16_t)held1;
  REQ(c->playing <= 1U && x->cd_active <= 1U && c->fader_target <= 1024U && c->fader_volume <= 1024U && x->fader_volume <= 1024U);
  REQ(c->last_index <= (s->disc != NULL ? s->disc->track_count : 0U));
  if (x->enabled) {
    /* 44.1 kHz frame n is at tick floor(n x TPS / 44100): the clock, its exact remainder and the frame count must tell one story, and the clock sits at guest time within a frame or two */
    const uint64_t whole = x->frames / 44100U, part = x->frames % 44100U;
    REQ(x->frames < TICKS_LIMIT / SCD_TICKS_PER_SECOND * 44100U);
    REQ(x->tick == whole * SCD_TICKS_PER_SECOND + part * SCD_TICKS_PER_SECOND / 44100U && x->rem == (uint32_t)((part * SCD_TICKS_PER_SECOND) % 44100U));
    REQ(near_tick(x->tick, s->now, 3U));
  }
  return 1;
}

static void put_backup(SvcWriter *w, const ScdMachine *m) {
  PU8(m->backup_ram_present);
  if (m->backup_ram_present) svc_w_bytes(w, m->backup_ram, SCD_BACKUP_RAM_BYTES);
}

static int get_backup(SvcReader *r, ScdMachine *s, const ScdMachine *live) {
  uint8_t present;
  GU8(present);
  REQ(!r->error && present == (live->backup_ram_present ? 1U : 0U));
  if (present) svc_r_bytes(r, s->backup_ram, SCD_BACKUP_RAM_BYTES);   /* only the in-memory bytes: no file is read or written */
  return svc_r_done(r);
}

static void put_input(SvcWriter *w, const ScdMachine *m) {
  if (m->input == NULL) { PU8(0); return; }
  PU8(1);
  PU8(m->input->pad[0]); PU8(m->input->pad[1]); PU8(m->input->live_pad[0]); PU8(m->input->live_pad[1]); PU64(m->input->live_last_tick);
}

static int get_input(SvcReader *r, ScdMachine *s, ScdInput *stage_input, const ScdMachine *live) {
  uint8_t present;
  GU8(present);
  REQ(!r->error && present == (live->input != NULL ? 1U : 0U));
  if (present) {
    GU8(stage_input->pad[0]); GU8(stage_input->pad[1]); GU8(stage_input->live_pad[0]); GU8(stage_input->live_pad[1]); GU64(stage_input->live_last_tick);
    stage_input->count = 0U;   /* every sampled event was applied before the barrier; none of the abandoned timeline is pending */
    stage_input->next = 0U;
    REQ(stage_input->live_last_tick <= s->now);
  }
  return svc_r_done(r);
}

static void put_overlay(SvcWriter *w, const ScdMachine *m) {
  uint32_t g;
  const uint32_t count = m->overlays != NULL ? m->overlays->group_count : 0U;
  PU32(count);
  for (g = 0U; g < count; ++g) {
    const ScdOverlayState *o = &m->overlay[g];
    PU32(o->selected); PU32(o->stale_from); PU8(o->all_dirty); PU8(o->any_dirty); PU32(o->dirty_lo); PU32(o->dirty_hi);
    svc_w_bytes(w, o->dirty, sizeof o->dirty);
  }
}

static int get_overlay(SvcReader *r, ScdMachine *s) {
  uint32_t g, count;
  GU32(count);
  REQ(!r->error && count == (s->overlays != NULL ? s->overlays->group_count : 0U) && count <= SCD_OVERLAY_MAX_GROUPS);
  for (g = 0U; g < count; ++g) {
    ScdOverlayState *o = &s->overlay[g];
    const ScdOverlayGroup *grp = &s->overlays->groups[g];
    GI32(o->selected); GI32(o->stale_from); GU8(o->all_dirty); GU8(o->any_dirty); GU32(o->dirty_lo); GU32(o->dirty_hi);
    svc_r_bytes(r, o->dirty, sizeof o->dirty);
    REQ(!r->error);
    REQ(o->selected >= -1 && o->selected < (int32_t)grp->candidate_count && o->stale_from >= -1 && o->stale_from < (int32_t)grp->candidate_count);
    REQ(o->all_dirty <= 1U && o->any_dirty <= 1U && o->dirty_lo <= SCD_OVERLAY_MAX_GRANULES && o->dirty_hi <= SCD_OVERLAY_MAX_GRANULES);
  }
  return svc_r_done(r);
}

static void put_dynamic(SvcWriter *w, const ScdMachine *m) {
  unsigned k, j;
  PU8(m->dyn.enabled); PU32(m->dyn.cpu); PU64(m->dyn.order);
  for (k = 0U; k < 2U; ++k)
    for (j = 0U; j < SCD_DYN_MAX_BINDINGS; ++j) {
      const ScdDynamicBinding *b = &m->dyn.binding[k][j];
      PU8(b->active);
      if (!b->active) continue;
      PU32(b->image); PU32(b->base); PU32(b->lo); PU32(b->hi); PU32(b->run_lo); PU32(b->run_hi); PU32(b->granule_first);
      PU64(b->order); PU8(b->stale);
      svc_w_bytes(w, b->dirty, sizeof b->dirty);
      svc_w_bytes(w, b->mutated, sizeof b->mutated);
    }
}

static int get_dynamic(SvcReader *r, ScdMachine *s) {
  unsigned k, j;
  uint8_t enabled;
  const ScdDynamicProgram *p = scd_dynamic_program(s);
  GU8(enabled);
  REQ(!r->error && enabled == (s->dyn.enabled ? 1U : 0U));
  GU32(s->dyn.cpu); GU64(s->dyn.order);
  REQ(!r->error && s->dyn.cpu <= 1U);
  for (k = 0U; k < 2U; ++k)
    for (j = 0U; j < SCD_DYN_MAX_BINDINGS; ++j) {
      ScdDynamicBinding *b = &s->dyn.binding[k][j];
      memset(b, 0, sizeof *b);
      GU8(b->active);
      REQ(!r->error && b->active <= 1U);
      if (!b->active) continue;
      REQ(enabled && p != NULL);
      GU32(b->image); GU32(b->base); GU32(b->lo); GU32(b->hi); GU32(b->run_lo); GU32(b->run_hi); GU32(b->granule_first);
      GU64(b->order); GU8(b->stale);
      svc_r_bytes(r, b->dirty, sizeof b->dirty);
      svc_r_bytes(r, b->mutated, sizeof b->mutated);
      REQ(!r->error && b->image < p->image_count && b->lo < b->hi && b->hi <= SCD_DYN_MAX_IMAGE_BYTES * 2U && b->run_lo <= b->run_hi && b->stale <= 1U);
      REQ(b->granule_first == (b->lo >> SCD_DYN_GRANULE_SHIFT) && b->order <= s->dyn.order);
    }
  memset(s->dyn.cache_valid, 0, sizeof s->dyn.cache_valid);   /* the sector comparison cache is rebuilt from the disc on demand */
  return svc_r_done(r);
}

static void put_ledger(SvcWriter *w, const ScdMachine *m) {
  uint32_t i;
  PU32(m->delivered_count); PU8(m->delivered_overflow);
  for (i = 0U; i < m->delivered_count; ++i) { PU32(m->delivered[i].first_lba); PU32(m->delivered[i].count); }
}

static int get_ledger(SvcReader *r, ScdMachine *s) {
  uint32_t i;
  GU32(s->delivered_count); GU8(s->delivered_overflow);
  REQ(!r->error && s->delivered_count <= SCD_LEDGER_MAX && s->delivered_overflow <= 1U);
  for (i = 0U; i < s->delivered_count; ++i) { GU32(s->delivered[i].first_lba); GU32(s->delivered[i].count); }
  return svc_r_done(r);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Main-side Genesis device host (VDP, I/O, Z80 bus) and the frame in progress.                                                         */

static int put_host(SvcWriter *w, ScdMachine *m) {
  void *host, *raster, *line0;
  size_t host_bytes, raster_bytes, line0_bytes;
  ScdVdpHostFields f;
  static uint8_t part[GENESIS_SNAPSHOT_PART_VDP_MAX];
  uint32_t n, i;
  const GenesisRuntime *rt;
  const GenesisRasterFrameState *rs;
  if (scd_vdp_host_access(m, &host, &host_bytes, &raster, &raster_bytes, &line0, &line0_bytes, &f) != 0) return 0;
  rt = (const GenesisRuntime *)host;
  rs = (const GenesisRasterFrameState *)raster;
  n = genesis_snapshot_part_vdp_put(rt, part, sizeof part);
  if (n == 0U) return 0;
  PU32(n);
  svc_w_bytes(w, part, n);
  n = genesis_snapshot_part_io_put(rt, part, sizeof part);
  if (n == 0U) return 0;
  PU32(n);
  svc_w_bytes(w, part, n);
  PU8(rt->pad1); PU8(rt->pad2);
  for (i = 0U; i < 4U; ++i) PU32(f.fifo_cycles[i]);
  PU32(f.fifo_idx); PU64(f.fill_end); PU32(f.capture_line); PU32(f.capture_lines_done); PU64(f.capture_line_tick); PU8(f.present);
  if (rs->has_vram_journal != 0U || rs->vram_write_count != 0U) return 0;
  for (i = 0U; i < GENESIS_RASTER_MAX_LINES; ++i) {
    unsigned j;
    for (j = 0U; j < GENESIS_VDP_REGISTER_COUNT; ++j) PU16(rs->lines[i].registers[j]);
    svc_w_bytes(w, rs->lines[i].cram, GENESIS_VDP_CRAM_BYTES);
    svc_w_bytes(w, rs->lines[i].vsram, GENESIS_VDP_VSRAM_BYTES);
  }
  svc_w_bytes(w, line0, GENESIS_VDP_VRAM_BYTES);
  return 1;
}

typedef struct HostStage {
  GenesisRuntime *rt;
  GenesisRasterFrameState *raster;
  uint8_t *line0;
  ScdVdpHostFields fields;
} HostStage;

static int get_host(SvcReader *r, ScdMachine *live, const ScdMachine *s, HostStage *st) {
  void *host, *raster, *line0;
  size_t host_bytes, raster_bytes, line0_bytes;
  ScdVdpHostFields f;
  uint32_t n, i;
  if (scd_vdp_host_access(live, &host, &host_bytes, &raster, &raster_bytes, &line0, &line0_bytes, &f) != 0) return 0;
  st->rt = (GenesisRuntime *)malloc(host_bytes);
  st->raster = (GenesisRasterFrameState *)malloc(raster_bytes);
  st->line0 = (uint8_t *)malloc(line0_bytes);
  if (st->rt == NULL || st->raster == NULL || st->line0 == NULL) return -1;
  memcpy(st->rt, host, host_bytes);        /* shallow clones: every pointer keeps its live value */
  memcpy(st->raster, raster, raster_bytes);
  memcpy(st->line0, line0, line0_bytes);
  st->fields = f;
  n = svc_r_u32(r);
  REQ(!r->error && n <= GENESIS_SNAPSHOT_PART_VDP_MAX);
  {
    const uint8_t *p = svc_r_take(r, n);
    REQ(p != NULL && genesis_snapshot_part_vdp_get(st->rt, p, n));
  }
  n = svc_r_u32(r);
  REQ(!r->error && n <= GENESIS_SNAPSHOT_PART_IO_MAX);
  {
    const uint8_t *p = svc_r_take(r, n);
    REQ(p != NULL && genesis_snapshot_part_io_get(st->rt, p, n));
  }
  st->rt->pad1 = svc_r_u8(r); st->rt->pad2 = svc_r_u8(r);
  for (i = 0U; i < 4U; ++i) st->fields.fifo_cycles[i] = svc_r_u32(r);
  st->fields.fifo_idx = svc_r_u32(r); st->fields.fill_end = svc_r_u64(r); st->fields.capture_line = (int32_t)svc_r_u32(r); st->fields.capture_lines_done = svc_r_u32(r);
  st->fields.capture_line_tick = svc_r_u64(r); { const uint8_t present = svc_r_u8(r); REQ(present == f.present); }
  for (i = 0U; i < GENESIS_RASTER_MAX_LINES; ++i) {
    unsigned j;
    for (j = 0U; j < GENESIS_VDP_REGISTER_COUNT; ++j) st->raster->lines[i].registers[j] = svc_r_u16(r);
    svc_r_bytes(r, st->raster->lines[i].cram, GENESIS_VDP_CRAM_BYTES);
    svc_r_bytes(r, st->raster->lines[i].vsram, GENESIS_VDP_VSRAM_BYTES);
  }
  st->raster->has_vram_journal = 0U;
  st->raster->vram_write_count = 0U;
  svc_r_bytes(r, st->line0, GENESIS_VDP_VRAM_BYTES);
  REQ(svc_r_done(r));
  REQ(st->fields.fifo_idx < 4U && st->fields.capture_line >= -1 && st->fields.capture_line <= (int32_t)GENESIS_RASTER_MAX_LINES && st->fields.capture_lines_done <= GENESIS_RASTER_MAX_LINES);
  REQ(not_far_ahead(st->fields.fill_end, s->now, 4U));
  REQ(st->fields.capture_line < 0 || near_tick(st->fields.capture_line_tick, s->now, 4U));
  REQ(st->fields.capture_line >= 0 || st->fields.capture_lines_done <= GENESIS_RASTER_MAX_LINES);
  st->fields.dma_armed = 0U;
  return 1;
}

static void host_stage_free(HostStage *st) {
  free(st->rt);
  free(st->raster);
  free(st->line0);
  memset(st, 0, sizeof *st);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* The attached Z80, YM2612, PSG and mixer through the Genesis section codecs.                                                          */

#ifdef SEGARECOMP_GENESIS_SOUND
static int put_z80(SvcWriter *w, ScdMachine *m, ScdSnapshotStatus *status) {
  ScdZ80SnapshotView v;
  int have;
  static uint8_t part[GENESIS_SNAPSHOT_PART_MIXER_MAX];
  uint32_t n, ym_bytes;
  GenesisSnapshotStatus gs;
  GenesisAudio *audio;
  z80_view(m, &v, &have);
  if (!have) { PU8(0); return 1; }
  if (v.refuse != NULL || v.audio == NULL) { *status = SCD_SNAPSHOT_NOT_CAPTURABLE; return 0; }
  audio = (GenesisAudio *)v.audio;
  PU8(1);
  n = genesis_snapshot_part_z80_put((const GenesisZ80Machine *)v.machine, part, sizeof part, &gs);
  if (n == 0U) { *status = SCD_SNAPSHOT_NOT_CAPTURABLE; return 0; }
  PU32(n); svc_w_bytes(w, part, n);
  n = genesis_snapshot_part_psg_put(audio, part, sizeof part);
  if (n == 0U) { *status = SCD_SNAPSHOT_NOT_CAPTURABLE; return 0; }
  PU32(n); svc_w_bytes(w, part, n);
  ym_bytes = audio->ym == NULL ? 0U : ym2612_snapshot_bytes(audio->ym);
  if (ym_bytes == 0U || ym_bytes > YM2612_SNAPSHOT_MAX_BYTES) { *status = SCD_SNAPSHOT_NOT_CAPTURABLE; return 0; }
  svc_w_reserve(w, ym_bytes + 4U);
  if (w->error) return 0;
  PU32(ym_bytes);
  if (ym2612_save(audio->ym, w->data + w->size, ym_bytes) != 0) { *status = SCD_SNAPSHOT_NOT_CAPTURABLE; return 0; }
  w->size += ym_bytes;
  n = genesis_snapshot_part_mixer_put(audio, part, sizeof part, &gs);
  if (n == 0U) { *status = SCD_SNAPSHOT_NOT_CAPTURABLE; return 0; }
  PU32(n); svc_w_bytes(w, part, n);
  n = genesis_snapshot_part_audio_put(audio, part, sizeof part);
  if (n == 0U) { *status = SCD_SNAPSHOT_NOT_CAPTURABLE; return 0; }
  PU32(n); svc_w_bytes(w, part, n);
  PU64(*v.audio_synced); PU8(*v.audio_stopped);
  {  /* The Sega CD mix reads the FM/PSG frames from the mixer's output ring by ABSOLUTE frame index (scd_z80_audio_frame: frame n needs ring[n - lag]). The Genesis codec drops the ring on
      * restore because there it is only host playback; here the frames not yet consumed are guest-timeline state and travel with the file. */
    const GenesisMixer *mixer = &audio->mixer;
    const uint64_t lo = m->mix.frames >= SCD_RING_LAG ? m->mix.frames - SCD_RING_LAG : 0U;
    uint32_t count = mixer->ring_write > lo ? (uint32_t)(mixer->ring_write - lo > GENESIS_MIXER_RING_FRAMES ? GENESIS_MIXER_RING_FRAMES : mixer->ring_write - lo) : 0U;
    uint32_t i;
    PU64(mixer->ring_write); PU32(count);
    for (i = 0U; i < count; ++i) {
      const uint64_t at = (mixer->ring_write - count + i) % GENESIS_MIXER_RING_FRAMES;
      PU16(mixer->ring[at][0]); PU16(mixer->ring[at][1]);
    }
  }
  return 1;
}

typedef struct Z80Stage {
  GenesisZ80Machine *machine;
  GenesisAudio *audio;
  const uint8_t *ym;
  uint32_t ym_length;
  uint64_t audio_synced;
} Z80Stage;

static const uint8_t *take_part(SvcReader *r, uint32_t max, uint32_t *length) {
  *length = svc_r_u32(r);
  if (r->error || *length > max) { r->error = 1; return NULL; }
  return svc_r_take(r, *length);
}

static ScdSnapshotStatus get_z80(SvcReader *r, ScdMachine *live, const ScdMachine *s, const GenesisRuntime *stage_rt, Z80Stage *st) {
  ScdZ80SnapshotView v;
  int have;
  uint8_t present;
  uint32_t n;
  const uint8_t *p;
  z80_view(live, &v, &have);
  present = svc_r_u8(r);
  if (r->error || present != (have ? 1U : 0U)) return SCD_SNAPSHOT_INCOMPATIBLE;
  if (!have) return svc_r_done(r) ? SCD_SNAPSHOT_OK : SCD_SNAPSHOT_INVALID_STATE;
  if (v.refuse != NULL || v.audio == NULL) return SCD_SNAPSHOT_INCOMPATIBLE;
  st->machine = (GenesisZ80Machine *)malloc(sizeof *st->machine);
  st->audio = (GenesisAudio *)malloc(sizeof *st->audio);
  if (st->machine == NULL || st->audio == NULL) return SCD_SNAPSHOT_NO_MEMORY;
  memcpy(st->machine, v.machine, sizeof *st->machine);
  memcpy(st->audio, v.audio, sizeof *st->audio);
  p = take_part(r, GENESIS_SNAPSHOT_PART_Z80_MAX, &n);
  if (p == NULL) return SCD_SNAPSHOT_INVALID_STATE;
  {
    const GenesisSnapshotStatus gs = genesis_snapshot_part_z80_get(st->machine, p, n);
    if (gs == GENESIS_SNAPSHOT_IMAGE_MISSING) return SCD_SNAPSHOT_IMAGE_MISSING;
    if (gs != GENESIS_SNAPSHOT_OK) return SCD_SNAPSHOT_INVALID_STATE;
  }
  p = take_part(r, GENESIS_SNAPSHOT_PART_PSG_MAX, &n);
  if (p == NULL || !genesis_snapshot_part_psg_get(st->audio, p, n)) return SCD_SNAPSHOT_INVALID_STATE;
  st->ym = take_part(r, YM2612_SNAPSHOT_MAX_BYTES, &st->ym_length);
  if (st->ym == NULL || st->audio->ym == NULL || ym2612_check(st->audio->ym, st->ym, st->ym_length) != 0) return SCD_SNAPSHOT_INVALID_STATE;
  p = take_part(r, GENESIS_SNAPSHOT_PART_MIXER_MAX, &n);
  if (p == NULL || !genesis_snapshot_part_mixer_get(st->audio, p, n)) return SCD_SNAPSHOT_INVALID_STATE;
  p = take_part(r, GENESIS_SNAPSHOT_PART_AUDIO_MAX, &n);
  if (p == NULL || !genesis_snapshot_part_audio_get(st->audio, p, n)) return SCD_SNAPSHOT_INVALID_STATE;
  st->audio_synced = svc_r_u64(r);
  { const uint8_t stopped = svc_r_u8(r); if (stopped != 0U) return SCD_SNAPSHOT_INVALID_STATE; }
  {
    GenesisMixer *mixer = &st->audio->mixer;
    const uint64_t write = svc_r_u64(r);
    const uint32_t count = svc_r_u32(r);
    uint32_t i;
    const uint64_t lo = s->mix.frames >= SCD_RING_LAG ? s->mix.frames - SCD_RING_LAG : 0U;
    if (r->error || count > GENESIS_MIXER_RING_FRAMES || write >= (UINT64_C(1) << 50) || count > write) return SCD_SNAPSHOT_INVALID_STATE;
    if (count != (write > lo ? (write - lo > GENESIS_MIXER_RING_FRAMES ? GENESIS_MIXER_RING_FRAMES : write - lo) : 0U)) return SCD_SNAPSHOT_INVALID_STATE;
    for (i = 0U; i < count; ++i) {
      const uint64_t at = (write - count + i) % GENESIS_MIXER_RING_FRAMES;
      mixer->ring[at][0] = (int16_t)svc_r_u16(r);
      mixer->ring[at][1] = (int16_t)svc_r_u16(r);
    }
    mixer->ring_write = write;
    mixer->ring_read = write;
  }
  if (!svc_r_done(r)) return SCD_SNAPSHOT_INVALID_STATE;
  /* the Z80 and the sound devices follow the main bus clock within a few frames from BOTH sides: a crafted far-behind clock would make the next catch-up run for ages */
  {
    const uint64_t master = stage_rt->scheduler.master_ticks;
    const uint64_t slack = 4U * GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
    const uint64_t base = st->machine->cycle_base_master_ticks, cycles = st->machine->cpu.state.cycles;
    const uint64_t ym = st->audio->ym_samples, psg = st->audio->psg.ticks;
    const int runnable = st->machine->bound_ordinal != 0U && stage_rt->devices.z80_bus.reset_released && !stage_rt->devices.z80_bus.bus_requested;
    (void)s;
    if (master > (UINT64_C(1) << 58)) return SCD_SNAPSHOT_INVALID_STATE;
    if (base > master + slack || cycles > (master + slack - base) / GENESIS_Z80_CLOCK_DIVIDER) return SCD_SNAPSHOT_INVALID_STATE;
    if (runnable && master > slack + base && cycles < (master - slack - base) / GENESIS_Z80_CLOCK_DIVIDER) return SCD_SNAPSHOT_INVALID_STATE;
    if (ym > (master + slack) / YM2612_MASTER_TICKS_PER_SAMPLE || (master > slack && ym < (master - slack) / YM2612_MASTER_TICKS_PER_SAMPLE)) return SCD_SNAPSHOT_INVALID_STATE;
    if (psg > (master + slack) / GENESIS_MIXER_PSG_TICK_MASTER || (master > slack && psg < (master - slack) / GENESIS_MIXER_PSG_TICK_MASTER)) return SCD_SNAPSHOT_INVALID_STATE;
    {
      const uint64_t ym_now = svc_le64(st->ym), ym_chip_samples = svc_le64(st->ym + 40);
      const GenesisMixerSource *src[2];
      unsigned k;
      if (ym_chip_samples != ym || ym_now > (master + slack) / YM2612_MASTER_TICKS_PER_CLOCK || (master > slack && ym_now < (master - slack) / YM2612_MASTER_TICKS_PER_CLOCK)) return SCD_SNAPSHOT_INVALID_STATE;
      src[0] = &st->audio->mixer.ym;
      src[1] = &st->audio->mixer.psg;
      for (k = 0U; k < 2U; ++k)
        if (src[k]->watermark > master + slack || src[k]->watermark + slack < master || src[k]->window_end > src[k]->watermark + slack || src[k]->window_end + slack < src[k]->watermark) return SCD_SNAPSHOT_INVALID_STATE;
    }
    if (st->audio_synced > master + slack || st->audio_synced + slack < master) return SCD_SNAPSHOT_INVALID_STATE;
  }
  return SCD_SNAPSHOT_OK;
}

static void z80_stage_free(Z80Stage *st) {
  free(st->machine);
  free(st->audio);
  memset(st, 0, sizeof *st);
}
#endif

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Capture                                                                                                                            */

ScdSnapshotStatus scd_snapshot_capture(const ScdMachine *machine, const ScdSnapshotThumbnail *thumbnail, ScdSnapshotBuffer *out) {
  ScdMachine *m = (ScdMachine *)machine;   /* read-only: scd_vdp_host_access hands out pointers into a non-const device host */
  SvcWriter w;
  uint32_t starts[SEC_LAST + 1U];
  uint32_t i, table_at, thumb_bytes = 0U, count = SEC_LAST;
  uint16_t flags;
  uint8_t fp[32], disc[32], bios[32], digest[32];
  ScdSnapshotStatus status = SCD_SNAPSHOT_OK;
  GenesisSha256 sha;
  if (out != NULL) { out->data = NULL; out->size = 0U; }
  if (m == NULL || out == NULL) return SCD_SNAPSHOT_BAD_ARGUMENT;
  if (scd_snapshot_refusal(m) != NULL) return SCD_SNAPSHOT_NOT_CAPTURABLE;
  if (thumbnail != NULL && thumbnail->rgb565 != NULL) {
    if (thumbnail->width == 0U || thumbnail->height == 0U || thumbnail->width > SCD_SNAPSHOT_MAX_THUMB_W || thumbnail->height > SCD_SNAPSHOT_MAX_THUMB_H) return SCD_SNAPSHOT_BAD_ARGUMENT;
    thumb_bytes = (uint32_t)thumbnail->width * thumbnail->height * 2U;
  }
  flags = machine_flags(m);
  exec_fingerprint(m, flags, fp);
  sha_str(k_disc_tag, m->program->disc.identity, disc);
  sha_str(k_bios_tag, m->authentic->bios_sha256, bios);
  if ((flags & FLAG_Z80) == 0U) count = SEC_LAST - 1U;   /* the Z80 section exists only for a machine that has the Z80 */
  svc_w_init(&w, SCD_SNAPSHOT_MAX_FILE_BYTES);
  svc_w_bytes(&w, SCD_SNAPSHOT_MAGIC, 8U);
  svc_w_u16(&w, SCD_SNAPSHOT_CONTAINER_VERSION);
  svc_w_u16(&w, SCD_SNAPSHOT_STATE_SCHEMA);
  svc_w_u16(&w, flags);
  svc_w_u16(&w, PLATFORM_ID);
  svc_w_u32(&w, 0U);
  svc_w_u32(&w, HEADER_FIXED_BYTES);
  svc_w_u32(&w, 0U);                      /* total size: patched below */
  svc_w_u32(&w, count);
  svc_w_bytes(&w, disc, 32U);
  svc_w_bytes(&w, bios, 32U);
  svc_w_bytes(&w, fp, 32U);
  svc_w_u64(&w, m->vdp_frames);
  svc_w_u64(&w, m->now);
  svc_w_u16(&w, thumb_bytes != 0U ? thumbnail->width : 0U);
  svc_w_u16(&w, thumb_bytes != 0U ? thumbnail->height : 0U);
  svc_w_u32(&w, thumb_bytes);
  svc_w_u32(&w, 0U);
  table_at = w.size;
  for (i = 0; i < count * SECTION_ENTRY_BYTES; ++i) svc_w_u8(&w, 0U);
  if (thumb_bytes != 0U) svc_w_bytes(&w, thumbnail->rgb565, thumb_bytes);
  for (i = 1U; i <= count && !w.error && status == SCD_SNAPSHOT_OK; ++i) {
    starts[i - 1U] = w.size;
    switch (i) {
      case SEC_CPU: put_cpu(&w, m); break;
      case SEC_CORE: put_core(&w, m); break;
      case SEC_PRG: svc_w_bytes(&w, m->prg_ram, sizeof m->prg_ram); break;
      case SEC_WORD: svc_w_bytes(&w, m->word_ram, sizeof m->word_ram); break;
      case SEC_WORK: svc_w_bytes(&w, m->work_ram, sizeof m->work_ram); break;
      case SEC_GATE: put_gate(&w, m); break;
      case SEC_CDC: put_cdc(&w, m); break;
      case SEC_CDD: put_cdd(&w, m); break;
      case SEC_PCM: put_pcm(&w, m); break;
      case SEC_GFX: put_gfx(&w, m); break;
      case SEC_AUDIO: put_audio(&w, m); break;
      case SEC_BACKUP: put_backup(&w, m); break;
      case SEC_INPUT: put_input(&w, m); break;
      case SEC_OVERLAY: put_overlay(&w, m); break;
      case SEC_DYNAMIC: put_dynamic(&w, m); break;
      case SEC_LEDGER: put_ledger(&w, m); break;
      case SEC_HOST: if (!put_host(&w, m)) status = SCD_SNAPSHOT_NOT_CAPTURABLE; break;
#ifdef SEGARECOMP_GENESIS_SOUND
      case SEC_Z80: (void)put_z80(&w, m, &status); break;
#endif
      default: status = SCD_SNAPSHOT_BAD_ARGUMENT; break;
    }
  }
  starts[count] = w.size;
  if (w.error) { free(w.data); return SCD_SNAPSHOT_NO_MEMORY; }
  if (status != SCD_SNAPSHOT_OK) { free(w.data); return status; }
  for (i = 0U; i < count; ++i) {
    const uint32_t length = starts[i + 1U] - starts[i];
    if (length > k_section_max[i + 1U]) { free(w.data); return SCD_SNAPSHOT_TOO_LARGE; }
    svc_put_le16(w.data + table_at + i * SECTION_ENTRY_BYTES, (uint16_t)(i + 1U));
    svc_put_le16(w.data + table_at + i * SECTION_ENTRY_BYTES + 2U, (uint16_t)SECTION_FLAG_REQUIRED);
    svc_put_le32(w.data + table_at + i * SECTION_ENTRY_BYTES + 4U, starts[i]);
    svc_put_le32(w.data + table_at + i * SECTION_ENTRY_BYTES + 8U, length);
  }
  svc_w_reserve(&w, DIGEST_BYTES);
  if (w.error) { free(w.data); return SCD_SNAPSHOT_TOO_LARGE; }
  svc_put_le32(w.data + 24U, w.size + DIGEST_BYTES);
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, w.data, w.size);
  genesis_sha256_final(&sha, digest);
  memcpy(w.data + w.size, digest, DIGEST_BYTES);
  w.size += DIGEST_BYTES;
  {
    uint8_t *fit = (uint8_t *)realloc(w.data, w.size);
    if (fit != NULL) w.data = fit;
  }
  out->data = w.data;
  out->size = w.size;
  return SCD_SNAPSHOT_OK;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Inspect (structure + integrity)                                                                                                    */

typedef struct SectionView {
  const uint8_t *data;
  uint32_t length;
  int present;
} SectionView;

static ScdSnapshotStatus parse_container(const uint8_t *data, size_t size, ScdSnapshotInfo *info, SectionView views[SEC_LAST + 1U]) {
  uint32_t total, count, i, thumb_bytes, cursor;
  uint16_t flags, platform;
  GenesisSha256 sha;
  uint8_t digest[32];
  if (data == NULL || info == NULL) return SCD_SNAPSHOT_BAD_ARGUMENT;
  memset(info, 0, sizeof *info);
  if (size > SCD_SNAPSHOT_MAX_FILE_BYTES) return SCD_SNAPSHOT_TOO_LARGE;
  if (size < 8U) return SCD_SNAPSHOT_TRUNCATED;
  if (memcmp(data, SCD_SNAPSHOT_MAGIC, 8U) != 0) return SCD_SNAPSHOT_BAD_MAGIC;
  if (size < HEADER_FIXED_BYTES + DIGEST_BYTES) return SCD_SNAPSHOT_TRUNCATED;
  info->container_version = svc_le16(data + 8);
  info->state_schema = svc_le16(data + 10);
  if (info->container_version != SCD_SNAPSHOT_CONTAINER_VERSION || info->state_schema != SCD_SNAPSHOT_STATE_SCHEMA) return SCD_SNAPSHOT_UNSUPPORTED_VERSION;
  flags = svc_le16(data + 12);
  platform = svc_le16(data + 14);
  total = svc_le32(data + 24);
  count = svc_le32(data + 28);
  if (platform != PLATFORM_ID || svc_le32(data + 16) != 0U || svc_le32(data + 20) != HEADER_FIXED_BYTES || (flags & ~FLAG_MASK) != 0U) return SCD_SNAPSHOT_BAD_LAYOUT;
  if (total != size) return total > size ? SCD_SNAPSHOT_TRUNCATED : SCD_SNAPSHOT_BAD_LAYOUT;
  if (count != ((flags & FLAG_Z80) != 0U ? SEC_LAST : SEC_LAST - 1U)) return SCD_SNAPSHOT_BAD_LAYOUT;
  info->machine_flags = flags;
  memcpy(info->disc_sha256, data + 32, 32U);
  memcpy(info->bios_sha256, data + 64, 32U);
  memcpy(info->exec_fingerprint, data + 96, 32U);
  info->guest_onsets = svc_le64(data + 128);
  info->guest_ticks = svc_le64(data + 136);
  info->thumb_width = svc_le16(data + 144);
  info->thumb_height = svc_le16(data + 146);
  thumb_bytes = svc_le32(data + 148);
  if (svc_le32(data + 152) != 0U) return SCD_SNAPSHOT_BAD_LAYOUT;
  if ((thumb_bytes == 0U) != (info->thumb_width == 0U && info->thumb_height == 0U) ||
      (thumb_bytes != 0U && (info->thumb_width > SCD_SNAPSHOT_MAX_THUMB_W || info->thumb_height > SCD_SNAPSHOT_MAX_THUMB_H ||
                             thumb_bytes != (uint32_t)info->thumb_width * (uint32_t)info->thumb_height * 2U)))
    return SCD_SNAPSHOT_BAD_LAYOUT;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, data, (uint32_t)(size - DIGEST_BYTES));
  genesis_sha256_final(&sha, digest);
  if (memcmp(digest, data + size - DIGEST_BYTES, DIGEST_BYTES) != 0) return SCD_SNAPSHOT_INTEGRITY;
  if (HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES + thumb_bytes + DIGEST_BYTES > size) return SCD_SNAPSHOT_TRUNCATED;
  cursor = HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES + thumb_bytes;
  if (thumb_bytes != 0U) info->thumb_rgb565 = data + HEADER_FIXED_BYTES + count * SECTION_ENTRY_BYTES;
  memset(views, 0, sizeof(SectionView) * (SEC_LAST + 1U));
  for (i = 0U; i < count; ++i) {
    const uint8_t *entry = data + HEADER_FIXED_BYTES + i * SECTION_ENTRY_BYTES;
    const uint32_t id = svc_le16(entry), sflags = svc_le16(entry + 2), offset = svc_le32(entry + 4), length = svc_le32(entry + 8);
    if (id == 0U || id > SEC_LAST) return SCD_SNAPSHOT_UNKNOWN_SECTION;
    if ((sflags & SECTION_FLAG_REQUIRED) == 0U || (sflags & ~SECTION_FLAG_REQUIRED) != 0U) return SCD_SNAPSHOT_UNKNOWN_SECTION;
    if (views[id].present) return SCD_SNAPSHOT_UNKNOWN_SECTION;
    if (length > k_section_max[id]) return SCD_SNAPSHOT_BAD_LAYOUT;
    if (offset != cursor || length > size - DIGEST_BYTES - cursor) return SCD_SNAPSHOT_BAD_LAYOUT;
    views[id].data = data + offset;
    views[id].length = length;
    views[id].present = 1;
    cursor += length;
  }
  if (cursor != size - DIGEST_BYTES) return SCD_SNAPSHOT_BAD_LAYOUT;
  for (i = 1U; i <= count; ++i)
    if (!views[i].present) return SCD_SNAPSHOT_UNKNOWN_SECTION;
  info->size = (uint32_t)size;
  return SCD_SNAPSHOT_OK;
}

ScdSnapshotStatus scd_snapshot_inspect(const uint8_t *data, size_t size, ScdSnapshotInfo *info) {
  SectionView views[SEC_LAST + 1U];
  return parse_container(data, size, info, views);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* Restore: stage everything, validate everything, then commit with plain copies.                                                     */

static ScdSnapshotStatus restore_impl(ScdMachine *machine, const uint8_t *data, size_t size, int commit, ScdSnapshotInfo *info_out) {
  ScdSnapshotInfo info;
  SectionView views[SEC_LAST + 1U];
  ScdSnapshotStatus status;
  ScdMachine *stage = NULL;
  ScdInput *stage_input = NULL;
  HostStage host;
  SvcReader r;
  uint8_t fp[32], disc[32], bios[32];
  uint16_t flags;
#ifdef SEGARECOMP_GENESIS_SOUND
  Z80Stage z80;
  memset(&z80, 0, sizeof z80);
#endif
  memset(&host, 0, sizeof host);
  if (machine == NULL || data == NULL) return SCD_SNAPSHOT_BAD_ARGUMENT;
  status = parse_container(data, size, &info, views);
  if (info_out != NULL) *info_out = info;
  if (status != SCD_SNAPSHOT_OK) return status;
  if (scd_snapshot_refusal(machine) != NULL) return SCD_SNAPSHOT_NOT_CAPTURABLE;   /* the running machine must itself be one a state can replace */
  sha_str(k_disc_tag, machine->program->disc.identity, disc);
  sha_str(k_bios_tag, machine->authentic->bios_sha256, bios);
  if (memcmp(info.disc_sha256, disc, 32U) != 0 || memcmp(info.bios_sha256, bios, 32U) != 0) return SCD_SNAPSHOT_WRONG_DISC;
  flags = machine_flags(machine);
  exec_fingerprint(machine, flags, fp);
  if (info.machine_flags != flags || memcmp(info.exec_fingerprint, fp, 32U) != 0) return SCD_SNAPSHOT_INCOMPATIBLE;

  stage = (ScdMachine *)malloc(sizeof *stage);
  if (stage == NULL) return SCD_SNAPSHOT_NO_MEMORY;
  memcpy(stage, machine, sizeof *stage);   /* a shallow clone: every binding, observer, sink and allowance keeps its live value */
  if (machine->input != NULL) {
    stage_input = (ScdInput *)malloc(sizeof *stage_input);
    if (stage_input == NULL) { free(stage); return SCD_SNAPSHOT_NO_MEMORY; }
    memcpy(stage_input, machine->input, sizeof *stage_input);
  }
  stage->stop.stop_class = SCD_STOP_NONE;

#define SECTION_READER(id) svc_r_init(&r, views[id].data, views[id].length)
  status = SCD_SNAPSHOT_INVALID_STATE;
  SECTION_READER(SEC_CPU);    if (!get_cpu(&r, stage, machine)) goto done;
  SECTION_READER(SEC_CORE);   if (!get_core(&r, stage)) goto done;
  if (views[SEC_PRG].length != SCD_PRG_RAM_BYTES || views[SEC_WORD].length != SCD_WORD_RAM_BYTES || views[SEC_WORK].length != SCD_WORK_RAM_BYTES) goto done;
  memcpy(stage->prg_ram, views[SEC_PRG].data, SCD_PRG_RAM_BYTES);
  memcpy(stage->word_ram, views[SEC_WORD].data, SCD_WORD_RAM_BYTES);
  memcpy(stage->work_ram, views[SEC_WORK].data, SCD_WORK_RAM_BYTES);
  SECTION_READER(SEC_GATE);   if (!get_gate(&r, stage)) goto done;
  SECTION_READER(SEC_CDC);    if (!get_cdc(&r, stage)) goto done;
  SECTION_READER(SEC_CDD);    if (!get_cdd(&r, stage)) goto done;
  SECTION_READER(SEC_PCM);    if (!get_pcm(&r, stage)) goto done;
  SECTION_READER(SEC_GFX);    if (!get_gfx(&r, stage)) goto done;
  SECTION_READER(SEC_AUDIO);  if (!get_audio(&r, stage)) goto done;
  SECTION_READER(SEC_BACKUP); if (!get_backup(&r, stage, machine)) goto done;
  SECTION_READER(SEC_INPUT);  if (!get_input(&r, stage, stage_input, machine)) goto done;
  SECTION_READER(SEC_OVERLAY);if (!get_overlay(&r, stage)) goto done;
  SECTION_READER(SEC_DYNAMIC);if (!get_dynamic(&r, stage)) goto done;
  SECTION_READER(SEC_LEDGER); if (!get_ledger(&r, stage)) goto done;
  SECTION_READER(SEC_HOST);
  {
    const int ok = get_host(&r, machine, stage, &host);
    if (ok < 0) { status = SCD_SNAPSHOT_NO_MEMORY; goto done; }
    if (!ok) goto done;
  }
  if (info.guest_ticks != stage->now || info.guest_onsets != stage->vdp_frames) { status = SCD_SNAPSHOT_BAD_LAYOUT; goto done; }
#ifdef SEGARECOMP_GENESIS_SOUND
  if ((flags & FLAG_Z80) != 0U) {
    SECTION_READER(SEC_Z80);
    status = get_z80(&r, machine, stage, host.rt, &z80);
    if (status != SCD_SNAPSHOT_OK) goto done;
  }
#endif
#undef SECTION_READER
  if (!commit) { status = SCD_SNAPSHOT_OK; goto done; }   /* validation only: the staged copies are dropped, the machine is never touched */
  /* ---- commit: everything above passed; the copies below cannot fail ---- */
  {
    void *live_host, *live_raster, *live_line0;
    size_t host_bytes, raster_bytes, line0_bytes;
    ScdVdpHostFields ignored;
    ScdInput *live_input = machine->input;
    (void)scd_vdp_host_access(machine, &live_host, &host_bytes, &live_raster, &raster_bytes, &live_line0, &line0_bytes, &ignored);
    memcpy(machine, stage, sizeof *machine);
    if (live_input != NULL) memcpy(live_input, stage_input, sizeof *live_input);
    memcpy(live_host, host.rt, host_bytes);
    memcpy(live_raster, host.raster, raster_bytes);
    memcpy(live_line0, host.line0, line0_bytes);
    scd_vdp_host_set_fields(machine, &host.fields);
#ifdef SEGARECOMP_GENESIS_SOUND
    if ((flags & FLAG_Z80) != 0U) {
      ScdZ80SnapshotView v;
      int have;
      z80_view(machine, &v, &have);
      memcpy(v.machine, z80.machine, sizeof *z80.machine);
      memcpy(v.audio, z80.audio, sizeof *z80.audio);
      (void)ym2612_load(((GenesisAudio *)v.audio)->ym, z80.ym, z80.ym_length);
      *v.audio_synced = z80.audio_synced;
      *v.audio_stopped = 0U;
    }
#endif
  }
  status = SCD_SNAPSHOT_OK;
done:
#ifdef SEGARECOMP_GENESIS_SOUND
  z80_stage_free(&z80);
#endif
  host_stage_free(&host);
  free(stage_input);
  free(stage);
  return status;
}

ScdSnapshotStatus scd_snapshot_validate(ScdMachine *machine, const uint8_t *data, size_t size, ScdSnapshotInfo *info) {
  ScdSnapshotInfo local;
  return restore_impl(machine, data, size, 0, info != NULL ? info : &local);
}

ScdSnapshotStatus scd_snapshot_restore(ScdMachine *machine, const uint8_t *data, size_t size) {
  return restore_impl(machine, data, size, 1, NULL);
}
