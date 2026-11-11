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
#ifdef SEGARECOMP_SNAPSHOT_NATIVE   /* the machine and devices are the production sound subsystem's, with the native GEMS replacement bound (ADR 0291) */
#include "genesis_sound.h"
static GenesisZ80Machine *machine_ptr;
static GenesisAudio *audio_ptr;
#define g_machine (*machine_ptr)
#define g_audio (*audio_ptr)
static uint8_t cart_data[0x10000];
static GenesisOwnedCartridgeRegion cart_region;

/* Project-authored cartridge resources of the same shape the driver unit tests use: patches {0 digital, 1 FM, 2 PSG}, sequences {0 digital note, 1 FM note, 2 off the cartridge, 4 PSG note,
 * 5 PSG loop}, sample 0 (3000 bytes, rate 5). No commercial data. */
static void put16(uint8_t *q, unsigned v) { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); }
static void cart_install(int digital_long_loop) {
  static const uint8_t t0[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x00, 0xC2, 0x30, 0x60};
  static const uint8_t t1[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x01, 0xC2, 0x30, 0x60};
  static const uint8_t t2[] = {0xC0, 0x64, 3};
  static const uint8_t t4[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x02, 0xC1, 0x3C, 0x60};
  static const uint8_t t5[] = {0xC0, 0x6A, 100, 0x6D, 0x61, 0x02, 0x64, 0x02, 0xC1, 0x3C, 0x65, 0x60};
  uint8_t *p;
  unsigned i;
  memset(cart_data, 0, sizeof cart_data);
  for (i = 0; i < 4U; ++i) put16(cart_data + 0x1000 + 2U * i, 8U + 39U * i);
  cart_data[0x1008] = 1; cart_data[0x1009] = 4;
  cart_data[0x1000 + 8 + 78] = 2; cart_data[0x1000 + 8 + 78 + 2] = 0x10; cart_data[0x1000 + 8 + 78 + 3] = 2; cart_data[0x1000 + 8 + 78 + 5] = 0x10; cart_data[0x1000 + 8 + 78 + 6] = 0x10;
  for (i = 0; i < 6U; ++i) put16(cart_data + 0x2000 + 2U * i, 16U + 3U * i);
  p = cart_data + 0x2000 + 16;
  p[0] = 1; put16(p + 1, 0x40); p[3] = 1; put16(p + 4, 0x60); p[6] = 1; put16(p + 7, 0xDFF0); p[12] = 1; put16(p + 13, 0xA0); p[15] = 1; put16(p + 16, 0xC0);
  memcpy(cart_data + 0x2040, t0, sizeof t0); memcpy(cart_data + 0x2060, t1, sizeof t1); memcpy(cart_data + 0x2000 + 0xDFF0, t2, sizeof t2);
  memcpy(cart_data + 0x20A0, t4, sizeof t4); memcpy(cart_data + 0x20C0, t5, sizeof t5);
  cart_data[0x3000] = digital_long_loop ? 0x15 : 0x05; cart_data[0x3001] = 0x00; cart_data[0x3002] = 0x10; put16(cart_data + 0x3006, 3000U);
  if (digital_long_loop) put16(cart_data + 0x3008, 100U);
  for (i = 0; i < 3000U + 256U; ++i) cart_data[0x4000 + i] = (uint8_t)(i * 7U + 3U);
  cart_region.begin = 0x100000U; cart_region.end = 0x100000U + (uint32_t)sizeof cart_data; cart_region.data = cart_data; cart_region.length = (uint32_t)sizeof cart_data;
  runtime.owned_regions = &cart_region;
  runtime.owned_region_count = 1U;
}
#else
static GenesisZ80Machine g_machine;
static GenesisAudio g_audio;
#endif
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
  m.z80 = &g_machine;
  m.audio = &g_audio;
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
  copy->owned_regions = NULL; copy->cartridge_sram = NULL; copy->cartridge_sram_storage = NULL; copy->live_frame_observer = NULL; copy->live_raster = NULL; copy->execution_coverage = NULL;
  copy->z80_hooks = NULL; copy->audio_hooks = NULL; copy->z80_epoch.on_epoch = NULL; copy->z80_epoch.context = NULL;   /* host pointers differ per process (ASLR) */
  genesis_sha256_update(&sha, (const uint8_t *)copy, (uint32_t)sizeof *copy);
  free(copy);
  if (runtime.cartridge_sram != NULL && runtime.cartridge_sram_storage != NULL)
    genesis_sha256_update(&sha, runtime.cartridge_sram_storage, runtime.cartridge_sram->storage_bytes);
