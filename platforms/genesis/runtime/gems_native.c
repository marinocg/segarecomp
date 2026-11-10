/*
 * Native GEMS sound-driver replacement (ADR 0291). Independent C11 implementation of the observable behaviour of the verified 2.5 driver; see gems_native.h
 * for the contract.
 *
 * Structure: an event scheduler on the guest master clock (boot, DAC sample, VBlank tick, command-FIFO check); the command parser over the RAM-visible FIFO;
 * the sequencer (16 control blocks, every sequence event); voice allocation and release for FM, PSG tone and PSG noise voices; patch loading; the pitch
 * envelope and pitch-bend processors; the PSG envelope emulator; and the digital (DAC) voice with its two-half FIFO. Every chip write is a real YM2612 / PSG
 * write handed to the host with a deterministic guest time.
 */
#include "gems_native.h"

#include <string.h>

#include "runtime.h"
#include "segarecomp/device/sega/ym2612/ym2612.h"

/* ---- RAM layout shared with the 68000 (verified against the uploaded image's address map) ---- */
enum {
  RAM_CMD_WRITE = 0x36u,   /* command FIFO write pointer, owned by the 68000 */
  RAM_CMD_READ = 0x37u,    /* read pointer, owned by the driver */
  RAM_MAILBOXES = 0x1B20u, /* 32 mailbox bytes; sequences and commands store into [2..31] */
  RAM_CMD_FIFO = 0x1B40u,  /* 64-byte ring */
  FIFO_MASK = 0x3Fu
};

enum { CCB_ENABLED = 0x01, CCB_MUTED = 0x02, CCB_SFX = 0x08, CCB_RUNNING = 0x10, CCB_LOCKED = 0x20, CCB_RETRIGGER = 0x40, CCB_SUSTAIN = 0x80 };
enum { VOICE_FREE = 0x80, VOICE_RELEASE = 0x40, VOICE_LOCKED = 0x20, VOICE_TIMED = 0x10, VOICE_SFX = 0x08 };
enum { DEFAULT_SBPT = 204, DAC_FIFO_HALF = 128, INTERRUPT_LINE_TICKS = 3420, DAC_VOICE = 4, CH3_VOICE = 5, MAX_STEPS_PER_RUN = 1000000 };
enum { EVENT_NONE = 0, EVENT_BOOT, EVENT_DAC, EVENT_TICK, EVENT_FIFO };
enum { TG3 = 2 };

/* ---- data of the verified driver: frequency tables, carrier masks, the FM register map ---- */
static const uint16_t k_fm_frequency[13] = {644, 682, 723, 766, 811, 859, 910, 965, 1022, 1083, 1147, 1215, 1288};
static const uint16_t k_psg_frequency[64] = {
    0x03F9, 0x03C0, 0x038A, 0x0357, 0x0327, 0x02FA, 0x02CF, 0x02A7, 0x0281, 0x025D, 0x023B, 0x021B, 0x01FC, 0x01E0, 0x01C5, 0x01AC,
    0x0194, 0x017D, 0x0168, 0x0153, 0x0140, 0x012E, 0x011D, 0x010D, 0x00FE, 0x00F0, 0x00E2, 0x00D6, 0x00CA, 0x00BE, 0x00B4, 0x00AA,
    0x00A0, 0x0097, 0x008F, 0x0087, 0x007F, 0x0078, 0x0071, 0x006B, 0x0065, 0x005F, 0x005A, 0x0055, 0x0050, 0x004C, 0x0047, 0x0043,
    0x0040, 0x003C, 0x0039, 0x0035, 0x0032, 0x002F, 0x002D, 0x002A, 0x0028, 0x0026, 0x0023, 0x0021, 0x0020, 0x001E, 0x001C, 0x001C};
static const uint8_t k_carriers[8] = {0x08, 0x08, 0x08, 0x08, 0x0A, 0x0E, 0x0E, 0x0F};
/* register, data offset into the patch past its type byte (0 = write zero, bit 7 = a total level that attenuation applies to when the operator is a carrier) */
static const uint8_t k_fm_map[][2] = {
    {0xB0, 2},  {0xB4, 3},  {0x30, 4},  {0x40, 0x85}, {0x50, 6},  {0x60, 7},  {0x70, 8},  {0x80, 9},  {0x90, 0},  {0x38, 16},
    {0x48, 0x91}, {0x58, 18}, {0x68, 19}, {0x78, 20}, {0x88, 21}, {0x98, 0},  {0x34, 10}, {0x44, 0x8B}, {0x54, 12}, {0x64, 13},
    {0x74, 14}, {0x84, 15}, {0x94, 0},  {0x3C, 22}, {0x4C, 0x97}, {0x5C, 24}, {0x6C, 25}, {0x7C, 26}, {0x8C, 27}, {0x9C, 0}};

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
  uint64_t index = t / GENESIS_NTSC_MASTER_TICKS_PER_FRAME;
  while (tick_time(index) < t) ++index;
  return index;
}

/* ---- chip access: the time of the event being processed travels with the driver pointer ---- */
typedef struct Cursor { GemsNative *g; uint64_t now; } Cursor;

static void ym(Cursor *c, uint32_t port, uint8_t value) { c->g->host.ym_write(c->g->host.context, port, value, c->now); }
static void ym_pair(Cursor *c, uint32_t base_port, uint8_t reg, uint8_t value) {
  ym(c, base_port, reg);
  ym(c, base_port + 1u, value);
}
static void fm_channel_write(Cursor *c, uint8_t bank, uint8_t channel, uint8_t reg, uint8_t value) { ym_pair(c, bank, (uint8_t)(reg | channel), value); }
static void psg(Cursor *c, uint8_t value) { c->g->host.psg_write(c->g->host.context, value, c->now); }

/* The DAC routine's next sample is due when Timer A next overflows: the chip is asked (the timer free-runs; clearing its flag does not move it). */
static void dac_rearm(GemsNative *g) { g->dac_due = g->host.ym_timer_a_expiry(g->host.context); }

static int fail(GemsNative *g, GemsNativeFault fault) {
  if (g->fault == GEMS_FAULT_NONE) g->fault = fault;
  return 1;
}

static int rom_read(GemsNative *g, uint32_t address, uint8_t *destination, uint32_t count) {
  if (count == 0u) return 0;
  if (!g->host.rom_read(g->host.context, address & 0xFFFFFFu, destination, count)) return fail(g, GEMS_FAULT_UNMAPPED_RESOURCE);
  return 0;
}

static unsigned mailbox_index(uint8_t index) { return (unsigned)RAM_MAILBOXES + 2u + index; }

/* ---- voice tables ---- */
typedef struct VTable { GemsVoice *voice; unsigned count; } VTable;
static VTable fm_table(GemsNative *g) { VTable t = {g->fm, 6u}; return t; }
static VTable psg_table(GemsNative *g) { VTable t = {g->psg, 3u}; return t; }
static VTable noise_table(GemsNative *g) { VTable t = {g->noise, 1u}; return t; }

