/* SEG-147-T004/T005 (ADR 0325): see sms_headless_mod.h. Strict C11. */
#include "sms_headless_mod.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_mod_host.h"
#include "sms_render.h"
#include "sms_snapshot.h"
#include "sms_vdp.h"

SmsRenderer *sms_headless_renderer(void); /* sms_devices_vdp.c */

static const char *ram_regions, *save_path, *load_path;
static uint64_t checkpoint;
static int have_checkpoint;
static char extra[256];

/* The checkpoint the cycle budget never allowed: stated, so that its absence can never be read as success. */
static void not_reached(void) {
  const size_t used = strlen(extra);
  snprintf(extra + used, sizeof extra - used, ",\"roundtrip\":{\"status\":\"not_reached\",\"frame\":%llu}", (unsigned long long)checkpoint);
}

int sms_headless_mod_option(int argc, char **argv, int *index) {
  const char *arg = argv[*index];
  const char *value = *index + 1 < argc ? argv[*index + 1] : NULL;
  if (strncmp(arg, "--mod-", 6) != 0) return 0;
  if (value == NULL) return -1;
  if (strcmp(arg, "--mod-ram") == 0) ram_regions = value;
  else if (strcmp(arg, "--mod-state-save") == 0) save_path = value;
  else if (strcmp(arg, "--mod-state-load") == 0) load_path = value;
  else if (strcmp(arg, "--mod-checkpoint") == 0) {
    char *end = NULL;
    unsigned long long v;
    if (*value < '0' || *value > '9') return -1;
    errno = 0;
    v = strtoull(value, &end, 10);
    if (errno == ERANGE || end == NULL || *end != '\0' || v == 0u || v > 1000000u) return -1;
    checkpoint = (uint64_t)v;
    have_checkpoint = 1;
  } else {
    return -1;
  }
  ++*index;
  return 1;
}

int sms_headless_mod_attach(SmsMachine *machine, SmsPad *pad) {
  if (sms_mod_host_attach(machine, pad, sms_vdp_from_machine(machine), sms_headless_renderer()) != 0) return 1;
  if (!sms_mod_host_observe(ram_regions)) {
    fputs("MOD_REFUSED {\"class\":\"E_REGION\"}\n", stderr);
    return 1;
  }
  return 0;
}

static SmsHeadlessModStart refused(const char *cls, const char *why) {
  fprintf(stderr, "MOD_REFUSED {\"class\":\"%s\",\"reason\":\"%s\"}\n", cls, why);
  return SMS_HEADLESS_MOD_REFUSED;
}

static void digest_hex(const uint8_t *data, size_t size, char out[65]) {
  static const char digits[] = "0123456789abcdef";
  uint8_t d[32];
  SmsSha256 sha;
  unsigned i;
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, data, size);
  sms_sha256_final(&sha, d);
  for (i = 0u; i < 32u; ++i) {
    out[2u * i] = digits[d[i] >> 4];
    out[2u * i + 1u] = digits[d[i] & 15u];
  }
  out[64] = '\0';
}

