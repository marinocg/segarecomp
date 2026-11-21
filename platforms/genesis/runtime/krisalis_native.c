/*
 * Native Krisalis sound-driver replacement (ADR 0318). See krisalis_native.h and docs/architecture/genesis-native-krisalis.md.
 *
 * Structure: the 68000 writes a command byte and (for a row) six pattern cells into the mailbox in the sound RAM while it holds the Z80 bus. When the
 * bus is back, the driver notices the command, consumes it and turns it into a time-ordered list of effects (YM2612 writes, bank-register bits, RAM
 * bytes) that are released on the guest master clock; the PCM channel is a separate event stream. The channel state, the mailbox and the voice bank
 * live in the sound RAM at the original's addresses, so a re-upload of the driver resets them exactly as it does the original, and a machine snapshot
 * carries them. The driver's private state (the effect queue and the DAC player) is what krisalis_native_state_put serialises.
 */
#include "krisalis_native.h"

#include <string.h>

#define CYCLE_TICKS 15u   /* master ticks per Z80 cycle */

/* Nominal Z80 cycle costs of the original's routines (event-level model: the order of the effects is exact, their spacing is calibrated against an
 * independent execution of the original image, tools/krisalis_reference_check.py; it is not instruction-cycle-accurate). */
#define CYC_YM_WRITE 69u        /* one YM2612 port write including the busy-wait call (generic) */
#define CYC_BANK_BIT 38u        /* one serial write of the bank register */


/* Per-revision spacing, measured on the original images run by an independent emulator (tools/krisalis_reference_check.py): the gaps between the YM2612 writes of a voice
 * load and of a frequency / key write, and the work around them. Revision 2 shares revision 3's figures until it is measured. */
typedef struct Timing {
  uint16_t dispatch;        /* command noticed -> first cell */
  uint16_t cell_gap;        /* an empty cell */
  uint16_t note_prologue;   /* a note cell's work before its first write */
  uint16_t tick_channel;    /* one channel of a tick */
  uint16_t voice_a2d, voice_d2a;                          /* voice register pair: address -> data, data -> next address */
  uint16_t f_a2d1, f_d2a1, f_a2d2, f_d2a2, f_a2d3;       /* A4/A0/$28 pairs of a frequency / key write */
  uint16_t relead;          /* the second frequency / key write of a re-trigger (key on after key off) */
} Timing;
static const Timing k_timing[4] = {
    {0},
    {420, 205, 378, 145, 67, 112, 73, 73, 66, 165, 64, 181},    /* revision 1 */
    {900, 320, 410, 572, 67, 183, 73, 99, 66, 217, 64, 259},    /* revision 2 (provisional) */
    {900, 320, 410, 572, 67, 183, 73, 99, 66, 217, 64, 259},    /* revision 3 */
};

enum { OP_YM = 1, OP_RAM = 2, OP_BANK = 3, OP_DAC_CUT = 4, OP_DAC_START = 5 };   /* OP_DAC_CUT: an FM note on FM6 ends a music sample: the player's count becomes 1 (the sample ends at its next byte) */

/* ---- the revision descriptors: the RAM layout the 68000 and the original share ---------------------------------------------------------------- */
typedef struct Layout {
  uint16_t mailbox, chan_base;
  uint8_t chan_stride;
  uint16_t note_table;
  uint16_t reg_dummy;   /* unused (kept for alignment of the table) */
} Layout;

static const Layout k_layout[4] = {
    {0, 0, 0, 0, 0},
    {0x0E58u, 0x0E27u, 7u, 0x11D0u, 0},    /* revision 1 (Chuck Rock) */
    {0x0F58u, 0x0EE8u, 14u, 0x15CFu, 0},   /* revision 2 (Sensible Soccer) */
    {0x0FACu, 0x0F3Cu, 14u, 0x179Fu, 0},   /* revision 3 (Boogerman, Mickey Mania, The Lion King) */
};

/* mailbox offsets (all revisions unless noted) */
enum {
  MB_CMD = 0x00, MB_JINGLE = 0x01, MB_NOTES = 0x02, MB_BANKHI = 0x0E, MB_JBANKHI = 0x0F, MB_SMPADDR = 0x10, MB_SMPRATE = 0x12, MB_SMPPHASE = 0x13, MB_SMPLEN = 0x14,
  MB_TRIGGER = 0x16, MB_SAMPLES = 0x18, MB_SFXBUSY = 0x38,
  MB_MUSICVOL = 0x3A, MB_JINGLEVOL = 0x3C, MB_SFXPAN = 0x3E, MB_JINGLEPAN = 0x40, MB_SFXBANK = 0x42, MB_MUSICBANK = 0x44, MB_LASTJINGLE = 0x46  /* revisions 2 and 3 */
};

/* channel-state field offsets: revision 1 has 7 bytes (+0 slide, +2 pitch, +4 voice); revisions 2/3 have 14 */
enum { CH_SLIDE = 0, CH_PITCH = 2, CH_TARGET = 4, CH_PORTA = 6, CH_VOICE8 = 8, CH_LOADED = 9, CH_FMS = 0x0B, CH_PAN = 0x0C, CH_RETRIGGER = 0x0D, CH1_VOICE = 4 };

#define VOICE_BANK 0x0400u
#define VOICE_SIZE 32u
#define RAM_MASK 0x1FFFu

const char *krisalis_native_fault_name(KrisalisFault fault) {
  switch (fault) {
    case KRISALIS_FAULT_NONE: return "none";
    case KRISALIS_FAULT_UNMAPPED_RESOURCE: return "unmapped_resource";
    case KRISALIS_FAULT_INVALID_STATE: return "invalid_state";
    case KRISALIS_FAULT_UNSUPPORTED_REVISION: return "unsupported_revision";
  }
  return "unknown";
}

/* ---- the note table: block (0..7) x 12 F-numbers, entry N = 2 + 12 * block + semitone, as published (tuned for PAL) ----------------------------- */
static const uint16_t k_fnum[12] = {0x0269, 0x028D, 0x02B4, 0x02DD, 0x0309, 0x0337, 0x0368, 0x039C, 0x03D3, 0x040D, 0x044B, 0x048C};

static uint16_t note_table_entry(const KrisalisNative *k, unsigned n) {
  if (n < 2u) return 0u;
  if (n <= 97u) {
    const unsigned index = n - 2u, block = index / 12u;
    return (uint16_t)((block << 11) | k_fnum[index % 12u]);
  }
  {  /* beyond the table the original reads whatever follows it in the uploaded image */
    const unsigned address = (k_layout[k->revision].note_table + 2u * n) & RAM_MASK;
    return (uint16_t)(k->host.ram[address] | (k->host.ram[(address + 1u) & RAM_MASK] << 8));
  }
}

/* The silent voice ("voice $80"): all four TLs $7F, everything else zero. */
static const uint8_t k_silent_voice[VOICE_SIZE] = {0, 0, 0, 0, 0x7F, 0x7F, 0x7F, 0x7F};