/* returns the flags of the voice allocated (and the voice in *out), or 0xFF; the original's priority rules */
static uint8_t alloc_voice(const GemsCcb *ccb, unsigned channel, VTable table, GemsVoice **out) {
  unsigned i, lowest = 0, freest = 0;
  unsigned lowest_priority = 0xFFu, freest_release = 0xFFu;
  int have_lowest = 0, have_freest = 0;
  for (i = 0; i < table.count; ++i) {
    GemsVoice *v = &table.voice[i];
    if ((v->flags & VOICE_LOCKED) != 0u) continue;
    if ((v->flags & VOICE_FREE) != 0u) {
      if (v->channel == channel && (ccb->flags & CCB_SUSTAIN) == 0u) { *out = v; return v->flags; }
      if (v->release < freest_release) { freest_release = v->release; freest = i; have_freest = 1; }
    } else if (v->priority < lowest_priority) {
      lowest_priority = v->priority;
      lowest = i;
      have_lowest = 1;
    }
  }
  if (have_freest) { *out = &table.voice[freest]; return table.voice[freest].flags; }
  if (!have_lowest || lowest_priority > ccb->priority) return 0xFFu;   /* no voice, or every voice outranks this channel */
  *out = &table.voice[lowest];
  return table.voice[lowest].flags;
}

static uint8_t alloc_special(const GemsCcb *ccb, GemsVoice *voice, GemsVoice **out) {
  if ((voice->flags & VOICE_FREE) != 0u) { *out = voice; return voice->flags; }
  if (voice->priority > ccb->priority) return 0xFFu;
  *out = voice;
  return voice->flags;
}

static void trigger_envelope(Cursor *c, GemsCcb *ccb, unsigned channel, uint8_t envelope);

/* the voice bookkeeping shared by FM, PSG and digital note-on (the original's VTANDET) */
static void voice_take(Cursor *c, GemsCcb *ccb, GemsVoice *voice, uint8_t flags) {
  GemsNative *g = c->g;
  flags = (uint8_t)(flags & 7u);
  g->note_voice = flags;
  voice->flags = flags;
  if (ccb->duration != 0u) voice->flags |= VOICE_TIMED;
  if ((ccb->flags & CCB_SFX) != 0u) voice->flags |= VOICE_SFX;
  voice->priority = ccb->priority;
  voice->note = g->note_number;
  voice->channel = g->note_channel;
  voice->duration = ccb->duration;
  voice->release = 254u;
  if ((ccb->flags & CCB_RETRIGGER) != 0u) trigger_envelope(c, ccb, g->note_channel, ccb->envelope);
}

/* ---- the PSG envelope emulator ---- */
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
      if (i == 3u) g->psg[TG3].flags = (uint8_t)(g->psg[TG3].flags & ~VOICE_LOCKED);
    }
    if ((command & 2u) != 0u && g->psg_mode[i] != 0u) {  /* key off: enter the release phase */
      g->psg_update[i] = 1u;
      g->psg_mode[i] = 4u;
    }
    if ((command & 1u) != 0u) {  /* key on: tone (and for tone voices the high part), then the attack phase */
      g->psg_level[i] = 0xFFu;
      psg(c, (uint8_t)(g->psg_low[i] | (0x80u + 0x20u * i)));
      if (i != 3u) psg(c, g->psg_high[i]);
      g->psg_update[i] = 1u;
      g->psg_mode[i] = 1u;
    }
    switch (g->psg_mode[i]) {
      case 1: {  /* attack */
        const int level = (int)g->psg_level[i] - (int)g->psg_attack[i];
        g->psg_update[i] = 1u;
        if (level > 0 && level > (int)g->psg_attack_level[i]) g->psg_level[i] = (uint8_t)level;
        else { g->psg_level[i] = g->psg_attack_level[i]; g->psg_mode[i] = 2u; }
        break;
      }
      case 2: {  /* decay */
        g->psg_update[i] = 1u;
        if (g->psg_level[i] < g->psg_sustain[i]) {
          const int level = (int)g->psg_level[i] + (int)g->psg_decay[i];
          if (level < 0x100 && level < (int)g->psg_sustain[i]) g->psg_level[i] = (uint8_t)level;
          else { g->psg_level[i] = g->psg_sustain[i]; g->psg_mode[i] = 3u; }
        } else if (g->psg_level[i] == g->psg_sustain[i]) {
          g->psg_mode[i] = 3u;
        } else {
          const int level = (int)g->psg_level[i] - (int)g->psg_decay[i];
          if (level >= 0 && level >= (int)g->psg_sustain[i]) g->psg_level[i] = (uint8_t)level;
          else { g->psg_level[i] = g->psg_sustain[i]; g->psg_mode[i] = 3u; }
        }
        break;
      }
      case 4: {  /* release */
        const int level = (int)g->psg_level[i] + (int)g->psg_release[i];
        g->psg_update[i] = 1u;
        if (level < 0x100) g->psg_level[i] = (uint8_t)level;
        else {
          g->psg_level[i] = 0xFFu;
          g->psg_mode[i] = 0u;
          if (i == 3u) g->psg[TG3].flags = (uint8_t)(g->psg[TG3].flags & ~VOICE_LOCKED);
        }
        break;
      }
      default: break;   /* off, sustain */
    }
  }
  for (i = 0; i < 4u; ++i)
    if (g->psg_update[i] != 0u) {
      g->psg_update[i] = 0u;
      psg(c, (uint8_t)((g->psg_level[i] >> 4) | (0x90u + 0x20u * i)));
    }
}

/* ---- pitch: frequency lookup, pitch / envelope bend ---- */
static uint16_t note_frequency(const GemsNative *g, int is_psg, unsigned channel, uint8_t note) {
  int16_t bend = (int16_t)(g->pitch_bend[channel & 15u] + g->envelope_bend[channel & 15u]);
  uint8_t index = (uint8_t)(note + (uint8_t)((uint16_t)bend >> 8));
  unsigned octave = 0;
  int32_t delta, scaled;
  int16_t value;
  const uint16_t *table;
  if (index >= 96u) {   /* outside C0..B7: peg at the end the bend went to */
    if (bend < 0) { bend = (int16_t)((uint16_t)bend & 0xFF00u); index = 0u; }
    else { bend = (int16_t)((uint16_t)bend | 0x00FFu); index = 95u; }
  }
  if (!is_psg) {
    octave = index / 12u;
    index = (uint8_t)(index % 12u);
    table = k_fm_frequency;
  } else {
    if (index < 33u) { index = 0u; bend = (int16_t)((uint16_t)bend & 0xFF00u); }
    else index = (uint8_t)(index - 33u);
    table = k_psg_frequency;
  }
  delta = (int32_t)table[index + 1u] - (int32_t)table[index];
  scaled = delta * (int32_t)((uint16_t)bend & 0xFFu);
  value = (int16_t)((scaled >> 8) & 0xFF);
  if ((value & 0x80) != 0) value = (int16_t)(value | (int16_t)0xFF00);
  value = (int16_t)((int32_t)table[index] + value);
  if (!is_psg) value = (int16_t)((uint16_t)value | (uint16_t)((octave << 3) << 8));
  return (uint16_t)value;
}

static uint8_t envelope_byte(const GemsNative *g, unsigned offset) { return offset < 128u ? g->envelope_buffer[offset] : g->fifo[(offset - 128u) & 0xFFu]; }

