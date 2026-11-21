/*
 * Native Krisalis (Shaun Hollingworth) sound-driver replacement for the Genesis (ADR 0318).
 *
 * An independently written, portable C11 re-implementation of the observable behaviour of the Krisalis Z80 player, identified by the
 * digest of the image the 68000 uploads (krisalis_driver_catalog.inc). It is NOT an interpreter, decoder or emulator of the original
 * Z80 instructions: the uploaded image is never fetched, decoded or executed. The 68000 stays the tracker sequencer (generated-native);
 * this module only plays what it hands over through the RAM-visible mailbox: it consumes the command byte, turns the six pattern cells of
 * a row (or a tick) into YM2612 writes, plays one 8-bit PCM channel on FM6 through the DAC, and acknowledges exactly as the original
 * does (revision 1 clears the command when the work is done, revisions 2 and 3 when it is read). Anything outside the verified subset
 * faults explicitly (KrisalisFault): the driver then stays silent. There is no fallback and no fabricated acknowledgement.
 *
 * One core serves the verified revisions; a revision is a small descriptor (RAM layout) plus a few explicit branches where the
 * published behaviour differs (docs/architecture/genesis-native-krisalis.md). The host interface is minimal so that the module is
 * unit-testable against synthetic data without the Z80 machine.
 */
#ifndef SEGARECOMP_GENESIS_KRISALIS_NATIVE_H
#define SEGARECOMP_GENESIS_KRISALIS_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#define KRISALIS_NATIVE_DRIVER "krisalis"
#define KRISALIS_NATIVE_IMPLEMENTATION_VERSION 1u /* bumps whenever the replacement's observable behaviour changes (part of the build identity) */

typedef enum KrisalisFault {
  KRISALIS_FAULT_NONE = 0,
  KRISALIS_FAULT_UNMAPPED_RESOURCE = 1,   /* a sample byte outside the owned cartridge image */
  KRISALIS_FAULT_INVALID_STATE = 2,       /* the command queue or a sample length is outside what the verified driver can produce */
  KRISALIS_FAULT_UNSUPPORTED_REVISION = 3 /* the host asked for a revision this implementation does not have */
} KrisalisFault;

const char *krisalis_native_fault_name(KrisalisFault fault);

/* The verified revisions (docs/architecture/genesis-native-krisalis.md section 3). */
#define KRISALIS_REVISION_1 1u
#define KRISALIS_REVISION_2 2u
#define KRISALIS_REVISION_3 3u

typedef struct KrisalisNativeHost {
  void *context;
  uint8_t *ram;   /* the 8 KiB Z80 sound RAM the 68000 also writes */
  /* Reads `count` bytes of the 68000 address space at `address24` (the cartridge). Returns 0 when any byte is not mapped. */
  int (*rom_read)(void *context, uint32_t address24, uint8_t *destination, uint32_t count);
  void (*ym_write)(void *context, uint32_t port, uint8_t value, uint64_t master_ticks);
  /* The Z80 bank register ($6000): the current 9-bit value, and one serial write (bit 0 of `bit`) that shifts it like the hardware does. */
  uint32_t (*bank_get)(void *context);
  void (*bank_write)(void *context, uint8_t bit, uint64_t master_ticks);
  /* Optional observer (may be NULL): each command the driver consumes (the command byte first, then the twelve cell bytes). Validation seam only. */
  void (*command_tap)(void *context, const uint8_t *bytes, unsigned count, uint64_t master_ticks);
} KrisalisNativeHost;

/* One scheduled effect of a command being processed: a YM2612 port write, a bank-register bit or a sound-RAM byte, due `dt` master ticks after
 * the previous entry (the first one after `queue_base`). */
typedef struct KrisalisOp {
  uint32_t dt;
  uint8_t kind, a, b, c;
} KrisalisOp;

#define KRISALIS_QUEUE_MAX 640u

typedef struct KrisalisNative {
  KrisalisNativeHost host;
  KrisalisFault fault;
  uint8_t revision;
  uint8_t started;
  uint64_t floor_ticks;       /* nothing runs earlier than this (the bus was held / the driver was started) */
  uint64_t idle_since;        /* the Z80 is waiting for work since this guest time */
  uint64_t held_since;        /* UINT64_MAX while running, else the time the bus was taken */
  uint32_t idle_phase;        /* where in its wait loop the Z80 stood (cycles) at `idle_since`: a loop interrupted by many short bus holds still reaches its command read */
  /* pending effects of the command in flight */
  KrisalisOp queue[KRISALIS_QUEUE_MAX];
  uint16_t queue_head, queue_count;
  uint64_t queue_base;        /* due time of queue[queue_head] */
  /* the DAC player (the alternate register set of the original) */
  uint8_t dac_active, dac_rate, dac_phase, dac_last_hi, dac_redirect;
  uint8_t pair_open;          /* a YM2612 address write of the command in flight has been issued and its data write has not: the DAC player never splits a pair */
  uint16_t dac_pointer, dac_left;
  uint16_t pend_pointer, pend_left;   /* a sample start of the command in flight (applied when its effect is due, like the original's register loads) */
  uint8_t pend_phase, pend_rate, pend_pan;
  uint8_t dac_pan;            /* revision 2: the FM6 pan the player rewrites on every poll */
  uint64_t dac_due;
  /* counters (aggregates only) */
  uint64_t count_commands, count_rows, count_ticks, count_dac_samples, count_samples_started, count_key_events;
} KrisalisNative;

/* Starts (or restarts) the driver at guest master time `at_ticks`: the original's start-up (vectors, YM2612 initialisation, channel state) is replayed. */
void krisalis_native_start(KrisalisNative *k, const KrisalisNativeHost *host, uint8_t revision, uint64_t at_ticks);

/* The Z80 bus was taken from the driver at `at_ticks` (BUSREQ asserted): nothing is due while it is held. */
void krisalis_native_suspend(KrisalisNative *k, uint64_t at_ticks);
/* The bus came back at `at_ticks`: pending work resumes, delayed by the hold. */
void krisalis_native_resume(KrisalisNative *k, uint64_t at_ticks);

/* Runs every effect due up to and including `master_ticks`. Returns the fault (KRISALIS_FAULT_NONE when healthy). A faulted driver does nothing further. */
KrisalisFault krisalis_native_run_to(KrisalisNative *k, uint64_t master_ticks);

/* ---- save states --------------------------------------------------------------------------------------------------------------------------
 * The complete execution state of a running driver as a canonical, versioned, pointer-free payload (fixed-width little-endian fields, no host pointer, no struct
 * layout, no padding). The 8 KiB sound RAM (the mailbox and the channel state live there, as in the original) belongs to the machine snapshot and is not repeated.
 * `put` returns the byte count (0 = it does not fit, or the driver is faulted). `get` decodes into `stage` - a private copy whose `host` is NOT touched - and validates
 * it against the guest clock; it returns 1 only for a state this implementation could have produced. Nothing here writes to a chip. */
#define KRISALIS_NATIVE_STATE_SCHEMA 1u
#define KRISALIS_NATIVE_STATE_MAX_BYTES 5632u
uint32_t krisalis_native_state_put(const KrisalisNative *k, uint8_t *out, uint32_t capacity);
int krisalis_native_state_get(KrisalisNative *stage, const uint8_t *in, uint32_t length, uint64_t master_ticks);

/* The verified-image catalog (krisalis_driver_catalog.inc): 1 and the revision text when `signature` (the activation signature of an uploaded Z80 image) is a verified Krisalis image. */
int krisalis_native_identity_lookup(const uint8_t signature[32], const char **version, uint8_t *revision);

#endif
