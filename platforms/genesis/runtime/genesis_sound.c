#include "genesis_sound.h"

#include <stdlib.h>
#include <string.h>

static GenesisZ80Machine g_machine;
static GenesisAudio g_audio;
static int g_attached;

#if defined(SEGARECOMP_NATIVE_SOUND_GEMS) || defined(SEGARECOMP_NATIVE_SOUND_KRISALIS)
/* ADR 0291 / 0317: the program was built with `--sound-driver gems` or `krisalis`. The machine gets the verified native replacement; nothing else about the sound
 * subsystem changes, and a program built without the option does not even contain this code. The host callbacks below are shared by both replacements. */
static GenesisZ80Machine *g_native_machine;

static int native_rom_read(void *context, uint32_t address, uint8_t *destination, uint32_t count) {
  const GenesisRuntime *runtime = (const GenesisRuntime *)context;
  uint32_t i, r;
  for (i = 0U; i < count; ++i) {
    int found = 0;
    const uint32_t a = address + i;
    if (a >= UINT32_C(0x400000)) return 0;
    for (r = 0U; r < runtime->owned_region_count && !found; ++r) {
      const GenesisOwnedCartridgeRegion *region = &runtime->owned_regions[r];
      if (region->end <= region->begin || region->length != region->end - region->begin) continue;
      if (a >= region->begin && a < region->end) { destination[i] = region->data[a - region->begin]; found = 1; }
    }
    if (!found) return 0;
  }
  return 1;
}

static void native_ym_write(void *context, uint32_t port, uint8_t value, uint64_t ticks) {
  ++g_native_machine->count_ym_writes;
  g_audio.tap_native = 1U;
  (void)genesis_ym2612_port_write((GenesisRuntime *)context, port, value, ticks);
  g_audio.tap_native = 0U;
}




static void native_command_tap(void *context, const uint8_t *bytes, unsigned count, uint64_t ticks) {
  unsigned i;
  (void)context;
  if (g_audio.write_tap != NULL)
    for (i = 0U; i < count; ++i) g_audio.write_tap(g_audio.write_tap_context, 'C', i, bytes[i], ticks);
}

#endif

#ifdef SEGARECOMP_NATIVE_SOUND_GEMS
#include "gems_native.h"

static GemsNative g_gems;

static uint8_t native_ym_read(void *context, uint32_t port, uint64_t ticks) {
  uint8_t value = 0xFFU;
  ++g_native_machine->count_ym_reads;
  if (!genesis_ym2612_port_read((GenesisRuntime *)context, port, ticks, &value)) return 0xFFU;
  return value;
}

static uint64_t native_timer_a_expiry(void *context) { (void)context; return ym2612_timer_a_expiry(g_audio.ym); }

static void native_psg_write(void *context, uint8_t value, uint64_t ticks) {
  ++g_native_machine->count_psg_writes;
  g_audio.tap_native = 1U;
  (void)genesis_psg_port_write((GenesisRuntime *)context, value, ticks);
  g_audio.tap_native = 0U;
}

static int native_identify(void *context, const uint8_t signature[32], const char **version) { (void)context; return gems_native_identity_lookup(signature, version); }

static void native_bind_host(GemsNativeHost *host, GenesisZ80Machine *machine) {
  memset(host, 0, sizeof *host);
  host->context = machine->runtime;
  host->ram = machine->runtime->devices.z80_bus.z80_ram;
  host->rom_read = native_rom_read;
  host->ym_write = native_ym_write;
  host->ym_read = native_ym_read;
  host->psg_write = native_psg_write;
  host->ym_timer_a_expiry = native_timer_a_expiry;
  host->command_tap = native_command_tap;
}

static void native_start(void *context, GenesisZ80Machine *machine, uint64_t ticks) {
  GemsNativeHost host;
  (void)context;
  g_native_machine = machine;
  native_bind_host(&host, machine);
  gems_native_start(&g_gems, &host, ticks);
}

static void native_suspend(void *context, uint64_t ticks) {   /* validation seam: where the Z80 bus was taken from the driver */
  (void)context;
  if (g_audio.write_tap != NULL) g_audio.write_tap(g_audio.write_tap_context, 'S', 0U, 0U, ticks);
}
static void native_resume(void *context, uint64_t ticks) {
  (void)context;
  if (g_audio.write_tap != NULL) g_audio.write_tap(g_audio.write_tap_context, 'U', 0U, 0U, ticks);
  gems_native_resume(&g_gems, ticks);
}
static int native_run_to(void *context, uint64_t ticks) { (void)context; return (int)gems_native_run_to(&g_gems, ticks); }