static void trigger_envelope(Cursor *c, GemsCcb *ccb, unsigned channel, uint8_t envelope) {
  GemsNative *g = c->g;
  GemsEcb *ecb = NULL;
  unsigned i;
  uint8_t offset_bytes[2];
  uint32_t offset;
  uint8_t chan_flags = (uint8_t)channel;
  for (i = 0; i < 4u; ++i)   /* an envelope already running for this very channel is retriggered */
    if (g->ecb[i].channel == (uint8_t)channel) { ecb = &g->ecb[i]; break; }
  if (ecb == NULL)
    for (i = 0; i < 4u; ++i)   /* else the first free one */
      if ((g->ecb[i].channel & 0x40u) != 0u) { ecb = &g->ecb[i]; break; }
  if (ecb == NULL) return;
  if ((ccb->flags & CCB_SFX) != 0u) chan_flags |= 0x20u;
  ecb->channel = chan_flags;
  ecb->counter = 0u;
  if (rom_read(g, g->envelope_table + (uint32_t)((envelope * 2u) & 0xFFu), offset_bytes, 2u)) return;
  offset = (uint32_t)offset_bytes[0] | ((uint32_t)offset_bytes[1] << 8);
  if (rom_read(g, g->envelope_table + offset, &g->envelope_buffer[ecb->buffer], 32u)) return;
  g->envelope_bend[channel & 15u] = (int16_t)((uint16_t)g->envelope_buffer[ecb->buffer] | ((uint16_t)g->envelope_buffer[ecb->buffer + 1u] << 8));   /* initial bend */
  ecb->pointer = (uint16_t)(ecb->buffer + 2u);
}

static void envelope_tick(GemsNative *g) {
  unsigned i;
  for (i = 0; i < 4u; ++i) {
    GemsEcb *ecb = &g->ecb[i];
    unsigned channel;
    int done = 0;
    if ((ecb->channel & 0x40u) != 0u) continue;   /* free */
    if ((ecb->channel & 0x20u) != 0u) { if ((g->tick_flags & 1u) == 0u) continue; }
    else if ((g->tick_flags & 2u) == 0u) continue;
    channel = ecb->channel & 0x0Fu;
    if (ecb->counter == 0u) {   /* the next segment: counter, then the 16-bit delta */
      const uint8_t segments = envelope_byte(g, ecb->pointer);
      if (segments == 0u) done = 1;
      else {
        ecb->delta = (uint16_t)(envelope_byte(g, ecb->pointer + 1u) | ((uint16_t)envelope_byte(g, ecb->pointer + 2u) << 8));
        ecb->pointer = (uint16_t)(ecb->pointer + 3u);
        ecb->counter = (uint8_t)(segments - 1u);
        g->envelope_bend[channel] = (int16_t)(g->envelope_bend[channel] + (int16_t)ecb->delta);
      }
    } else {
      --ecb->counter;
      g->envelope_bend[channel] = (int16_t)(g->envelope_bend[channel] + (int16_t)ecb->delta);
    }
    if (done) {
      g->envelope_bend[channel] = 0;
      ecb->channel = 0x40u;
    }
    g->bend_changed[channel] = 1u;
    g->need_bend = 1u;
  }
}

static void apply_bend(Cursor *c) {
  GemsNative *g = c->g;
  unsigned i;
  if (!g->need_bend) return;
  g->need_bend = 0u;
  for (i = 0; i < 6u; ++i) {
    const GemsVoice *v = &g->fm[i];
    uint8_t voice = (uint8_t)(v->flags & 7u), bank = 0u;
    const unsigned channel = v->channel & 15u;
    uint16_t frequency;
    if (g->bend_changed[channel] == 0u) continue;
    frequency = note_frequency(g, 0, channel, v->note);
    if (voice > 3u) { voice = (uint8_t)(voice - 4u); bank = 2u; }
    fm_channel_write(c, bank, voice, 0xA4u, (uint8_t)(frequency >> 8));
    fm_channel_write(c, bank, voice, 0xA0u, (uint8_t)frequency);
  }
  for (i = 0; i < 3u; ++i) {
    const GemsVoice *v = &g->psg[i];
    const unsigned channel = v->channel & 15u;
    uint16_t frequency;
    if (g->bend_changed[channel] == 0u) continue;
    frequency = note_frequency(g, 1, channel, v->note);
    psg(c, (uint8_t)((frequency & 0x0Fu) | 0x80u | (unsigned)((v->flags & 7u) << 5)));
    psg(c, (uint8_t)((frequency & 0xFF0u) >> 4));
  }
  memset(g->bend_changed, 0, sizeof g->bend_changed);
}

/* ---- the digital voice ---- */
static void dac_shutdown(Cursor *c) {  /* the FDF7 path: DAC routine and FIFO refill off, DAC mode off, voice free + releasing */
  GemsNative *g = c->g;
  g->dac_active = 0;
  g->fill_enabled = 0;
  ym_pair(c, 0u, 0x2Bu, 0x00u);
  g->fm[DAC_VOICE].flags = 0xC6u;
  g->fm[DAC_VOICE].duration = 0u;
}

/* FILLDACFIFO "needed" path */
static int dac_fill(Cursor *c) {
  GemsNative *g = c->g;
  uint8_t destination;
  int32_t remaining;
  uint32_t left;
  if (g->fdf_state >= 7u) { dac_shutdown(c); return 0; }
  remaining = (int32_t)g->sample_counter - DAC_FIFO_HALF;
  destination = g->fifo_write;
  g->fifo_write = (uint8_t)(g->fifo_write + DAC_FIFO_HALF);
  if (remaining > 0) {
    g->sample_counter = (uint16_t)remaining;
    if (rom_read(g, g->sample_pointer, &g->fifo[destination], DAC_FIFO_HALF)) return 1;
    g->sample_pointer = (g->sample_pointer + DAC_FIFO_HALF) & 0xFFFFFFu;
    return 0;
  }
  left = (uint32_t)(remaining + DAC_FIFO_HALF) & 0xFFu;   /* the samples that are left */
  if (left != 0u && rom_read(g, g->sample_pointer, &g->fifo[destination], left)) return 1;
  destination = (uint8_t)(destination + left);
  if (g->fdf_state != 5u) {   /* a one-shot sample (or a released loop) ends here: the original shuts the DAC down at once */
    dac_shutdown(c);
    return 0;
  }
  /* looped: continue from the loop start and complete the half */
  g->sample_pointer = (g->sample_pointer + left - g->sample_loop) & 0xFFFFFFu;
  g->sample_counter = g->sample_loop;
  left = (uint32_t)(DAC_FIFO_HALF - left);
  if (left == 0u) return 0;
  g->sample_counter = (uint16_t)(g->sample_counter - left);
  if (rom_read(g, g->sample_pointer, &g->fifo[destination], left)) return 1;
  g->sample_pointer = (g->sample_pointer + left) & 0xFFFFFFu;
  return 0;
}

static int dac_check_fill(Cursor *c) {
  GemsNative *g = c->g;
  if (g->fill_enabled && ((g->fifo_write ^ g->fifo_read) & 0x80u) != 0u) return dac_fill(c);
  return 0;
}