/* ---- RAM access ------------------------------------------------------------------------------------------------------------------------- */
static uint8_t *mb(KrisalisNative *k, unsigned offset) { return &k->host.ram[(k_layout[k->revision].mailbox + offset) & RAM_MASK]; }
static uint16_t mb16(KrisalisNative *k, unsigned offset) { return (uint16_t)(*mb(k, offset) | (*mb(k, offset + 1u) << 8)); }

static uint8_t *chan(KrisalisNative *k, unsigned index, unsigned field) {
  const Layout *l = &k_layout[k->revision];
  return &k->host.ram[(l->chan_base + index * l->chan_stride + field) & RAM_MASK];
}
static uint16_t chan16(KrisalisNative *k, unsigned index, unsigned field) { return (uint16_t)(*chan(k, index, field) | (*chan(k, index, field + 1u) << 8)); }
static void chan_set16(KrisalisNative *k, unsigned index, unsigned field, uint16_t v) { *chan(k, index, field) = (uint8_t)v; *chan(k, index, field + 1u) = (uint8_t)(v >> 8); }
/* revision-aware voice byte (rev 1 keeps the voice at +4) */
static uint8_t *chan_voice(KrisalisNative *k, unsigned index) { return chan(k, index, k->revision == KRISALIS_REVISION_1 ? CH1_VOICE : CH_VOICE8); }

/* ---- effect queue ------------------------------------------------------------------------------------------------------------------------ */
typedef struct Build {
  KrisalisNative *k;
  uint64_t t;      /* time of the next effect */
  uint64_t last;   /* time of the last queued effect */
} Build;

static int fail(KrisalisNative *k, KrisalisFault fault) {
  if (k->fault == KRISALIS_FAULT_NONE) k->fault = fault;
  return 0;
}

static void push(Build *b, uint8_t kind, uint8_t a, uint8_t bb, uint8_t c) {
  KrisalisNative *k = b->k;
  KrisalisOp *op;
  uint64_t previous;
  uint64_t dt;
  uint16_t slot;
  if (k->fault != KRISALIS_FAULT_NONE) return;
  if (k->queue_count >= KRISALIS_QUEUE_MAX) { fail(k, KRISALIS_FAULT_INVALID_STATE); return; }
  slot = (uint16_t)((k->queue_head + k->queue_count) % KRISALIS_QUEUE_MAX);
  if (k->queue_count == 0u) { k->queue_base = b->t; dt = 0u; }
  else {
    previous = b->last;
    dt = b->t - previous;
    if (dt > 0xFFFFFFFFu) { fail(k, KRISALIS_FAULT_INVALID_STATE); return; }
  }
  op = &k->queue[slot];
  op->dt = (uint32_t)dt;
  op->kind = kind;
  op->a = a;
  op->b = bb;
  op->c = c;
  ++k->queue_count;
  b->last = b->t;
}

static void adv(Build *b, unsigned cycles) { b->t += (uint64_t)cycles * CYCLE_TICKS; }
static void ymg(Build *b, uint8_t port, uint8_t value, unsigned gap) { adv(b, gap); push(b, OP_YM, port, 0u, value); }
/* an address/data pair on part I (part 0) or part II (part 2): `gap_addr` cycles after the previous effect, then `gap_data` more for the data */
static void ym_pair_g(Build *b, unsigned part, uint8_t reg, uint8_t value, unsigned gap_addr, unsigned gap_data) {
  ymg(b, (uint8_t)part, reg, gap_addr);
  ymg(b, (uint8_t)(part + 1u), value, gap_data);
}
static void ym_pair(Build *b, unsigned part, uint8_t reg, uint8_t value) { ym_pair_g(b, part, reg, value, CYC_YM_WRITE, CYC_YM_WRITE); }
static void ram_set(Build *b, unsigned offset, uint8_t value) { push(b, OP_RAM, (uint8_t)offset, (uint8_t)(offset >> 8), value); }

/* ---- the DAC player ------------------------------------------------------------------------------------------------------------------------- */
static uint32_t bank_now(const KrisalisNative *k) { return k->host.bank_get(k->host.context); }

static int sample_byte(KrisalisNative *k, uint8_t *out) {
  uint8_t byte;
  const uint32_t address = (bank_now(k) << 15) | (k->dac_pointer & 0x7FFFu);
  if (!k->host.rom_read(k->host.context, address, &byte, 1u)) return fail(k, KRISALIS_FAULT_UNMAPPED_RESOURCE);
  *out = (uint8_t)(byte + 0x80u);   /* signed -> unsigned */
  return 1;
}

static void ym_now(KrisalisNative *k, uint8_t port, uint8_t value, uint64_t t) { k->host.ym_write(k->host.context, port, value, t); }

/* the end of a sample: FM6's pan comes back from its voice, the DAC goes off, the SFX flag clears */
static void dac_end(KrisalisNative *k, uint64_t t) {
  const unsigned voice = *chan_voice(k, 5);
  const uint8_t pan = k->host.ram[(VOICE_BANK + voice * VOICE_SIZE + 0x19u) & RAM_MASK];
  k->dac_redirect = 0u;
  ym_now(k, 2u, 0xB6u, t);
  ym_now(k, 3u, pan, t);
  ym_now(k, 0u, 0x2Bu, t);
  ym_now(k, 1u, 0x00u, t);
  *mb(k, MB_SFXBUSY) = 0u;
  k->dac_active = 0u;
}

/* The event-level pacing of the original's polling. Revisions 2 and 3 add the rate to an 8-bit phase on every poll of their wait loop and emit one byte
 * per carry; revision 1 writes the current byte on every poll and advances the pointer when the integer phase changes. */
#define CYC_IDLE_ITERATION_R3 112u
#ifndef CYC_BUSY_ITERATION_R3
#define CYC_BUSY_ITERATION_R3 150u   /* while a row is being worked the DAC is polled between register writes: a poll every ~150 cycles on average instead of every 112 */
#endif
#define CYC_DAC_BYTE_R3 72u
#define CYC_DAC_POLL_R1 284u
#define CYC_DAC_ADVANCE_R1 7u

static void dac_schedule(KrisalisNative *k, uint64_t from) {
  if (k->revision == KRISALIS_REVISION_1) { k->dac_due = from + (uint64_t)CYC_DAC_POLL_R1 * CYCLE_TICKS; return; }   /* (a poll that advances the pointer adds a few cycles: dac_event) */
  if (k->dac_rate == 0u) { k->dac_due = UINT64_MAX; return; }
  {
    const unsigned need = 256u - k->dac_phase;
    const unsigned n = (need + k->dac_rate - 1u) / k->dac_rate;   /* loop iterations until the 8-bit phase carries */
    k->dac_due = from + (uint64_t)n * (k->queue_count != 0u ? CYC_BUSY_ITERATION_R3 : CYC_IDLE_ITERATION_R3) * CYCLE_TICKS;
  }
}

