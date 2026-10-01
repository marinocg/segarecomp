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

/* Number of native samples generated so far, and a deterministic 64-bit digest of the complete device state (ymfm registers and
 * internal phase/envelope state, host timers, busy end and clock). */
uint64_t ym2612_samples_generated(const Ym2612 *chip);
uint64_t ym2612_state_digest(const Ym2612 *chip);

#ifdef __cplusplus
}
#endif

#endif