static void native_report(const void *context, GenesisZ80NativeReport *out) {
  (void)context;
  out->driver = GEMS_NATIVE_DRIVER;
  out->version = g_native_machine != NULL && g_native_machine->native_version != NULL ? g_native_machine->native_version : "";
  out->fault = gems_native_fault_name(g_gems.fault);
  out->commands = g_gems.count_commands;
  out->sequences = g_gems.count_sequences;
  out->dac_samples = g_gems.count_dac_samples;
  out->ticks = g_gems.count_ticks;
  out->missed_ticks = g_gems.count_missed_ticks;
  out->activations = g_native_machine != NULL ? g_native_machine->native_activations : 0U;
}

/* The driver digest is the SHA-256 of the canonical state payload: it covers every field a save state carries, so two runs are equal exactly when their drivers are. */
static void native_digest(const void *context, uint8_t out[32]) {
  GenesisSha256 sha;
  uint8_t payload[GEMS_NATIVE_STATE_MAX_BYTES];
  uint32_t length;
  (void)context;
  length = gems_native_state_put(&g_gems, payload, (uint32_t)sizeof payload);
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, payload, length);   /* a faulted driver (length 0) digests as the empty payload; the fault itself is in the machine digest */
  genesis_sha256_final(&sha, out);
}

static uint32_t native_state_put(const void *context, uint8_t *out, uint32_t capacity) {
  (void)context;
  return gems_native_state_put(&g_gems, out, capacity);
}

/* Decodes into a private copy of the live driver (the host stays the live one); nothing live changes until commit. */
static void *native_state_stage(void *context, const uint8_t *in, uint32_t length, uint64_t master_ticks, uint64_t chip_timer_a_expiry) {
  GemsNative *stage = (GemsNative *)malloc(sizeof *stage);
  (void)context;
  if (stage == NULL) return NULL;
  *stage = g_gems;
  if (!gems_native_state_get(stage, in, length, master_ticks, chip_timer_a_expiry)) { free(stage); return NULL; }
  return stage;
}

static void native_state_commit(void *context, void *staged) {
  GemsNativeHost host;
  (void)context;
  g_native_machine = &g_machine;
  native_bind_host(&host, &g_machine);
  g_gems = *(GemsNative *)staged;
  g_gems.host = host;   /* no chip is written: the restored chip state came with the snapshot */
  free(staged);
}

static void native_state_discard(void *context, void *staged) { (void)context; free(staged); }

static const GenesisZ80NativeDriver g_gems_ops = {NULL, native_identify, native_start, native_suspend, native_resume, native_run_to, native_report, native_digest,
                                              native_state_put, native_state_stage, native_state_commit, native_state_discard, GEMS_NATIVE_DRIVER,
                                              GEMS_NATIVE_IMPLEMENTATION_VERSION, GEMS_NATIVE_STATE_SCHEMA};
#endif

#ifdef SEGARECOMP_NATIVE_SOUND_KRISALIS
/* ADR 0318: the verified Krisalis replacement. It serves only an uploaded image the catalog names; the binding, suspend / resume, state and report seams are the
 * same ones GEMS uses (one replacement per program). */
#include "krisalis_native.h"

static KrisalisNative g_krisalis;

static uint32_t native_bank_get(void *context) { return ((const GenesisRuntime *)context)->devices.z80_bus.bank; }
static void native_bank_write(void *context, uint8_t bit, uint64_t ticks) {
  if (g_audio.write_tap != NULL) g_audio.write_tap(g_audio.write_tap_context, 'K', bit & 1u, 0U, ticks);   /* validation seam: the driver's own bank-register writes */
  GenesisRuntime *runtime = (GenesisRuntime *)context;
  runtime->devices.z80_bus.bank = (uint16_t)((((uint32_t)bit & 1U) << 8) | (runtime->devices.z80_bus.bank >> 1));
  ++g_native_machine->count_bank_writes;
}

static int native_identify(void *context, const uint8_t signature[32], const char **version) {
  (void)context;
  return krisalis_native_identity_lookup(signature, version, NULL);
}

static void native_bind_host(KrisalisNativeHost *host, GenesisZ80Machine *machine) {
  memset(host, 0, sizeof *host);
  host->context = machine->runtime;
  host->ram = machine->runtime->devices.z80_bus.z80_ram;
  host->rom_read = native_rom_read;
  host->ym_write = native_ym_write;
  host->bank_get = native_bank_get;
  host->bank_write = native_bank_write;
  host->command_tap = native_command_tap;
}

static void native_start(void *context, GenesisZ80Machine *machine, uint64_t ticks) {
  KrisalisNativeHost host;
  uint8_t revision = 0U;
  (void)context;
  g_native_machine = machine;
  native_bind_host(&host, machine);
  (void)krisalis_native_identity_lookup(machine->native_signature, NULL, &revision);
  if (g_audio.write_tap != NULL) {   /* validation seam: the activation, the revision and the bank register the driver starts with */
    g_audio.write_tap(g_audio.write_tap_context, 'A', revision, 0U, ticks);
    g_audio.write_tap(g_audio.write_tap_context, 'B', native_bank_get(machine->runtime), 0U, ticks);
  }
  krisalis_native_start(&g_krisalis, &host, revision, ticks);
}

