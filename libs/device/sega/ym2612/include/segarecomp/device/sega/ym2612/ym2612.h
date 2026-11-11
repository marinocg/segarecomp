#ifndef SEGARECOMP_DEVICE_SEGA_YM2612_YM2612_H
#define SEGARECOMP_DEVICE_SEGA_YM2612_YM2612_H

#include <stddef.h>
#include <stdint.h>

/* Genesis YM2612 (OPN2) as a deterministic, host-clocked device (SEG-032-T007; ADR 0074).
 *
 * A small C ABI over the vendored ymfm YM2612 core (BSD-3-Clause, third_party/ymfm). The HOST owns time: every call carries the guest
 * master-clock tick of the access, and the device advances itself to that instant first. One YM2612 input clock is 7 master ticks;
 * one native sample is 144 input clocks = 1,008 master ticks (sample k starts at tick k x 1,008). Timers A/B and the busy flag are
 * driven from the same clock (ymfm leaves both to the host): a timer expiry at clock E fires before the sample that starts at E, and
 * every access at tick T first runs all events at or before clock(T) (= T / 7), so a write takes effect for the first sample that
 * starts after it.
 *
 * Reads return the status byte (bit 7 busy, bit 1 timer B, bit 0 timer A) at every port: Genesis Plus GX and ares agree on that
 * (ymfm itself answers port 0 only; the wrapper reads the status for every port).
 *
 * The device never allocates after creation except inside ymfm's own channel tables, never reads host time and has no global state.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define YM2612_MASTER_TICKS_PER_CLOCK 7u
#define YM2612_CLOCKS_PER_SAMPLE 144u
#define YM2612_MASTER_TICKS_PER_SAMPLE (YM2612_MASTER_TICKS_PER_CLOCK * YM2612_CLOCKS_PER_SAMPLE)

typedef struct Ym2612 Ym2612;

/* Receives each native stereo sample as soon as it is generated. `start_master_ticks` = sample index x 1,008. The values are ymfm's
 * 16-bit-range output sums (DAC discontinuity included); callers clip to int16. */
typedef void (*Ym2612SampleSink)(void *context, uint64_t start_master_ticks, int32_t left, int32_t right);

/* Creates a device in its power-on state at guest time 0. NULL on allocation failure. */
Ym2612 *ym2612_create(void);
void ym2612_destroy(Ym2612 *chip);

/* The sink used by every implicit advance (access or explicit). May be NULL (samples are generated and dropped). */
void ym2612_set_sink(Ym2612 *chip, Ym2612SampleSink sink, void *context);

/* Chip reset (the Z80 /RESET line): registers, timers and the busy flag return to the reset state; time continues. */
void ym2612_reset(Ym2612 *chip, uint64_t master_ticks);

/* Runs the device to guest time `master_ticks` (a time in the past does nothing: the device clock is monotonic). */
void ym2612_advance(Ym2612 *chip, uint64_t master_ticks);

/* Accesses at guest time `master_ticks`. `port` is the address-decoded port 0..3 (part I address / data, part II address / data). */
uint8_t ym2612_read(Ym2612 *chip, uint32_t port, uint64_t master_ticks);
void ym2612_write(Ym2612 *chip, uint32_t port, uint8_t value, uint64_t master_ticks);

/* The guest master tick at which Timer A next overflows (UINT64_MAX when it is stopped), as of the device's current time (the time of the last access): a
 * free-running schedule, so a write that only clears the overflow flag does not move it. A sound driver that paces itself on Timer A asks this right after the
 * write that (re)arms the timer. */
uint64_t ym2612_timer_a_expiry(const Ym2612 *chip);

/* The same question asked of a saved image (ym2612_save) without loading it: the guest master tick at which its Timer A next overflows (UINT64_MAX = stopped or the image is too
 * short). Used to validate a sound driver's own timeline against the chip it is restored together with. */
uint64_t ym2612_image_timer_a_expiry(const uint8_t *image, uint32_t length);

/* Number of native samples generated so far, and a deterministic 64-bit digest of the complete device state (ymfm registers and
 * internal phase/envelope state, host timers, busy end and clock). */
uint64_t ym2612_samples_generated(const Ym2612 *chip);
uint64_t ym2612_state_digest(const Ym2612 *chip);

/* SEG-131: complete, bounded, paired save/restore of the device (save states and rewind). The image holds the host-side clock fields (`now`, next sample start,
 * both timer expiries, busy end, samples generated) AND ymfm's own register/phase/envelope/LFO/SSG state; nothing is derived by replaying register writes. The size is a
 * property of the chip build and is the same for every chip, so a caller can size its buffer once. `ym2612_snapshot_bytes` is that exact size.
 * Layout (little-endian, fixed): 6 x u64 host fields, then ymfm's blob (exactly `ym2612_snapshot_bytes() - 48` bytes). The sink is attached to the chip and is not part of the image. */
#define YM2612_SNAPSHOT_HOST_BYTES 48u
#define YM2612_SNAPSHOT_MAX_BYTES 16384u
uint32_t ym2612_snapshot_bytes(const Ym2612 *chip);
/* Writes exactly ym2612_snapshot_bytes() bytes; returns 0 on success, non-zero when `capacity` is smaller (nothing written). */
int ym2612_save(const Ym2612 *chip, uint8_t *out, uint32_t capacity);
/* Validates an image without touching the chip: exact length, and the host-clock invariants (next sample start = samples x 144 and not before `now`; each timer expiry is
 * "none" or not before `now`). Returns 0 when the image can be loaded. */
int ym2612_check(const Ym2612 *chip, const uint8_t *in, uint32_t length);
/* Loads an image that passed ym2612_check (it re-checks and returns non-zero, leaving the chip untouched, otherwise). Cannot fail after the check. */
int ym2612_load(Ym2612 *chip, const uint8_t *in, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif
