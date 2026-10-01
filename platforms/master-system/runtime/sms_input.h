#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_INPUT_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_INPUT_H

#include <stddef.h>
#include <stdint.h>

/* Scripted input (machine contract section 11, ADR 0064 section 8): UTF-8 text, one event per line,
 * `<frame> <p1> <p2> <pause>`; `<frame>` decimal and non-decreasing; `<p1>`/`<p2>` six characters over `UDLR12` with
 * `-` for released (for example `U---1-`); `<pause>` is `P` or `-`; `#` starts a comment. An event takes effect at the
 * start of its frame (T = frame x 59,736) and holds until the next one. This module only parses and stores; the
 * machine applies events at frame starts (pause edge = `-` to `P`) and T006 turns pad masks into port bits. */
#ifdef __cplusplus
extern "C" {
#endif

/* Pad mask bits: pressed = 1. */
#define SMS_PAD_UP 0x01u
#define SMS_PAD_DOWN 0x02u
#define SMS_PAD_LEFT 0x04u
#define SMS_PAD_RIGHT 0x08u
#define SMS_PAD_BUTTON1 0x10u /* TL */
#define SMS_PAD_BUTTON2 0x20u /* TR */

typedef struct SmsInputEvent {
  uint64_t frame;
  uint8_t p1;
  uint8_t p2;
  uint8_t pause; /* 1 = pressed */
} SmsInputEvent;

typedef struct SmsInputState {
  uint8_t p1;
  uint8_t p2;
  uint8_t pause;
} SmsInputState;

/* Parses `text[0..size)` into `events[0..capacity)`. Returns 0 on success (`*count` set) or the 1-based line number of
 * the first malformed line, a decreasing frame, or capacity overflow (`*count` then holds the events stored so far). */
unsigned sms_input_parse(const char *text, size_t size, SmsInputEvent *events, uint32_t capacity, uint32_t *count);

#ifdef __cplusplus
}
#endif
#endif