static void native_suspend(void *context, uint64_t ticks) {
  (void)context;
  if (g_audio.write_tap != NULL) g_audio.write_tap(g_audio.write_tap_context, 'S', 0U, 0U, ticks);
  krisalis_native_suspend(&g_krisalis, ticks);
}
static void native_resume(void *context, uint64_t ticks) {
  (void)context;
  if (g_audio.write_tap != NULL) g_audio.write_tap(g_audio.write_tap_context, 'U', 0U, 0U, ticks);
  krisalis_native_resume(&g_krisalis, ticks);
}
static int native_run_to(void *context, uint64_t ticks) { (void)context; return (int)krisalis_native_run_to(&g_krisalis, ticks); }

static void native_report(const void *context, GenesisZ80NativeReport *out) {
  (void)context;
  out->driver = KRISALIS_NATIVE_DRIVER;
  out->version = g_native_machine != NULL && g_native_machine->native_version != NULL ? g_native_machine->native_version : "";
  out->fault = krisalis_native_fault_name(g_krisalis.fault);
  out->commands = g_krisalis.count_commands;
  out->sequences = g_krisalis.count_rows;
  out->dac_samples = g_krisalis.count_dac_samples;
  out->ticks = g_krisalis.count_ticks;
  out->missed_ticks = 0U;
  out->activations = g_native_machine != NULL ? g_native_machine->native_activations : 0U;
}

/* The driver digest is the SHA-256 of the canonical state payload (every field a save state carries). */
static void native_digest(const void *context, uint8_t out[32]) {
  GenesisSha256 sha;
  uint8_t payload[KRISALIS_NATIVE_STATE_MAX_BYTES];
  uint32_t length;
  (void)context;
  length = krisalis_native_state_put(&g_krisalis, payload, (uint32_t)sizeof payload);
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, payload, length);
  genesis_sha256_final(&sha, out);
}

static uint32_t native_state_put(const void *context, uint8_t *out, uint32_t capacity) {
  (void)context;
  return krisalis_native_state_put(&g_krisalis, out, capacity);
}

static void *native_state_stage(void *context, const uint8_t *in, uint32_t length, uint64_t master_ticks, uint64_t chip_timer_a_expiry) {
  KrisalisNative *stage = (KrisalisNative *)malloc(sizeof *stage);
  (void)context;
  (void)chip_timer_a_expiry;
  if (stage == NULL) return NULL;
  *stage = g_krisalis;
  if (!krisalis_native_state_get(stage, in, length, master_ticks)) { free(stage); return NULL; }
  return stage;
}

static void native_state_commit(void *context, void *staged) {
  KrisalisNativeHost host;
  (void)context;
  g_native_machine = &g_machine;
  native_bind_host(&host, &g_machine);
  g_krisalis = *(KrisalisNative *)staged;
  g_krisalis.host = host;   /* no chip is written: the restored chip state came with the snapshot */
  free(staged);
}

static void native_state_discard(void *context, void *staged) { (void)context; free(staged); }

static const GenesisZ80NativeDriver g_krisalis_ops = {NULL, native_identify, native_start, native_suspend, native_resume, native_run_to, native_report, native_digest,
                                                      native_state_put, native_state_stage, native_state_commit, native_state_discard, KRISALIS_NATIVE_DRIVER,
                                                      KRISALIS_NATIVE_IMPLEMENTATION_VERSION, KRISALIS_NATIVE_STATE_SCHEMA};
#endif

GenesisZ80Machine *genesis_sound_attach(GenesisRuntime *runtime) {
  if (!g_attached) {
    genesis_z80_machine_attach(&g_machine, runtime);
#ifdef SEGARECOMP_NATIVE_SOUND_GEMS
    g_machine.native_driver = &g_gems_ops;
#endif
#ifdef SEGARECOMP_NATIVE_SOUND_KRISALIS
    g_machine.native_driver = &g_krisalis_ops;
#endif
    genesis_audio_attach(&g_audio, runtime);
    g_attached = 1;
  }
  return &g_machine;
}

const GenesisAudio *genesis_sound_audio(void) { return g_attached ? &g_audio : NULL; }

GenesisAudio *genesis_sound_audio_mutable(void) { return g_attached ? &g_audio : NULL; }

GenesisMixer *genesis_sound_mixer(void) { return g_attached ? &g_audio.mixer : NULL; }

void genesis_sound_finish(GenesisRuntime *runtime, int guest_stopped) {
  GenesisRuntimeStop stop;
  if (!g_attached) return;
  memset(&stop, 0, sizeof(stop));
  if (!guest_stopped) (void)genesis_z80_machine_run_to(&g_machine, runtime->scheduler.master_ticks, &stop);
  genesis_audio_sync(&g_audio, runtime->scheduler.master_ticks);
}

void genesis_sound_detach(void) {
  if (g_attached) genesis_audio_detach(&g_audio);
  g_attached = 0;
}