static void note_off_digital(GemsNative *g) {
  if ((g->sample_flags & 0x20u) != 0u) g->fdf_state = 7u;       /* clip at note-off: shut down at the next refill point */
  else if ((g->sample_flags & 0x10u) != 0u) g->fdf_state = 6u;  /* a looped sample: end of loop */
}

static int note_on_digital(Cursor *c, GemsCcb *ccb) {
  GemsNative *g = c->g;
  GemsVoice *voice = NULL;
  uint8_t header[12], flags, negated;
  uint32_t sample_number, base;
  const uint8_t allocated = alloc_special(ccb, &g->fm[DAC_VOICE], &voice);
  if (allocated == 0xFFu) return 0;
  if ((allocated & VOICE_FREE) == 0u) {
    g->dac_active = 0;   /* the running sample is cut: the DAC routine is disabled while the new one is prepared */
    if ((allocated & VOICE_LOCKED) == 0u) ym_pair(c, 0u, 0x28u, (uint8_t)(allocated & 7u));   /* an FM note held the voice: key it off */
  }
  voice_take(c, ccb, voice, allocated);
  voice->flags |= VOICE_LOCKED;   /* lock the voice from FM allocation */
  /* notes 48.. are samples 0.., notes 0..47 are samples 48..95 (the original subtracts 48 and adds 96 on a borrow) */
  sample_number = g->note_number >= 48u ? (uint32_t)g->note_number - 48u : (uint32_t)g->note_number + 48u;
  if (rom_read(g, g->sample_table + 12u * sample_number, header, 12u)) return 1;
  g->sample_flags = header[0];
  memcpy(g->sample_pointer_header, header + 1, 3);
  g->sample_skip = (uint16_t)(header[4] | (header[5] << 8));
  g->sample_first = (uint16_t)(header[6] | (header[7] << 8));
  g->sample_loop = (uint16_t)(header[8] | (header[9] << 8));
  g->sample_end = (uint16_t)(header[10] | (header[11] << 8));
  if (g->sample_first == 0u) { voice->flags = 0xC6u; return 0; }   /* an empty sample frees the voice again */
  if (ccb->patch[1] != 4u) g->sample_flags = (uint8_t)((g->sample_flags & 0xF0u) | ccb->patch[1]);   /* the patch's rate override */
  flags = g->sample_flags;
  g->dac_period_samples = (flags & 0x0Fu) == 0u ? 1024u : (unsigned)(flags & 0x0Fu);
  /* Timer A reload: -rate split over the two timer registers, so the period is `rate` native samples */
  negated = (uint8_t)(0x100u - (flags & 0x0Fu));
  g->fifo_read = 0u;
  ym_pair(c, 0u, 0x24u, (uint8_t)(((int8_t)negated) >> 2));
  ym_pair(c, 0u, 0x25u, (uint8_t)(negated & 3u));
  ym_pair(c, 0u, 0x2Bu, 0x80u);   /* DAC on */
  ym_pair(c, 0u, 0x27u, g->dac_timer_b);   /* load + start Timer A */
  dac_rearm(g);
  ym_pair(c, 2u, 0xB6u, 0xC0u);   /* channel 6 output to both speakers */
  base = (uint32_t)g->sample_pointer_header[0] | ((uint32_t)g->sample_pointer_header[1] << 8) | ((uint32_t)g->sample_pointer_header[2] << 16);
  g->sample_pointer = (g->sample_table + base + g->sample_skip) & 0xFFFFFFu;
  g->sample_counter = g->sample_first;
  g->fill_enabled = 1;
  g->fifo_write = 0u;
  g->fdf_state = (flags & 0x10u) != 0u ? 5u : 4u;
  if (dac_fill(c)) return 1;      /* the forced first fill */
  g->dac_active = 1;              /* the DAC routine is enabled after it, even if a short sample already shut the DAC down (as in the original) */
  g->dac_compressed = (flags & 0x80u) != 0u;
  if (g->dac_compressed) g->dac_pattern = 0xAAu;
  if (dac_check_fill(c)) return 1;   /* the refill the main loop performs right after */
  ++g->count_notes_digital;
  return 0;
}

static int dac_event(Cursor *c) {
  GemsNative *g = c->g;
  uint8_t sample = g->fifo[g->fifo_read];
  if (!g->dac_compressed) {
    g->fifo_read = (uint8_t)(g->fifo_read + 1u);
  } else {   /* 4-bit samples: low nibble first, then the high nibble of the same byte */
    g->dac_pattern = (uint8_t)((g->dac_pattern >> 1) | (g->dac_pattern << 7));
    if ((g->dac_pattern & 0x80u) != 0u) g->fifo_read = (uint8_t)(g->fifo_read + 1u);
    else sample = (uint8_t)(sample << 4);
    sample = (uint8_t)(sample & 0xF0u);
  }
  ym_pair(c, 0u, 0x27u, g->dac_timer_b);  /* reset Timer A (starts the next period at this instant) */
  ym_pair(c, 0u, 0x2Au, sample);
  ++g->count_dac_samples;
  dac_rearm(g);
  return dac_check_fill(c);
}

/* ---- FM and PSG notes ---- */
static void write_fm_patch(Cursor *c, const uint8_t *instrument, uint8_t bank, uint8_t channel) {
  GemsNative *g = c->g;
  size_t i;
  for (i = 0; i < sizeof k_fm_map / sizeof k_fm_map[0]; ++i) {
    uint8_t data = k_fm_map[i][1];
    ym(c, bank, (uint8_t)(k_fm_map[i][0] + channel));
    if (data != 0u) {
      if ((data & 0x80u) != 0u) {   /* a total level */
        data = (uint8_t)(data & 0x7Fu);
        if ((g->carriers & 1u) != 0u) {   /* a carrier: the note's attenuation applies */
          uint8_t level = (uint8_t)(127u - instrument[data]);
          const uint8_t reduction = (uint8_t)((((unsigned)level << 1) * g->note_attenuation) >> 8);
          level = (uint8_t)(level - reduction);
          data = (uint8_t)(127u - level);
        } else {
          data = instrument[data];
        }
        g->carriers >>= 1;
      } else {
        data = instrument[data];
      }
    }
    ym(c, bank + 1u, data);
  }
}