/* one DAC event at time t */
static void dac_event(KrisalisNative *k, uint64_t t) {
  uint8_t value;
  if (k->revision == KRISALIS_REVISION_1) {
    const uint8_t rate = *mb(k, MB_SMPRATE);
    unsigned sum;
    int advanced = 0;
    if (!sample_byte(k, &value)) return;
    ym_now(k, 0u, 0x2Au, t);
    ym_now(k, 1u, value, t);
    sum = (unsigned)rate + k->dac_phase;
    k->dac_phase = (uint8_t)sum;
    ym_now(k, 2u, 0xB6u, t);
    k->dac_rate = (uint8_t)(k->dac_rate + (sum >> 8));   /* the integer phase (D') */
    if (k->dac_rate != k->dac_last_hi) {
      ++k->dac_pointer;
      --k->dac_left;
      advanced = 1;
    }
    k->dac_last_hi = k->dac_rate;
    ym_now(k, 3u, 0xC0u, t);
    ++k->count_dac_samples;
    if (k->dac_left == 0u) { dac_end(k, t); return; }
    dac_schedule(k, t);
    if (advanced) k->dac_due += (uint64_t)CYC_DAC_ADVANCE_R1 * CYCLE_TICKS;
    return;
  }
  /* revisions 2 and 3: a byte per carry; the phase advanced by the iterations that led here */
  {
    const unsigned need = 256u - k->dac_phase;
    const unsigned n = (need + k->dac_rate - 1u) / k->dac_rate;
    k->dac_phase = (uint8_t)(k->dac_phase + n * k->dac_rate);
  }
  if (!sample_byte(k, &value)) return;
  ym_now(k, 0u, 0x2Au, t);
  ym_now(k, 1u, value, t);
  ++k->dac_pointer;
  --k->dac_left;
  ++k->count_dac_samples;
  if (k->dac_left == 0u) { dac_end(k, t); return; }
  dac_schedule(k, t + (uint64_t)CYC_DAC_BYTE_R3 * CYCLE_TICKS);
}

/* ---- the row-processing routines -------------------------------------------------------------------------------------------------------------- */
static uint8_t jingle_state(KrisalisNative *k) { return *mb(k, MB_JINGLE); }

/* the register list of a voice: entry i of the original's table is (part, register) for channel c; the first 22 follow the voice bytes, the 26th is $B4 */
static uint8_t voice_register(unsigned entry) {
  static const uint8_t base[26] = {0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0xB0, 0xB4};
  return base[entry];
}

static void write_register_entry(Build *b, unsigned channel, unsigned entry, uint8_t value) {
  const unsigned part = channel >= 3u ? 2u : 0u;
  uint8_t reg = (uint8_t)(voice_register(entry) + (channel % 3u));
  /* revision 3 patches FM6's $B6 entry to $28 while a sample plays, so that a voice load cannot disturb the sample's pan (the write lands on an unused register) */
  if (b->k->revision == KRISALIS_REVISION_3 && channel == 5u && entry == 25u && b->k->dac_redirect) reg = 0x28u;
  ym_pair_g(b, part, reg, value, k_timing[b->k->revision].voice_d2a, k_timing[b->k->revision].voice_a2d);
}

/* the carriers of an algorithm: bit i set = operator byte +4+i is a carrier */
static unsigned carrier_mask(uint8_t algorithm) {
  switch (algorithm & 7u) {
    case 4: return 0xCu;
    case 5: case 6: return 0xEu;
    case 7: return 0xFu;
    default: return 0x8u;
  }
}

/* ApplyVolume: the four TL registers of a voice with the attenuation added to the carriers (clamped at $7F) */
static void apply_volume(Build *b, unsigned channel, const uint8_t *voice, uint8_t attenuation) {
  const unsigned mask = carrier_mask(voice[0x18]);
  unsigned i;
  for (i = 0; i < 4u; ++i) {
    uint8_t value = voice[4u + i];
    if (mask & (1u << i)) {
      value = (uint8_t)(value + attenuation);
      if (value & 0x80u) value = 0x7Fu;
    }
    write_register_entry(b, channel, 4u + i, value);
  }
}

static const uint8_t *voice_pointer(KrisalisNative *k, uint8_t number) {
  if (number & 0x80u) return k_silent_voice;
  return &k->host.ram[(VOICE_BANK + (unsigned)number * VOICE_SIZE) & RAM_MASK];
}

/* LoadVoice: a whole voice into channel `channel`; `jingle_volume` selects the jingle attenuation (revisions 2 and 3) */
static void load_voice(Build *b, unsigned channel, uint8_t number, int jingle_volume, unsigned state) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  const uint8_t *voice = voice_pointer(k, number);
  uint8_t attenuation = 0u;
  unsigned entry;
  for (entry = 0; entry < 4u; ++entry) write_register_entry(b, channel, entry, voice[entry]);
  if (!rev1) attenuation = jingle_volume ? *mb(k, MB_JINGLEVOL) : *mb(k, MB_MUSICVOL);
  if (attenuation != 0u) {
    apply_volume(b, channel, voice, attenuation);
    entry = 8u;
  } else {
    entry = 4u;
  }
  for (; entry < 26u; ++entry) {
    uint8_t value = voice[entry];
    if (entry == 25u && !rev1) {   /* $B4: pan / AMS / FMS: the copy of the voice byte the overrides patch (and leave behind in the voice's scratch byte) */
      const uint8_t fms = *chan(k, state, CH_FMS), pan = *chan(k, state, CH_PAN);
      uint8_t copy = voice[0x19];
      if ((fms | pan) != 0u) {
        if (fms != 0u) copy = (uint8_t)((copy & 0xF8u) | fms);
        if (pan != 0u) copy = (uint8_t)((copy & 0x3Fu) | pan);
      }
      if (!(number & 0x80u)) k->host.ram[(VOICE_BANK + (unsigned)number * VOICE_SIZE + 0x1Au) & RAM_MASK] = copy;
      value = copy;
    }
    write_register_entry(b, channel, entry, value);
  }
}

/* LoadChannelVoice (revisions 2 and 3): remembers the voice as loaded; FM5 while a jingle plays uses the jingle volume and pan */
static void load_channel_voice(Build *b, unsigned channel, uint8_t number, unsigned state) {
  KrisalisNative *k = b->k;
  if (k->revision == KRISALIS_REVISION_1) { load_voice(b, channel, number, 0, state); return; }
  *chan(k, state, CH_LOADED) = number;
  if (channel == 4u && jingle_state(k) != 0u) {
    const uint8_t pan = *mb(k, MB_JINGLEPAN);
    const uint8_t *voice = voice_pointer(k, number);
    load_voice(b, channel, number, 1, state);
    *mb(k, MB_JINGLE) = 0xFEu;   /* the jingle voice is loaded */
    if (pan != 0u) ym_pair(b, 2u, 0xB5u, (uint8_t)((voice[0x19] & 0x3Fu) | (pan & 0xC0u)));
    return;
  }
  load_voice(b, channel, number, 0, state);
}

