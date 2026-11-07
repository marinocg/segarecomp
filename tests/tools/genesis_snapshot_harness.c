/*
 * SEG-131: replays a 68K bus script against the REAL Genesis runtime (with the generated-native Z80 machine and the YM2612/PSG/mixer in a
 * sound build) and exercises genesis_snapshot_capture / _restore.
 *   genesis_snapshot_harness <script> [sram|eeprom] [-]        (build with -DSEGARECOMP_GENESIS_SOUND for the sound machine)
 * Script ops (decimal times, hex addresses/values), in addition to the genesis_z80_machine_harness ones (retire, w8/w16, r8/r16, quantum):
 *   snap N | restore N            capture into slot N / restore from slot N (prints "SNAP N <status>" / "RESTORE N <status>")
 *   eq N M                        print EQ when slots N and M hold identical bytes
 *   tamper N off xor              xor one byte of slot N (no re-digest)        redigest N   recompute the trailing SHA-256 of slot N
 *   trunc N len                   cut slot N to `len` bytes                    grow N       append one zero byte
 *   layout N                      print "SEC <id> <offset> <length>" for each section of slot N and "HDR <size> <frame> <ticks>"
 *   save N path | load N path     write / read a slot's bytes
 *   ident <byte> | region <n>     change the identity used for restore / the runtime region profile
 *   fullhash                      SHA-256 of all execution-affecting state (independent of the snapshot code)
 *   state                         scenario witnesses (pc, interrupts, DMA, EEPROM, Z80, mixer)
 *   mixer                         flush both devices like the program does and print the mixer digest
 *   sramfill <hex>                set every SRAM/EEPROM byte to <hex>         storage <index>   print one SRAM byte
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_snapshot.h"
#ifdef SEGARECOMP_GENESIS_SOUND
#include "genesis_audio.h"
#include "z80_machine.h"
#endif

#define SLOTS 8

static GenesisRuntime runtime;
#ifdef SEGARECOMP_GENESIS_SOUND
static GenesisZ80Machine machine;
static GenesisAudio audio;
#endif
static GenesisSnapshotBuffer slots[SLOTS];
static GenesisSnapshotIdentity identity;
static GenesisCartridgeSramConfig sram_config;
static uint8_t sram_storage[1024];

static GenesisSnapshotMachine snap_machine(void) {
  GenesisSnapshotMachine m;
  memset(&m, 0, sizeof m);
  m.runtime = &runtime;
#ifdef SEGARECOMP_GENESIS_SOUND
  m.z80 = &machine;
  m.audio = &audio;
#endif
  return m;
}

static void print_hex(const uint8_t *p, size_t n) {
  size_t i;
  for (i = 0; i < n; ++i) printf("%02x", (unsigned)p[i]);
}

static void fullhash(uint8_t out[32]) {
  GenesisSha256 sha;
  GenesisRuntime *copy = (GenesisRuntime *)malloc(sizeof *copy);
  genesis_sha256_init(&sha);
  memcpy(copy, &runtime, sizeof *copy);
  memset(&copy->execution_history, 0, sizeof copy->execution_history);
  memset(&copy->m68k_checkpoint, 0, sizeof copy->m68k_checkpoint);
  memset(&copy->device_checkpoint, 0, sizeof copy->device_checkpoint);
  copy->pad1 = copy->pad2 = 0U;
  genesis_sha256_update(&sha, (const uint8_t *)copy, (uint32_t)sizeof *copy);
  free(copy);
  if (runtime.cartridge_sram != NULL && runtime.cartridge_sram_storage != NULL)
    genesis_sha256_update(&sha, runtime.cartridge_sram_storage, runtime.cartridge_sram->storage_bytes);
#ifdef SEGARECOMP_GENESIS_SOUND
  {
    GenesisZ80Machine z = machine;
    GenesisAudio *a = (GenesisAudio *)malloc(sizeof *a);
    uint64_t ym = ym2612_state_digest(audio.ym);
    uint32_t i, k;
    memset(&z.count_reset_assert, 0, (size_t)((char *)(&z + 1) - (char *)&z.count_reset_assert));
    z.cpu.state.deadline = 0U;
    z.cpu.outcome = (Z80Outcome)0;
    memset(z.cpu.live_code, 0, sizeof z.cpu.live_code);
    genesis_sha256_update(&sha, (const uint8_t *)&z, (uint32_t)sizeof z);
    memcpy(a, &audio, sizeof *a);
    memset(a->trace_ticks, 0, sizeof a->trace_ticks);
    memset(a->trace_bytes, 0, sizeof a->trace_bytes);
    memset(a->ym_trace_ticks, 0, sizeof a->ym_trace_ticks);
    memset(a->ym_trace_port, 0, sizeof a->ym_trace_port);
    memset(a->ym_trace_value, 0, sizeof a->ym_trace_value);
    a->trace_count = a->ym_trace_count = 0U;
    memset(a->mixer.ring, 0, sizeof a->mixer.ring);
    a->mixer.ring_read = a->mixer.ring_write = a->mixer.ring_dropped = 0U;
    a->mixer.sink = NULL;
    for (k = 0U; k < 2U; ++k) {
      GenesisMixerSource *s = k == 0U ? &a->mixer.ym : &a->mixer.psg;
      const GenesisMixerSource *o = k == 0U ? &audio.mixer.ym : &audio.mixer.psg;
      uint8_t word[4];
      memset(s->fifo, 0, sizeof s->fifo);
      s->fifo_head = 0U;
      genesis_sha256_update(&sha, (const uint8_t *)s, (uint32_t)sizeof *s);
      for (i = 0U; i < o->fifo_count; ++i) {
        const uint32_t at = (o->fifo_head + i) % GENESIS_MIXER_FIFO_WINDOWS;
        memcpy(word, &o->fifo[at][0], 4U);
        genesis_sha256_update(&sha, word, 4U);
        memcpy(word, &o->fifo[at][1], 4U);
        genesis_sha256_update(&sha, word, 4U);
      }
      memset(s, 0, sizeof *s);
    }
    genesis_sha256_update(&sha, (const uint8_t *)a, (uint32_t)sizeof *a);
    genesis_sha256_update(&sha, (const uint8_t *)&ym, 8U);
    free(a);
  }
#endif
  genesis_sha256_final(&sha, out);
}

static void redigest(GenesisSnapshotBuffer *b) {
  GenesisSha256 sha;
  if (b->size < 32U) return;
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, b->data, b->size - 32U);
  genesis_sha256_final(&sha, b->data + b->size - 32U);
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int main(int argc, char **argv) {
  char line[512];
  FILE *script;
  if (argc < 2) return 2;
  script = fopen(argv[1], "r");
  if (script == NULL) return 2;
  memset(identity.source_sha256, 0xA5, 32U);
  memset(identity.variant_sha256, 0, 32U);
  runtime.a[7] = 0x00FFFE00U;
  runtime.sr = 0x2700U;
  runtime.pc = 0x00000200U;
#ifdef SEGARECOMP_GENESIS_SOUND
  genesis_z80_machine_attach(&machine, &runtime);
  genesis_audio_attach(&audio, &runtime);
#endif
  if (argc > 2 && (strcmp(argv[2], "sram") == 0 || strcmp(argv[2], "eeprom") == 0)) {
    const int eeprom = strcmp(argv[2], "eeprom") == 0;
    sram_config.start = 0x200001U;
    sram_config.end = eeprom ? 0x200001U : 0x2003FFU;
    sram_config.storage_bytes = eeprom ? 128U : 512U;
    sram_config.lane_odd = 1U;
    sram_config.layout_supported = 1U;
    sram_config.always_mapped = 1U;
    sram_config.kind = eeprom ? SEGARECOMP_GENESIS_CARTRIDGE_KIND_EEPROM_X24C01 : SEGARECOMP_GENESIS_CARTRIDGE_KIND_SRAM;
    genesis_cartridge_sram_install(&runtime, &sram_config, sram_storage);
  }
  while (fgets(line, sizeof line, script) != NULL) {
    char op[16];
    unsigned long a = 0, b = 0, c = 0;
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%15s %lx %lx %lx", op, &a, &b, &c) < 1) return 3;
    if (strcmp(op, "retire") == 0) {
      GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(&runtime, (uint32_t)strtoul(line + 7, NULL, 10), 0U);
      if (t.kind == GENESIS_STOP) { printf("STOP %d %d\n", (int)t.stop.stop_class, (int)t.stop.diagnostic_category); return 0; }
    } else if (strcmp(op, "quantum") == 0) {
      runtime.z80_sync_quantum = (uint32_t)strtoul(line + 8, NULL, 10);
    } else if (strcmp(op, "snap") == 0) {
      const GenesisSnapshotMachine m = snap_machine();
      GenesisSnapshotThumbnail thumb;
      static const uint8_t px[4 * 2 * 2] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
      GenesisSnapshotStatus st;
      thumb.width = 4U; thumb.height = 2U; thumb.rgb565 = px;
      genesis_snapshot_buffer_free(&slots[a % SLOTS]);
      st = genesis_snapshot_capture(&m, &identity, &thumb, &slots[a % SLOTS]);
      printf("SNAP %lu %d size=%u\n", a, (int)st, (unsigned)slots[a % SLOTS].size);
    } else if (strcmp(op, "restore") == 0) {
      const GenesisSnapshotMachine m = snap_machine();
      const GenesisSnapshotStatus st = genesis_snapshot_restore(&m, &identity, slots[a % SLOTS].data, slots[a % SLOTS].size);
      printf("RESTORE %lu %d\n", a, (int)st);
    } else if (strcmp(op, "eq") == 0) {
      const GenesisSnapshotBuffer *x = &slots[a % SLOTS], *y = &slots[b % SLOTS];
      printf("%s\n", x->size == y->size && x->size != 0U && memcmp(x->data, y->data, x->size) == 0 ? "EQ" : "NE");
    } else if (strcmp(op, "tamper") == 0) {
      GenesisSnapshotBuffer *x = &slots[a % SLOTS];
      if (b < x->size) x->data[b] ^= (uint8_t)c;
    } else if (strcmp(op, "redigest") == 0) {
      redigest(&slots[a % SLOTS]);
    } else if (strcmp(op, "trunc") == 0) {
      if (b < slots[a % SLOTS].size) slots[a % SLOTS].size = (uint32_t)b;
    } else if (strcmp(op, "grow") == 0) {
      GenesisSnapshotBuffer *x = &slots[a % SLOTS];
      uint8_t *g = (uint8_t *)realloc(x->data, x->size + 1U);
      if (g != NULL) { x->data = g; x->data[x->size++] = 0U; }
    } else if (strcmp(op, "layout") == 0) {
      const GenesisSnapshotBuffer *x = &slots[a % SLOTS];
      GenesisSnapshotInfo info;
      uint32_t i, count;
      const GenesisSnapshotStatus st = genesis_snapshot_inspect(x->data, x->size, &info);
      printf("INSPECT %d\n", (int)st);
      if (st == GENESIS_SNAPSHOT_OK) {
        count = le32(x->data + 28);
        printf("HDR %u %llu %llu thumb=%ux%u\n", (unsigned)x->size, (unsigned long long)info.guest_frame, (unsigned long long)info.guest_master_ticks,
               (unsigned)info.thumb_width, (unsigned)info.thumb_height);
        for (i = 0U; i < count; ++i) {
          const uint8_t *e = x->data + 156U + i * 12U;
          printf("SEC %u %u %u\n", (unsigned)(e[0] | (e[1] << 8)), (unsigned)le32(e + 4), (unsigned)le32(e + 8));
        }
      }
    } else if (strcmp(op, "save") == 0) {
      char path[256];
      FILE *f;
      if (sscanf(line, "%*s %*s %255s", path) == 1 && (f = fopen(path, "wb")) != NULL) {
        fwrite(slots[a % SLOTS].data, 1U, slots[a % SLOTS].size, f);
        fclose(f);
      }
    } else if (strcmp(op, "load") == 0) {
      char path[256];
      FILE *f;
      if (sscanf(line, "%*s %*s %255s", path) == 1 && (f = fopen(path, "rb")) != NULL) {
        uint8_t *buf = (uint8_t *)malloc(1U << 22);
        const size_t n = fread(buf, 1U, 1U << 22, f);
        fclose(f);
        genesis_snapshot_buffer_free(&slots[a % SLOTS]);
        slots[a % SLOTS].data = buf;
        slots[a % SLOTS].size = (uint32_t)n;
      }
    } else if (strcmp(op, "ident") == 0) {
      memset(identity.source_sha256, (int)a, 32U);
    } else if (strcmp(op, "region") == 0) {
      runtime.region_profile = (uint8_t)a;
    } else if (strcmp(op, "fullhash") == 0) {
      uint8_t d[32];
      fullhash(d);
      printf("FULL ");
      print_hex(d, 32U);
      printf("\n");
    } else if (strcmp(op, "sramfill") == 0) {
      if (runtime.cartridge_sram != NULL) memset(runtime.cartridge_sram_storage, (int)a, runtime.cartridge_sram->storage_bytes);
    } else if (strcmp(op, "storage") == 0) {
      printf("STORAGE %lx %02x\n", a, runtime.cartridge_sram_storage != NULL ? (unsigned)runtime.cartridge_sram_storage[a] : 0U);
    } else if (strcmp(op, "mixer") == 0) {
#ifdef SEGARECOMP_GENESIS_SOUND
      uint8_t digest[32];
      GenesisRuntimeStop stop;
      memset(&stop, 0, sizeof stop);
      (void)genesis_z80_machine_run_to(&machine, runtime.scheduler.master_ticks, &stop);
      genesis_audio_sync(&audio, runtime.scheduler.master_ticks);
      genesis_mixer_digest(&audio.mixer, digest);
      printf("MIXER frames=%llu clipped=%llu changes=%llu nonsilent=%d ring=%u sha=", (unsigned long long)audio.mixer.frames,
             (unsigned long long)audio.mixer.clipped, (unsigned long long)audio.mixer.changes, genesis_mixer_non_silent(&audio.mixer),
             (unsigned)genesis_mixer_ring_available(&audio.mixer));
      print_hex(digest, 32U);
      printf("\n");
#endif
    } else if (strcmp(op, "state") == 0) {
      printf("STATE pc=%08x sr=%04x ticks=%llu frame=%llu vblank_pending=%u hint_pending=%u dma=%d/%u eeprom_phase=%u eeprom_cycles=%u", (unsigned)runtime.pc,
             (unsigned)runtime.sr, (unsigned long long)runtime.scheduler.master_ticks, (unsigned long long)runtime.virtual_frame_count,
             (unsigned)runtime.devices.interrupt.vblank_pending, (unsigned)runtime.raster.hint_pending, (int)runtime.devices.vdp.dma.phase,
             (unsigned)runtime.devices.vdp.dma.remaining_length, (unsigned)runtime.cartridge_eeprom.phase, (unsigned)runtime.cartridge_eeprom.cycles);
#ifdef SEGARECOMP_GENESIS_SOUND
      printf(" z80_pc=%04x z80_cycles=%llu bound=%u reset_released=%u ym_samples=%llu", (unsigned)machine.cpu.state.pc,
             (unsigned long long)machine.cpu.state.cycles, (unsigned)machine.bound_ordinal, (unsigned)runtime.devices.z80_bus.reset_released,
             (unsigned long long)audio.ym_samples);
#endif
      printf("\n");
    } else {
      GenesisRuntimeStop stop;
      uint32_t value = (uint32_t)b;
      GenesisAccessWidth width = (op[1] == '8') ? GENESIS_ACCESS_BYTE : GENESIS_ACCESS_WORD;
      GenesisAccessDirection direction = (op[0] == 'w') ? GENESIS_ACCESS_WRITE : GENESIS_ACCESS_READ;
      memset(&stop, 0, sizeof stop);
      if (genesis_route_access(&runtime, (uint32_t)a, width, direction, &value, &stop) != GENESIS_ACCESS_OK) {
        printf("STOP %d %d\n", (int)stop.stop_class, (int)stop.diagnostic_category);
        return 0;
      }
      if (direction == GENESIS_ACCESS_READ) printf("R %06lx %lx\n", a, (unsigned long)value);
    }
  }
  printf("END\n");
  fclose(script);
  return 0;
}