static int note_on_fm(Cursor *c, GemsCcb *ccb) {
  GemsNative *g = c->g;
  const uint8_t *patch = ccb->patch + 1;
  const int ch3_mode = (ccb->patch[2] & 0x40u) != 0u;
  GemsVoice *voice = NULL;
  uint8_t allocated, bank = 0u, voice_number;
  uint16_t frequency = 0;
  allocated = ch3_mode ? alloc_special(ccb, &g->fm[CH3_VOICE], &voice) : alloc_voice(ccb, g->note_channel, fm_table(g), &voice);
  if (allocated == 0xFFu) return 0;
  if ((allocated & VOICE_FREE) == 0u) ym_pair(c, 0u, 0x28u, (uint8_t)(allocated & 7u));   /* in use: key it off first */
  voice_take(c, ccb, voice, allocated);
  if (!ch3_mode) frequency = note_frequency(g, 0, g->note_channel, g->note_number);
  if (g->note_voice == 2u) {   /* channel 3: its mode bits join the DAC routine's timer reset */
    g->dac_timer_b = (uint8_t)(patch[1] | 0x15u);
    ym_pair(c, 0u, 0x27u, (uint8_t)(patch[1] | 0x05u));
    if (g->dac_active) dac_rearm(g);
  }
  voice_number = g->note_voice;
  if (voice_number > 3u) { voice_number = (uint8_t)(voice_number - 4u); bank = 2u; }
  if ((patch[0] & 0x08u) != 0u) ym_pair(c, 0u, 0x22u, patch[0]);   /* LFO, only when this patch uses it */
  g->carriers = k_carriers[patch[2] & 7u];
  write_fm_patch(c, patch, bank, voice_number);
  if ((patch[1] & 0x40u) == 0u) {
    fm_channel_write(c, bank, voice_number, 0xA4u, (uint8_t)(frequency >> 8));
    fm_channel_write(c, bank, voice_number, 0xA0u, (uint8_t)frequency);
  } else {   /* channel-3 mode: four operator frequencies from the patch */
    ym_pair(c, 0u, 0xA6u, patch[28]); ym_pair(c, 0u, 0xA2u, patch[29]);
    ym_pair(c, 0u, 0xACu, patch[30]); ym_pair(c, 0u, 0xA8u, patch[31]);
    ym_pair(c, 0u, 0xADu, patch[32]); ym_pair(c, 0u, 0xA9u, patch[33]);
    ym_pair(c, 0u, 0xAEu, patch[34]); ym_pair(c, 0u, 0xAAu, patch[35]);
  }
  ym_pair(c, 0u, 0x28u, (uint8_t)((patch[36] << 4) | g->note_voice));   /* key on */
  ++g->count_notes_fm;
  return 0;
}

static int note_on_psg(Cursor *c, GemsCcb *ccb, int noise) {
  GemsNative *g = c->g;
  const uint8_t *patch = ccb->patch + 1;
  GemsVoice *voice = NULL;
  uint8_t allocated;
  uint16_t frequency;
  unsigned v;
  allocated = noise ? alloc_special(ccb, &g->noise[0], &voice) : alloc_voice(ccb, g->note_channel, psg_table(g), &voice);
  if (allocated == 0xFFu) return 0;
  voice_take(c, ccb, voice, allocated);
  frequency = note_frequency(g, 1, g->note_channel, g->note_number);
  v = g->note_voice & 3u;
  g->psg_low[v] = (uint8_t)(frequency & 0x0Fu);
  g->psg_high[v] = (uint8_t)((frequency & 0xFF0u) >> 4);
  if (g->note_voice == 3u) {
    g->psg[TG3].flags = (uint8_t)(g->psg[TG3].flags & ~VOICE_LOCKED);   /* assume tone generator 3 is not locked by this noise patch */
    if ((ccb->patch[0] & 3u) == 3u) {   /* clocked by tone generator 3: the frequency goes there directly */
      psg(c, (uint8_t)(g->psg_low[3] | 0xC0u));
      psg(c, g->psg_high[3]);
      g->psg[TG3].flags = 0xA2u;
      g->psg[TG3].note = g->note_number;
      g->psg[TG3].channel = g->note_channel;
      g->psg_command[2] = 4u;
    }
    g->psg_low[3] = patch[0];   /* the noise data */
  }
  g->psg_attack[v] = patch[1];
  g->psg_sustain[v] = (uint8_t)(patch[2] << 4);
  g->psg_attack_level[v] = (uint8_t)(patch[3] << 4);
  g->psg_decay[v] = patch[4];
  g->psg_release[v] = patch[5];
  g->psg_command[v] |= 1u;
  ++g->count_notes_psg;
  return 0;
}

static int note_on(Cursor *c, unsigned channel, uint8_t note) {
  GemsNative *g = c->g;
  GemsCcb *ccb = &g->ccb[channel];
  const unsigned attenuation = (unsigned)g->master_attenuation + ccb->attenuation;
  g->note_attenuation = (attenuation & 0x80u) != 0u ? 127u : (uint8_t)attenuation;
  g->note_number = note;
  g->note_channel = (uint8_t)channel;
  switch (ccb->patch[0]) {
    case 0: return note_on_fm(c, ccb);
    case 1: return note_on_digital(c, ccb);
    case 2: return note_on_psg(c, ccb, 0);
    case 3: return note_on_psg(c, ccb, 1);
    default: return 0;   /* an undefined patch plays nothing, exactly like the original */
  }
}

/* returns the flags of the voice released, or 0xFF when no voice held this note on this channel */
static uint8_t dealloc_voice(VTable table, uint8_t channel, uint8_t note) {
  unsigned i;
  for (i = 0; i < table.count; ++i) {
    GemsVoice *v = &table.voice[i];
    if ((v->flags & VOICE_FREE) != 0u) continue;
    if (v->note == note && v->channel == channel) {
      const uint8_t old = v->flags;
      if ((old & 0x27u) != 0x26u) v->flags = (uint8_t)((old & 0x27u) | 0xC0u);   /* not the digital voice: free and releasing */
      return old;
    }
  }
  return 0xFFu;
}

static void note_off(Cursor *c, uint8_t channel, uint8_t note) {
  GemsNative *g = c->g;
  uint8_t flags = dealloc_voice(fm_table(g), channel, note);
  if (flags != 0xFFu) {
    if ((flags & 0x27u) == 0x26u) note_off_digital(g);
    else ym_pair(c, 0u, 0x28u, (uint8_t)(flags & 7u));
    return;
  }
  flags = dealloc_voice(psg_table(g), channel, note);
  if (flags == 0xFFu) flags = dealloc_voice(noise_table(g), channel, note);
  if (flags != 0xFFu) g->psg_command[flags & 3u] |= 2u;
}

/* ---- timers and clipping ---- */
static void voice_timer_table(Cursor *c, VTable table, int is_psg) {
  GemsNative *g = c->g;
  unsigned i;
  for (i = 0; i < table.count; ++i) {
    GemsVoice *v = &table.voice[i];
    const unsigned wanted = (v->flags & VOICE_SFX) != 0u ? 1u : 2u;
    if ((g->tick_flags & wanted) == 0u) continue;
    if ((v->flags & VOICE_RELEASE) != 0u) {
      if (--v->release == 0u) v->flags = (uint8_t)(v->flags & ~VOICE_RELEASE);
      continue;
    }
    if ((v->flags & VOICE_TIMED) == 0u) continue;
    ++v->duration;
    if (v->duration != 0u) continue;
    v->flags = (uint8_t)(v->flags & ~(VOICE_TIMED | VOICE_SFX));   /* the registered duration elapsed: a note-off */
    if ((v->flags & 0x2Fu) == 0x26u) {
      note_off_digital(g);
    } else {
      const uint8_t voice = (uint8_t)(v->flags & 7u);
      v->flags |= 0xC0u;
      if (is_psg) g->psg_command[voice & 3u] |= 2u;
      else ym_pair(c, 0u, 0x28u, voice);
    }
  }
}

static void voice_timer(Cursor *c) {
  GemsNative *g = c->g;
  voice_timer_table(c, fm_table(g), 0);
  voice_timer_table(c, psg_table(g), 1);
  voice_timer_table(c, noise_table(g), 1);
}