/* WriteFreqKey: block/F-number and key state of a channel */
static void write_freq_key(Build *b, unsigned channel, uint16_t pitch, unsigned lead) {
  const unsigned part = channel >= 3u ? 2u : 0u, ch3 = channel >= 3u ? channel - 3u : channel;
  unsigned code = channel & 7u;
  const Timing *t = &k_timing[b->k->revision];
  ym_pair_g(b, part, (uint8_t)(0xA4u + ch3), (uint8_t)((pitch >> 8) & 0x3Fu), lead, t->f_a2d1);
  ym_pair_g(b, part, (uint8_t)(0xA0u + ch3), (uint8_t)pitch, t->f_d2a1, t->f_a2d2);
  if (code >= 3u) ++code;
  if (pitch & 0x4000u) ++b->k->count_key_events;
  ym_pair_g(b, 0u, 0x28u, (uint8_t)(code | ((pitch & 0x4000u) ? 0xF0u : 0u)), t->f_d2a2, t->f_a2d3);
}

/* A sample's bank: nine serial bits of the bank register (A15..A22, then A23 = 0) */
static void set_bank(Build *b, uint8_t bank) {
  unsigned i, value = bank;
  for (i = 0; i < 9u; ++i) {
    push(b, OP_BANK, 0u, 0u, (uint8_t)(value & 1u));
    value >>= 1;
    adv(b, CYC_BANK_BIT);
  }
}

/* the DAC player is armed (StartSample): DAC enable, the player registers */
static void start_sample(Build *b, uint8_t bank, int have_bank) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  uint16_t address = mb16(k, MB_SMPADDR), length = mb16(k, MB_SMPLEN);
  uint8_t rate = *mb(k, MB_SMPRATE);
  if (have_bank) set_bank(b, bank);
  ym_pair(b, 0u, 0x2Bu, 0x80u);
  if (!rev1) {   /* revisions 2 and 3 skip the first eight and the last four bytes (whole sample when it is shorter than twelve) */
    if (length >= 12u) { address = (uint16_t)(address + 8u); length = (uint16_t)(length - 12u); }
  }
  k->pend_pointer = address;
  k->pend_left = length;
  k->pend_phase = rate;                                 /* E' = the rate byte as loaded */
  k->pend_rate = rev1 ? *mb(k, MB_SMPPHASE) : rate;     /* D': revision 1 the integer phase, 2/3 the rate */
  *mb(k, MB_TRIGGER) = 0u;
  push(b, OP_DAC_START, 0u, 0u, 0u);                    /* the player's registers are loaded at this point of the command, not before */
}

static void start_sfx_sample(Build *b) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  *mb(k, MB_SFXBUSY) = 0xFFu;
  if (rev1) { start_sample(b, 0u, 0); return; }
  {
    const uint8_t pan = *mb(k, MB_SFXPAN) ? *mb(k, MB_SFXPAN) : 0xC0u;
    ym_pair(b, 2u, 0xB6u, pan);
    start_sample(b, *mb(k, MB_SFXBANK), 1);
  }
}

/* a drum cell (N = $6D..$74) on FM6 */
static void music_sample_note(Build *b, unsigned n, uint8_t rate, unsigned channel) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  unsigned index;
  uint8_t entry[4];
  if (channel != 5u) return;
  if (*mb(k, MB_SFXBUSY) != 0u) return;   /* an SFX sample has priority */
  index = (n - 0x6Du) * 4u;
  *mb(k, MB_SMPRATE) = rate;
  memcpy(entry, mb(k, MB_SAMPLES + index), 4u);
  *mb(k, MB_SMPADDR) = entry[1];
  *mb(k, MB_SMPADDR + 1u) = (uint8_t)(entry[0] | 0x80u);
  *mb(k, MB_SMPLEN) = entry[3];
  *mb(k, MB_SMPLEN + 1u) = entry[2];
  if (rev1) { start_sample(b, 0u, 0); return; }
  ym_pair(b, 2u, 0xB6u, 0xC0u);                            /* a music drum is always centred */
  if (*mb(k, MB_MUSICVOL) != 0u) return;                   /* drums are dropped while the music fades */
  start_sample(b, *mb(k, MB_MUSICBANK), 1);
}

/* N >= $6D: drum samples and the cell commands */
static void command_note(Build *b, unsigned n, uint8_t lo, unsigned channel) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  if (n < 0x75u) { music_sample_note(b, n, lo, channel); return; }
  if (n == 0x7Du) {   /* speed (the 68000's) and the upper voice bank flag (bit 7) */
    const uint8_t flag = (lo & 0x80u) ? 0xFFu : 0u;
    if (channel == 4u && jingle_state(k) != 0u) *mb(k, MB_JBANKHI) = flag;
    else *mb(k, MB_BANKHI) = flag;
    return;
  }
  if (rev1) return;   /* $7A..$7C belong to the later revisions */
  if (n == 0x7Au) { *chan(k, channel, CH_PORTA) = lo; return; }
  if (n == 0x7Cu) {
    *chan(k, channel, CH_FMS) = (uint8_t)(lo & 7u);
    load_channel_voice(b, channel, *chan(k, channel, CH_VOICE8), channel);
    *chan(k, channel, CH_LOADED) = 0xFFu;
    return;
  }
  if (n == 0x7Bu) {
    *chan(k, channel, CH_PAN) = (uint8_t)((lo & 3u) << 6);
    load_channel_voice(b, channel, *chan(k, channel, CH_VOICE8), channel);
    *chan(k, channel, CH_LOADED) = 0xFFu;
  }
}

static uint8_t apply_voice_bank(KrisalisNative *k, unsigned channel, uint8_t voice) {
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  if (jingle_state(k) != 0u && channel == 4u) {
    if (!rev1) { *chan(k, 4u, CH_VOICE8) = 0xFFu; *chan(k, 4u, CH_LOADED) = 0xFFu; }
    return *mb(k, MB_JBANKHI) != 0u ? (uint8_t)(voice | 0x20u) : voice;
  }
  return *mb(k, MB_BANKHI) != 0u ? (uint8_t)(voice | 0x20u) : voice;
}

