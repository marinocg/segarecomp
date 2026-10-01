#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_PAD_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_PAD_H

#include "sms_machine.h"
#include "sms_vdp.h"

/* Master System II controller ports and I/O control (SEG-009-T006; machine contract section 11).
 *
 * The device owns the port classes SMS_PORT_PAD_DC / SMS_PORT_PAD_DD (reads) and SMS_PORT_IO_CONTROL (write, port $3F).
 * Two standard two-button pads (player 1 on port A, player 2 on port B); every other peripheral is out of scope.
 * Each of the four I/O-control pins (A.TR, A.TH, B.TR, B.TH) is either an input (direction bit 1: the pin reads the pad;
 * TH inputs read 1, there is no light gun) or an output (direction bit 0: the pin reads back its output level). A pad
 * press is an active-low bit; the pad state is the machine's scripted-input state at the `io_in` instruction-start
 * T-state (U11), so a read is a pure function of (input state, I/O control).
 *
 * H counter trigger (U3): the VDP H counter is latched when either TH pin level rises 0 -> 1 (the pin level is 1 while
 * the pin is an input, the output level while it is an output). GPGX and ares latch on the output-level bit
 * rising; Gearsystem additionally requires the pin to be an input; the pin-level model agrees with all three for the
 * usual output low -> high software pattern and is the documented hardware behaviour. The reset pin state is $FF (all
 * inputs, all levels 1), so the first edge needs a preceding falling write. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SmsPad {
  const SmsInputState *input; /* the machine's live scripted-input state */
  SmsVdp *vdp;                /* receives the H counter latch; NULL: no latch */
  uint8_t io_control;         /* last write to port $3F, reset $FF */
  uint32_t h_latches;         /* TH rising edges seen since reset */
} SmsPad;

/* Port value of $DC / $DD (or any mirror) for the given state: pure, exposed for the unit tests. */
uint8_t sms_pad_port_dc(const SmsInputState *input, uint8_t io_control);
uint8_t sms_pad_port_dd(const SmsInputState *input, uint8_t io_control);
/* Pin level of A.TH (port 0) or B.TH (port 1) under `io_control`. */
uint8_t sms_pad_th_level(uint8_t io_control, unsigned port);

void sms_pad_reset(SmsPad *pad);
uint8_t sms_pad_read(SmsPad *pad, SmsPortClass cls, uint64_t cycles);
void sms_pad_write(SmsPad *pad, SmsPortClass cls, uint8_t value, uint64_t cycles);
void sms_pad_digest(const SmsPad *pad, SmsSha256 *sha);

/* Wires `pad` into `machine->pad` and binds the machine input state and (optionally) the VDP for the H counter latch.
 * Call before `sms_machine_reset`. The machine's `io_control` mirror and the pad's agree by construction. */
void sms_pad_install(SmsMachine *machine, SmsPad *pad, SmsVdp *vdp);

/* `sms_pad_install` with the VDP found through `sms_vdp_from_machine` (NULL for a non-T004 VDP slot). */
void sms_pad_attach(SmsMachine *machine, SmsPad *pad);

#ifdef __cplusplus
}
#endif
#endif
