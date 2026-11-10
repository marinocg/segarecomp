/*
 * Native GEMS sound-driver replacement (ADR 0291). Independent C11 implementation of the verified observable behaviour of one
 * original driver lineage; see gems_native.h for the contract and the verified subset.
 *
 * Structure: an event scheduler on the guest master clock (boot, DAC sample, VBlank tick, command-FIFO check), a command parser over the
 * RAM-visible FIFO, a small sequencer (16 control blocks), one digital (DAC) voice and the idle FM/PSG clipping that every stop performs.
 * Every chip write is a real YM2612 / PSG write handed to the host with a deterministic guest time.
 */
#include "gems_native.h"

#include <string.h>

#include "runtime.h"
#include "segarecomp/device/sega/ym2612/ym2612.h"

/* ---- RAM layout shared with the 68000 (verified against the uploaded image's address map) ---- */
enum {
  RAM_CMD_WRITE = 0x36u,   /* command FIFO write pointer, owned by the 68000 */
  RAM_CMD_READ = 0x37u,    /* read pointer, owned by the driver */
  RAM_CMD_FIFO = 0x1B40u,  /* 64-byte ring */
  FIFO_MASK = 0x3Fu
};

enum { CMD_GET_POINTERS = 11, CMD_START_SEQUENCE = 16, CMD_STOP_SEQUENCE = 18, CMD_STOP_ALL = 22 };
enum { PATCH_FM = 0, PATCH_DIGITAL = 1, PATCH_TONE = 2, PATCH_NOISE = 3 };
enum { CCB_ENABLED = 0x01, CCB_MUTED = 0x02, CCB_SFX = 0x08, CCB_RUNNING = 0x10, CCB_LOCKED = 0x20, CCB_RETRIGGER = 0x40 };
enum { VOICE_FREE = 0x80, VOICE_RELEASE = 0x40, VOICE_LOCKED = 0x20, VOICE_TIMED = 0x10, VOICE_SFX = 0x08, DAC_VOICE_NUMBER = 6 };
enum { DEFAULT_SBPT = 204, DAC_FIFO_HALF = 128, INTERRUPT_LINE_TICKS = 3420 };
enum { EVENT_NONE = 0, EVENT_BOOT, EVENT_DAC, EVENT_TICK, EVENT_FIFO };

const char *gems_native_fault_name(GemsNativeFault fault) {
  switch (fault) {
    case GEMS_FAULT_NONE: return "none";
    case GEMS_FAULT_UNSUPPORTED_COMMAND: return "unsupported_command";
    case GEMS_FAULT_INVALID_COMMAND_STREAM: return "invalid_command_stream";
    case GEMS_FAULT_UNMAPPED_RESOURCE: return "unmapped_resource";
    case GEMS_FAULT_INVALID_RESOURCE: return "invalid_resource";
    case GEMS_FAULT_UNSUPPORTED_SEQUENCE_EVENT: return "unsupported_sequence_event";
    case GEMS_FAULT_UNSUPPORTED_PATCH: return "unsupported_patch";
    case GEMS_FAULT_UNSUPPORTED_SAMPLE: return "unsupported_sample";
    case GEMS_FAULT_DEVICE_TIMING: return "device_timing";
  }
  return "unknown";
}

/* ---- the catalog of verified images (activation signature -> driver version) ---- */
typedef struct CatalogEntry { const char *name, *version, *verification, *signature_hex; } CatalogEntry;
static const CatalogEntry k_catalog[] = {
#define GEMS_VERIFIED_IMAGE(name, version, verification, signature) {name, version, verification, signature},
#include "gems_driver_catalog.inc"
#undef GEMS_VERIFIED_IMAGE
};

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int gems_native_identity_lookup(const uint8_t signature[32], const char **version) {
  size_t entry, i;
  for (entry = 0; entry < sizeof k_catalog / sizeof k_catalog[0]; ++entry) {
    const char *hex = k_catalog[entry].signature_hex;
    int same = 1;
    for (i = 0; i < 32 && same; ++i) {
      const int hi = hex_nibble(hex[2 * i]), lo = hex_nibble(hex[2 * i + 1]);
      if (hi < 0 || lo < 0 || (uint8_t)((hi << 4) | lo) != signature[i]) same = 0;
    }
    if (same) {
      if (version != NULL) *version = k_catalog[entry].version;
      return 1;
    }
  }
  return 0;
}

