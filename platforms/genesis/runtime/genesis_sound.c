#include "genesis_sound.h"

#include <string.h>

static GenesisZ80Machine g_machine;
static GenesisAudio g_audio;
static int g_attached;

#ifdef SEGARECOMP_NATIVE_SOUND_GEMS
/* ADR 0291: the program was built with `--sound-driver gems`. The machine gets the verified native replacement; nothing else about the sound
 * subsystem changes, and a program built without the option does not even contain this code. */
#include "gems_native.h"

static GemsNative g_gems;
static GenesisZ80Machine *g_gems_machine;

static int gems_rom_read(void *context, uint32_t address, uint8_t *destination, uint32_t count) {
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

static void gems_ym_write(void *context, uint32_t port, uint8_t value, uint64_t ticks) {
  ++g_gems_machine->count_ym_writes;
  (void)genesis_ym2612_port_write((GenesisRuntime *)context, port, value, ticks);
}

static uint8_t gems_ym_read(void *context, uint32_t port, uint64_t ticks) {
  uint8_t value = 0xFFU;
  ++g_gems_machine->count_ym_reads;
  if (!genesis_ym2612_port_read((GenesisRuntime *)context, port, ticks, &value)) return 0xFFU;
  return value;
}

static void gems_psg_write(void *context, uint8_t value, uint64_t ticks) {
  ++g_gems_machine->count_psg_writes;
  (void)genesis_psg_port_write((GenesisRuntime *)context, value, ticks);
}

static void gems_command_tap(void *context, const uint8_t *bytes, unsigned count, uint64_t ticks) {
  unsigned i;
  (void)context;
  if (g_audio.write_tap != NULL)
    for (i = 0U; i < count; ++i) g_audio.write_tap(g_audio.write_tap_context, 'C', i, bytes[i], ticks);
}

static int native_identify(void *context, const uint8_t signature[32], const char **version) { (void)context; return gems_native_identity_lookup(signature, version); }

static void native_start(void *context, GenesisZ80Machine *machine, uint64_t ticks) {
  GemsNativeHost host;
  (void)context;
  g_gems_machine = machine;
  memset(&host, 0, sizeof host);
  host.context = machine->runtime;
  host.ram = machine->runtime->devices.z80_bus.z80_ram;
  host.rom_read = gems_rom_read;
  host.ym_write = gems_ym_write;
  host.ym_read = gems_ym_read;
  host.psg_write = gems_psg_write;
  host.command_tap = gems_command_tap;
  gems_native_start(&g_gems, &host, ticks);
}

static void native_resume(void *context, uint64_t ticks) { (void)context; gems_native_resume(&g_gems, ticks); }
static int native_run_to(void *context, uint64_t ticks) { (void)context; return (int)gems_native_run_to(&g_gems, ticks); }

static void native_report(const void *context, GenesisZ80NativeReport *out) {
  (void)context;
  out->driver = GEMS_NATIVE_DRIVER;
  out->version = g_gems_machine != NULL && g_gems_machine->native_version != NULL ? g_gems_machine->native_version : "";
  out->fault = gems_native_fault_name(g_gems.fault);
  out->commands = g_gems.count_commands;
  out->sequences = g_gems.count_sequences;
  out->dac_samples = g_gems.count_dac_samples;
  out->ticks = g_gems.count_ticks;
  out->missed_ticks = g_gems.count_missed_ticks;
  out->activations = g_gems_machine != NULL ? g_gems_machine->native_activations : 0U;
}

static void native_digest(const void *context, uint8_t out[32]) {
  GenesisSha256 sha;
  uint8_t word[8];
  unsigned i, b;
  uint64_t values[12];
  (void)context;
  values[0] = g_gems.count_commands; values[1] = g_gems.count_sequences; values[2] = g_gems.count_dac_samples; values[3] = g_gems.count_ticks;
  values[4] = g_gems.count_missed_ticks; values[5] = g_gems.dac_due; values[6] = (uint64_t)g_gems.fault; values[7] = g_gems.sample_pointer;
  values[8] = g_gems.sample_counter; values[9] = ((uint64_t)g_gems.fifo_read << 8) | g_gems.fifo_write; values[10] = g_gems.dac_active;
  values[11] = ((uint64_t)g_gems.dac_voice.flags << 8) | g_gems.fdf_state;
  genesis_sha256_init(&sha);
  for (i = 0U; i < 12U; ++i) {
    for (b = 0U; b < 8U; ++b) word[b] = (uint8_t)(values[i] >> (8U * b));
    genesis_sha256_update(&sha, word, 8U);
  }
  for (i = 0U; i < 16U; ++i) {
    genesis_sha256_update(&sha, &g_gems.ccb[i].flags, 1U);
    word[0] = (uint8_t)g_gems.ccb[i].timer; word[1] = (uint8_t)(g_gems.ccb[i].timer >> 8);
    word[2] = (uint8_t)g_gems.ccb[i].address; word[3] = (uint8_t)(g_gems.ccb[i].address >> 8); word[4] = (uint8_t)(g_gems.ccb[i].address >> 16);
    genesis_sha256_update(&sha, word, 5U);
  }
  genesis_sha256_final(&sha, out);
}

static const GenesisZ80NativeDriver g_gems_ops = {NULL, native_identify, native_start, native_resume, native_run_to, native_report, native_digest};
#endif

GenesisZ80Machine *genesis_sound_attach(GenesisRuntime *runtime) {
  if (!g_attached) {
    genesis_z80_machine_attach(&g_machine, runtime);
#ifdef SEGARECOMP_NATIVE_SOUND_GEMS
    g_machine.native_driver = &g_gems_ops;
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