/* one pattern cell */
static void do_note_word(Build *b, unsigned channel, uint8_t lo, uint8_t hi) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  const unsigned n = (hi >> 1) & 0x7Fu;
  uint16_t pitch;
  unsigned instrument;
  if (!rev1) *chan(k, channel, CH_RETRIGGER) = 0xFFu;
  if (n >= 0x6Du) { command_note(b, n, lo, channel); return; }
  if ((hi | lo) == 0u) return;
  instrument = (((unsigned)hi << 8 | lo) >> 4) & 0x1Fu;
  if (instrument != 0u) {
    const uint8_t voice = apply_voice_bank(k, channel, (uint8_t)(instrument - 1u));
    uint8_t *stored = chan_voice(k, channel);
    const int same = voice == *stored;
    *stored = voice;
    if (rev1) {
      if (!same) load_voice(b, channel, voice, 0, channel);
    } else {
      int load = !same;
      if (same) {
        if (*chan(k, channel, CH_PORTA) != 0u) *chan(k, channel, CH_RETRIGGER) = 0u;
        if (*chan(k, channel, CH_FMS) != 0u) load = 1;
      }
      if (load) {
        *chan(k, channel, CH_FMS) = 0u;
        if (*chan(k, channel, CH_VOICE8) != *chan(k, channel, CH_LOADED)) load_channel_voice(b, channel, *chan(k, channel, CH_VOICE8), channel);
      }
    }
  }
  if (n != 0u) {
    const uint16_t target = note_table_entry(k, n);
    uint16_t current;
    if (rev1) {
      chan_set16(k, channel, CH_PITCH, target);
    } else {
      chan_set16(k, channel, CH_TARGET, target);
      current = chan16(k, channel, CH_PITCH);
      current |= 0x4000u;
      if (*chan(k, channel, CH_RETRIGGER) != 0u) current = (uint16_t)(current & ~0x4000u);
      if (*chan(k, channel, CH_PORTA) == 0u) current = target;
      chan_set16(k, channel, CH_PITCH, current);
    }
    chan_set16(k, channel, CH_SLIDE, 0u);
    if (channel == 5u && jingle_state(k) == 0u && *mb(k, MB_SFXBUSY) == 0u && k->dac_active) push(b, OP_DAC_CUT, 0u, 0u, (uint8_t)(rev1 ? 0u : 1u));   /* an FM note on FM6 stops a music sample, at this point of the row */
  }
  if ((lo & 0x0Fu) != 0u) {   /* effect nibble: slide step = +/-(E & 7) * 4 per tick, bit 3 = down */
    uint8_t magnitude = (uint8_t)(lo & 7u);
    int16_t step;
    if (lo & 8u) magnitude = (uint8_t)(0u - magnitude);
    step = (int16_t)(int8_t)magnitude;
    chan_set16(k, channel, CH_SLIDE, (uint16_t)(step * 4));
  }
  pitch = chan16(k, channel, CH_PITCH);
  if (!(pitch & 0x4000u)) {   /* key flag clear: key off, then key on */
    write_freq_key(b, channel, pitch, 0u);
    write_freq_key(b, channel, (uint16_t)(pitch | 0x4000u), k_timing[k->revision].relead);
  }
  *chan(k, channel, CH_PITCH + 1u) = (uint8_t)(*chan(k, channel, CH_PITCH + 1u) | 0x40u);
}

/* ---- the per-tick pitch update ------------------------------------------------------------------------------------------------------------------ */
static void slide_channel(Build *b, unsigned channel) {
  KrisalisNative *k = b->k;
  uint16_t slide = chan16(k, channel, CH_SLIDE), pitch;
  if (slide == 0u) return;
  pitch = chan16(k, channel, CH_PITCH);
  if (k->revision == KRISALIS_REVISION_1) {
    pitch = (uint16_t)(slide + pitch);
    pitch |= 0x4000u;
  } else {
    uint16_t hl = (uint16_t)((uint16_t)(slide * 2u) + pitch);
    if ((hl & 0x07FFu) < 0x0269u) hl = (uint16_t)(hl + 0x0269u - 0x0800u);   /* below $269: the previous block */
    if ((hl & 0x07FFu) >= 0x04D0u) hl = (uint16_t)(hl - 0x0269u + 0x0800u);  /* above $4D0: the next block */
    pitch = (uint16_t)(hl | 0x4000u);
  }
  chan_set16(k, channel, CH_PITCH, pitch);
  write_freq_key(b, channel, pitch, 0u);
}

static void update_channel(Build *b, unsigned channel) {
  KrisalisNative *k = b->k;
  uint8_t speed;
  uint16_t target, current;
  if (k->revision == KRISALIS_REVISION_1 || (speed = *chan(k, channel, CH_PORTA)) == 0u) { slide_channel(b, channel); return; }
  target = chan16(k, channel, CH_TARGET);
  current = (uint16_t)(chan16(k, channel, CH_PITCH) & 0x3FFFu);
  if (current == 0u) {   /* the first portamento step from silence jumps to the target */
    chan_set16(k, channel, CH_PITCH, target);
    write_freq_key(b, channel, target, 0u);
    return;
  }
  if (target == current) return;
  if (target > current) {
    uint16_t hl = (uint16_t)(current + speed);
    if ((hl & 0x07FFu) >= 0x04D0u) hl = (uint16_t)(hl - 0x0269u + 0x0800u);
    if (hl >= target) hl = target;
    hl = (uint16_t)(hl | 0x4000u);
    chan_set16(k, channel, CH_PITCH, hl);
    write_freq_key(b, channel, hl, 0u);
  } else {
    uint16_t hl = (uint16_t)(current - speed);
    if ((hl & 0x07FFu) < 0x0269u) hl = (uint16_t)(hl + 0x0269u - 0x0800u);
    if (hl < target) hl = target;
    hl = (uint16_t)(hl | 0x4000u);
    chan_set16(k, channel, CH_PITCH, hl);
    write_freq_key(b, channel, hl, 0u);
  }
}

static void update_all_channels(Build *b) {
  unsigned c;
  for (c = 0; c < 6u; ++c) {
    update_channel(b, c);
    adv(b, k_timing[b->k->revision].tick_channel);
  }
}

/* ---- volume refresh (revisions 2 and 3): after every command the carrier TLs are rewritten with the music (and jingle) attenuation ----------------- */
static void apply_volume_for_voice(Build *b, unsigned channel, uint8_t number, uint8_t attenuation) {
  apply_volume(b, channel, voice_pointer(b->k, number), attenuation);
}

static void main_loop_refresh(Build *b) {
  KrisalisNative *k = b->k;
  unsigned c;
  if (k->revision == KRISALIS_REVISION_1) return;
  if (*mb(k, MB_MUSICVOL) != 0u) {
    const uint8_t attenuation = *mb(k, MB_MUSICVOL);
    for (c = 0; c < 6u; ++c) {
      const uint8_t number = *chan(k, c, CH_VOICE8);
      if (c == 4u && jingle_state(k) != 0u) continue;
      if (!(number & 0x80u)) apply_volume_for_voice(b, c, number, attenuation);
    }
  }
  if (jingle_state(k) == 0xFEu) {
    const uint8_t number = *chan(k, 4u, CH_VOICE8);
    if (!(number & 0x80u)) apply_volume_for_voice(b, 4u, number, *mb(k, MB_JINGLEVOL));
  }
}