/* ---- time ---- */
static uint64_t align_z80(uint64_t t) { return ((t + 14u) / 15u) * 15u; }
static uint64_t tick_time(uint64_t index) { return index * GENESIS_NTSC_MASTER_TICKS_PER_FRAME + GENESIS_NTSC_VBLANK_ONSET_TICK; }
static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

static uint64_t first_tick_index_at_or_after(uint64_t t) {
  const uint64_t frame = GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  uint64_t index = t / frame;
  while (tick_time(index) < t) ++index;
  return index;
}

/* ---- chip access ---- */
/* The time of the event being processed travels with the driver pointer, keeping the public header free of scheduler internals. */
typedef struct Cursor { GemsNative *g; uint64_t now; } Cursor;

static void ym(Cursor *c, uint32_t port, uint8_t value) { c->g->host.ym_write(c->g->host.context, port, value, c->now); }
static void ym_pair(Cursor *c, uint32_t base_port, uint8_t reg, uint8_t value) {
  ym(c, base_port, reg);
  ym(c, base_port + 1u, value);
}
static void psg(Cursor *c, uint8_t value) { c->g->host.psg_write(c->g->host.context, value, c->now); }

static int fail(GemsNative *g, GemsNativeFault fault) {
  if (g->fault == GEMS_FAULT_NONE) g->fault = fault;
  return 1;
}

static int rom_read(GemsNative *g, uint32_t address, uint8_t *destination, uint32_t count) {
  if (count == 0u) return 0;
  if (!g->host.rom_read(g->host.context, address & 0xFFFFFFu, destination, count)) return fail(g, GEMS_FAULT_UNMAPPED_RESOURCE);
  return 0;
}

/* ---- the digital voice ---- */
static void dac_shutdown(Cursor *c) {  /* the FDF7 path: DAC routine and FIFO refill off, DAC mode off, voice free + releasing */
  GemsNative *g = c->g;
  g->dac_active = 0;
  g->fill_enabled = 0;
  ym_pair(c, 0u, 0x2Bu, 0x00u);
  g->dac_voice.flags = 0xC6u;
  g->dac_voice.duration = 0u;
}

/* FILLDACFIFO "needed" path. One-shot samples only: a loop state never exists (looped samples fault at note-on). */
static int dac_fill(Cursor *c) {
  GemsNative *g = c->g;
  uint8_t destination;
  int32_t remaining;
  if (g->fdf_state >= 7u) {
    dac_shutdown(c);
    return 0;
  }
  remaining = (int32_t)g->sample_counter - DAC_FIFO_HALF;
  destination = g->fifo_write;
  g->fifo_write = (uint8_t)(g->fifo_write + DAC_FIFO_HALF);
  if (remaining > 0) {
    g->sample_counter = (uint16_t)remaining;
    if (rom_read(g, g->sample_pointer, &g->fifo[destination], DAC_FIFO_HALF)) return 1;
    g->sample_pointer += DAC_FIFO_HALF;
    return 0;
  }
  /* the remaining samples (the original then shuts the DAC down at once: it cuts the tail that has not been played yet) */
  if (rom_read(g, g->sample_pointer, &g->fifo[destination], (uint32_t)(remaining + DAC_FIFO_HALF))) return 1;
  dac_shutdown(c);
  return 0;
}