static void clip_table(Cursor *c, VTable table, int is_psg) {
  GemsNative *g = c->g;
  unsigned i;
  for (i = 0; i < table.count; ++i) {
    GemsVoice *v = &table.voice[i];
    uint8_t original, voice, bank_channel, bank;
    if ((g->ccb[v->channel & 0x0Fu].flags & CCB_RUNNING) != 0u) continue;   /* its channel is still running */
    original = v->flags;
    voice = (uint8_t)(original & 7u);
    v->flags = (uint8_t)(voice | VOICE_FREE);
    v->duration = 0u;
    v->release = 0u;
    if (is_psg) { g->psg_command[voice & 3u] = 4u; continue; }
    if ((original & VOICE_LOCKED) != 0u) {   /* digital mode */
      g->dac_active = 0;
      g->fill_enabled = 0;
      ym_pair(c, 0u, 0x2Bu, 0x00u);
      continue;
    }
    bank_channel = voice;
    bank = 0u;
    if (voice >= 3u) { bank_channel = (uint8_t)(voice - 4u); bank = 2u; }
    fm_channel_write(c, bank, bank_channel, 0x40u, 0x7Fu);   /* clamp all four operators' total level */
    fm_channel_write(c, bank, bank_channel, 0x44u, 0x7Fu);
    fm_channel_write(c, bank, bank_channel, 0x48u, 0x7Fu);
    fm_channel_write(c, bank, bank_channel, 0x4Cu, 0x7Fu);
    ym_pair(c, 0u, 0x28u, voice);   /* key off */
  }
}

static void clip_all(Cursor *c) {
  GemsNative *g = c->g;
  unsigned i;
  ++g->count_clips;
  clip_table(c, fm_table(g), 0);
  clip_table(c, psg_table(g), 1);
  clip_table(c, noise_table(g), 1);
  for (i = 0; i < 4u; ++i) {   /* envelopes of channels that are no longer running */
    if ((g->ecb[i].channel & 0x40u) != 0u) continue;
    if ((g->ccb[g->ecb[i].channel & 0x0Fu].flags & CCB_RUNNING) != 0u) continue;
    g->ecb[i].channel |= 0x40u;
  }
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

static void set_tempo(GemsNative *g, uint8_t bpm) { g->sbpt = (uint16_t)(((unsigned)bpm * 218u) >> 7); }

static int start_sequence(Cursor *c, uint8_t number);
static int stop_sequence(Cursor *c, uint8_t number);
static void pause_sequence(Cursor *c, uint8_t number);
static void resume_all(GemsNative *g);

/* the result of a sequence command: continue with the next byte, go through the registered delay, or leave the sequencer */
enum { SEQ_NEXT = 0, SEQ_DELAY = 1, SEQ_LEAVE = 2 };

static int sequence_command(Cursor *c, unsigned index, uint8_t byte, int *action) {
  GemsNative *g = c->g;
  GemsCcb *ccb = &g->ccb[index];
  uint8_t a, b;
  *action = SEQ_DELAY;
  switch (byte - 96u) {
    case 0:   /* end of sequence: the control block is freed */
      ccb->flags = 0u;
      ccb->duration = 0u;
      *action = SEQ_LEAVE;
      return 0;
    case 1:   /* program change */
      if (getsbyte(g, ccb, &a)) return 1;
      ccb->program = a;
      return fetch_patch(g, ccb);
    case 2:   /* envelope */
      if (getsbyte(g, ccb, &a)) return 1;
      ccb->envelope = a;
      if ((ccb->flags & CCB_RETRIGGER) == 0u) trigger_envelope(c, ccb, index, a);
      return g->fault != GEMS_FAULT_NONE;
    case 3: return 0;   /* no-op: only triggers the registered delay */
    case 4: {   /* loop start */
      unsigned slot = 0;
      while (slot < 3u && ccb->loops[slot][0] != 0u) ++slot;   /* the first free stack entry (the last one is reused when the stack is full) */
      if (getsbyte(g, ccb, &a)) return 1;
      ccb->loops[slot][0] = a;
      ccb->loops[slot][1] = (uint8_t)ccb->address;
      ccb->loops[slot][2] = (uint8_t)(ccb->address >> 8);
      *action = SEQ_NEXT;
      return 0;
    }
    case 5: {   /* loop end */
      int slot = 3;
      uint16_t old_low, new_low;
      while (slot > 0 && ccb->loops[slot][0] == 0u) --slot;
      *action = SEQ_NEXT;
      if (ccb->loops[slot][0] == 0u) return 0;   /* a loop end without a loop start */
      if (ccb->loops[slot][0] != 127u) {
        --ccb->loops[slot][0];
        if (ccb->loops[slot][0] == 0u) return 0;   /* end of a finite loop: do not go back */
      }
      old_low = (uint16_t)(ccb->loops[slot][1] | (ccb->loops[slot][2] << 8));
      new_low = (uint16_t)(ccb->address & 0xFFFFu);
      ccb->address = (ccb->address & 0xFF0000u) | old_low;
      if (old_low > new_low) ccb->address = (ccb->address - 0x10000u) & 0xFFFFFFu;   /* the loop start is in the previous 64 KiB */
      return 0;
    }
    case 6:   /* retrigger mode */
      if (getsbyte(g, ccb, &a)) return 1;
      ccb->flags = a != 0u ? (uint8_t)(ccb->flags | CCB_RETRIGGER) : (uint8_t)(ccb->flags & ~CCB_RETRIGGER);
      return 0;
    case 7:   /* sustain */
      if (getsbyte(g, ccb, &a)) return 1;
      ccb->flags = a != 0u ? (uint8_t)(ccb->flags | CCB_SUSTAIN) : (uint8_t)(ccb->flags & ~CCB_SUSTAIN);
      return 0;
    case 8:   /* tempo (stored offset by -40) */
      if (getsbyte(g, ccb, &a)) return 1;
      set_tempo(g, (uint8_t)(a + 40u));
      return 0;
    case 9: {   /* mute / unmute channel `a & 15` of this sequence */
      unsigned i;
      if (getsbyte(g, ccb, &a)) return 1;
      for (i = 0; i < 16u; ++i) {
        GemsCcb *other = &g->ccb[i];
        if ((other->flags & CCB_ENABLED) == 0u || other->sequence != ccb->sequence || (a & 0x0Fu) != other->channel) continue;
        other->flags = (a & 0x10u) != 0u ? (uint8_t)(other->flags & ~CCB_MUTED) : (uint8_t)(other->flags | CCB_MUTED);
        break;
      }
      return 0;
    }
    case 10: return getsbyte(g, ccb, &ccb->priority);   /* priority */
    case 11:   /* start another sequence */
      if (getsbyte(g, ccb, &a)) return 1;
      return start_sequence(c, a);
    case 12:   /* pitch bend */
      if (getsbyte(g, ccb, &a) || getsbyte(g, ccb, &b)) return 1;
      g->pitch_bend[index] = (int16_t)((uint16_t)a | ((uint16_t)b << 8));
      g->bend_changed[index] = 1u;
      g->need_bend = 1u;
      return 0;
    case 13: ccb->flags |= CCB_SFX; return 0;   /* SFX time base */
    case 14:   /* sample playback rate of this channel's digital patch */
      if (getsbyte(g, ccb, &a)) return 1;
      if (ccb->patch[0] == 1u) ccb->patch[1] = a;
      return 0;
    case 15: {   /* goto: a signed 16-bit offset from the address after the operand */
      int16_t offset;
      if (getsbyte(g, ccb, &a) || getsbyte(g, ccb, &b)) return 1;
      offset = (int16_t)((uint16_t)a | ((uint16_t)b << 8));
      ccb->address = (uint32_t)((int32_t)ccb->address + offset) & 0xFFFFFFu;
      *action = SEQ_NEXT;
      return 0;
    }
    case 16:   /* store a value in a mailbox */
      if (getsbyte(g, ccb, &a) || getsbyte(g, ccb, &b)) return 1;
      if (a >= 30u) return fail(g, GEMS_FAULT_INVALID_RESOURCE);
      g->host.ram[mailbox_index(a)] = b;
      return 0;
    case 17: {   /* if (mailbox relation value) skip the operand offset, else jump by it */
      uint8_t mailbox, relation, value, offset;
      int condition;
      if (getsbyte(g, ccb, &a) || getsbyte(g, ccb, &relation) || getsbyte(g, ccb, &value) || getsbyte(g, ccb, &offset)) return 1;
      if (a >= 30u) return fail(g, GEMS_FAULT_INVALID_RESOURCE);
      mailbox = g->host.ram[mailbox_index(a)];
      switch (relation) {
        case 1: condition = value != mailbox; break;
        case 2: condition = value > mailbox; break;
        case 3: condition = value >= mailbox; break;
        case 4: condition = value < mailbox; break;
        case 5: condition = value <= mailbox; break;
        default: condition = value == mailbox; break;
      }
      if (!condition) ccb->address = (ccb->address + offset) & 0xFFFFFFu;
      *action = SEQ_NEXT;
      return 0;
    }
    case 18: {   /* special codes */
      if (getsbyte(g, ccb, &a) || getsbyte(g, ccb, &b)) return 1;
      switch (a) {
        case 4: g->master_attenuation = b; break;
        case 5: ccb->attenuation = b; break;
        case 0: return stop_sequence(c, b);
        case 1: pause_sequence(c, b); break;
        case 3: pause_sequence(c, g->host.ram[RAM_MAILBOXES + 2u]); break;
        case 2: resume_all(g); break;
        default: break;
      }
      return g->fault != GEMS_FAULT_NONE;
    }
    default: *action = SEQ_NEXT; return 0;   /* an unknown command: the original goes straight on to the next byte */
  }
}

static int sequencer(Cursor *c, unsigned index) {
  GemsNative *g = c->g;
  GemsCcb *ccb = &g->ccb[index];
  uint8_t byte;
  unsigned steps = 0;
  ++ccb->timer;
  if (ccb->timer != 0u) return 0;
  if (getsbyte(g, ccb, &byte)) return 1;
  for (;;) {
    int action = SEQ_DELAY;
    if (++steps > MAX_STEPS_PER_RUN) return fail(g, GEMS_FAULT_INVALID_RESOURCE);   /* a sequence that never yields would hang the machine */
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
      if ((ccb->flags & CCB_MUTED) == 0u && note_on(c, index, byte)) return 1;
    } else {
      if (sequence_command(c, index, byte, &action)) return 1;
      if (action == SEQ_LEAVE) return 0;
    }
    if (action == SEQ_DELAY && ccb->delay != 0u) {   /* a non-zero registered delay ends this run */
      ccb->timer = ccb->delay;
      return 0;
    }
    if (getsbyte(g, ccb, &byte)) return 1;
  }
}

