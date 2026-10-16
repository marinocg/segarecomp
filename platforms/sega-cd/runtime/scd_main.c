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
  fprintf(out, "],\"ga\":{\"sub_reset\":%u,\"sub_busreq\":%u,\"ifl2\":%u,\"write_protect\":%u,\"prg_bank\":%u,\"mode_1m\":%u,\"ret\":%u,"
               "\"dmna\":%u,\"main_flag\":%u,\"sub_flag\":%u,\"interrupt_enable\":%u,\"pending\":%u,\"comcmd\":[",
          m->ga.sub_reset, m->ga.sub_busreq, m->ga.ifl2, m->ga.write_protect, m->ga.prg_bank, m->ga.mode_1m, m->ga.ret, m->ga.dmna,
          m->ga.main_flag, m->ga.sub_flag, m->ga.interrupt_enable, m->ga.pending);
  for (i = 0; i < 8U; ++i) fprintf(out, "%s%u", i ? "," : "", m->ga.comcmd[i]);
  fprintf(out, "],\"comstat\":[");
  for (i = 0; i < 8U; ++i) fprintf(out, "%s%u", i ? "," : "", m->ga.comstat[i]);
  fprintf(out, "]},\"sub_interrupts_taken\":[");
  for (i = 0; i < 8U; ++i) fprintf(out, "%s%llu", i ? "," : "", (unsigned long long)m->sub_interrupts_taken[i]);
  fprintf(out, "],\"prg_write_protect_drops\":%llu,\"device_log\":\"%016llx\",\"digests\":{", (unsigned long long)m->prg_write_protect_drops,
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
  fprintf(out, "\",\"cdc\":{\"edt\":%u,\"dma_active\":%u,\"dma_done\":%u,\"dma_total\":%u,\"pt\":%u,\"dac\":%u,\"dma_address\":%u}",
          m->cdc.edt, m->cdc.dma_active, m->cdc.dma_done, m->cdc.dma_total, m->cdc.pt, m->cdc.dac, m->cdc.dma_address);
  fprintf(out, ",\"cdd\":{\"status\":%u,\"lba\":%u,\"delivered\":%llu},\"frame_sha256\":%s%s%s,\"instructions\":%llu}\n", m->cdd.status,
          m->cdd.lba, (unsigned long long)m->cdd.delivered, frame_sha ? "\"" : "", frame_sha ? frame_sha : "null", frame_sha ? "\"" : "",
          (unsigned long long)(m->cpu[0].retired + m->cpu[1].retired));
}

int scd_program_main(const ScdProgram *program, int argc, char **argv) {
  static ScdMachine machine;
  static GenesisFrameArtifact frame;
  ScdDisc disc = program->disc;
  const char *disc_dir = NULL, *report_path = NULL, *frame_path = NULL;
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
  if (scd_machine_init(&machine, program, &disc, budget) != 0) {
    printf("{\"result\":\"disc_refused\",\"error\":\"system_area\",\"disc_identity\":\"%s\"}\n", program->disc.identity);
    scd_disc_close(&disc);
    return 20;
  }
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
  report = report_path != NULL ? fopen(report_path, "w") : stdout;
  if (report == NULL) return 1;
  scd_write_report(report, &machine, program, result, frame_digest);
  if (report != stdout && fclose(report) != 0) return 1;
  scd_machine_release(&machine);
  scd_disc_close(&disc);
  return result == SCD_RUN_IDLE ? 0 : result == SCD_RUN_STOPPED ? 10 : 11;
}
