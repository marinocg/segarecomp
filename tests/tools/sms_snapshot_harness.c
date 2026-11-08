/*
 * SEG-132: drives the REAL Master System viewer rig (machine, VDP, renderer, PSG ring, pad -- the same composition the viewer runs) with a project-authored
 * fixture ROM and exercises sms_snapshot_capture / _restore / _validate. Plain C11; linked with the generated program (z80_run, sms_rom_*).
 *
 *   sms_snapshot_harness <script>
 *
 * One command per line (decimal numbers, hex only where stated). Every command prints one result line.
 *   input <path>             install a scripted-input schedule from a file (script mode) | live  (live mode, the default)
 *   run <frame>              run to the start of absolute frame <frame>, frame by frame: sms_run_until_frame, PSG sync, ring drain (the viewer's order)
 *   snap N | restore N | validate N     capture into slot N / restore from slot N / staged validation only  (prints "<op> N <status text key>")
 *   eq N M                   print EQ when slots N and M hold identical bytes
 *   sha                      SHA-256 of a fresh capture (the canonical state bytes, no thumbnail)         digest   sms_machine_digest
 *   layout N                 "SEC <id> <offset> <length>" per section of slot N
 *   poke N <sec> <off> <xor> xor one byte inside section <sec> of slot N (offset within the section)        redigest N   recompute the trailing SHA-256
 *   tamper N <off> <xor>     xor one raw byte of slot N       trunc N <len>      cut slot N         grow N   append a zero byte
 *   ident <byte>             xor the source identity's first byte (the restore identity)               variant <byte>   the same for the variant
 *   family <n>               set the memory's mapper family (identity test; restore with the original value)
 *   psgcfg <xor>             xor the PSG noise taps (an incompatible chip variant)
 *   fault                    latch a memory error (a latched stop)
 *   pcm_ref | pcm_cmp        copy the PCM produced so far as the reference | compare what was produced since the last restore with the reference
 *   state                    witnesses: frame, cycles, pc, mapper regs, irq_mask, vdp line, pcm index
 *   mutate_all               change every restorable field (a dirty machine for the no-mutation witness): runs 3 more frames
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sms_snapshot.h"
#include "sms_viewer.h"

extern const uint8_t sms_rom_data[];
extern const uint32_t sms_rom_size;
extern const uint32_t sms_rom_mapper_family;

#define SLOTS 128
#define PCM_MAX 400000U

static SmsViewerRig rig;
static SmsSnapshotBuffer slots[SLOTS];
static SmsSnapshotIdentity identity;
static SmsSnapshotMachine snap;
static int16_t *pcm_cur, *pcm_ref;
static uint64_t pcm_cur_len, pcm_ref_len, pcm_since, pcm_next;
static SmsInputEvent script_events[4096];

static void hex(const uint8_t *p, size_t n) { size_t i; for (i = 0; i < n; ++i) printf("%02x", (unsigned)p[i]); }

static void drain(void) {
  int16_t buf[512];
  uint64_t first = 0;
  uint32_t n;
  while ((n = sn76489_ring_read(&rig.ring, buf, 512U, &first)) != 0U) {
    (void)first;   /* the ring counts what it was handed; the PCM stream's own index is what a restore must continue */
    if (pcm_next + n <= PCM_MAX) memcpy(&pcm_cur[pcm_next], buf, n * sizeof(int16_t));
    pcm_next += n;
    if (pcm_next > pcm_cur_len) pcm_cur_len = pcm_next;
  }
}

static void run_to(uint64_t frame) {
  while (rig.machine.rt.state.cycles / SMS_CYCLES_PER_FRAME < frame) {
    const SmsStop stop = sms_run_until_frame(&rig.machine, rig.machine.rt.state.cycles / SMS_CYCLES_PER_FRAME + 1U);
    sms_psg_sync(&rig.psg, stop.cycles);
    drain();
    if (!sms_stop_is_resumable(stop.kind)) { printf("STOP %s\n", sms_stop_kind_name(stop.kind)); return; }
  }
}