static int dac_check_fill(Cursor *c) {
  GemsNative *g = c->g;
  if (g->fill_enabled && ((g->fifo_write ^ g->fifo_read) & 0x80u) != 0u) return dac_fill(c);
  return 0;
}

static void note_off_digital(GemsNative *g) {
  if ((g->sample_flags & 0x20u) != 0u) g->fdf_state = 7u;  /* clip at note-off: shut down at the next refill point */
  /* a looped sample would enter its release state; looped samples fault at note-on, so nothing else can be pending */
}

static int note_on_digital(Cursor *c, unsigned channel, uint8_t note) {
  GemsNative *g = c->g;
  GemsCcb *ccb = &g->ccb[channel];
  GemsVoice *voice = &g->dac_voice;
  uint8_t header[12];
  uint32_t sample_number, first, skip, base;
  unsigned rate;
  uint8_t timer_hi, timer_lo, negated;
  if ((voice->flags & VOICE_FREE) == 0u) {  /* in use: only a channel of at least the same priority may take it */
    if (voice->priority > ccb->priority) return 0;
    g->dac_active = 0;  /* the running sample is cut: the DAC routine is disabled while the new one is prepared */
    if ((voice->flags & VOICE_LOCKED) == 0u) return fail(g, GEMS_FAULT_UNSUPPORTED_PATCH);  /* an FM note holds the voice: no FM voice ever exists here */
  }
  /* VTANDET: voice bookkeeping */
  voice->flags = DAC_VOICE_NUMBER;
  if (ccb->duration != 0u) voice->flags |= VOICE_TIMED;
  if ((ccb->flags & CCB_SFX) != 0u) voice->flags |= VOICE_SFX;
  voice->priority = ccb->priority;
  voice->note = note;
  voice->channel = (uint8_t)channel;
  voice->duration = ccb->duration;
  voice->release = 254u;
  if ((ccb->flags & CCB_RETRIGGER) != 0u) return fail(g, GEMS_FAULT_UNSUPPORTED_SEQUENCE_EVENT);  /* envelope retrigger: envelopes are outside the subset */
  voice->flags |= VOICE_LOCKED;
  /* the sample table entry (12 bytes per sample, note 48.. = sample 0.., lower notes wrap by 96) */
  sample_number = note >= 48u ? (uint32_t)note - 48u : (uint32_t)note + 96u;
  if (rom_read(g, g->sample_table + 12u * sample_number, header, 12u)) return 1;
  first = (uint32_t)header[6] | ((uint32_t)header[7] << 8);
  if (first == 0u) {  /* an empty sample frees the voice again */
    voice->flags = 0xC6u;
    return 0;
  }
  if (first <= DAC_FIFO_HALF) return fail(g, GEMS_FAULT_UNSUPPORTED_SAMPLE);  /* shorter than one FIFO half: the original then keeps its DAC routine alive with the DAC off */
  g->sample_flags = header[0];
  if (ccb->patch[1] != 4u) g->sample_flags = (uint8_t)((g->sample_flags & 0xF0u) | ccb->patch[1]);  /* the patch's rate override */
  rate = g->sample_flags & 0x0Fu;
  if ((g->sample_flags & 0x80u) != 0u || (g->sample_flags & 0x10u) != 0u || rate >= 10u || rate == 0u) return fail(g, GEMS_FAULT_UNSUPPORTED_SAMPLE);
  skip = (uint32_t)header[4] | ((uint32_t)header[5] << 8);
  base = (uint32_t)header[1] | ((uint32_t)header[2] << 8) | ((uint32_t)header[3] << 16);
  /* Timer A reload: -rate split over the two timer registers, so the period is `rate` native samples */
  negated = (uint8_t)(0x100u - rate);
  timer_hi = (uint8_t)(((int8_t)negated) >> 2);
  timer_lo = (uint8_t)(negated & 3u);
  ym_pair(c, 0u, 0x24u, timer_hi);
  ym_pair(c, 0u, 0x25u, timer_lo);
  ym_pair(c, 0u, 0x2Bu, 0x80u);   /* DAC on */
  ym_pair(c, 0u, 0x27u, g->dac_timer_b);  /* load + start Timer A */
  g->dac_period_samples = rate;
  g->dac_due = c->now + (uint64_t)rate * YM2612_MASTER_TICKS_PER_SAMPLE;
  ym_pair(c, 2u, 0xB6u, 0xC0u);   /* channel 6 output to both speakers */
  g->sample_pointer = (g->sample_table + base + skip) & 0xFFFFFFu;
  g->sample_counter = (uint16_t)first;
  g->fill_enabled = 1;
  g->fifo_write = 0u;
  g->fifo_read = 0u;
  g->fdf_state = 4u;
  if (dac_fill(c)) return 1;      /* the forced first fill, then the refill the main loop performs right after */
  if (dac_check_fill(c)) return 1;
  g->dac_active = 1;
  return 0;
}

static int dac_event(Cursor *c) {
  GemsNative *g = c->g;
  const uint8_t sample = g->fifo[g->fifo_read];
  g->fifo_read = (uint8_t)(g->fifo_read + 1u);
  ym_pair(c, 0u, 0x27u, g->dac_timer_b);  /* reset Timer A (starts the next period at this instant) */
  ym_pair(c, 0u, 0x2Au, sample);
  ++g->count_dac_samples;
  g->dac_due = c->now + (uint64_t)g->dac_period_samples * YM2612_MASTER_TICKS_PER_SAMPLE;
  return dac_check_fill(c);
}

/* ---- idle FM / PSG clipping (CLIPALL) ---- */
static void clip_all(Cursor *c) {
  /* FM voices in table order; voice 6 may be in digital mode. All other voices are never allocated by the verified subset. */
  static const uint8_t k_voice[6] = {0, 1, 4, 5, 6, 2};
  GemsNative *g = c->g;
  unsigned i;
  ++g->count_clips;
  for (i = 0; i < 6u; ++i) {
    const int is_dac = k_voice[i] == DAC_VOICE_NUMBER;
    uint8_t channel = is_dac ? g->dac_voice.channel : 0u;
    uint8_t flags = is_dac ? g->dac_voice.flags : (uint8_t)(0x80u | k_voice[i]);
    uint8_t voice, bank_channel, bank;
    if ((g->ccb[channel & 0x0Fu].flags & CCB_RUNNING) != 0u) continue;
    voice = (uint8_t)(flags & 7u);
    if (is_dac) {
      g->dac_voice.flags = (uint8_t)(voice | VOICE_FREE);
      g->dac_voice.duration = 0u;
      g->dac_voice.release = 0u;
    }
    if (is_dac && (flags & VOICE_LOCKED) != 0u) {  /* digital mode */
      g->dac_active = 0;
      g->fill_enabled = 0;
      ym_pair(c, 0u, 0x2Bu, 0x00u);
      continue;
    }
    bank_channel = voice;
    bank = 0u;
    if (voice >= 3u) {
      bank_channel = (uint8_t)(voice - 4u);
      bank = 2u;
    }
    ym_pair(c, bank, (uint8_t)(0x40u | bank_channel), 0x7Fu);  /* clamp all four operators' total level */
    ym_pair(c, bank, (uint8_t)(0x44u | bank_channel), 0x7Fu);
    ym_pair(c, bank, (uint8_t)(0x48u | bank_channel), 0x7Fu);
    ym_pair(c, bank, (uint8_t)(0x4Cu | bank_channel), 0x7Fu);
    ym_pair(c, 0u, 0x28u, voice);   /* key off */
  }
  /* PSG voices (three tone + noise), all with channel 0: a stop command for each voice whose control block is not running */
  if ((g->ccb[0].flags & CCB_RUNNING) == 0u)
    for (i = 0; i < 4u; ++i) g->psg_command[i] = 4u;
}

/* ---- sequencer ---- */
static int getsbyte(GemsNative *g, GemsCcb *ccb, uint8_t *value) {
  if (rom_read(g, ccb->address, value, 1u)) return 1;
  ccb->address = (ccb->address + 1u) & 0xFFFFFFu;
  return 0;
}

static int fetch_patch(GemsNative *g, GemsCcb *ccb) {
  uint8_t offset_bytes[2];
  uint32_t offset;
  if (rom_read(g, g->patch_table + (uint32_t)((ccb->program * 2u) & 0xFFu), offset_bytes, 2u)) return 1;
  offset = (uint32_t)offset_bytes[0] | ((uint32_t)offset_bytes[1] << 8);
  return rom_read(g, g->patch_table + offset, ccb->patch, 39u);
}

static int sequencer(Cursor *c, unsigned index) {
  GemsNative *g = c->g;
  GemsCcb *ccb = &g->ccb[index];
  uint8_t byte;
  ++ccb->timer;
  if (ccb->timer != 0u) return 0;
  if (getsbyte(g, ccb, &byte)) return 1;
  for (;;) {
    if ((byte & 0x80u) != 0u) {  /* delay (11xxxxxx) or duration (10xxxxxx), 6 bits per byte, most significant first */
      const int is_delay = (byte & 0x40u) != 0u;
      uint16_t value = (uint16_t)(byte & 0x3Fu);
      for (;;) {
        uint8_t next;
        if (getsbyte(g, ccb, &next)) return 1;
        if ((next & 0x80u) != 0u && (((next & 0x40u) != 0u) == is_delay)) value = (uint16_t)((value << 6) | (next & 0x3Fu));
        else { byte = next; break; }
      }
      if (is_delay) ccb->delay = (uint16_t)(0u - value);
      else ccb->duration = (uint16_t)(0u - value);
      continue;  /* the byte that ended the run is dispatched next */
    }
    if (byte < 96u) {  /* a note */
      if ((ccb->flags & CCB_MUTED) == 0u) {
        switch (ccb->patch[0]) {
          case PATCH_DIGITAL:
            if (note_on_digital(c, index, byte)) return 1;
            break;
          case PATCH_FM: case PATCH_TONE: case PATCH_NOISE: return fail(g, GEMS_FAULT_UNSUPPORTED_PATCH);
          default: break;  /* an undefined patch plays nothing, exactly like the original */
        }
      }
    } else {
      switch (byte - 96u) {
        case 0:  /* end of sequence: the control block is freed */
          ccb->flags = 0u;
          ccb->duration = 0u;
          return 0;
        case 1: {  /* program change */
          uint8_t program;
          if (getsbyte(g, ccb, &program)) return 1;
          ccb->program = program;
          if (fetch_patch(g, ccb)) return 1;
          break;
        }
        case 3: break;  /* no-op: only triggers the registered delay */
        case 10: {  /* priority */
          if (getsbyte(g, ccb, &ccb->priority)) return 1;
          break;
        }
        case 13: ccb->flags |= CCB_SFX; break;  /* SFX time base */
        case 14: {  /* sample playback rate of this channel's digital patch */
          uint8_t rate;
          if (getsbyte(g, ccb, &rate)) return 1;
          if (ccb->patch[0] == PATCH_DIGITAL) ccb->patch[1] = rate;
          break;
        }
        default: return fail(g, GEMS_FAULT_UNSUPPORTED_SEQUENCE_EVENT);
      }
    }
    /* every note and command except end-of-sequence goes through the registered delay */
    if (ccb->delay != 0u) {
      ccb->timer = ccb->delay;
      return 0;
    }
    if (getsbyte(g, ccb, &byte)) return 1;
  }
}

