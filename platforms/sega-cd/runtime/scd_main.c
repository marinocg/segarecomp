/* SEG-062: the generated Sega CD program's driver. Parses the command line, opens and verifies the external disc against the
 * build-time identity, establishes the synthetic post-BIOS profile, runs both processors under a finite instruction budget
 * (optionally in deadline slices: the result must not depend on them), and writes a JSON report and, on request, the VDP frame.
 *
 *   program --disc-dir DIR --instruction-budget N [--report FILE] [--frame FILE.ppm] [--slice-ticks T | --slice-seed S]
 *           [--set main|sub:dN|aN=VALUE]... [--until-tick T] [--backup-ram FILE]
 *           [--capture-onset N]... --capture-dir DIR   (BIOS-image profile; SEG-065-T003)
 *           [--input-script FILE [--input-log FILE]]   (BIOS-image profile; SEG-070-T002, ADR 0149)
 *           [--record-input FILE]   (live programs only, SEG-078-T001, ADR 0204: the script the live run recorded)
 *
 * Exit status: 0 idle (success), 10 typed stop, 11 budget exhausted or deadline (read `result`), 20 disc refused at start-up, 22 backup RAM input refused, 23 input script refused, 2 usage.
 * A live program (a viewer registered a host, SEG-078-T001) additionally: 24 host failure, 25 recording capacity reached (the valid prefix is
 * recorded), 26 recorder refused, 27 recording could not be written; leaving by the host's quit is 0. */
#include "scd_runtime.h"

#include "scd_live.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_export.h"
#include "runtime.h"
#include "vdp_render.h"

static int scd_parse_u64(const char *text, uint64_t *out) {
  char *end;
  unsigned long long value;
  if (text == NULL || *text == '\0' || *text == '-') return 0;
  errno = 0;
  value = strtoull(text, &end, 0);
  if (errno != 0 || *end != '\0') return 0;
  *out = (uint64_t)value;
  return 1;
}

static void scd_sha_hex(const uint8_t *data, size_t length, char out[65]) {
  GenesisSha256 sha;
  uint8_t digest[32];
  unsigned i;
  genesis_sha256_init(&sha);
  while (length > 0U) {
    const uint32_t piece = length > 0x40000000U ? 0x40000000U : (uint32_t)length;
    genesis_sha256_update(&sha, data, piece);
    data += piece;
    length -= piece;
  }
  genesis_sha256_final(&sha, digest);
  for (i = 0; i < 32U; ++i) snprintf(out + 2U * i, 3U, "%02x", digest[i]);
}

static const char *scd_result_name(ScdRunResult result) {
  switch (result) {
  case SCD_RUN_IDLE: return "idle";
  case SCD_RUN_STOPPED: return "stopped";
  case SCD_RUN_BUDGET: return "budget_exhausted";
  case SCD_RUN_DEADLINE: return "deadline";
  }
  return "unknown";
}

static int scd_apply_set(ScdMachine *m, const char *spec) {
  uint32_t cpu, reg;
  uint64_t value;
  const char *colon = strchr(spec, ':');
  const char *equals = strchr(spec, '=');
  if (colon == NULL || equals == NULL || equals < colon) return 0;
  if ((size_t)(colon - spec) == 4U && strncmp(spec, "main", 4) == 0) cpu = SCD_MAIN;
  else if ((size_t)(colon - spec) == 3U && strncmp(spec, "sub", 3) == 0) cpu = SCD_SUB;
  else return 0;
  if (equals - colon != 3 || (colon[1] != 'd' && colon[1] != 'a') || colon[2] < '0' || colon[2] > '7') return 0;
  reg = (uint32_t)(colon[2] - '0');
  if (!scd_parse_u64(equals + 1, &value) || value > 0xFFFFFFFFU) return 0;
  if (colon[1] == 'd') m->cpu[cpu].d[reg] = (uint32_t)value;
  else m->cpu[cpu].a[reg] = (uint32_t)value;
  return 1;
}

static void scd_write_cdda_report(FILE *out, const ScdMachine *m);

