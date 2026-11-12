/*
 * Native GEMS sound-driver replacement for the Genesis (ADR 0291).
 *
 * This is an independently written, portable C11 re-implementation of the observable behaviour of ONE verified original driver
 * (GEMS 2.5 lineage, identified by the digest of the image the 68000 uploads; see gems_driver_catalog.inc). It is NOT an interpreter,
 * decoder or emulator of the original Z80 instructions: the original image is never fetched, decoded or executed. It reads the same
 * RAM-visible mailboxes the 68000 writes (command FIFO, FIFO pointers, mailboxes), resolves its resources from the cartridge exactly
 * as the original does (a 24-bit pointer table delivered by the 11 "get pointers" command), and schedules real YM2612 / PSG writes on
 * the guest master clock. Anything outside the verified subset faults explicitly (GemsNativeFault) and the driver then stays silent:
 * there is no fallback, no guessed behaviour and no fabricated acknowledgement.
 *
 * Scope (see docs/architecture/genesis-native-gems.md): the whole command set of the verified driver, every sequence event, FM / PSG tone / PSG noise / digital
 * patches (voice allocation, envelopes, pitch bend, tempo, loops, mailboxes), one-shot, looped, slow-rate and 4-bit compressed samples.
 *
 * The module has no global state, never reads host time and has no dependency on the Z80 machine: its host environment is the small
 * GemsNativeHost interface below, so it is unit-testable against synthetic cartridge data.
 */
#ifndef SEGARECOMP_GENESIS_GEMS_NATIVE_H
#define SEGARECOMP_GENESIS_GEMS_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#define GEMS_NATIVE_DRIVER "gems"
#define GEMS_NATIVE_IMPLEMENTATION_VERSION 1u /* bumps whenever the replacement's observable behaviour changes (part of the build identity) */

typedef enum GemsNativeFault {
  GEMS_FAULT_NONE = 0,
  GEMS_FAULT_UNSUPPORTED_COMMAND = 1,      /* reserved: every command id of the verified driver is implemented; an unknown id is ignored exactly as the original ignores it */
  GEMS_FAULT_INVALID_COMMAND_STREAM = 2,   /* FIFO write pointer outside the 64-byte ring, or a channel number outside 0..15 */
  GEMS_FAULT_UNMAPPED_RESOURCE = 3,        /* a pointer-table / sequence / patch / envelope / sample read outside the owned cartridge image */
  GEMS_FAULT_INVALID_RESOURCE = 4,         /* a resource that is structurally invalid (track count, mailbox index, a sequence that never yields, ...) */
  GEMS_FAULT_UNSUPPORTED_SEQUENCE_EVENT = 5, /* reserved (see UNSUPPORTED_COMMAND) */
  GEMS_FAULT_UNSUPPORTED_PATCH = 6,        /* reserved */
  GEMS_FAULT_UNSUPPORTED_SAMPLE = 7,       /* reserved */
  GEMS_FAULT_DEVICE_TIMING = 8             /* the YM2612 timer did not expire where the verified rate model requires */
} GemsNativeFault;

const char *gems_native_fault_name(GemsNativeFault fault);

/* Host interface. `ram` is the 8 KiB Z80 sound RAM the 68000 also writes. */
typedef struct GemsNativeHost {
  void *context;
  uint8_t *ram;
  /* Reads `count` bytes of the 68000 address space at `address24` (the cartridge). Returns 0 when any byte is not mapped. */
  int (*rom_read)(void *context, uint32_t address24, uint8_t *destination, uint32_t count);
  void (*ym_write)(void *context, uint32_t port, uint8_t value, uint64_t master_ticks);
  uint8_t (*ym_read)(void *context, uint32_t port, uint64_t master_ticks);
  void (*psg_write)(void *context, uint8_t value, uint64_t master_ticks);
  /* The guest master tick at which the YM2612's Timer A next overflows (UINT64_MAX when it is stopped), as of the last access made through `ym_write`/`ym_read`. */
  uint64_t (*ym_timer_a_expiry)(void *context);
  /* Optional observer (may be NULL): each command the driver consumes, start byte first, with the time it was consumed. Validation seam only. */
  void (*command_tap)(void *context, const uint8_t *bytes, unsigned count, uint64_t master_ticks);
} GemsNativeHost;

typedef struct GemsCcb {
  uint8_t flags;       /* 0 enabled, 1 muted, 3 SFX time base, 4 running, 5 locked, 6 retrigger, 7 sustain */
  uint32_t address;    /* absolute 68000 address of the next sequence byte */
  uint16_t timer;      /* counts up to 0 (two's complement of the pending delay) */
  uint16_t delay, duration;  /* registered, negated */
  uint8_t program, sequence, channel, priority, envelope, attenuation;
  uint8_t loops[4][3]; /* loop stack: count (0 = free, 127 = endless), address low, address middle */
  uint8_t patch[39];   /* this channel's patch buffer; patch[0] == 0xFF = undefined */
} GemsCcb;

typedef struct GemsVoice {
  uint8_t flags;       /* F R L T S V V V: free, release, locked, self-timed, SFX time base, voice number */
  uint8_t priority, note, channel;
  uint16_t duration;
  uint8_t release;
} GemsVoice;

typedef struct GemsEcb {
  uint8_t channel;     /* bit 7 end of list, bit 6 free, bit 5 SFX time base, low bits: channel */
  uint8_t counter;
  uint16_t delta;
  uint16_t pointer;    /* offset of the next segment in the 128-byte envelope buffer area (it may run on into the DAC FIFO, as in the original) */
  uint8_t buffer;      /* offset of this block's 32-byte envelope buffer */
} GemsEcb;