SmsHeadlessModStart sms_headless_mod_before_run(SmsMachine *machine, SmsPsg *psg, SmsPad *pad, uint64_t cycle_budget, uint64_t frames) {
  static uint8_t file[SMS_SNAPSHOT_MAX_FILE_BYTES + 1u];
  SmsSnapshotMachine m;
  SmsSnapshotIdentity identity;
  memset(&m, 0, sizeof m);
  m.machine = machine;
  m.vdp = sms_vdp_from_machine(machine);
  m.renderer = sms_headless_renderer();
  m.psg = psg;
  m.pad = pad;
  m.script_input = 1;
  m.mod = sms_mod_host_state();
  sms_mod_host_identity(identity.source_sha256, identity.variant_sha256);
  if (save_path != NULL && !have_checkpoint) return refused("E_ARGUMENT", "--mod-state-save needs --mod-checkpoint");
  if (load_path != NULL) {
    SmsSnapshotStatus st;
    FILE *f = fopen(load_path, "rb");
    size_t size;
    char sha[65];
    if (f == NULL) return refused("E_STATE_RESTORE", "the state file cannot be opened");
    size = fread(file, 1, sizeof file, f);
    fclose(f);
    st = sms_snapshot_restore(&m, &identity, file, size);
    if (st != SMS_SNAPSHOT_OK) return refused("E_STATE_RESTORE", sms_snapshot_status_text(st));
    digest_hex(file, size, sha);
    snprintf(extra, sizeof extra, ",\"loaded_state_sha256\":\"%s\"", sha);
    if (cycle_budget != SMS_NO_LIMIT && machine->rt.state.cycles > cycle_budget) {
      if (have_checkpoint) not_reached();
      return SMS_HEADLESS_MOD_BUDGET_SPENT;
    }
    if (have_checkpoint && machine->rt.state.cycles / SMS_CYCLES_PER_FRAME > checkpoint) return refused("E_ARGUMENT", "the loaded state is already past the checkpoint");
  }
  if (have_checkpoint) {
    SmsSnapshotBuffer first, second;
    SmsSnapshotStatus st;
    SmsStop stop;
    char sha[65];
    size_t used = strlen(extra);
    int identical;
    if (frames != SMS_NO_LIMIT && checkpoint > frames) return refused("E_ARGUMENT", "the checkpoint lies beyond --frames");
    stop = sms_run_bounded(machine, cycle_budget, checkpoint);
    if (stop.kind == SMS_STOP_CYCLE_BUDGET) { /* the budget ends before the checkpoint: no evidence, no file; the run reports the budget */
      not_reached();
      return SMS_HEADLESS_MOD_RUN;
    }
    if (stop.kind != SMS_STOP_FRAME) return refused("E_STATE_RESTORE", "the guest stopped before the checkpoint");
    sms_psg_sync(psg, stop.cycles); /* the barrier: the PSG follows the guest clock (the codec requires it, as the viewer does every frame) */
    st = sms_snapshot_capture(&m, &identity, NULL, &first);
    if (st != SMS_SNAPSHOT_OK) return refused("E_STATE_UNAVAILABLE", sms_snapshot_status_text(st));
    st = sms_snapshot_restore(&m, &identity, first.data, first.size);
    if (st != SMS_SNAPSHOT_OK) { sms_snapshot_buffer_free(&first); return refused("E_STATE_RESTORE", sms_snapshot_status_text(st)); }
    st = sms_snapshot_capture(&m, &identity, NULL, &second);
    if (st != SMS_SNAPSHOT_OK) { sms_snapshot_buffer_free(&first); return refused("E_STATE_RESTORE", sms_snapshot_status_text(st)); }
    identical = first.size == second.size && memcmp(first.data, second.data, first.size) == 0;
    if (identical && save_path != NULL) { /* only a verified state becomes a file */
      FILE *f = fopen(save_path, "wb");
      int ok = f != NULL && fwrite(first.data, 1, first.size, f) == first.size;
      if (f != NULL && fclose(f) != 0) ok = 0;
      if (!ok) {
        if (f != NULL) (void)remove(save_path);
        sms_snapshot_buffer_free(&first);
        sms_snapshot_buffer_free(&second);
        return refused("E_STATE_RESTORE", "the state file cannot be written");
      }
    }
    digest_hex(first.data, first.size, sha);
    snprintf(extra + used, sizeof extra - used, ",\"roundtrip\":{\"status\":\"%s\",\"frame\":%llu,\"state_sha256\":\"%s\"}", identical ? "ok" : "differs", (unsigned long long)checkpoint, sha);
    sms_snapshot_buffer_free(&first);
    sms_snapshot_buffer_free(&second);
  }
  return SMS_HEADLESS_MOD_RUN;
}

int sms_headless_mod_finish(const char *artifacts) {
  char path[1024];
  FILE *f;
  uint32_t height = 0u;
  const uint8_t *rgb = sms_mod_host_last_presented(&height);
  int ok = 1;
  sms_mod_host_summary(stderr, extra);
  if (artifacts == NULL) return 1;
  if (snprintf(path, sizeof path, "%s/mod.json", artifacts) >= (int)sizeof path) return 0;
  f = fopen(path, "wb");
  if (f == NULL) return 0;
  sms_mod_host_summary(f, extra);
  ok &= fclose(f) == 0;
  if (rgb != NULL) {
    if (snprintf(path, sizeof path, "%s/presented.rgb", artifacts) >= (int)sizeof path) return 0;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    ok &= fwrite(rgb, 1, (size_t)SMS_FB_WIDTH * height * 3u, f) == (size_t)SMS_FB_WIDTH * height * 3u;
    ok &= fclose(f) == 0;
  }
  return ok;
}