static void scd_write_report(FILE *out, const ScdMachine *m, const ScdProgram *program, ScdRunResult result, const char *frame_sha,
                             const ScdLiveSummary *live) {
  const ScdAuthentic *authentic = m->authentic;
  char digest[65];
  unsigned k, i;
  const GenesisRuntime *host = (const GenesisRuntime *)m->vdp_host;
  fprintf(out, "{\"result\":\"%s\",\"program_identity\":\"%s\",\"disc_identity\":\"%s\",\"ticks\":%llu,", scd_result_name(result),
          program->program_identity, program->disc.identity, (unsigned long long)m->now);
  fprintf(out, "\"stop\":{\"class\":\"%s\",\"cpu\":%u,\"pc\":%u,\"address\":%u,\"width\":%u,\"write\":%u,\"tick\":%llu},",
          scd_stop_class_name(m->stop.stop_class), m->stop.cpu, m->stop.pc, m->stop.address, m->stop.width, m->stop.is_write,
          (unsigned long long)m->stop.tick);
  fprintf(out, "\"cpus\":[");
  for (k = 0; k < 2U; ++k) {
    const ScdCpu *c = &m->cpu[k];
    fprintf(out, "%s{\"pc\":%u,\"sr\":%u,\"usp\":%u,\"stopped\":%u,\"retired\":%llu,\"cycles\":%llu,\"trace\":\"%016llx\",\"d\":[",
            k ? "," : "", c->pc, c->sr, c->usp, c->irq.stopped, (unsigned long long)c->retired, (unsigned long long)c->cycles,
            (unsigned long long)c->trace);
    for (i = 0; i < 8U; ++i) fprintf(out, "%s%u", i ? "," : "", c->d[i]);
    fprintf(out, "],\"a\":[");
    for (i = 0; i < 8U; ++i) fprintf(out, "%s%u", i ? "," : "", c->a[i]);
    fprintf(out, "]}");
  }
  fprintf(out, "],\"ga\":{\"ctl\":%u,\"sub_halted\":%u,\"sub_wait\":%u,\"prg_window\":%u,\"ifl2\":%u,\"ien2_mirror\":%u,"
               "\"write_protect\":%u,\"prg_bank\":%u,\"mode\":%u,\"mode_1m\":%u,\"ret\":%u,\"dmna\":%u,\"dmna_int\":%u,"
               "\"main_flag\":%u,\"sub_flag\":%u,\"led\":%u,\"interrupt_enable\":%u,\"pending\":%u,\"timer_reload\":%u,"
               "\"cdd_control\":%u,\"comcmd\":[",
          m->ga.ctl, m->ga.sub_halted, m->ga.sub_wait, m->ga.prg_window, m->ga.ifl2, m->ga.ien2_mirror, m->ga.write_protect,
          m->ga.prg_bank, m->ga.mode, (m->ga.mode >> 2) & 1U, m->ga.mode & 1U, (m->ga.mode >> 1) & 1U, m->ga.dmna_int, m->ga.main_flag,
          m->ga.sub_flag, m->ga.led, m->ga.interrupt_enable, m->ga.pending, m->ga.timer_reload,
          (unsigned)((m->ga.cdd_ctrl_hi << 8) | m->ga.cdd_ctrl_lo));
  for (i = 0; i < 8U; ++i) fprintf(out, "%s%u", i ? "," : "", m->ga.comcmd[i]);
  fprintf(out, "],\"comstat\":[");
  for (i = 0; i < 8U; ++i) fprintf(out, "%s%u", i ? "," : "", m->ga.comstat[i]);
  fprintf(out, "]},\"sub_interrupts_taken\":[");
  for (i = 0; i < 8U; ++i) fprintf(out, "%s%llu", i ? "," : "", (unsigned long long)m->sub_interrupts_taken[i]);
  fprintf(out, "],\"prg_write_protect_drops\":%llu,\"ignored_writes\":%llu,\"sub_waits\":%llu,\"device_log\":\"%016llx\",\"digests\":{",
          (unsigned long long)m->prg_write_protect_drops, (unsigned long long)m->ignored_writes, (unsigned long long)m->sub_waits,
          (unsigned long long)m->device_log);
  scd_sha_hex(m->prg_ram, sizeof m->prg_ram, digest);
  fprintf(out, "\"prg_ram\":\"%s\",", digest);
  scd_sha_hex(m->word_ram, sizeof m->word_ram, digest);
  fprintf(out, "\"word_ram\":\"%s\",", digest);
  scd_sha_hex(m->work_ram, sizeof m->work_ram, digest);
  fprintf(out, "\"work_ram\":\"%s\",", digest);
  scd_sha_hex(m->cdc.buffer, SCD_CDC_BUFFER_BYTES, digest);
  fprintf(out, "\"cdc_buffer\":\"%s\",", digest);
  scd_sha_hex(host->devices.vdp.vram, sizeof host->devices.vdp.vram, digest);
  fprintf(out, "\"vram\":\"%s\",", digest);
  scd_sha_hex(host->devices.vdp.cram, sizeof host->devices.vdp.cram, digest);
  fprintf(out, "\"cram\":\"%s\",", digest);
  scd_sha_hex(host->devices.vdp.vsram, sizeof host->devices.vdp.vsram, digest);
  fprintf(out, "\"vsram\":\"%s\"},\"result_block\":\"", digest);
  for (i = 0; i < 256U; ++i) fprintf(out, "%02x", m->work_ram[0xC000U + i]);
  fprintf(out, "\",\"vdp_registers\":[");
  for (i = 0; i < GENESIS_VDP_REGISTER_COUNT; ++i) fprintf(out, "%s%u", i ? "," : "", host->devices.vdp.registers[i]);
  fprintf(out, "],\"cram_words\":\"");
  for (i = 0; i < GENESIS_VDP_CRAM_BYTES; ++i) fprintf(out, "%02x", host->devices.vdp.cram[i]);
  fprintf(out, "\",\"cdc\":{\"edt\":%u,\"dsr\":%u,\"destination\":%u,\"dma_active\":%u,\"dma_state\":%u,\"dma_done\":%u,"
               "\"dma_total\":%u,\"dbc\":%u,\"pt\":%u,\"wa\":%u,\"dac\":%u,\"dma_address\":%u,\"ifstat\":%u,\"ifctrl\":%u,"
               "\"ctrl0\":%u,\"ctrl1\":%u,\"stat2\":%u,\"stat3\":%u,\"irq\":%u,\"head\":[%u,%u,%u,%u]}",
          m->cdc.edt, m->cdc.dsr, m->cdc.destination, m->cdc.dma_state != SCD_DMA_NONE ? 1U : 0U, m->cdc.dma_state, m->cdc.dma_done,
          m->cdc.dma_total, m->cdc.dbc, m->cdc.pt, m->cdc.wa, m->cdc.dac, m->cdc.dma_address, m->cdc.ifstat, m->cdc.ifctrl, m->cdc.ctrl0,
          m->cdc.ctrl1, m->cdc.stat2, m->cdc.stat3, m->cdc.irq, m->cdc.head[0][0], m->cdc.head[0][1], m->cdc.head[0][2],
          m->cdc.head[0][3]);
  /* SEG-064-T004: the sub-side devices of the boot path (stopwatch, graphics ASIC, PCM write state, CDC host register). */
  fprintf(out, ",\"sub_devices\":{\"stopwatch\":{\"origin\":%llu,\"base\":%u},\"gfx\":{\"busy\":%u,\"ctl\":%u,\"lines_left\":%u,"
               "\"operations\":%llu,\"lines_rendered\":%llu},\"pcm\":{\"index\":%u,\"bank\":%u,\"enabled\":%u,\"onoff\":%u,\"running\":%u,\"origin\":%llu,\"samples\":%llu,\"syncs\":%llu,\"channel_steps\":%llu,\"max_catchup\":%llu,\"addr\":[%u,%u,%u,%u,%u,%u,%u,%u],\"ram\":\"",
          (unsigned long long)m->ga.stopwatch_origin, m->ga.stopwatch_base, m->gfx.busy, m->gfx.ctl, m->gfx.lines_left,
          (unsigned long long)m->gfx.operations, (unsigned long long)m->gfx.lines_rendered, m->pcm.index, m->pcm.bank, m->pcm.enabled,
          m->pcm.onoff, m->pcm.running, (unsigned long long)m->pcm.origin, (unsigned long long)m->pcm.samples,
          (unsigned long long)m->pcm.syncs, (unsigned long long)m->pcm.channel_steps, (unsigned long long)m->pcm.max_catchup,
          m->pcm.addr[0], m->pcm.addr[1], m->pcm.addr[2], m->pcm.addr[3], m->pcm.addr[4], m->pcm.addr[5], m->pcm.addr[6], m->pcm.addr[7]);
  scd_sha_hex(m->pcm.ram, sizeof m->pcm.ram, digest);
  fprintf(out, "%s\"},\"cdc_host_word\":%u}", digest, m->cdc.host_word);
  fprintf(out, ",\"cdd\":{\"status\":%u,\"lba\":%ld,\"index\":%u,\"latency\":%u,\"pending\":%u,\"delivered\":%llu,\"rx\":[",
          m->cdd.status, (long)m->cdd.lba, m->cdd.index, m->cdd.latency, m->cdd.pending, (unsigned long long)m->cdd.delivered);
  for (i = 0; i < 10U; ++i) fprintf(out, "%s%u", i ? "," : "", m->cdd.rx[i]);
  fprintf(out, "],\"tx\":[");
  for (i = 0; i < 10U; ++i) fprintf(out, "%s%u", i ? "," : "", m->cdd.tx[i]);
  fprintf(out, "]},\"frame_sha256\":%s%s%s,\"instructions\":%llu", frame_sha ? "\"" : "", frame_sha ? frame_sha : "null",
          frame_sha ? "\"" : "", (unsigned long long)(m->cpu[0].retired + m->cpu[1].retired));
  if (authentic != NULL) /* SEG-064-T002: BIOS-image profile (absent for synthetic programs: their reports are unchanged) */
    fprintf(out, ",\"bios_profile\":{\"epochs\":%u,\"epoch_image\":%d,\"epoch_tick\":%llu,\"sub_pristine\":%u,\"vblank_onsets\":%llu}", m->epochs,
            m->epoch_image, (unsigned long long)m->epoch_tick, m->sub_pristine, (unsigned long long)m->vdp_frames);
  if (authentic != NULL) { /* SEG-064-T005: per-level interrupt acceptances of the main processor (the sub's are reported above) */
    fprintf(out, ",\"main_interrupts_taken\":[");
    for (i = 0; i < 8U; ++i) fprintf(out, "%s%llu", i ? "," : "", (unsigned long long)m->main_interrupts_taken[i]);
    fprintf(out, "]");
  }
  scd_vdp_capture_report(m, out); /* SEG-065-T003: absent without --capture-onset */
  if (m->vdp_dma_count != 0U) /* SEG-065-T002: 68K->VDP DMA transfers and the main processor's total wait (absent without one) */
    fprintf(out, ",\"vdp_dma\":{\"count\":%llu,\"stall_ticks\":%llu,\"bank_1m\":%llu,\"bank_mode_changes_during_wait\":%llu}", (unsigned long long)m->vdp_dma_count,
            (unsigned long long)m->vdp_dma_stall_ticks, (unsigned long long)m->vdp_dma_bank_count, (unsigned long long)m->wordram_mode_changes_in_dma);
  if (m->backup_ram_present) { /* SEG-065-T002: the backup RAM machine input (absent without --backup-ram: reports unchanged) */
    scd_sha_hex(m->backup_ram, sizeof m->backup_ram, digest);
    fprintf(out, ",\"backup_ram\":{\"reads\":%llu,\"writes\":%llu,\"sha256\":\"%s\"}", (unsigned long long)m->backup_ram_reads,
            (unsigned long long)m->backup_ram_writes, digest);
  }
  scd_input_report(m, out); /* SEG-070-T002: absent without --input-script (reports unchanged) */
  if (live != NULL) { /* SEG-078-T001: the host-loop summary of a live program (absent from every headless report) */
    uint64_t rendered, skipped, rejected;
    scd_vdp_present_counts(m, &rendered, &skipped, &rejected);
    fprintf(out, ",\"live\":{\"outcome\":\"%s\",\"frames\":%llu,\"presented\":%llu,\"rendered\":%llu,\"skipped\":%llu,\"rejected\":%llu,"
                 "\"paused_polls\":%llu,\"steps\":%llu}",
            scd_live_outcome_name(live->outcome), (unsigned long long)live->boundaries, (unsigned long long)live->frames_presented,
            (unsigned long long)rendered, (unsigned long long)skipped, (unsigned long long)rejected,
            (unsigned long long)live->paused_polls, (unsigned long long)live->steps);
  }
  if (m->overlays != NULL) { /* SEG-063: overlay activation (absent for programs without modules: their reports are unchanged) */
    const ScdOverlayProgram *o = m->overlays;
    unsigned g, c;
    fprintf(out, ",\"overlays\":{\"manifest\":\"%s\",\"groups\":[", o->manifest_identity);
    for (g = 0; g < o->group_count; ++g) {
      const ScdOverlayGroup *group = &o->groups[g];
      const ScdOverlayState *state = &m->overlay[g];
      fprintf(out, "%s{\"cpu\":%u,\"start\":%u,\"end\":%u,\"selected\":%d,\"stale_from\":%d,\"selections\":%llu,"
                   "\"revalidations\":%llu,\"invalidations\":%llu,\"candidates\":[",
              g ? "," : "", group->cpu, group->start, group->end, state->selected, state->stale_from,
              (unsigned long long)state->selections, (unsigned long long)state->revalidations, (unsigned long long)state->invalidations);
      for (c = 0; c < group->candidate_count; ++c)
        fprintf(out, "%s{\"name\":\"%s\",\"image_id\":%u,\"start\":%u,\"length\":%u,\"retired\":%llu}", c ? "," : "",
                group->candidates[c].name, group->candidates[c].image_id, group->candidates[c].start, group->candidates[c].length,
                (unsigned long long)state->retired[c]);
      fprintf(out, "]}");
    }
    fprintf(out, "],\"event_count\":%u,\"events\":[", m->overlay_events);
    for (i = 0; i < m->overlay_events && i < SCD_OVERLAY_LOG; ++i) {
      const ScdOverlayEvent *e = &m->overlay_log[i];
      const char *name = e->candidate < o->groups[e->group].candidate_count ? o->groups[e->group].candidates[e->candidate].name : "";
      fprintf(out, "%s{\"tick\":%llu,\"cpu\":%u,\"group\":%u,\"candidate\":\"%s\",\"pc\":%u,\"kind\":\"%s\"}", i ? "," : "",
              (unsigned long long)e->tick, e->cpu, e->group, name, e->pc, e->kind == 1U ? "selected" : "invalidated");
    }
    fprintf(out, "]}");
  }
  scd_dynamic_report(m, out); /* SEG-100: absent for a program without dynamic images (reports unchanged) */
  if (m->cdda.enabled) scd_write_cdda_report(out, m); /* SEG-104: absent unless CD-DA output was requested (reports unchanged) */
  fprintf(out, "}\n");
}