/* ---- the commands ------------------------------------------------------------------------------------------------------------------------- */
static void cmd_reset(Build *b) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  const int jingle = jingle_state(k) != 0u;
  unsigned c;
  for (c = 0; c < 6u; ++c) {
    const int skip = c == 4u && jingle;   /* FM5 keeps its state while a jingle owns it */
    if (!rev1) {
      if (c != 4u) *chan(k, c, CH_PORTA) = 0u;
      *chan(k, c, CH_FMS) = 0u;
      *chan(k, c, CH_PAN) = 0u;
    }
    if (!skip) { chan_set16(k, c, CH_SLIDE, 0u); chan_set16(k, c, CH_PITCH, 0x2000u); *chan_voice(k, c) = 0xFFu; }
  }
  if (!rev1 && !jingle) {
    *chan(k, 4u, CH_PORTA) = 0u;
    for (c = 0; c < 6u; ++c) *chan(k, c, CH_LOADED) = 0xFFu;
  }
}

static void cmd_silence(Build *b) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  const uint8_t jingle = jingle_state(k);
  unsigned c;
  if (!rev1) {
    *mb(k, MB_MUSICVOL) = 0u;
    if (*mb(k, MB_SFXBUSY) == 0u && k->dac_active) k->dac_left = 1u;   /* stop a music sample (not an SFX sample) */
  } else if (k->dac_active) {
    k->dac_left = 1u;
  }
  for (c = 0; c < 6u; ++c) {
    if (!rev1) *chan(k, c, CH_LOADED) = 0xFFu;
    if (!(c == 4u && jingle != 0u)) load_channel_voice(b, c, 0x80u, c);
    if (!rev1) {
      *chan(k, c, CH_PAN) = 0u;
      *chan(k, c, CH_FMS) = 0u;
      *chan(k, c, CH_PORTA) = 0u;
      *chan(k, c, CH_LOADED) = 0xFFu;
    }
  }
}

static void cmd_reload(Build *b) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  unsigned c;
  for (c = 0; c < 6u; ++c) {
    const uint8_t number = *chan_voice(k, c);
    if (!rev1) { *chan(k, c, CH_FMS) = 0u; *chan(k, c, CH_PAN) = 0u; }
    load_channel_voice(b, c, number, c);
    if (!rev1) *chan(k, c, CH_LOADED) = 0xFFu;
  }
  if (!rev1) { ram_set(b, k_layout[k->revision].mailbox + MB_CMD, 0u); }
}

/* one full command from the mailbox (the wait loop's dispatch) */
static void consume_command(Build *b, uint8_t command) {
  KrisalisNative *k = b->k;
  const int rev1 = k->revision == KRISALIS_REVISION_1;
  uint8_t cells[12];
  unsigned c;
  memcpy(cells, mb(k, MB_NOTES), 12u);
  if (k->host.command_tap != NULL) {
    uint8_t bytes[13];
    bytes[0] = command;
    memcpy(bytes + 1, cells, 12u);
    k->host.command_tap(k->host.context, bytes, 13u, b->t);
  }
  ++k->count_commands;
  if (!rev1) *mb(k, MB_CMD) = 0u;   /* revisions 2 and 3 acknowledge when they read the command */
  switch (command) {
    case 1: ++k->count_ticks; update_all_channels(b); break;
    case 2: cmd_reset(b); break;
    case 3: {
      if (cells[1] >= 6u) { fail(k, KRISALIS_FAULT_INVALID_STATE); return; }   /* load voice cells[0] on channel cells[1]; no 68000 module issues it */
      load_channel_voice(b, cells[1], cells[0], 7u);   /* the original keeps this command's state in a scratch channel record */
      break;
    }
    case 4: cmd_silence(b); break;
    case 7: cmd_reload(b); break;
    default: {   /* a new row */
      const uint8_t jstate = (uint8_t)(jingle_state(k) & 0xFEu);
      ++k->count_rows;
      if (!rev1 && *mb(k, MB_LASTJINGLE) != jstate) {
        *mb(k, MB_LASTJINGLE) = jstate;
        *chan(k, 4u, CH_PORTA) = 0u;
        *chan(k, 4u, CH_FMS) = 0u;
        *chan(k, 4u, CH_PAN) = 0u;
      }
      adv(b, k_timing[k->revision].dispatch);
      for (c = 0; c < 6u; ++c) {
        const unsigned n = (cells[2u * c + 1u] >> 1) & 0x7Fu;
        if (n >= 1u && n < 0x6Du) adv(b, k_timing[k->revision].note_prologue);
        do_note_word(b, c, cells[2u * c], cells[2u * c + 1u]);
        adv(b, k_timing[k->revision].cell_gap);
      }
      update_all_channels(b);
      break;
    }
  }
  main_loop_refresh(b);
  if (rev1) ram_set(b, k_layout[k->revision].mailbox + MB_CMD, 0u);   /* revision 1 acknowledges when the work is done */
}

static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/* The wait loop of the original, measured on the original images: one iteration (trigger check, DAC check, command read) takes this many cycles; the command byte is read
 * near its end and the SFX trigger at its start. The 68000 takes the bus again and again for a few cycles while it waits for the acknowledge: the Z80 stands still in the middle of
 * its loop during a hold, so the loop's phase (not the time since the last hold) decides when it reaches a command. */
static unsigned loop_cycles(const KrisalisNative *k) {
  if (k->revision == KRISALIS_REVISION_1) return k->dac_active ? CYC_DAC_POLL_R1 : 84u;
  return CYC_IDLE_ITERATION_R3;
}
static uint64_t idle_reach(const KrisalisNative *k, unsigned target_phase) {   /* the time at which the loop stands at `target_phase` */
  const unsigned l = loop_cycles(k);
  const unsigned phase = k->idle_phase % l;
  const unsigned wait = (target_phase % l + l - phase) % l;
  return max_u64(k->idle_since, k->floor_ticks) + (uint64_t)(wait + 1u) * CYCLE_TICKS;
}

/* ---- start-up ------------------------------------------------------------------------------------------------------------------------------- */
void krisalis_native_start(KrisalisNative *k, const KrisalisNativeHost *host, uint8_t revision, uint64_t at_ticks) {
  Build b;
  unsigned c;
  memset(k, 0, sizeof *k);
  k->host = *host;
  k->held_since = UINT64_MAX;
  if (revision < KRISALIS_REVISION_1 || revision > KRISALIS_REVISION_3) { k->fault = KRISALIS_FAULT_UNSUPPORTED_REVISION; return; }
  k->revision = revision;
  k->floor_ticks = at_ticks;
  k->idle_since = at_ticks;
  b.k = k;
  b.t = at_ticks + (uint64_t)(revision == KRISALIS_REVISION_1 ? 868u : 1536u) * CYCLE_TICKS;   /* the original's block copies (vectors, mailbox address) take this long */
  b.last = b.t;
  {   /* "nop / jp $0C00 / dw mailbox": the mailbox address the 68000 reads at $0004 */
    const uint16_t mailbox = k_layout[revision].mailbox;
    ram_set(&b, 0u, 0x00u);
    ram_set(&b, 1u, 0xC3u);
    ram_set(&b, 2u, 0x00u);
    ram_set(&b, 3u, 0x0Cu);
    ram_set(&b, 4u, (uint8_t)mailbox);
    ram_set(&b, 5u, (uint8_t)(mailbox >> 8));
  }
  adv(&b, 60u);
  ym_pair(&b, 0u, 0x22u, 0x0Cu);   /* LFO on (6.02 Hz) */
  ym_pair(&b, 0u, 0x27u, 0x00u);   /* timers and channel-3 mode off */
  if (revision != KRISALIS_REVISION_1)
    for (c = 0; c < 6u; ++c) *chan(k, c, CH_LOADED) = 0xB6u;   /* invalid: forces a voice reload */
  k->started = 1u;
  k->idle_since = b.t;
  k->dac_due = UINT64_MAX;
}