/* ---- tick: PSG processor, voice timers, sequencers ---- */
static void psg_envelope_tick(Cursor *c) {
  GemsNative *g = c->g;
  unsigned i;
  for (i = 0; i < 4u; ++i) {
    const uint8_t command = g->psg_command[i];
    g->psg_command[i] = 0u;
    if ((command & 4u) != 0u) {  /* stop */
      g->psg_level[i] = 0xFFu;
      g->psg_update[i] = 1u;
      g->psg_mode[i] = 0u;
    }
  }
  for (i = 0; i < 4u; ++i)
    if (g->psg_update[i] != 0u) {
      g->psg_update[i] = 0u;
      psg(c, (uint8_t)((g->psg_level[i] >> 4) | (0x90u + 0x20u * i)));
    }
}

static void voice_timer(Cursor *c, unsigned flags_b) {
  GemsNative *g = c->g;
  GemsVoice *voice = &g->dac_voice;
  const unsigned wanted = (voice->flags & VOICE_SFX) != 0u ? 1u : 2u;
  if ((flags_b & wanted) == 0u) return;
  if ((voice->flags & VOICE_RELEASE) != 0u) {
    if (--voice->release == 0u) voice->flags = (uint8_t)(voice->flags & ~VOICE_RELEASE);
    return;
  }
  if ((voice->flags & VOICE_TIMED) == 0u) return;
  ++voice->duration;
  if (voice->duration != 0u) return;
  /* the registered duration elapsed: a digital voice receives its note-off */
  voice->flags = (uint8_t)(voice->flags & ~(VOICE_TIMED | VOICE_SFX));
  if ((voice->flags & 0x2Fu) == 0x26u) note_off_digital(g);
}

static int update_sequences(Cursor *c, unsigned flags_b) {
  GemsNative *g = c->g;
  unsigned i;
  for (i = 0; i < 16u; ++i) {
    GemsCcb *ccb = &g->ccb[i];
    if ((ccb->flags & CCB_RUNNING) == 0u) continue;
    if ((ccb->flags & CCB_SFX) != 0u) { if ((flags_b & 1u) == 0u) continue; }
    else if ((flags_b & 2u) == 0u) continue;
    if (sequencer(c, i)) return 1;
  }
  return 0;
}

static int tick_event(Cursor *c) {
  GemsNative *g = c->g;
  unsigned flags_b = 1u;
  ++g->count_ticks;
  g->sbptacc = (uint16_t)(g->sbptacc + g->sbpt);
  psg_envelope_tick(c);
  if ((g->sbptacc >> 8) >= 1u) {
    g->sbptacc = (uint16_t)(g->sbptacc - 0x100u);
    flags_b |= 2u;
  }
  voice_timer(c, flags_b);
  return update_sequences(c, flags_b);
}

/* ---- commands ---- */
static int start_sequence(Cursor *c, uint8_t number) {
  GemsNative *g = c->g;
  uint8_t offset_bytes[2], header[33];
  uint32_t offset, tracks_assigned = 0, ccb_index;
  unsigned count, header_cursor = 1u;
  if (rom_read(g, g->sequence_table + (uint32_t)((number * 2u) & 0xFFu), offset_bytes, 2u)) return 1;
  offset = (uint32_t)offset_bytes[0] | ((uint32_t)offset_bytes[1] << 8);
  if (rom_read(g, g->sequence_table + offset, header, 33u)) return 1;
  count = header[0];
  if (count == 0u) return 0;
  if (count > 16u) return fail(g, GEMS_FAULT_INVALID_RESOURCE);
  ++g->count_sequences;
  for (ccb_index = 0; ccb_index < 16u && tracks_assigned < count; ++ccb_index) {
    GemsCcb *ccb = &g->ccb[ccb_index];
    uint32_t track;
    if ((ccb->flags & (CCB_ENABLED | CCB_LOCKED)) != 0u) continue;
    track = (uint32_t)header[header_cursor] | ((uint32_t)header[header_cursor + 1u] << 8);
    header_cursor += 2u;
    ccb->flags = CCB_ENABLED | CCB_RUNNING;
    ccb->address = (g->sequence_table + track) & 0xFFFFFFu;
    ccb->timer = 0xFFFFu;
    ccb->sequence = number;
    ccb->channel = (uint8_t)tracks_assigned;
    ccb->priority = 0u;   /* the registered delay / duration and the program number deliberately survive from the previous use, as in the original */
    ++tracks_assigned;
  }
  return 0;
}