/* SEG-104 (ADR 0244): the CD-DA tap between the machine and its consumers. It hashes the canonical stream (little-endian S16 bytes written
 * explicitly, so the digest is host-endianness independent), optionally writes it to a file and forwards the sector to a live host's sink. The
 * digest therefore always describes what the machine produced, whatever the host did with it. */
typedef struct ScdCddaTap {
  GenesisSha256 sha;
  FILE *pcm;
  const ScdLiveHost *host;
  uint64_t io_errors;
} ScdCddaTap;

static ScdCddaTap g_cdda_tap;

static int scd_cdda_tap(void *context, const int16_t *frames, uint32_t count) {
  ScdCddaTap *const tap = (ScdCddaTap *)context;
  uint8_t bytes[2U * 2U * SCD_CDDA_SECTOR_FRAMES];
  uint32_t i;
  if (count != SCD_CDDA_SECTOR_FRAMES) return 1;
  for (i = 0; i < 2U * count; ++i) {
    bytes[2U * i] = (uint8_t)((uint16_t)frames[i] & 0xFFU);
    bytes[2U * i + 1U] = (uint8_t)((uint16_t)frames[i] >> 8);
  }
  genesis_sha256_update(&tap->sha, bytes, 4U * count);
  if (tap->pcm != NULL && fwrite(bytes, 1U, 4U * count, tap->pcm) != 4U * count) tap->io_errors += 1U;
  return tap->host != NULL && tap->host->audio != NULL ? tap->host->audio(tap->host->ctx, frames, count) : 0;
}