static int update_sequences(Cursor *c) {
  GemsNative *g = c->g;
  unsigned i;
  for (i = 0; i < 16u; ++i) {
    GemsCcb *ccb = &g->ccb[i];
    if ((ccb->flags & CCB_RUNNING) == 0u) continue;
    if ((ccb->flags & CCB_SFX) != 0u) { if ((g->tick_flags & 1u) == 0u) continue; }
    else if ((g->tick_flags & 2u) == 0u) continue;
    if (sequencer(c, i)) return 1;
  }
  return 0;
}

static int tick_event(Cursor *c) {
  GemsNative *g = c->g;
  g->tick_flags = 1u;
  ++g->count_ticks;
  g->sbptacc = (uint16_t)(g->sbptacc + g->sbpt);
  psg_envelope_tick(c);
  /* The original's main loop consumes one sub-beat per pass and keeps passing until the accumulator is drained: at fast tempos a tick carries more than one beat,
   * and each further beat is a pass of its own (envelopes, voice timers and sequencers run again, now without the tick flag). */
  for (;;) {
    if ((g->sbptacc >> 8) >= 1u) {
      g->sbptacc = (uint16_t)(g->sbptacc - 0x100u);
      g->tick_flags |= 2u;
    }
    if (g->tick_flags == 0u) break;
    envelope_tick(g);
    voice_timer(c);
    if (update_sequences(c)) return 1;
    apply_bend(c);
    if (g->fault != GEMS_FAULT_NONE) return 1;
    g->tick_flags = 0u;
  }
  return 0;
}

/* ---- sequence control ---- */
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
    memset(ccb->loops, 0, sizeof ccb->loops);
    ccb->envelope = 0u;
    ccb->priority = 0u;   /* the registered delay / duration and the program number deliberately survive from the previous use, as in the original */
    ccb->attenuation = 0u;
    g->envelope_bend[ccb_index] = 0;
    g->pitch_bend[ccb_index] = 0;
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

static void pause_sequence(Cursor *c, uint8_t number) {
  GemsNative *g = c->g;
  unsigned i;
  for (i = 0; i < 16u; ++i) {
    GemsCcb *ccb = &g->ccb[i];
    if ((ccb->flags & CCB_ENABLED) == 0u || (ccb->flags & CCB_LOCKED) != 0u) continue;
    if (ccb->sequence == number) ccb->flags = (uint8_t)(ccb->flags & ~CCB_RUNNING);
  }
  clip_all(c);
}

static void resume_all(GemsNative *g) {
  unsigned i;
  for (i = 0; i < 16u; ++i)
    if ((g->ccb[i].flags & CCB_LOCKED) == 0u && (g->ccb[i].flags & CCB_ENABLED) != 0u) g->ccb[i].flags |= CCB_RUNNING;
}

/* ---- commands from the 68000 ---- */
static int command_arguments(uint8_t command) {
  switch (command) {
    case 0: case 1: case 2: case 6: case 7: case 14: case 20: case 26: case 27: case 31: return 2;
    case 3: case 5: case 16: case 18: case 28: case 29: case 32: return 1;
    case 4: case 23: return 3;
    case 30: return 4;
    case 11: return 12;
    default: return 0;   /* no arguments - or an unknown id, which the original ignores (the bytes that follow are skipped one by one until the next start byte) */
  }
}