static void sha_of(const uint8_t *d, size_t n, uint8_t out[32]) {
  SmsSha256 s;
  sms_sha256_init(&s);
  sms_sha256_update(&s, d, n);
  sms_sha256_final(&s, out);
}

static void redigest(SmsSnapshotBuffer *b) {
  if (b->size >= 32U) sha_of(b->data, b->size - 32U, b->data + b->size - 32U);
}

static const char *status_key(SmsSnapshotStatus s) {
  static const char *keys[] = {"OK", "BAD_ARGUMENT", "NO_MEMORY", "TOO_LARGE", "TRUNCATED", "BAD_MAGIC", "UNSUPPORTED_VERSION", "BAD_LAYOUT", "UNKNOWN_SECTION",
                               "INTEGRITY", "WRONG_CARTRIDGE", "INCOMPATIBLE", "INVALID_STATE", "NOT_CAPTURABLE"};
  return (unsigned)s < sizeof keys / sizeof keys[0] ? keys[s] : "?";
}

static int sec_find(const SmsSnapshotBuffer *b, unsigned id, uint32_t *offset, uint32_t *length) {
  uint32_t count, i;
  if (b->data == NULL || b->size < 156U + 32U) return 0;
  count = (uint32_t)b->data[28] | ((uint32_t)b->data[29] << 8) | ((uint32_t)b->data[30] << 16) | ((uint32_t)b->data[31] << 24);
  for (i = 0; i < count && 156U + (i + 1U) * 12U <= b->size; ++i) {
    const uint8_t *e = b->data + 156U + i * 12U;
    if ((unsigned)(e[0] | (e[1] << 8)) == id) {
      *offset = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
      *length = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
      return 1;
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  FILE *f;
  char line[512];
  SmsError error;
  if (argc != 2 || (f = fopen(argv[1], "r")) == NULL) { fprintf(stderr, "usage: sms_snapshot_harness <script>\n"); return 64; }
  pcm_cur = (int16_t *)calloc(PCM_MAX, sizeof(int16_t));
  pcm_ref = (int16_t *)calloc(PCM_MAX, sizeof(int16_t));
  if (pcm_cur == NULL || pcm_ref == NULL) return 70;
  if (!sms_viewer_rig_init(&rig, sms_rom_data, sms_rom_size, (SmsMapperFamily)sms_rom_mapper_family, &error)) { printf("RIG_FAILED %s\n", sms_error_name(error)); return 4; }
  memset(&identity, 0, sizeof identity);
  memset(identity.source_sha256, 0x5A, 32U);
  snap.machine = &rig.machine; snap.vdp = &rig.vdp; snap.renderer = &rig.renderer; snap.psg = &rig.psg; snap.pad = &rig.pad; snap.script_input = 0;
  while (fgets(line, sizeof line, f) != NULL) {
    char op[32] = "";
    unsigned long a = 0, b = 0, c = 0;
    char path[400] = "";
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%31s", op) != 1) continue;
    if (strcmp(op, "input") == 0) {
      if (sscanf(line, "%*s %399s", path) != 1) return 64;
      if (strcmp(path, "live") == 0) {
        snap.script_input = 0;
        printf("INPUT live\n");
      } else {
        FILE *in = fopen(path, "rb");
        static char text[1 << 16];
        size_t n = in != NULL ? fread(text, 1, sizeof text, in) : 0U;
        uint32_t count = 0;
        if (in != NULL) fclose(in);
        if (sms_input_parse(text, n, script_events, 4096U, &count) != 0U) { printf("INPUT bad\n"); return 64; }
        memcpy(rig.events, script_events, count * sizeof(SmsInputEvent));
        rig.event_count = count;
        sms_machine_set_input(&rig.machine, rig.events, count);
        snap.script_input = 1;
        printf("INPUT script %u\n", (unsigned)count);
      }
    } else if (strcmp(op, "run") == 0) {
      sscanf(line, "%*s %lu", &a);
      run_to(a);
      printf("RUN frame %llu cycles %llu\n", (unsigned long long)(rig.machine.rt.state.cycles / SMS_CYCLES_PER_FRAME), (unsigned long long)rig.machine.rt.state.cycles);
    } else if (strcmp(op, "snap") == 0) {
      SmsSnapshotStatus st;
      sscanf(line, "%*s %lu", &a);
      sms_snapshot_buffer_free(&slots[a]);
      st = sms_snapshot_capture(&snap, &identity, NULL, &slots[a]);
      printf("SNAP %lu %s %u\n", a, status_key(st), (unsigned)slots[a].size);
    } else if (strcmp(op, "restore") == 0 || strcmp(op, "validate") == 0) {
      SmsSnapshotStatus st;
      sscanf(line, "%*s %lu", &a);
      st = op[0] == 'r' ? sms_snapshot_restore(&snap, &identity, slots[a].data, slots[a].size) : sms_snapshot_validate(&snap, &identity, slots[a].data, slots[a].size, NULL);
      if (op[0] == 'r' && st == SMS_SNAPSHOT_OK) { sn76489_ring_clear(&rig.ring); pcm_since = rig.psg.pcm.index; pcm_next = pcm_since; }
      printf("%s %lu %s\n", op[0] == 'r' ? "RESTORE" : "VALIDATE", a, status_key(st));
    } else if (strcmp(op, "eq") == 0) {
      sscanf(line, "%*s %lu %lu", &a, &b);
      printf("%s %lu %lu\n", slots[a].size == slots[b].size && slots[a].data != NULL && memcmp(slots[a].data, slots[b].data, slots[a].size) == 0 ? "EQ" : "NE", a, b);
    } else if (strcmp(op, "sha") == 0 || strcmp(op, "digest") == 0) {
      uint8_t out[32];
      if (op[0] == 's') {
        SmsSnapshotBuffer buf;
        if (sms_snapshot_capture(&snap, &identity, NULL, &buf) != SMS_SNAPSHOT_OK) { printf("SHA none\n"); continue; }
        sha_of(buf.data, buf.size, out);
        sms_snapshot_buffer_free(&buf);
      } else {
        sms_machine_digest(&rig.machine, out);
      }
      printf("%s ", op[0] == 's' ? "SHA" : "DIGEST");
      hex(out, 32U);
      printf("\n");
    } else if (strcmp(op, "layout") == 0) {
      unsigned id;
      sscanf(line, "%*s %lu", &a);
      printf("HDR %u\n", (unsigned)slots[a].size);
      for (id = 1; id <= 7U; ++id) {
        uint32_t off, len;
        if (sec_find(&slots[a], id, &off, &len)) printf("SEC %u %u %u\n", id, (unsigned)off, (unsigned)len);
      }
    } else if (strcmp(op, "poke") == 0) {
      uint32_t off, len;
      {
        unsigned long x = 0;
        sscanf(line, "%*s %lu %lu %lu %lx", &a, &b, &c, &x);
        if (!sec_find(&slots[a], (unsigned)b, &off, &len) || c >= len) { printf("POKE bad\n"); continue; }
        slots[a].data[off + c] ^= (uint8_t)x;
        printf("POKE ok\n");
      }
    } else if (strcmp(op, "redigest") == 0) {
      sscanf(line, "%*s %lu", &a);
      redigest(&slots[a]);
      printf("REDIGEST %lu\n", a);
    } else if (strcmp(op, "tamper") == 0) {
      unsigned long x = 0;
      sscanf(line, "%*s %lu %lu %lx", &a, &b, &x);
      if (b < slots[a].size) slots[a].data[b] ^= (uint8_t)x;
      printf("TAMPER %lu\n", a);
    } else if (strcmp(op, "trunc") == 0) {
      sscanf(line, "%*s %lu %lu", &a, &b);
      if (b < slots[a].size) slots[a].size = (uint32_t)b;
      printf("TRUNC %lu\n", a);
    } else if (strcmp(op, "grow") == 0) {
      uint8_t *g;
      sscanf(line, "%*s %lu", &a);
      g = (uint8_t *)realloc(slots[a].data, slots[a].size + 1U);
      if (g != NULL) { slots[a].data = g; slots[a].data[slots[a].size++] = 0U; }
      printf("GROW %lu\n", a);
    } else if (strcmp(op, "ident") == 0) {
      unsigned long x = 0;
      sscanf(line, "%*s %lx", &x);
      identity.source_sha256[0] ^= (uint8_t)x;
      printf("IDENT\n");
    } else if (strcmp(op, "variant") == 0) {
      unsigned long x = 0;
      sscanf(line, "%*s %lx", &x);
      identity.variant_sha256[0] ^= (uint8_t)x;
      printf("VARIANT\n");
    } else if (strcmp(op, "family") == 0) {
      sscanf(line, "%*s %lu", &a);
      rig.machine.mem.family = (SmsMapperFamily)a;
      printf("FAMILY %lu\n", a);
    } else if (strcmp(op, "psgcfg") == 0) {
      sscanf(line, "%*s %lx", &a);
      rig.psg.chip.config.noise_taps = (uint16_t)(rig.psg.chip.config.noise_taps ^ a);
      printf("PSGCFG %lx\n", a);
    } else if (strcmp(op, "fault") == 0) {
      sms_memory_latch_error(&rig.machine.mem, SMS_ERROR_UNMAPPED_READ, 0x9000U, 0xFFU, rig.machine.rt.state.cycles);
      printf("FAULT\n");
    } else if (strcmp(op, "pcm_ref") == 0) {
      memcpy(pcm_ref, pcm_cur, PCM_MAX * sizeof(int16_t));
      pcm_ref_len = pcm_cur_len;
      printf("PCM_REF %llu\n", (unsigned long long)pcm_ref_len);
    } else if (strcmp(op, "pcm_cmp") == 0) {
      const int same = pcm_cur_len == pcm_ref_len && pcm_since <= pcm_cur_len &&
                       memcmp(&pcm_cur[pcm_since], &pcm_ref[pcm_since], (size_t)(pcm_cur_len - pcm_since) * sizeof(int16_t)) == 0;
      printf("PCM_CMP %s since %llu len %llu\n", same ? "EQ" : "NE", (unsigned long long)pcm_since, (unsigned long long)pcm_cur_len);
    } else if (strcmp(op, "state") == 0) {
      const SmsMachine *m = &rig.machine;
      printf("STATE frame %llu cycles %llu pc %04x banks %02x%02x%02x%02x irq %u line %u pcm_index %llu mem_ctl %02x\n", (unsigned long long)(m->rt.state.cycles / SMS_CYCLES_PER_FRAME),
             (unsigned long long)m->rt.state.cycles, (unsigned)m->rt.state.pc, m->mem.mapper_regs[0], m->mem.mapper_regs[1], m->mem.mapper_regs[2], m->mem.mapper_regs[3],
             (unsigned)m->irq_mask, (unsigned)rig.vdp.line, (unsigned long long)rig.psg.pcm.index, (unsigned)m->mem.memory_control);
    } else if (strcmp(op, "mutate_all") == 0) {
      run_to(rig.machine.rt.state.cycles / SMS_CYCLES_PER_FRAME + 3U);
      rig.machine.mem.ram[5] ^= 0xFFU;
      rig.machine.mem.cart_ram[7] ^= 0x33U;
      rig.vdp.cram[1] ^= 0x15U;
      rig.psg.chip.tone_period[0] ^= 5U;
      printf("MUTATED\n");
    } else {
      printf("UNKNOWN %s\n", op);
      return 64;
    }
    fflush(stdout);
  }
  fclose(f);
  return 0;
}