static void scd_write_cdda_report(FILE *out, const ScdMachine *m) {
  GenesisSha256 sha = g_cdda_tap.sha;
  uint8_t digest[32];
  unsigned i;
  const ScdCdda *const c = &m->cdda;
  genesis_sha256_final(&sha, digest);
  fprintf(out, ",\"cdda\":{\"sectors\":%llu,\"frames\":%llu,\"sample_rate\":44100,\"segments\":%llu,\"interruptions\":%llu,"
               "\"discontinuities\":%llu,\"track_changes\":%llu,\"pregap_ticks\":%llu,\"suppressed_pending\":%llu,\"sink_refused\":%llu,"
               "\"io_errors\":%llu,\"commands\":{\"stop\":%llu,\"play\":%llu,\"seek\":%llu,\"pause\":%llu,\"resume\":%llu,\"jump\":%llu},"
               "\"fader\":{\"register\":%u,\"target\":%u,\"volume\":%u},\"pcm_sha256\":\"",
          (unsigned long long)c->sectors, (unsigned long long)c->frames, (unsigned long long)c->segments, (unsigned long long)c->interruptions,
          (unsigned long long)c->discontinuities, (unsigned long long)c->track_changes, (unsigned long long)c->pregap_ticks,
          (unsigned long long)c->suppressed_pending, (unsigned long long)c->sink_refused, (unsigned long long)g_cdda_tap.io_errors,
          (unsigned long long)c->commands[1], (unsigned long long)c->commands[3], (unsigned long long)c->commands[4],
          (unsigned long long)c->commands[6], (unsigned long long)c->commands[7], (unsigned long long)c->commands[10], c->fader_register,
          c->fader_target, c->fader_volume);
  for (i = 0; i < 32U; ++i) fprintf(out, "%02x", digest[i]);
  fprintf(out, "\"}");
}

const ScdLiveHost *scd_live_host_registered = NULL;

int scd_program_main(const ScdProgram *program, int argc, char **argv) { return scd_program_main_overlays(program, NULL, argc, argv); }

/* SEG-064-T002 (ADR 0142 decision 1): read the separately supplied BIOS and refuse it unless it is the image the program was compiled
 * from. Returns NULL with `error` set; the bytes are kept for the whole run. */
static const uint8_t *scd_load_bios(const ScdAuthentic *a, const char *path, const char **error) {
  static uint8_t image[0x20000U + 1U];
  char digest[65];
  size_t n;
  FILE *f;
  if (path == NULL || (f = fopen(path, "rb")) == NULL) {
    *error = "bios_missing";
    return NULL;
  }
  n = fread(image, 1U, sizeof image, f);
  fclose(f);
  if (n != a->bios_size || n > 0x20000U) {
    *error = "bios_size_invalid";
    return NULL;
  }
  scd_sha_hex(image, n, digest);
  if (strcmp(digest, a->bios_sha256) != 0) {
    *error = "bios_identity_mismatch";
    return NULL;
  }
  return image;
}

static void scd_hex(FILE *out, const uint8_t *bytes, uint32_t length) {
  uint32_t i;
  for (i = 0; i < length; ++i) fprintf(out, "%02x", bytes[i]);
}

#define SCD_CAPTURE_RUN_SLOTS 128U /* kMaxTemplateRunSlots */

/* SEG-079-T002: the 6 bytes at `address` are a live JMP (xxx).L slot: wholly RAM, opcode $4EF9, even operand. */
static int scd_slot_live(const ScdMachine *m, unsigned cpu, uint32_t address) {
  uint32_t i;
  const uint8_t *b[6];
  if ((address & 1U) != 0U) return 0;
  for (i = 0; i < 6U; ++i) {
    b[i] = scd_code_byte(m, cpu, address + i);
    if (b[i] == NULL) return 0;
  }
  return *b[0] == 0x4EU && *b[1] == 0xF9U && (*b[5] & 1U) == 0U;
}

/* SEG-078-T003 (ADR 0205): the live bytes of the RAM window around a frontier and the machine's delivery ledger - the evidence the build driver
 * hands to `segarecomp sega-cd resolve-source` (which finds, or refuses to find, the disc span those bytes came from). The window is the
 * processor's executable-extent RAM (main work RAM or the 2M Word RAM view; sub PRG RAM or its 2M Word RAM view), clipped to the bytes the
 * processor can see now. Returns 0 when region.bin cannot be written. */