static int run_command(Cursor *c, uint8_t command, const uint8_t *args) {
  GemsNative *g = c->g;
  unsigned i;
  switch (command) {
    case 0: case 6: case 7: case 14: case 20: case 28: case 29: case 31: {   /* commands that address a control block first */
      GemsCcb *ccb;
      if (args[0] >= 16u) return fail(g, GEMS_FAULT_INVALID_COMMAND_STREAM);
      ccb = &g->ccb[args[0]];
      switch (command) {
        case 0: return note_on(c, args[0], args[1]);
        case 6:
          ccb->envelope = args[1];
          if ((ccb->flags & CCB_RETRIGGER) == 0u) trigger_envelope(c, ccb, args[0], args[1]);
          break;
        case 7: ccb->flags = args[1] != 0u ? (uint8_t)(ccb->flags | CCB_RETRIGGER) : (uint8_t)(ccb->flags & ~CCB_RETRIGGER); break;
        case 14: ccb->flags = args[1] != 0u ? (uint8_t)(ccb->flags | CCB_SUSTAIN) : (uint8_t)(ccb->flags & ~CCB_SUSTAIN); break;
        case 20: ccb->priority = args[1]; break;
        case 28: ccb->flags |= CCB_LOCKED; break;
        case 29: ccb->flags = (uint8_t)(ccb->flags & ~CCB_LOCKED); break;
        default: ccb->attenuation = args[1]; break;
      }
      return g->fault != GEMS_FAULT_NONE;
    }
    case 1: note_off(c, args[0], args[1]); return 0;
    case 2: {
      GemsCcb *ccb;
      if (args[0] >= 16u) return fail(g, GEMS_FAULT_INVALID_COMMAND_STREAM);
      ccb = &g->ccb[args[0]];
      ccb->program = args[1];
      return fetch_patch(g, ccb);
    }
    case 3:
      for (i = 0; i < 16u; ++i)
        if (g->ccb[i].program == args[0] && fetch_patch(g, &g->ccb[i])) return 1;
      return 0;
    case 4:
      if (args[0] >= 16u) return fail(g, GEMS_FAULT_INVALID_COMMAND_STREAM);
      g->pitch_bend[args[0]] = (int16_t)((uint16_t)args[1] | ((uint16_t)args[2] << 8));
      g->bend_changed[args[0]] = 1u;
      g->need_bend = 1u;
      return 0;
    case 5: set_tempo(g, args[0]); return 0;
    case 11:
      g->patch_table = (uint32_t)args[0] | ((uint32_t)args[1] << 8) | ((uint32_t)args[2] << 16);
      g->envelope_table = (uint32_t)args[3] | ((uint32_t)args[4] << 8) | ((uint32_t)args[5] << 16);
      g->sequence_table = (uint32_t)args[6] | ((uint32_t)args[7] << 8) | ((uint32_t)args[8] << 16);
      g->sample_table = (uint32_t)args[9] | ((uint32_t)args[10] << 8) | ((uint32_t)args[11] << 16);
      g->have_pointers = 1;
      return 0;
    case 12:
      for (i = 0; i < 16u; ++i) g->ccb[i].flags = (uint8_t)(g->ccb[i].flags & ~CCB_RUNNING);
      clip_all(c);
      return 0;
    case 13: resume_all(g); return 0;
    case 16: return start_sequence(c, args[0]);
    case 18: return stop_sequence(c, args[0]);
    case 22:
      for (i = 0; i < 16u; ++i) { g->ccb[i].flags = 0u; g->ccb[i].duration = 0u; }
      clip_all(c);
      return 0;
    case 23:   /* mute / unmute channel args[1] of sequence args[0] */
      for (i = 0; i < 16u; ++i) {
        GemsCcb *ccb = &g->ccb[i];
        if ((ccb->flags & CCB_ENABLED) == 0u || ccb->sequence != args[0] || ccb->channel != args[1]) continue;
        ccb->flags = (args[2] & 1u) != 0u ? (uint8_t)(ccb->flags | CCB_MUTED) : (uint8_t)(ccb->flags & ~CCB_MUTED);
      }
      return 0;
    case 26:
      if (args[0] >= 16u) return fail(g, GEMS_FAULT_INVALID_COMMAND_STREAM);
      if (g->ccb[args[0]].patch[0] == 1u) g->ccb[args[0]].patch[1] = args[1];
      return 0;
    case 27:
      if (args[0] >= 30u) return fail(g, GEMS_FAULT_INVALID_RESOURCE);
      g->host.ram[mailbox_index(args[0])] = args[1];
      return 0;
    case 30:   /* bend the first running channel of this sequence / MIDI channel */
      for (i = 0; i < 16u; ++i) {
        GemsCcb *ccb = &g->ccb[i];
        if ((ccb->flags & CCB_ENABLED) == 0u || ccb->sequence != args[0] || ccb->channel != args[1]) continue;
        g->pitch_bend[i] = (int16_t)((uint16_t)args[2] | ((uint16_t)args[3] << 8));
        g->bend_changed[i] = 1u;
        g->need_bend = 1u;
        break;
      }
      return 0;
    case 32: g->master_attenuation = args[0]; return 0;
    default: return 0;
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
    needed = 2u + (unsigned)arguments;
    if (available < needed) return 0;  /* arguments still being written */
    {
      uint8_t args[14];
      int i;
      for (i = 0; i < arguments; ++i) args[i] = ram[RAM_CMD_FIFO + ((read + 2u + (unsigned)i) & FIFO_MASK)];
      ram[RAM_CMD_READ] = (uint8_t)((read + needed) & FIFO_MASK);
      ++g->count_commands;
      if (g->host.command_tap != NULL) {
        uint8_t whole[16];
        whole[0] = start; whole[1] = command;
        memcpy(whole + 2, args, (size_t)arguments);
        g->host.command_tap(g->host.context, whole, needed, c->now);
      }
      if (run_command(c, command, args)) return 1;
      apply_bend(c);   /* the next pass of the original's main loop applies pending bends before it looks at the FIFO again */
      if (g->fault != GEMS_FAULT_NONE) return 1;
    }
  }
}

/* ---- boot / scheduling ---- */
static void reset_state(GemsNative *g) {
  static const uint8_t k_fm_flags[6] = {0x80, 0x81, 0x84, 0x85, 0x86, 0x82};
  unsigned i;
  const GemsNativeHost host = g->host;
  memset(g, 0, sizeof *g);
  g->host = host;
  g->sbpt = DEFAULT_SBPT;
  g->dac_timer_b = 0x15u;
  for (i = 0; i < 16u; ++i) g->ccb[i].patch[0] = 0xFFu;
  for (i = 0; i < 4u; ++i) g->psg_level[i] = 0xFFu;
  for (i = 0; i < 6u; ++i) { g->fm[i].flags = k_fm_flags[i]; g->fm[i].note = 0x50u; }
  for (i = 0; i < 3u; ++i) { g->psg[i].flags = (uint8_t)(0x80u + i); g->psg[i].note = 0x50u; }
  g->noise[0].flags = 0x83u;
  g->noise[0].note = 0x50u;
  for (i = 0; i < 4u; ++i) { g->ecb[i].channel = 0x40u; g->ecb[i].buffer = (uint8_t)(32u * i); }
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
      if (g->dac_active && g->dac_due != UINT64_MAX) {
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
