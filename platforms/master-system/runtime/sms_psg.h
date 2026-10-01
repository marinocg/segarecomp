#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_PSG_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_PSG_H

#include "segarecomp/device/sega/psg/sn76489.h"
#include "sms_machine.h"

/* Master System wiring of the platform-neutral Sega PSG device (SEG-009-T007; machine contract section 10).
 *
 * The device runs on the machine's Z80 T-state timebase (one chip tick = 16 T). A write to any PSG port class catches
 * the chip up to the instruction-start T-state (U11) of the `io_out` and then applies the byte, so the PCM stream depends
 * only on guest write timestamps. A data byte before any latch byte latches SMS_ERROR_PSG_DATA_BEFORE_LATCH (U5) and
 * stops the machine. The device owns its whole reset state (chip registers, counters, LFSR, PCM stream index). */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmsPsg {
  Sn76489 chip;
  Sn76489Pcm pcm;
  SmsMachine *machine;
} SmsPsg;

/* Binds the device to `machine->psg` (write, reset and digest seams) and resets it. `sink` (optional) receives the PCM
 * stream (s16le mono 44,100 Hz, contract section 10) in whole samples; the ring of the device library is a ready sink.
 * Returns 0 on an unusable device configuration. */
int sms_psg_attach(SmsPsg *psg, SmsMachine *machine, Sn76489PcmSink sink, void *sink_context);
/* Replaces the variant parameters (test mutation controls; the defaults are the contract model). Resets the chip. */
int sms_psg_configure(SmsPsg *psg, const Sn76489Config *config);
/* Catches the chip up to `cycles` and delivers every pending PCM sample to the sink. Call at the end of a run (or at a
 * frame boundary) before reading the stream; it does not change guest-visible state beyond advancing time. */
void sms_psg_sync(SmsPsg *psg, uint64_t cycles);

#ifdef __cplusplus
}
#endif
#endif