static int scd_capture_region(ScdMachine *m, FILE *json, const char *dir) {
  static uint8_t window[0x80000U];
  const uint32_t pc = m->stop.pc & UINT32_C(0x00FFFFFF);
  uint32_t wlo, whi, lo, hi, i;
  char path[4096];
  FILE *f;
  if (m->stop.cpu == SCD_MAIN) {
    if (pc >= UINT32_C(0xFF0000)) { wlo = UINT32_C(0xFF0000); whi = UINT32_C(0x1000000); }
    else if (pc >= UINT32_C(0x200000) && pc < UINT32_C(0x240000)) { wlo = UINT32_C(0x200000); whi = UINT32_C(0x240000); }
    else return 1;
  } else {
    if (pc < SCD_PRG_RAM_BYTES) { wlo = 0U; whi = SCD_PRG_RAM_BYTES; }
    else if (pc >= UINT32_C(0x80000) && pc < UINT32_C(0xC0000)) { wlo = UINT32_C(0x80000); whi = UINT32_C(0xC0000); }
    else return 1;
  }
  (void)scd_cd_observe(m); /* a Word RAM view is current only after the CDC DMA has been brought up to date */
  if (scd_code_byte(m, m->stop.cpu, pc) == NULL) return 1;
  for (lo = pc; lo > wlo && scd_code_byte(m, m->stop.cpu, lo - 1U) != NULL; --lo) {}
  for (hi = pc + 1U; hi < whi && scd_code_byte(m, m->stop.cpu, hi) != NULL; ++hi) {}
  for (i = lo; i < hi; ++i) window[i - lo] = *scd_code_byte(m, m->stop.cpu, i);
  snprintf(path, sizeof path, "%s/region.bin", dir);
  f = fopen(path, "wb");
  if (f == NULL) return 0;
  if (fwrite(window, 1U, hi - lo, f) != hi - lo || fclose(f) != 0) return 0;
  fprintf(json, ",\"region\":{\"base\":%u,\"length\":%u,\"track1_first_lba\":%u,\"delivered_overflow\":%u,\"delivered\":[", lo, hi - lo,
          m->disc->track_count != 0U ? m->disc->tracks[0].first_lba : 0U, (unsigned)m->delivered_overflow);
  for (i = 0; i < m->delivered_count; ++i) fprintf(json, "%s[%u,%u]", i ? "," : "", m->delivered[i].first_lba, m->delivered[i].count);
  fprintf(json, "]}");
  return 1;
}

/* Build pass (ADR 0142 decisions 3 and 7): an unknown epoch image is written to <dir>/epoch.bin; a stop at a RAM PC with no compiled
 * variant for its live bytes is reported as a frontier. Both carry the identity of this program (the provenance chain). */
static int scd_capture(ScdMachine *m, const ScdProgram *program, const char *dir) {
  char path[4096];
  FILE *f;
  if (m->stop.stop_class == SCD_STOP_MATERIALIZATION_CAPTURED && m->capture_kind == 1U) {
    snprintf(path, sizeof path, "%s/epoch.bin", dir);
    f = fopen(path, "wb");
    if (f == NULL || fwrite(m->prg_ram, 1U, m->capture_length, f) != m->capture_length || fclose(f) != 0) return 0;
    snprintf(path, sizeof path, "%s/capture.json", dir);
    f = fopen(path, "w");
    if (f == NULL) return 0;
    fprintf(f, "{\"kind\":\"epoch\",\"pass_identity\":\"%s\",\"length\":%u,\"tick\":%llu}\n", program->program_identity,
            m->capture_length, (unsigned long long)m->now);
    return fclose(f) == 0;
  }
  /* A frontier is a stop at the processor's PC where no compiled variant matched the live bytes: a code-byte mismatch there, or the
   * dispatch default (entry_not_compiled whose detail is that PC). An uncompiled computed or template destination (entry_not_compiled
   * whose detail is the destination) is an ordinary stop, never a new admission. */
  if (m->stop.cpu < 2U && m->stop.pc == m->cpu[m->stop.cpu].pc &&
      (m->stop.stop_class == SCD_STOP_CODE_BYTES_MISMATCH ||
       (m->stop.stop_class == SCD_STOP_ENTRY_NOT_COMPILED && m->stop.address == m->stop.pc))) {
    uint32_t i;
    for (i = 0; i < 8U; ++i) {
      const uint8_t *b = scd_code_byte(m, m->stop.cpu, m->stop.pc + i);
      if (b == NULL) return 1; /* not RAM: an ordinary stop, nothing to capture */
      m->capture_bytes[i] = *b;
    }
    m->capture_kind = 2U;
    snprintf(path, sizeof path, "%s/capture.json", dir);
    f = fopen(path, "w");
    if (f == NULL) return 0;
    fprintf(f, "{\"kind\":\"frontier\",\"pass_identity\":\"%s\",\"cpu\":%u,\"pc\":%u,\"bytes\":\"", program->program_identity,
            m->stop.cpu, m->stop.pc);
    scd_hex(f, m->capture_bytes, 8U);
    fprintf(f, "\"");
    /* SEG-079-T002 (ADR 0180): when the stopped instruction is a JMP (xxx).L slot ($4EF9, even operand), also report the bounded
     * contiguous run of such live slots (6-byte stride, wholly RAM) that contains it, so one admission can cover a jump-slot table. */
    if (m->capture_bytes[0] == 0x4EU && m->capture_bytes[1] == 0xF9U && (m->capture_bytes[5] & 1U) == 0U) {
      uint32_t first = m->stop.pc, last = m->stop.pc; /* slot addresses */
      uint8_t buffer[SCD_CAPTURE_RUN_SLOTS * 6U];
      uint32_t slots = 0U, i;
      while (last - first < (SCD_CAPTURE_RUN_SLOTS - 1U) * 6U && first >= 6U && scd_slot_live(m, m->stop.cpu, first - 6U)) first -= 6U;
      while (last - first < (SCD_CAPTURE_RUN_SLOTS - 1U) * 6U && last <= 0x00FFFFF0U && scd_slot_live(m, m->stop.cpu, last + 6U)) last += 6U;
      slots = (last - first) / 6U + 1U;
      for (i = 0; i < slots * 6U; ++i) buffer[i] = *scd_code_byte(m, m->stop.cpu, first + i);
      fprintf(f, ",\"run\":{\"base\":%u,\"slots\":%u,\"bytes\":\"", first, slots);
      scd_hex(f, buffer, slots * 6U);
      fprintf(f, "\"}");
    }
    if (!scd_capture_region(m, f, dir)) {
      fclose(f);
      return 0;
    }
    fprintf(f, "}\n");
    return fclose(f) == 0;
  }
  return 1;
}

/* SEG-064-T004 diagnostic: the RAM images of the stopped machine, for comparison with a reference (local only; main work RAM, PRG RAM,
 * Word RAM in the 2M layout, PCM wave RAM and the CDC buffer). */
static int scd_dump_file(const char *dir, const char *name, const uint8_t *data, size_t length) {
  char path[4096];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (f == NULL) return 1;
  if (fwrite(data, 1U, length, f) != length) {
    fclose(f);
    return 1;
  }
  return fclose(f) != 0;
}

