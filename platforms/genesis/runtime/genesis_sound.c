#include "genesis_sound.h"

static GenesisZ80Machine g_machine;
static GenesisAudio g_audio;
static int g_attached;

GenesisZ80Machine *genesis_sound_attach(GenesisRuntime *runtime) {
  if (!g_attached) {
    genesis_z80_machine_attach(&g_machine, runtime);
    genesis_audio_attach(&g_audio, runtime);
    g_attached = 1;
  }
  return &g_machine;
}

const GenesisAudio *genesis_sound_audio(void) { return g_attached ? &g_audio : NULL; }

void genesis_sound_detach(void) {
  if (g_attached) genesis_audio_detach(&g_audio);
  g_attached = 0;
}
