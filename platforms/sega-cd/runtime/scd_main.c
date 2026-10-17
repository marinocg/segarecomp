/* SEG-062: the generated Sega CD program's driver. Parses the command line, opens and verifies the external disc against the
 * build-time identity, establishes the synthetic post-BIOS profile, runs both processors under a finite instruction budget
 * (optionally in deadline slices: the result must not depend on them), and writes a JSON report and, on request, the VDP frame.
 *
 *   program --disc-dir DIR --instruction-budget N [--report FILE] [--frame FILE.ppm] [--slice-ticks T | --slice-seed S]
 *           [--set main|sub:dN|aN=VALUE]...
 *
 * Exit status: 0 idle (success), 10 typed stop, 11 budget exhausted, 20 disc refused at start-up, 2 usage. */
#include "scd_runtime.h"

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

static void scd_write_report(FILE *out, const ScdMachine *m, const ScdProgram *program, ScdRunResult result, const char *frame_sha) {
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
  fprintf(out, ",\"cdd\":{\"status\":%u,\"lba\":%ld,\"index\":%u,\"latency\":%u,\"pending\":%u,\"delivered\":%llu,\"rx\":[",
          m->cdd.status, (long)m->cdd.lba, m->cdd.index, m->cdd.latency, m->cdd.pending, (unsigned long long)m->cdd.delivered);
  for (i = 0; i < 10U; ++i) fprintf(out, "%s%u", i ? "," : "", m->cdd.rx[i]);
  fprintf(out, "],\"tx\":[");
  for (i = 0; i < 10U; ++i) fprintf(out, "%s%u", i ? "," : "", m->cdd.tx[i]);
  fprintf(out, "]},\"frame_sha256\":%s%s%s,\"instructions\":%llu", frame_sha ? "\"" : "", frame_sha ? frame_sha : "null",
          frame_sha ? "\"" : "", (unsigned long long)(m->cpu[0].retired + m->cpu[1].retired));
  if (authentic != NULL) /* SEG-064-T002: BIOS-image profile (absent for synthetic programs: their reports are unchanged) */
    fprintf(out, ",\"bios_profile\":{\"epochs\":%u,\"epoch_image\":%d,\"epoch_tick\":%llu,\"sub_pristine\":%u}", m->epochs,
            m->epoch_image, (unsigned long long)m->epoch_tick, m->sub_pristine);
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
  fprintf(out, "}\n");
}

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
  if ((m->stop.stop_class == SCD_STOP_ENTRY_NOT_COMPILED || m->stop.stop_class == SCD_STOP_CODE_BYTES_MISMATCH) &&
      m->stop.cpu < 2U && m->stop.pc == m->cpu[m->stop.cpu].pc) {
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
    fprintf(f, "\"}\n");
    return fclose(f) == 0;
  }
  return 1;
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
  unsigned set_count = 0, i;
  uint64_t budget = 0U, slice_ticks = 0U, slice_seed = 0U;
  int have_seed = 0;
  ScdRunResult result;
  char frame_sha[65];
  const char *frame_digest = NULL;
  FILE *report;
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
  scd_overlay_attach(&machine, overlays);
  for (i = 0; i < set_count; ++i)
    if (!scd_apply_set(&machine, sets[i])) return 2;
  if (slice_ticks == 0U && !have_seed) {
    result = scd_machine_run(&machine, UINT64_MAX);
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
      result = scd_machine_run(&machine, deadline);
    } while (result == SCD_RUN_DEADLINE);
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
  if (materialize_dir != NULL && result == SCD_RUN_STOPPED && !scd_capture(&machine, program, materialize_dir)) return 1;
  report = report_path != NULL ? fopen(report_path, "w") : stdout;
  if (report == NULL) return 1;
  scd_write_report(report, &machine, program, result, frame_digest);
  if (report != stdout && fclose(report) != 0) return 1;
  scd_machine_release(&machine);
  scd_disc_close(&disc);
  return result == SCD_RUN_IDLE ? 0 : result == SCD_RUN_STOPPED ? 10 : 11;
}
