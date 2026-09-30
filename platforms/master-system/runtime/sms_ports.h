#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_PORTS_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_PORTS_H

#include <stdint.h>

/* Master System II I/O port decode (machine contract section 6, MacDonald SMS/GG hardware notes section 3).
 *
 * Only A7, A6 and A0 of the 16-bit port matter; A8-A15 and A5-A1 are ignored, so every port mirrors across its row.
 * The decode is a pure function of (port, direction): it owns no device policy. Every decoded class belongs to exactly
 * one owner (`sms_port_owner`); the machine routes by owner and fails closed on a class whose device is not attached. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum SmsPortClass {
  SMS_PORT_OPEN_READ = 0,      /* read of $00-$3F: $FF, no owner */
  SMS_PORT_IGNORED_WRITE = 1,  /* write of $C0-$FF: no effect, no owner */
  SMS_PORT_MEMORY_CONTROL = 2, /* write, A7 A6 A0 = 0 0 0 */
  SMS_PORT_IO_CONTROL = 3,     /* write, 0 0 1 */
  SMS_PORT_PSG = 4,            /* write, 0 1 x */
  SMS_PORT_V_COUNTER = 5,      /* read, 0 1 0 */
  SMS_PORT_H_COUNTER = 6,      /* read, 0 1 1 */
  SMS_PORT_VDP_DATA = 7,       /* read/write, 1 0 0 */
  SMS_PORT_VDP_CONTROL = 8,    /* write, 1 0 1 */
  SMS_PORT_VDP_STATUS = 9,     /* read, 1 0 1 */
  SMS_PORT_PAD_DC = 10,        /* read, 1 1 0 */
  SMS_PORT_PAD_DD = 11         /* read, 1 1 1 */
} SmsPortClass;

typedef enum SmsPortOwner {
  SMS_OWNER_NONE = 0,
  SMS_OWNER_MEMORY = 1, /* T002 memory control */
  SMS_OWNER_VDP = 2,    /* T004 */
  SMS_OWNER_PSG = 3,    /* T007 */
  SMS_OWNER_PAD = 4     /* T006 controllers and I/O control */
} SmsPortOwner;

#define SMS_PORT_CLASS_COUNT 12

static inline SmsPortClass sms_port_decode(uint16_t port, int is_write) {
  const unsigned row = (port >> 6) & 3u; /* A7 A6 */
  const unsigned odd = port & 1u;        /* A0 */
  switch (row) {
    case 0: return is_write ? (odd ? SMS_PORT_IO_CONTROL : SMS_PORT_MEMORY_CONTROL) : SMS_PORT_OPEN_READ;
    case 1:
      if (is_write) return SMS_PORT_PSG;
      return odd ? SMS_PORT_H_COUNTER : SMS_PORT_V_COUNTER;
    case 2:
      if (!odd) return SMS_PORT_VDP_DATA;
      return is_write ? SMS_PORT_VDP_CONTROL : SMS_PORT_VDP_STATUS;
    default: break;
  }
  if (is_write) return SMS_PORT_IGNORED_WRITE;
  return odd ? SMS_PORT_PAD_DD : SMS_PORT_PAD_DC;
}

static inline SmsPortOwner sms_port_owner(SmsPortClass cls) {
  switch (cls) {
    case SMS_PORT_OPEN_READ:
    case SMS_PORT_IGNORED_WRITE: return SMS_OWNER_NONE;
    case SMS_PORT_MEMORY_CONTROL: return SMS_OWNER_MEMORY;
    case SMS_PORT_IO_CONTROL:
    case SMS_PORT_PAD_DC:
    case SMS_PORT_PAD_DD: return SMS_OWNER_PAD;
    case SMS_PORT_PSG: return SMS_OWNER_PSG;
    case SMS_PORT_V_COUNTER:
    case SMS_PORT_H_COUNTER:
    case SMS_PORT_VDP_DATA:
    case SMS_PORT_VDP_CONTROL:
    case SMS_PORT_VDP_STATUS: return SMS_OWNER_VDP;
  }
  return SMS_OWNER_NONE;
}

#ifdef __cplusplus
}
#endif
#endif
