/*
 * SEG-032-T005 (ADR 0072): replays a 68K bus script against the REAL Genesis runtime with an attached generated-native Z80 machine.
 *   genesis_z80_machine_harness <script> [sync-quantum|-] [audio [pcm-file]]
 * script lines (decimal times, hex addresses/values):
 *   retire <m68k cycles>          advance guest time through the real retirement hook (genesis_runtime_retire_m68k_instruction)
 *   w8|w16 <addr> <value>         68K write through genesis_route_access        r8|r16 <addr>     68K read (prints R <addr> <value>)
 *   quantum <ticks>               change the retirement-hook synchronization cadence
 *   ymstate                       (with `audio`) run the YM2612 to the current guest time; print its state digest, sample count/digest, write trace
 *   psgrun | psgstate             (with `audio`) run the shared PSG to the current guest time / print its state, counters and delivery trace
 *   mixer                         (with `audio`) SEG-032-T009: flush like the program does (Z80 then both devices to the current guest time) and print
 *                                 the mixer aggregates; a fifth argument names a file that receives the canonical s16le stereo stream
 *   state                         print Z80 machine digest, Z80 time, bound image, bank, latches
 * Any rejected access or retirement prints STOP <class> <diagnostic> and ends the run with exit status 0 (the stop is the result).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_audio.h"
#include "z80_machine.h"

static GenesisRuntime runtime;
static GenesisZ80Machine machine;
static GenesisAudio audio;
static FILE *pcm;

static void pcm_sink(void *context, uint64_t index, int16_t left, int16_t right) {
  uint8_t bytes[4];
  (void)context;
  (void)index;
  bytes[0] = (uint8_t)((uint16_t)left & 0xFFU);
  bytes[1] = (uint8_t)((uint16_t)left >> 8);
  bytes[2] = (uint8_t)((uint16_t)right & 0xFFU);
  bytes[3] = (uint8_t)((uint16_t)right >> 8);
  if (pcm != NULL) fwrite(bytes, 1U, sizeof bytes, pcm);
}

static void print_digest(const uint8_t digest[32]) {
  unsigned i;
  for (i = 0; i < 32U; ++i) printf("%02x", (unsigned)digest[i]);
}

int main(int argc, char **argv) {
  char line[256];
  FILE *script;
  if (argc < 2) return 2;
  script = fopen(argv[1], "r");
  if (script == NULL) return 2;
  genesis_z80_machine_attach(&machine, &runtime);
  if (argc > 2 && strcmp(argv[2], "-") != 0) runtime.z80_sync_quantum = (uint32_t)strtoul(argv[2], NULL, 10);
  if (argc > 3 && strcmp(argv[3], "audio") == 0) genesis_audio_attach(&audio, &runtime);
  if (argc > 4) {
    pcm = fopen(argv[4], "wb");
    if (pcm == NULL) return 2;
    genesis_mixer_set_sink(&audio.mixer, pcm_sink, NULL);
  }
  while (fgets(line, sizeof line, script) != NULL) {
    char op[16];
    unsigned long a = 0, b = 0;
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%15s %lx %lx", op, &a, &b) < 1) return 3;
    if (strcmp(op, "retire") == 0) {
      unsigned long cycles = strtoul(line + 7, NULL, 10);
      GenesisControlTransfer t = genesis_runtime_retire_m68k_instruction(&runtime, (uint32_t)cycles, 0U);
      if (t.kind == GENESIS_STOP) { printf("STOP %d %d\n", (int)t.stop.stop_class, (int)t.stop.diagnostic_category); return 0; }
    } else if (strcmp(op, "quantum") == 0) {
      runtime.z80_sync_quantum = (uint32_t)strtoul(line + 8, NULL, 10);
    } else if (strcmp(op, "psgstate") == 0) {
      uint8_t bytes[SN76489_STATE_BYTES];
      uint32_t i;
      genesis_audio_psg_state(&audio, bytes);
      printf("PSG ");
      for (i = 0; i < SN76489_STATE_BYTES; ++i) printf("%02x", (unsigned)bytes[i]);
      printf(" writes=%llu data_before_latch=%llu\n", (unsigned long long)audio.psg_writes, (unsigned long long)audio.psg_data_before_latch);
      for (i = 0; i < audio.trace_count; ++i) printf("TRACE %llu %02x\n", (unsigned long long)audio.trace_ticks[i], (unsigned)audio.trace_bytes[i]);
    } else if (strcmp(op, "ymstate") == 0) {
      uint32_t i;
      genesis_audio_ym_run_to(&audio, runtime.scheduler.master_ticks);
      printf("YM digest=%016llx samples=%llu fnv=%016llx writes=%llu\n", (unsigned long long)ym2612_state_digest(audio.ym),
             (unsigned long long)audio.ym_samples, (unsigned long long)audio.ym_sample_fnv, (unsigned long long)audio.ym_writes);
      for (i = 0; i < audio.ym_trace_count; ++i)
        printf("YMTRACE %llu %u %u\n", (unsigned long long)audio.ym_trace_ticks[i], (unsigned)audio.ym_trace_port[i], (unsigned)audio.ym_trace_value[i]);
    } else if (strcmp(op, "psgrun") == 0) {
      genesis_audio_psg_run_to(&audio, runtime.scheduler.master_ticks);
    } else if (strcmp(op, "mixer") == 0) {
      uint8_t digest[32];
      GenesisRuntimeStop stop;
      memset(&stop, 0, sizeof stop);
      (void)genesis_z80_machine_run_to(&machine, runtime.scheduler.master_ticks, &stop);
      genesis_audio_sync(&audio, runtime.scheduler.master_ticks);
      genesis_mixer_digest(&audio.mixer, digest);
      printf("MIXER frames=%llu clipped=%llu changes=%llu span=%d nonsilent=%d fault=%d ticks=%llu sha=", (unsigned long long)audio.mixer.frames,
             (unsigned long long)audio.mixer.clipped, (unsigned long long)audio.mixer.changes, (int)genesis_mixer_span(&audio.mixer),
             genesis_mixer_non_silent(&audio.mixer), (int)audio.mixer.fault, (unsigned long long)runtime.scheduler.master_ticks);
      print_digest(digest);
      printf("\n");
      for (uint32_t i = 0; i < audio.trace_count; ++i)
        printf("PSGTRACE %llu %u\n", (unsigned long long)audio.trace_ticks[i], (unsigned)audio.trace_bytes[i]);
      for (uint32_t i = 0; i < audio.ym_trace_count; ++i)
        printf("YMTRACE %llu %u %u\n", (unsigned long long)audio.ym_trace_ticks[i], (unsigned)audio.ym_trace_port[i], (unsigned)audio.ym_trace_value[i]);
    } else if (strcmp(op, "state") == 0) {
      uint8_t digest[32];
      const GenesisZ80BusState *bus = &runtime.devices.z80_bus;
      genesis_z80_machine_state_digest(&machine, digest);
      printf("STATE z80=");
      print_digest(digest);
      printf(" cycles=%llu pc=%04x bound=%u bank=%03x req=%u granted=%u released=%u view_stop=%d ticks=%llu\n",
             (unsigned long long)machine.cpu.state.cycles, (unsigned)machine.cpu.state.pc, (unsigned)machine.bound_ordinal,
             (unsigned)bus->bank, (unsigned)bus->bus_requested, (unsigned)bus->bus_granted, (unsigned)bus->reset_released,
             (int)machine.view_stop, (unsigned long long)runtime.scheduler.master_ticks);
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
  if (pcm != NULL) fclose(pcm);
  fclose(script);
  return 0;
}