static int stop_sequence(Cursor *c, uint8_t number) {
  GemsNative *g = c->g;
  unsigned i;
  for (i = 0; i < 16u; ++i) {
    GemsCcb *ccb = &g->ccb[i];
    if ((ccb->flags & CCB_ENABLED) == 0u || (ccb->flags & CCB_LOCKED) != 0u) continue;
    if (number != 255u && ccb->sequence != number) continue;
    ccb->flags = 0u;
    ccb->duration = 0u;
  }
  clip_all(c);
  return 0;
}

static int command_arguments(uint8_t command) {
  switch (command) {
    case CMD_GET_POINTERS: return 12;
    case CMD_START_SEQUENCE: case CMD_STOP_SEQUENCE: return 1;
    case CMD_STOP_ALL: return 0;
    default: return -1;
  }
}

static int fifo_event(Cursor *c) {
  GemsNative *g = c->g;
  uint8_t *ram = g->host.ram;
  for (;;) {
    const uint8_t write = ram[RAM_CMD_WRITE];
    uint8_t read = ram[RAM_CMD_READ];
    unsigned available, needed;
    uint8_t start, command;
    int arguments;
    if (write > FIFO_MASK) return fail(g, GEMS_FAULT_INVALID_COMMAND_STREAM);
    read &= FIFO_MASK;
    available = (unsigned)((write - read) & FIFO_MASK);
    if (available == 0u) return 0;
    start = ram[RAM_CMD_FIFO + read];
    if (start != 0xFFu) {  /* not a command start: the original discards the byte */
      ram[RAM_CMD_READ] = (uint8_t)((read + 1u) & FIFO_MASK);
      continue;
    }
    if (available < 2u) return 0;  /* the command byte has not been written yet: wait for it */
    command = ram[RAM_CMD_FIFO + ((read + 1u) & FIFO_MASK)];
    arguments = command_arguments(command);
    if (arguments < 0) return fail(g, GEMS_FAULT_UNSUPPORTED_COMMAND);
    needed = 2u + (unsigned)arguments;
    if (available < needed) return 0;  /* arguments still being written */
    {
      uint8_t args[12];
      int i;
      for (i = 0; i < arguments; ++i) args[i] = ram[RAM_CMD_FIFO + ((read + 2u + (unsigned)i) & FIFO_MASK)];
      ram[RAM_CMD_READ] = (uint8_t)((read + needed) & FIFO_MASK);
      ++g->count_commands;
      if (g->host.command_tap != NULL) {
        uint8_t whole[14];
        whole[0] = start; whole[1] = command;
        memcpy(whole + 2, args, (size_t)arguments);
        g->host.command_tap(g->host.context, whole, needed, c->now);
      }
      switch (command) {
        case CMD_GET_POINTERS:
          g->patch_table = (uint32_t)args[0] | ((uint32_t)args[1] << 8) | ((uint32_t)args[2] << 16);
          g->envelope_table = (uint32_t)args[3] | ((uint32_t)args[4] << 8) | ((uint32_t)args[5] << 16);
          g->sequence_table = (uint32_t)args[6] | ((uint32_t)args[7] << 8) | ((uint32_t)args[8] << 16);
          g->sample_table = (uint32_t)args[9] | ((uint32_t)args[10] << 8) | ((uint32_t)args[11] << 16);
          g->have_pointers = 1;
          break;
        case CMD_START_SEQUENCE:
          if (start_sequence(c, args[0])) return 1;
          break;
        case CMD_STOP_SEQUENCE:
          if (stop_sequence(c, args[0])) return 1;
          break;
        case CMD_STOP_ALL: {
          unsigned i;
          for (i = 0; i < 16u; ++i) {
            g->ccb[i].flags = 0u;
            g->ccb[i].duration = 0u;
          }
          clip_all(c);
          break;
        }
        default: break;
      }
    }
  }
}