static int scd_dump_pcm_registers(const ScdMachine *m, const char *dir) {
  char path[4096];
  FILE *f;
  unsigned c;
  snprintf(path, sizeof path, "%s/pcm.txt", dir);
  f = fopen(path, "w");
  if (f == NULL) return 1;
  fprintf(f, "index %u bank %u enabled %u onoff %02X\n", m->pcm.index, m->pcm.bank, m->pcm.enabled, m->pcm.onoff);
  for (c = 0; c < 8U; ++c)
    fprintf(f, "ch %u env %02X pan %02X st %02X fd %04X ls %04X\n", c, m->pcm.env[c], m->pcm.pan[c], m->pcm.st[c], m->pcm.fd[c], m->pcm.ls[c]);
  return fclose(f) != 0;
}

static int scd_dump_state(const ScdMachine *m, const char *dir) {
  return scd_dump_pcm_registers(m, dir) ||
         scd_dump_file(dir, "work_ram.bin", m->work_ram, sizeof m->work_ram) || scd_dump_file(dir, "prg_ram.bin", m->prg_ram, sizeof m->prg_ram) ||
         scd_dump_file(dir, "word_ram_2m.bin", m->word_ram, sizeof m->word_ram) || scd_dump_file(dir, "pcm_ram.bin", m->pcm.ram, sizeof m->pcm.ram) ||
         scd_dump_file(dir, "cdc_buffer.bin", m->cdc.buffer, SCD_CDC_BUFFER_BYTES) ||
         (m->backup_ram_present && scd_dump_file(dir, "backup_ram.bin", m->backup_ram, sizeof m->backup_ram));
}

/* SEG-065-T002: read the backup RAM machine input; exactly SCD_BACKUP_RAM_BYTES bytes, anything else is refused. */
static const char *scd_load_backup_ram(ScdMachine *m, const char *path) {
  static uint8_t bytes[SCD_BACKUP_RAM_BYTES + 1U];
  size_t n;
  FILE *f = fopen(path, "rb");
  if (f == NULL) return "unreadable";
  n = fread(bytes, 1U, sizeof bytes, f);
  if (ferror(f)) n = 0U;
  fclose(f);
  return scd_backup_ram_install(m, bytes, n) == 0 ? NULL : "length";
}

/* SEG-070-T002 (ADR 0149): read, validate and install the input script before any guest instruction; a refusal is typed. */
static int scd_load_input_script(ScdMachine *m, const char *path, int log, int bios_profile) {
  static char text[SCD_INPUT_MAX_SCRIPT_BYTES + 1U];
  const char *error = NULL;
  unsigned line = 0U;
  size_t n = 0U;
  FILE *f = fopen(path, "rb");
  if (!bios_profile) {
    if (f != NULL) fclose(f);
    printf("{\"result\":\"input_refused\",\"error\":\"unsupported_profile\",\"line\":0}\n");
    return 23;
  }
  if (f == NULL) {
    printf("{\"result\":\"input_refused\",\"error\":\"unreadable\",\"line\":0}\n");
    return 23;
  }
  n = fread(text, 1U, sizeof text, f);
  if (ferror(f)) n = sizeof text;
  fclose(f);
  if (scd_input_install(m, text, n, log, &error, &line) != 0) {
    printf("{\"result\":\"input_refused\",\"error\":\"%s\",\"line\":%u}\n", error, line);
    return 23;
  }
  return 0;
}

/* SEG-078-T002 (ADR 0204): `--describe` prints what this program claims about itself - the contract version, its build identities and
 * the BIOS and disc files it will accept (sizes and SHA-256 only) - and runs nothing. A launcher compares these claims with the files the
 * user chose; the run itself re-verifies the BIOS and every disc file against the same table before any guest instruction. */
static void scd_json_name(const char *name) {
  putchar('"');
  for (; *name != '\0'; ++name) {
    if (*name == '"' || *name == '\\') putchar('\\');
    putchar(*name);
  }
  putchar('"');
}

static void scd_describe(const ScdProgram *program, const ScdAuthentic *authentic, int viewer) {
  uint32_t i;
  printf("{\"describe\":1,\"runtime_abi\":%u,\"profile\":\"%s\",\"viewer\":%s,\"program_identity\":\"%s\",\"disc_identity\":\"%s\",",
         SCD_RUNTIME_ABI, authentic != NULL ? "bios_image" : "synthetic", viewer ? "true" : "false", program->program_identity,
         program->disc.identity);
  if (authentic != NULL) printf("\"bios\":{\"size\":%u,\"sha256\":\"%s\"},", authentic->bios_size, authentic->bios_sha256);
  else printf("\"bios\":null,");
  printf("\"disc_files\":[");
  for (i = 0U; i < program->disc.file_count; ++i) {
    printf("%s{\"name\":", i ? "," : "");
    scd_json_name(program->disc.files[i].name);
    printf(",\"size\":%llu,\"sha256\":\"%s\"}", (unsigned long long)program->disc.files[i].size, program->disc.files[i].sha256);
  }
  printf("]}\n");
}

/* SEG-063: `overlays` is the generated overlay registry (NULL for programs without disc-loaded modules). */
static int scd_program_run(const ScdProgram *program, const ScdOverlayProgram *overlays, const ScdAuthentic *authentic, int argc,
                           char **argv);

int scd_program_main_overlays(const ScdProgram *program, const ScdOverlayProgram *overlays, int argc, char **argv) {
  return scd_program_run(program, overlays, NULL, argc, argv);
}

int scd_program_main_bios(const ScdProgram *program, const ScdAuthentic *authentic, int argc, char **argv) {
  return authentic == NULL ? 2 : scd_program_run(program, NULL, authentic, argc, argv);
}