void krisalis_native_suspend(KrisalisNative *k, uint64_t at_ticks) {
  if (k->held_since == UINT64_MAX) {
    k->held_since = at_ticks;
    if (k->started && k->queue_count == 0u) {   /* idle: remember where in its loop the Z80 stopped */
      const uint64_t ref = max_u64(k->idle_since, k->floor_ticks);
      if (at_ticks > ref) k->idle_phase = (uint32_t)((k->idle_phase + (at_ticks - ref) / CYCLE_TICKS) % loop_cycles(k));
      k->idle_since = at_ticks;
    }
  }
}

void krisalis_native_resume(KrisalisNative *k, uint64_t at_ticks) {
  if (k->held_since != UINT64_MAX) {
    const uint64_t delay = at_ticks > k->held_since ? at_ticks - k->held_since : 0u;
    if (k->queue_count != 0u) k->queue_base += delay;
    if (k->dac_active && k->dac_due != UINT64_MAX) k->dac_due += delay;
    k->held_since = UINT64_MAX;
  }
  if (at_ticks > k->floor_ticks) k->floor_ticks = at_ticks;
}

/* ---- the scheduler -------------------------------------------------------------------------------------------------------------------------- */
static void apply_queue_head(KrisalisNative *k) {
  const KrisalisOp *op = &k->queue[k->queue_head];
  const uint64_t t = k->queue_base;
  switch (op->kind) {
    case OP_YM: k->host.ym_write(k->host.context, op->a, op->c, t); k->pair_open = (uint8_t)((op->a & 1u) == 0u); break;
    case OP_RAM: k->host.ram[((unsigned)op->a | ((unsigned)op->b << 8)) & RAM_MASK] = op->c; break;
    case OP_BANK: k->host.bank_write(k->host.context, op->c); break;
    case OP_DAC_START:
      k->dac_pointer = k->pend_pointer;
      k->dac_left = k->pend_left;
      k->dac_phase = k->pend_phase;
      k->dac_rate = k->pend_rate;
      k->dac_last_hi = 0u;
      k->dac_redirect = 1u;
      k->dac_active = k->pend_left != 0u;   /* a zero-length sample is never polled by the original: nothing plays (the DAC is enabled, the SFX flag stays) */
      ++k->count_samples_started;
      if (k->dac_active) dac_schedule(k, t);
      break;
    case OP_DAC_CUT:
      if (k->dac_active) { k->dac_left = 1u; if (op->c) k->dac_redirect = 0u; }
      break;
    default: break;
  }
  k->queue_head = (uint16_t)((k->queue_head + 1u) % KRISALIS_QUEUE_MAX);
  --k->queue_count;
  if (op->kind == OP_BANK) k->pair_open = (uint8_t)(k->queue_count != 0u && k->queue[k->queue_head].kind == OP_BANK);   /* the nine bank-register writes are one tight loop: no DAC poll inside it */
  if (k->queue_count != 0u) {
    k->queue_base += (uint64_t)k->queue[k->queue_head].dt;
    k->queue[k->queue_head].dt = 0u;   /* the head's due time is queue_base: a canonical state has no gap in front of it */
  }
  else {
    k->idle_since = t + (uint64_t)CYC_YM_WRITE * CYCLE_TICKS;
    k->idle_phase = 0u;
    if (k->revision == KRISALIS_REVISION_3 && k->dac_active && k->dac_rate != 0u) dac_schedule(k, t);   /* back to the idle loop's pace */
  }
}

KrisalisFault krisalis_native_run_to(KrisalisNative *k, uint64_t master_ticks) {
  Build b;
  uint64_t started_before = 0u;
  b.k = k;
  for (;;) {
    uint64_t best = UINT64_MAX;
    int kind = 0;   /* 1 queue, 2 dac, 3 work */
    if (k->fault != KRISALIS_FAULT_NONE) return k->fault;
    if (!k->started || k->held_since != UINT64_MAX) break;
    if (k->queue_count != 0u) { best = max_u64(k->queue_base, k->floor_ticks); kind = 1; }
    if (k->dac_active && k->dac_due != UINT64_MAX && !k->pair_open) {   /* the original polls the DAC between register pairs, never inside one */
      const uint64_t due = max_u64(k->dac_due, k->floor_ticks);
      if (due < best) { best = due; kind = 2; }
    }
    if (k->queue_count == 0u && (*mb(k, MB_CMD) != 0u || *mb(k, MB_TRIGGER) != 0u)) {
      const unsigned l = loop_cycles(k);
      const uint64_t at = *mb(k, MB_TRIGGER) != 0u ? idle_reach(k, 0u) : idle_reach(k, l - 20u);   /* the trigger is read first in the loop, the command last */
      if (at < best) { best = at; kind = 3; }
    }
    if (kind == 0 || best > master_ticks) break;
    switch (kind) {
      case 1: apply_queue_head(k); break;
      case 2: dac_event(k, best); break;
      default: {
        b.t = best;
        b.last = best;
        started_before = k->count_samples_started;
        if (*mb(k, MB_TRIGGER) != 0u) start_sfx_sample(&b);
        else consume_command(&b, *mb(k, MB_CMD));
        if (k->revision != KRISALIS_REVISION_3 && k->dac_active && k->dac_due != UINT64_MAX && k->queue_count != 0u)   /* revisions 1 and 2 do not poll the DAC while they work */
          k->dac_due = max_u64(k->dac_due, b.last + (uint64_t)CYC_YM_WRITE * CYCLE_TICKS);
        if (k->queue_count == 0u) { k->idle_since = best; k->idle_phase = 0u; }
        else if (k->revision == KRISALIS_REVISION_3 && k->dac_active && k->dac_rate != 0u && k->count_samples_started == started_before) dac_schedule(k, best);   /* the row slows the polls of a sample that goes on */
        break;
      }
    }
  }
  return k->fault;
}