#ifdef SEGARECOMP_GENESIS_SOUND
  {
    GenesisZ80Machine z = g_machine;
    GenesisAudio *a = (GenesisAudio *)malloc(sizeof *a);
    uint64_t ym = ym2612_state_digest(g_audio.ym);
    uint32_t i, k;
    memset(&z.count_reset_assert, 0, (size_t)((char *)(&z + 1) - (char *)&z.count_reset_assert));
    z.cpu.state.deadline = 0U;
    z.runtime = NULL; memset(&z.hooks, 0, sizeof z.hooks); memset(&z.cpu.host, 0, sizeof z.cpu.host); z.cpu.observe_pc = NULL; z.cpu.observer = NULL; z.on_unknown_image = NULL; z.unknown_image_context = NULL; z.bank_view = NULL;
    z.cpu.outcome = (Z80Outcome)0;
    memset(z.cpu.live_code, 0, sizeof z.cpu.live_code);
    genesis_sha256_update(&sha, (const uint8_t *)&z, (uint32_t)sizeof z);
#ifdef SEGARECOMP_SNAPSHOT_NATIVE
    {  /* the replacement's complete canonical state, and whether it serves the image */
      uint8_t native[32];
      genesis_sha256_update(&sha, &g_machine.native_active, 1U);
      if (g_machine.native_driver != NULL && g_machine.native_activations != 0U) {
        g_machine.native_driver->digest(g_machine.native_driver->context, native);
        genesis_sha256_update(&sha, native, 32U);
      }
    }
#endif
    memcpy(a, &g_audio, sizeof *a);
    memset(a->trace_ticks, 0, sizeof a->trace_ticks);
    memset(a->trace_bytes, 0, sizeof a->trace_bytes);
    memset(a->ym_trace_ticks, 0, sizeof a->ym_trace_ticks);
    memset(a->ym_trace_port, 0, sizeof a->ym_trace_port);
    memset(a->ym_trace_value, 0, sizeof a->ym_trace_value);
    a->trace_count = a->ym_trace_count = 0U;
    memset(a->mixer.ring, 0, sizeof a->mixer.ring);
    a->mixer.ring_read = a->mixer.ring_write = a->mixer.ring_dropped = 0U;
    a->mixer.sink = NULL; a->mixer.sink_context = NULL; a->ym = NULL; a->runtime = NULL; memset(&a->hooks, 0, sizeof a->hooks); a->psg.config.levels = NULL;
    for (k = 0U; k < 2U; ++k) {
      GenesisMixerSource *s = k == 0U ? &a->mixer.ym : &a->mixer.psg;
      const GenesisMixerSource *o = k == 0U ? &g_audio.mixer.ym : &g_audio.mixer.psg;
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
#ifdef SEGARECOMP_SNAPSHOT_NATIVE
  machine_ptr = genesis_sound_attach(&runtime);
  audio_ptr = genesis_sound_audio_mutable();
#else
  genesis_z80_machine_attach(&g_machine, &runtime);
  genesis_audio_attach(&g_audio, &runtime);
#endif
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
#ifdef SEGARECOMP_SNAPSHOT_NATIVE
    } else if (strcmp(op, "cart") == 0) {
      cart_install((int)a);
    } else if (strcmp(op, "native") == 0) {
      GenesisZ80NativeReport rep;
      memset(&rep, 0, sizeof rep);
      if (g_machine.native_driver != NULL) g_machine.native_driver->report(g_machine.native_driver->context, &rep);
      printf("NATIVE active=%u activations=%u fault=%s commands=%llu sequences=%llu dac=%llu ticks=%llu sound_fault=%d ym_writes=%llu psg_writes=%llu\n", (unsigned)g_machine.native_active,
             (unsigned)g_machine.native_activations, rep.fault != NULL ? rep.fault : "-", (unsigned long long)rep.commands, (unsigned long long)rep.sequences,
             (unsigned long long)rep.dac_samples, (unsigned long long)rep.ticks, (int)g_machine.sound_fault, (unsigned long long)g_audio.ym_writes, (unsigned long long)g_audio.psg_writes);
#endif
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
    } else if (strcmp(op, "validate") == 0) {
      const GenesisSnapshotMachine m = snap_machine();
      printf("VALIDATE %lu %d\n", a, (int)genesis_snapshot_validate(&m, &identity, slots[a % SLOTS].data, slots[a % SLOTS].size, NULL));
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
      (void)genesis_z80_machine_run_to(&g_machine, runtime.scheduler.master_ticks, &stop);
      genesis_audio_sync(&g_audio, runtime.scheduler.master_ticks);
      genesis_mixer_digest(&g_audio.mixer, digest);
      printf("MIXER frames=%llu clipped=%llu changes=%llu nonsilent=%d ring=%u sha=", (unsigned long long)g_audio.mixer.frames,
             (unsigned long long)g_audio.mixer.clipped, (unsigned long long)g_audio.mixer.changes, genesis_mixer_non_silent(&g_audio.mixer),
             (unsigned)genesis_mixer_ring_available(&g_audio.mixer));
      print_hex(digest, 32U);
      printf("\n");
#endif
    } else if (strcmp(op, "state") == 0) {
      printf("STATE pc=%08x sr=%04x ticks=%llu frame=%llu vblank_pending=%u hint_pending=%u dma=%d/%u eeprom_phase=%u eeprom_cycles=%u", (unsigned)runtime.pc,
             (unsigned)runtime.sr, (unsigned long long)runtime.scheduler.master_ticks, (unsigned long long)runtime.virtual_frame_count,
             (unsigned)runtime.devices.interrupt.vblank_pending, (unsigned)runtime.raster.hint_pending, (int)runtime.devices.vdp.dma.phase,
             (unsigned)runtime.devices.vdp.dma.remaining_length, (unsigned)runtime.cartridge_eeprom.phase, (unsigned)runtime.cartridge_eeprom.cycles);
#ifdef SEGARECOMP_GENESIS_SOUND
      printf(" z80_pc=%04x z80_cycles=%llu bound=%u reset_released=%u ym_samples=%llu", (unsigned)g_machine.cpu.state.pc,
             (unsigned long long)g_machine.cpu.state.cycles, (unsigned)g_machine.bound_ordinal, (unsigned)runtime.devices.z80_bus.reset_released,
             (unsigned long long)g_audio.ym_samples);
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