typedef struct GemsNative {
  GemsNativeHost host;
  GemsNativeFault fault;
  /* time */
  uint64_t floor_ticks;       /* nothing runs earlier than this (the bus was held / the driver was started) */
  uint64_t boot_ticks;
  uint8_t booted;
  uint8_t fifo_check_pending;
  uint64_t next_tick_index;   /* next VBlank interrupt edge not yet considered */
  /* tables delivered by the get-pointers command (24-bit absolute addresses) */
  uint32_t patch_table, envelope_table, sequence_table, sample_table;
  uint8_t have_pointers;
  /* sequencer */
  uint16_t sbpt, sbptacc;     /* sub-beats per tick and its accumulator, 8 fraction bits */
  uint8_t tick_flags;         /* bit 0: a tick passed, bit 1: a sub-beat passed (the flags of the tick being processed) */
  uint8_t master_attenuation;
  GemsCcb ccb[16];
  /* voices: FM voices in table order {0,1,4,5,6,2} (index 4 is the digital-capable voice 6, index 5 the channel-3 mode voice), three PSG tone voices, one noise voice */
  GemsVoice fm[6], psg[3], noise[1];
  uint8_t note_voice, note_attenuation, note_number, note_channel;   /* the voice, attenuation, note and channel of the note being started */
  uint8_t carriers;
  /* PSG envelope processor */
  uint8_t psg_command[4], psg_level[4], psg_attack[4], psg_decay[4], psg_sustain[4], psg_release[4], psg_mode[4], psg_low[4], psg_high[4], psg_attack_level[4], psg_update[4];
  /* pitch / envelope bend */
  GemsEcb ecb[4];
  uint8_t envelope_buffer[128];
  int16_t pitch_bend[16], envelope_bend[16];
  uint8_t bend_changed[16], need_bend;
  /* digital voice */
  uint8_t sample_flags;
  uint8_t sample_pointer_header[3];
  uint16_t sample_skip, sample_first, sample_loop, sample_end;
  uint32_t sample_pointer;    /* next ROM byte to load into the FIFO */
  uint16_t sample_counter;
  uint8_t fdf_state;          /* 4 = one-shot, 5 = looped, 6 = loop released, 7 = shut down */
  uint8_t fifo[256];
  uint8_t fifo_write, fifo_read;
  uint8_t dac_active;         /* DAC output routine enabled */
  uint8_t fill_enabled;
  uint8_t dac_compressed, dac_pattern;
  uint8_t dac_timer_b;        /* value the DAC routine writes to register $27 on every sample (timer reset; carries the channel-3 mode bits) */
  uint32_t dac_period_samples; /* Timer A period in native YM2612 samples */
  uint64_t dac_due;           /* master tick at which the next sample is due */
  /* counters (aggregates only) */
  uint64_t count_commands, count_sequences, count_dac_samples, count_ticks, count_missed_ticks, count_clips, count_notes_fm, count_notes_psg, count_notes_digital;
} GemsNative;

/* Starts (or restarts) the driver at guest master time `at_ticks`: all driver state is reset (the RAM-visible FIFO bytes and pointers
 * are not: they belong to the 68000 and to the previous run of the driver). */
void gems_native_start(GemsNative *g, const GemsNativeHost *host, uint64_t at_ticks);

/* The Z80 bus came back to the driver at `at_ticks` (BUSREQ released): work due earlier happens now; a VBlank interrupt edge that
 * passed while the bus was held is lost exactly like the real Z80's. The command FIFO is looked at again. */
void gems_native_resume(GemsNative *g, uint64_t at_ticks);

/* Runs every event due up to and including `master_ticks`. Returns the fault (GEMS_FAULT_NONE when healthy). A faulted driver
 * does nothing further. */
GemsNativeFault gems_native_run_to(GemsNative *g, uint64_t master_ticks);

/* ---- save states ------------------------------------------------------------------------------------------------------------------------------------
 * The complete execution state of a running driver as a canonical, versioned, pointer-free payload (fixed-width little-endian fields; no host pointer, no struct layout,
 * no padding). The 8 KiB sound RAM that holds the command FIFO and the mailboxes belongs to the machine snapshot and is not repeated here. `put` returns the byte count (0 =
 * it does not fit, or the driver is faulted: a faulted driver is inert and is recorded by the machine's fault latch instead). `get` decodes into `stage` - a private copy whose
 * `host` is NOT touched - and validates it against the guest clock (`master_ticks`) and the chip's Timer A schedule (`chip_timer_a_expiry`, UINT64_MAX = stopped); it returns 1 only
 * for a state this implementation could have produced. Nothing here writes to a chip. */
#define GEMS_NATIVE_STATE_SCHEMA 1u
#define GEMS_NATIVE_STATE_MAX_BYTES 4096u
uint32_t gems_native_state_put(const GemsNative *g, uint8_t *out, uint32_t capacity);
int gems_native_state_get(GemsNative *stage, const uint8_t *in, uint32_t length, uint64_t master_ticks, uint64_t chip_timer_a_expiry);

/* Identity: 1 when `signature` (the 32-byte activation signature of the uploaded image) is a verified GEMS image of the catalog.
 * `version` receives the catalog's version text. */
int gems_native_identity_lookup(const uint8_t signature[32], const char **version);

#endif