/* ---- save states ----------------------------------------------------------------------------------------------------------------------------- */
typedef struct W { uint8_t *data; uint32_t capacity, size; int error; } W;
typedef struct R { const uint8_t *data; uint32_t size, offset; int error; } R;
static void w8(W *w, uint8_t v) { if (w->size < w->capacity) w->data[w->size++] = v; else w->error = 1; }
static void w16(W *w, uint16_t v) { w8(w, (uint8_t)v); w8(w, (uint8_t)(v >> 8)); }
static void w32(W *w, uint32_t v) { w16(w, (uint16_t)v); w16(w, (uint16_t)(v >> 16)); }
static void w64(W *w, uint64_t v) { w32(w, (uint32_t)v); w32(w, (uint32_t)(v >> 32)); }
static uint8_t r8(R *r) { if (r->offset < r->size) return r->data[r->offset++]; r->error = 1; return 0u; }
static uint16_t r16(R *r) { const uint16_t lo = r8(r); return (uint16_t)(lo | (uint16_t)((unsigned)r8(r) << 8)); }
static uint32_t r32(R *r) { const uint32_t lo = r16(r); return lo | ((uint32_t)r16(r) << 16); }
static uint64_t r64(R *r) { const uint64_t lo = r32(r); return lo | ((uint64_t)r32(r) << 32); }

uint32_t krisalis_native_state_put(const KrisalisNative *k, uint8_t *out, uint32_t capacity) {
  W w;
  uint16_t i;
  if (k->fault != KRISALIS_FAULT_NONE) return 0u;
  w.data = out;
  w.capacity = capacity;
  w.size = 0u;
  w.error = 0;
  w8(&w, k->revision);
  w8(&w, k->started);
  w64(&w, k->floor_ticks);
  w64(&w, k->idle_since);
  w64(&w, k->held_since);
  w32(&w, k->idle_phase);
  w8(&w, k->dac_active);
  w8(&w, k->dac_rate);
  w8(&w, k->dac_phase);
  w8(&w, k->dac_last_hi);
  w8(&w, k->dac_redirect);
  w8(&w, k->pair_open);
  w16(&w, k->dac_pointer);
  w16(&w, k->dac_left);
  w16(&w, k->pend_pointer);
  w16(&w, k->pend_left);
  w8(&w, k->pend_phase);
  w8(&w, k->pend_rate);
  w64(&w, k->dac_due);
  w64(&w, k->count_commands);
  w64(&w, k->count_rows);
  w64(&w, k->count_ticks);
  w64(&w, k->count_dac_samples);
  w64(&w, k->count_samples_started);
  w64(&w, k->count_key_events);
  w64(&w, k->queue_base);
  w16(&w, k->queue_count);
  for (i = 0; i < k->queue_count; ++i) {   /* in order from the head: the ring position is not part of the state */
    const KrisalisOp *op = &k->queue[(k->queue_head + i) % KRISALIS_QUEUE_MAX];
    w32(&w, op->dt);
    w8(&w, op->kind);
    w8(&w, op->a);
    w8(&w, op->b);
    w8(&w, op->c);
  }
  return w.error ? 0u : w.size;
}

int krisalis_native_state_get(KrisalisNative *stage, const uint8_t *in, uint32_t length, uint64_t master_ticks) {
  R r;
  uint16_t i, count;
  uint64_t base;
  KrisalisNative s = *stage;
  r.data = in;
  r.size = length;
  r.offset = 0u;
  r.error = 0;
  s.fault = KRISALIS_FAULT_NONE;
  s.revision = r8(&r);
  s.started = r8(&r);
  s.floor_ticks = r64(&r);
  s.idle_since = r64(&r);
  s.held_since = r64(&r);
  s.idle_phase = r32(&r);
  s.dac_active = r8(&r);
  s.dac_rate = r8(&r);
  s.dac_phase = r8(&r);
  s.dac_last_hi = r8(&r);
  s.dac_redirect = r8(&r);
  s.pair_open = r8(&r);
  s.dac_pointer = r16(&r);
  s.dac_left = r16(&r);
  s.pend_pointer = r16(&r);
  s.pend_left = r16(&r);
  s.pend_phase = r8(&r);
  s.pend_rate = r8(&r);
  s.dac_due = r64(&r);
  s.count_commands = r64(&r);
  s.count_rows = r64(&r);
  s.count_ticks = r64(&r);
  s.count_dac_samples = r64(&r);
  s.count_samples_started = r64(&r);
  s.count_key_events = r64(&r);
  base = r64(&r);
  count = r16(&r);
  if (r.error || s.revision < KRISALIS_REVISION_1 || s.revision > KRISALIS_REVISION_3 || s.started > 1u || s.dac_active > 1u || s.dac_redirect > 1u || s.pair_open > 1u) return 0;
  if (count > KRISALIS_QUEUE_MAX) return 0;
  s.queue_head = 0u;
  s.queue_count = count;
  s.queue_base = base;
  for (i = 0; i < count; ++i) {
    KrisalisOp *op = &s.queue[i];
    op->dt = r32(&r);
    op->kind = r8(&r);
    op->a = r8(&r);
    op->b = r8(&r);
    op->c = r8(&r);
    if (op->kind < OP_YM || op->kind > OP_DAC_START || (op->kind == OP_YM && op->a > 3u)) return 0;
    if (i == 0u && op->dt != 0u) return 0;
  }
  if (r.error || r.offset != r.size) return 0;
  /* the state must be one this implementation could have produced at this guest time */
  if (s.floor_ticks > master_ticks || s.idle_since == UINT64_MAX) return 0;
  if (s.held_since != UINT64_MAX && s.held_since > master_ticks) return 0;
  if (count != 0u && base == UINT64_MAX) return 0;
  if (s.dac_active && s.dac_left == 0u) return 0;
  *stage = s;
  return 1;
}

/* ---- the verified-image catalog ------------------------------------------------------------------------------------------------------------------ */
typedef struct CatalogEntry { const char *driver; const char *revision; const char *verification; const char *signature; } CatalogEntry;

static const CatalogEntry k_catalog[] = {
#define KRISALIS_VERIFIED_IMAGE(driver, revision, verification, signature) {driver, revision, verification, signature},
#define KRISALIS_CATALOG_REVISION(n) /* read by the build tool only */
#include "krisalis_driver_catalog.inc"
#undef KRISALIS_VERIFIED_IMAGE
#undef KRISALIS_CATALOG_REVISION
};

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int krisalis_native_identity_lookup(const uint8_t signature[32], const char **version, uint8_t *revision) {
  size_t i, j;
  for (i = 0; i < sizeof k_catalog / sizeof k_catalog[0]; ++i) {
    int match = 1;
    for (j = 0; j < 32u && match; ++j) {
      const int hi = hex_nibble(k_catalog[i].signature[2u * j]), lo = hex_nibble(k_catalog[i].signature[2u * j + 1u]);
      if (hi < 0 || lo < 0 || (uint8_t)((hi << 4) | lo) != signature[j]) match = 0;
    }
    if (match) {
      if (version != NULL) *version = k_catalog[i].revision;
      if (revision != NULL) *revision = (uint8_t)(k_catalog[i].revision[0] - '0');
      return 1;
    }
  }
  return 0;
}