static int scd_program_run(const ScdProgram *program, const ScdOverlayProgram *overlays, const ScdAuthentic *authentic, int argc,
                           char **argv) {
  static ScdMachine machine;
  static GenesisFrameArtifact frame;
  ScdDisc disc = program->disc;
  const char *disc_dir = NULL, *report_path = NULL, *frame_path = NULL, *bios_path = NULL, *materialize_dir = NULL;
  const char *sets[16];
  const char *capture_dir = NULL;
  uint64_t capture_onsets[8];
  unsigned capture_count = 0U;
  const char *dump_dir = NULL, *coverage_path = NULL, *trace_path = NULL, *wlog_path = NULL, *event_path = NULL, *backup_ram_path = NULL, *pcm_trace_path = NULL;
  const char *input_script_path = NULL, *input_log_path = NULL, *record_path = NULL, *cdda_pcm_path = NULL;
  int cdda_requested = 0;
  const ScdLiveHost *const live_host = scd_live_host_registered; /* SEG-078-T001: a viewer registered its host */
  ScdLiveSummary live_summary;
  int live_exit = 0;
  unsigned set_count = 0, i;
  uint64_t budget = 0U, slice_ticks = 0U, slice_seed = 0U, until_tick = UINT64_MAX;
  int have_seed = 0;
  ScdRunResult result;
  char frame_sha[65];
  const char *frame_digest = NULL;
  FILE *report;
  if (argc == 2 && strcmp(argv[1], "--describe") == 0) {
    scd_describe(program, authentic, live_host != NULL);
    return 0;
  }
  for (i = 1; i < (unsigned)argc; i += 2U) {
    const char *flag = argv[i];
    const char *value = (int)(i + 1U) < argc ? argv[i + 1U] : NULL;
    if (value == NULL) return 2;
    if (strcmp(flag, "--disc-dir") == 0) disc_dir = value;
    else if (strcmp(flag, "--instruction-budget") == 0) {
      if (!scd_parse_u64(value, &budget) || budget == 0U) return 2;
    } else if (strcmp(flag, "--report") == 0) report_path = value;
    else if (strcmp(flag, "--frame") == 0) frame_path = value;
    else if (strcmp(flag, "--slice-ticks") == 0) {
      if (!scd_parse_u64(value, &slice_ticks) || slice_ticks == 0U) return 2;
    } else if (strcmp(flag, "--slice-seed") == 0) {
      if (!scd_parse_u64(value, &slice_seed)) return 2;
      have_seed = 1;
    } else if (strcmp(flag, "--set") == 0 && set_count < 16U) sets[set_count++] = value;
    else if (strcmp(flag, "--until-tick") == 0) { /* SEG-064-T005: end the run at a machine-time instant (result `deadline`) */
      if (!scd_parse_u64(value, &until_tick) || until_tick == 0U) return 2;
    } else if (strcmp(flag, "--capture-onset") == 0) { /* SEG-065-T003: frame capture at a vertical-blank onset count */
      if (capture_count >= 8U || !scd_parse_u64(value, &capture_onsets[capture_count]) || capture_onsets[capture_count] < 2U) return 2;
      capture_count += 1U;
    } else if (strcmp(flag, "--capture-dir") == 0) capture_dir = value;
    else if (strcmp(flag, "--backup-ram") == 0) backup_ram_path = value; /* SEG-065-T002: the backup RAM machine input */
    else if (strcmp(flag, "--input-script") == 0) input_script_path = value; /* SEG-070-T002: the timed pad machine input */
    else if (strcmp(flag, "--input-log") == 0) input_log_path = value;       /* its read log (local diagnostic) */
    else if (live_host != NULL && strcmp(flag, "--record-input") == 0) record_path = value; /* SEG-078-T001 */
    else if (strcmp(flag, "--dump-dir") == 0) dump_dir = value;           /* SEG-064-T004 diagnostics (local only) */
    else if (strcmp(flag, "--coverage") == 0) coverage_path = value;
    else if (strcmp(flag, "--access-trace") == 0) trace_path = value;
    else if (strcmp(flag, "--event-log") == 0) event_path = value;
    else if (strcmp(flag, "--pcm-trace") == 0) pcm_trace_path = value; /* SEG-102 diagnostic (local only) */
    else if (strcmp(flag, "--cdda-pcm") == 0) { cdda_pcm_path = value; cdda_requested = 1; } /* SEG-104: canonical CD-DA stream (S16LE stereo, 44.1 kHz) */
    else if (strcmp(flag, "--cdda-digest") == 0) cdda_requested = strcmp(value, "1") == 0; /* SEG-104: `1` = produce CD-DA and report its digest and counters */
    else if (strcmp(flag, "--ram-write-log") == 0) wlog_path = value;     /* SEG-079 diagnostic (local only) */
    else if (authentic != NULL && strcmp(flag, "--bios") == 0) bios_path = value;           /* SEG-064-T002 */
    else if (authentic != NULL && strcmp(flag, "--materialize-dir") == 0) materialize_dir = value;
    else return 2;
  }
  if (disc_dir == NULL || budget == 0U) {
    fprintf(stderr, "usage: %s --disc-dir DIR --instruction-budget N [--report FILE] [--frame FILE] [--slice-ticks T | --slice-seed S]\n",
            argc > 0 ? argv[0] : "program");
    return 2;
  }
  {
    const ScdDiscError error = scd_disc_open(&disc, disc_dir);
    if (error != SCD_DISC_OK) {
      printf("{\"result\":\"disc_refused\",\"error\":\"%s\",\"disc_identity\":\"%s\"}\n", scd_disc_error_name(error), program->disc.identity);
      return 20;
    }
  }
  if (authentic != NULL) { /* SEG-064-T002: refuse a missing or different BIOS before any guest instruction */
    const char *error = NULL;
    const uint8_t *bios = scd_load_bios(authentic, bios_path, &error);
    if (bios == NULL) {
      printf("{\"result\":\"bios_refused\",\"error\":\"%s\",\"bios_sha256\":\"%s\"}\n", error, authentic->bios_sha256);
      scd_disc_close(&disc);
      return 21;
    }
    if (scd_machine_init_bios(&machine, program, authentic, &disc, bios, authentic->bios_size, budget) != 0) {
      scd_disc_close(&disc);
      return 1;
    }
    machine.materialize = materialize_dir != NULL ? 1U : 0U;
  } else if (scd_machine_init(&machine, program, &disc, budget) != 0) {
    printf("{\"result\":\"disc_refused\",\"error\":\"system_area\",\"disc_identity\":\"%s\"}\n", program->disc.identity);
    scd_disc_close(&disc);
    return 20;
  }
  if (backup_ram_path != NULL) {
    const char *error = scd_load_backup_ram(&machine, backup_ram_path);
    if (error != NULL) {
      printf("{\"result\":\"backup_ram_refused\",\"error\":\"%s\"}\n", error);
      scd_disc_close(&disc);
      return 22;
    }
  }
  if (live_host != NULL && authentic == NULL) return 2;                        /* SEG-078-T001: the viewer drives the BIOS-image profile only */
  if (record_path != NULL && input_script_path != NULL) return 2;               /* a replay records nothing: its script is its input */
  if (input_log_path != NULL && input_script_path == NULL && live_host == NULL) return 2;
  if (live_host != NULL && input_script_path == NULL) { /* live input: an empty script the host's pad transitions are appended to */
    if (scd_input_live_begin(&machine, input_log_path != NULL, record_path != NULL) != 0) {
      scd_machine_release(&machine);
      scd_disc_close(&disc);
      return 1;
    }
  }
  if (input_script_path != NULL) {
    const int refused = scd_load_input_script(&machine, input_script_path, input_log_path != NULL, authentic != NULL);
    if (refused != 0) {
      scd_machine_release(&machine);
      scd_disc_close(&disc);
      return refused;
    }
  }
  if ((capture_count != 0U) != (capture_dir != NULL) || (capture_count != 0U && authentic == NULL)) return 2;
  for (i = 0; i < capture_count; ++i)
    if (scd_vdp_capture_request(&machine, capture_onsets[i], capture_dir) != 0) return 2;
  scd_overlay_attach(&machine, overlays);
  scd_dynamic_attach(&machine); /* SEG-100: idle for a program without dynamic images */
  if (coverage_path != NULL && scd_coverage_enable(&machine) != 0) return 1;
  if (trace_path != NULL && scd_trace_open(&machine, trace_path) != 0) return 1;
  if (wlog_path != NULL && scd_wlog_open(&machine, wlog_path) != 0) return 1;
  if (event_path != NULL && scd_event_open(&machine, event_path) != 0) return 1;
  if (pcm_trace_path != NULL && scd_pcm_trace_open(&machine, pcm_trace_path) != 0) return 1;
  for (i = 0; i < set_count; ++i)
    if (!scd_apply_set(&machine, sets[i])) return 2;
  if (cdda_requested && authentic == NULL) return 2; /* SEG-104: CD-DA is a BIOS-image profile output (the synthetic profile keeps its typed stop) */
  if (authentic != NULL && (cdda_requested || (live_host != NULL && live_host->audio != NULL))) { /* SEG-104: attach the CD-DA tap */
    memset(&g_cdda_tap, 0, sizeof g_cdda_tap);
    genesis_sha256_init(&g_cdda_tap.sha);
    g_cdda_tap.host = live_host;
    if (cdda_pcm_path != NULL) {
      g_cdda_tap.pcm = fopen(cdda_pcm_path, "wb");
      if (g_cdda_tap.pcm == NULL) return 1;
    }
    machine.cdda.sink = scd_cdda_tap;
    machine.cdda.sink_context = &g_cdda_tap;
    machine.cdda.enabled = 1U;
  }
  memset(&live_summary, 0, sizeof live_summary);
  if (live_host != NULL) {
    ScdLiveConfig live_config;
    memset(&live_config, 0, sizeof live_config);
    live_config.sample_pad = input_script_path == NULL; /* a script drives the machine (replay): the host pad is not sampled */
    live_config.slice_ticks = slice_ticks;
    live_config.have_seed = have_seed;
    live_config.slice_seed = slice_seed;
    live_config.until_tick = until_tick;
    live_summary = scd_live_run(&machine, live_host, &live_config);
    result = live_summary.result;
    live_exit = live_summary.outcome == SCD_LIVE_HOST_ERROR ? 24 : live_summary.outcome == SCD_LIVE_RECORDING_CAPACITY ? 25
                : live_summary.outcome == SCD_LIVE_RECORDING_REFUSED ? 26 : 0;
  } else if (slice_ticks == 0U && !have_seed) {
    result = scd_machine_run(&machine, until_tick);
  } else {
    uint64_t deadline = 0U, state = slice_seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    do {
      uint64_t step = slice_ticks;
      if (have_seed) { /* a deterministic pseudo-random partition: 1 tick .. ~2^40 ticks */
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        step = (state >> 24) & ((UINT64_C(1) << ((state >> 58) & 0x3FU) % 41U) - 1U);
        step += 1U;
      }
      deadline = deadline > UINT64_MAX - step ? UINT64_MAX : deadline + step;
      if (deadline > until_tick) deadline = until_tick;
      result = scd_machine_run(&machine, deadline);
    } while (result == SCD_RUN_DEADLINE && deadline < until_tick);
  }
  {
    const GenesisRuntime *host = (const GenesisRuntime *)machine.vdp_host;
    if (genesis_vdp_produce_frame(host->devices.vdp.vram, host->devices.vdp.vsram, host->devices.vdp.cram,
                                  host->devices.vdp.registers, &frame) == 0) {
      for (i = 0; i < 32U; ++i) snprintf(frame_sha + 2U * i, 3U, "%02x", frame.frame_digest[i]);
      frame_digest = frame_sha;
      if (frame_path != NULL && genesis_frame_export_ppm_to_path(&frame, frame_path) != GENESIS_FRAME_EXPORT_STATUS_OK) {
        fprintf(stderr, "frame export failed\n");
        return 1;
      }
    }
  }
  if (coverage_path != NULL && scd_coverage_write(&machine, coverage_path) != 0) return 1;
  if (dump_dir != NULL && scd_dump_state(&machine, dump_dir) != 0) return 1;
  if (input_log_path != NULL && scd_input_write_log(&machine, input_log_path) != 0) return 1;
  if (record_path != NULL && scd_input_live_write(&machine, record_path) != 0) return 27; /* SEG-078-T001: the recorded script, also after a quit or a stop */
  if (materialize_dir != NULL && result == SCD_RUN_STOPPED && !scd_capture(&machine, program, materialize_dir)) return 1;
  report = report_path != NULL ? fopen(report_path, "w") : stdout;
  if (report == NULL) return 1;
  scd_write_report(report, &machine, program, result, frame_digest, live_host != NULL ? &live_summary : NULL);
  if (report != stdout && fclose(report) != 0) return 1;
  if (g_cdda_tap.pcm != NULL && fclose(g_cdda_tap.pcm) != 0) return 1;
  scd_trace_close(&machine);
  scd_wlog_close(&machine);
  scd_event_close(&machine);
  scd_pcm_trace_close(&machine);
  scd_coverage_release(&machine);
  scd_dynamic_release(&machine);
  scd_machine_release(&machine);
  scd_disc_close(&disc);
  if (live_exit != 0) return live_exit;
  if (live_host != NULL && live_summary.outcome == SCD_LIVE_QUIT) return 0;
  return result == SCD_RUN_IDLE ? 0 : result == SCD_RUN_STOPPED ? 10 : 11;
}