/* ---- boot / scheduling ---- */
static void reset_state(GemsNative *g) {
  unsigned i;
  const GemsNativeHost host = g->host;
  memset(g, 0, sizeof *g);
  g->host = host;
  g->sbpt = DEFAULT_SBPT;
  g->dac_timer_b = 0x15u;
  for (i = 0; i < 16u; ++i) g->ccb[i].patch[0] = 0xFFu;
  for (i = 0; i < 4u; ++i) g->psg_level[i] = 0xFFu;
  g->dac_voice.flags = 0x86u;
  g->dac_voice.priority = 0u;
  g->dac_voice.note = 0x50u;
}

void gems_native_start(GemsNative *g, const GemsNativeHost *host, uint64_t at_ticks) {
  g->host = *host;
  reset_state(g);
  g->floor_ticks = align_z80(at_ticks);
  g->boot_ticks = g->floor_ticks;
  g->next_tick_index = first_tick_index_at_or_after(g->boot_ticks);
  g->fifo_check_pending = 1;
}

void gems_native_resume(GemsNative *g, uint64_t at_ticks) {
  g->floor_ticks = max_u64(g->floor_ticks, align_z80(at_ticks));
  g->fifo_check_pending = 1;
}

GemsNativeFault gems_native_run_to(GemsNative *g, uint64_t master_ticks) {
  Cursor cursor;
  cursor.g = g;
  for (;;) {
    uint64_t best = UINT64_MAX;
    int kind = EVENT_NONE;
    if (g->fault != GEMS_FAULT_NONE) return g->fault;
    if (!g->booted) {
      best = align_z80(max_u64(g->boot_ticks, g->floor_ticks));
      kind = EVENT_BOOT;
    } else {
      const uint64_t due = tick_time(g->next_tick_index);
      const uint64_t tick_at = max_u64(align_z80(due), g->floor_ticks);
      if (tick_at >= due + INTERRUPT_LINE_TICKS) {  /* the interrupt line fell while the bus was held: the Z80 never saw it */
        ++g->count_missed_ticks;
        ++g->next_tick_index;
        continue;
      }
      if (g->dac_active) {
        best = max_u64(g->dac_due, g->floor_ticks);   /* anchored on the chip's Timer A expiry, not on a Z80 poll phase: no latency accumulates */
        kind = EVENT_DAC;
      }
      if (tick_at < best) { best = tick_at; kind = EVENT_TICK; }
      if (g->fifo_check_pending && g->floor_ticks < best) { best = g->floor_ticks; kind = EVENT_FIFO; }
    }
    if (kind == EVENT_NONE || best > master_ticks) break;
    cursor.now = best;
    switch (kind) {
      case EVENT_BOOT:
        g->booted = 1;
        psg(&cursor, 0x9Fu);  /* silence the four PSG voices */
        psg(&cursor, 0xBFu);
        psg(&cursor, 0xDFu);
        psg(&cursor, 0xFFu);
        break;
      case EVENT_DAC:
        /* The sample is due when Timer A overflows: that is confirmed on the device itself, never assumed. */
        if ((g->host.ym_read(g->host.context, 0u, best) & 1u) == 0u) { fail(g, GEMS_FAULT_DEVICE_TIMING); break; }
        (void)dac_event(&cursor);
        break;
      case EVENT_TICK:
        ++g->next_tick_index;
        (void)tick_event(&cursor);
        break;
      case EVENT_FIFO:
        g->fifo_check_pending = 0;
        (void)fifo_event(&cursor);
        break;
      default: break;
    }
  }
  return g->fault;
}
