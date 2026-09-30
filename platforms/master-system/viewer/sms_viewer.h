#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_VIEWER_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_VIEWER_H

/* Master System viewer core (SEG-009-T009; ADR 0070). Host-neutral and SDL-free: the guest run is exactly the T003
 * machine loop (`sms_run_until_frame`, one guest frame per iteration), so a viewer run and a headless run with the same
 * recorded input give identical guest results. The host supplies clock, sleeper, input poll, presenter and an optional
 * audio sink through `SmsViewerHost`; tests inject fakes, the SDL3 adapter (sms_viewer_sdl3.c) supplies real ones.
 *
 * Determinism rules. Wall-clock time only paces presentation and never enters guest state. Input is sampled once per
 * iteration (a deterministic frame boundary) and recorded as a scripted-input event whose frame is the next frame start
 * the machine has not applied yet, so `sms_viewer_format_script` is a script the headless driver replays to the same
 * state digest, per-frame framebuffer hashes and PCM. Audio is drained from the PSG ring after the frame completes; a
 * host audio failure or absence is counted and ignored and cannot reach guest state. A reset re-runs the deterministic
 * machine reset and restarts the recording (the script describes the run since the last reset). */
#include <stddef.h>
#include <stdint.h>

#include "sms_machine.h"
#include "sms_pad.h"
#include "sms_psg.h"
#include "sms_render.h"
#include "sms_vdp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_VIEWER_PCM_RATE 44100u
#define SMS_VIEWER_RING_SAMPLES 8192u
/* The trace capacities equal the headless driver's: the VDP and machine digests include the trace entry counts. */
#define SMS_VIEWER_TRACE_CAPACITY 65536u
#define SMS_VIEWER_FRAME_RECORDS 4096u
#define SMS_VIEWER_INPUT_CAPACITY 65536u

typedef struct SmsViewerRig {
  SmsMachine machine;
  SmsVdp vdp;
  SmsVdpTraceEntry vdp_trace[SMS_VIEWER_TRACE_CAPACITY];
  SmsRenderer renderer;
  SmsFrameRecord records[SMS_VIEWER_FRAME_RECORDS];
  SmsPsg psg;
  SmsPad pad;
  Sn76489Ring ring;
  int16_t ring_storage[SMS_VIEWER_RING_SAMPLES];
  SmsIrqTraceEntry irq_trace[SMS_VIEWER_TRACE_CAPACITY];
  SmsMapperTraceEntry mapper_trace[SMS_VIEWER_TRACE_CAPACITY];
  SmsInputEvent events[SMS_VIEWER_INPUT_CAPACITY];
  uint32_t event_count;
  SmsInputState recorded; /* the last recorded input state */
} SmsViewerRig;

/* Composes the machine with the VDP, renderer, PSG (PCM into the ring) and pad exactly as the headless driver does and
 * performs the deterministic reset. Returns 0 and sets `*error` when the machine cannot be built. */
int sms_viewer_rig_init(SmsViewerRig *rig, const uint8_t *rom, uint32_t rom_size, SmsMapperFamily family, SmsError *error);
/* Deterministic machine reset with every viewer-side record (renderer records, ring, input recording) restarted. */
void sms_viewer_rig_reset(SmsViewerRig *rig);

/* Script text of the recorded input (`<frame> <p1> <p2> <pause>` per event, sms_input.h). Returns the byte count written
 * (no terminator), or 0 when `cap` is too small. */
size_t sms_viewer_format_script(const SmsViewerRig *rig, char *out, size_t cap);

/* One input sample. Pad masks use SMS_PAD_*; `pause` is the held pause button (an NMI edge on a press), `reset` and
 * `quit` are one-shot requests. */
typedef struct SmsViewerInput {
  uint8_t p1, p2, pause, reset, quit;
} SmsViewerInput;

typedef struct SmsViewerHost {
  void *ctx;
  uint64_t (*now_ns)(void *ctx);            /* monotonic clock */
  void (*sleep_ns)(void *ctx, uint64_t ns); /* may be NULL only if unthrottled */
  int (*poll)(void *ctx, SmsViewerInput *in);                              /* pumps events; 0 == ok */
  int (*present)(void *ctx, const uint8_t *framebuffer, uint32_t height);  /* 256 x height CRAM bytes; 0 == ok */
  int (*audio)(void *ctx, const int16_t *samples, uint32_t count);         /* s16 mono; optional; 0 == accepted */
} SmsViewerHost;

/* Fixed player-1 key map (SDL3 adapter): arrows = D-pad, Z = button 1, X = button 2, P = pause, R = reset. */
typedef struct SmsViewerKeys {
  uint8_t up, down, left, right, button1, button2; /* nonzero == held */
} SmsViewerKeys;
uint8_t sms_viewer_pad_from_keys(const SmsViewerKeys *keys);

typedef struct SmsViewerOptions {
  uint8_t unthrottled; /* presentation policy only */
  uint8_t mute;        /* no audio device */
  uint64_t frames;     /* stop after this many guest frames; SMS_NO_LIMIT for none */
  uint32_t scale;      /* initial window scale, > 0 */
  const char *record;  /* path to write the recorded input script to on exit, or NULL */
} SmsViewerOptions;

/* Recognizes --viewer-unthrottled, --viewer-mute, --viewer-frames <n>, --viewer-scale <n>, --viewer-record <path>.
 * Unknown arguments are ignored. Returns 0 on success, -1 on a malformed option. */
int sms_viewer_options_parse(int argc, const char *const *argv, SmsViewerOptions *out);

/* Accumulated-deadline frame pacer with the exact rational period 59,736 T x 11 / 39,375,000 Hz. */
typedef struct SmsPacer {
  uint64_t deadline_ns, rem, last_now_ns;
  uint8_t started, unthrottled;
  uint32_t sleep_calls, resyncs;
} SmsPacer;
#define SMS_PACER_MAX_LATE_PERIODS UINT64_C(4)
void sms_pacer_init(SmsPacer *pacer, int unthrottled);
uint64_t sms_pacer_period_floor_ns(void);
/* Sleeps until the accumulated deadline (unless unthrottled/late) and advances it by one period. Returns 0, or -1 (state
 * unmodified) on null arguments or a non-monotonic clock. */
int sms_pacer_wait(SmsPacer *pacer, const SmsViewerHost *host);

typedef enum SmsViewerOutcome {
  SMS_VIEWER_FRAME_LIMIT = 1,
  SMS_VIEWER_QUIT = 2,
  SMS_VIEWER_GUEST_ERROR = 3, /* permanent machine stop (Z80 outcome or SMS_ERROR_*); see `stop` */
  SMS_VIEWER_INVALID_ARGUMENT = 4,
  SMS_VIEWER_HOST_ERROR = 5,  /* poll, present or clock failure */
  SMS_VIEWER_INPUT_FULL = 6   /* the input recording is full */
} SmsViewerOutcome;

typedef struct SmsViewerResult {
  SmsViewerOutcome outcome;
  SmsStop stop;            /* last machine stop */
  uint64_t frames;         /* guest frames run (all resets included) */
  uint64_t frames_presented;
  uint64_t audio_samples;  /* samples drained from the ring */
  uint64_t audio_failures; /* host audio refusals (never affect the guest) */
  uint32_t resets;
} SmsViewerResult;

/* Runs at most `max_frames` guest frames (SMS_NO_LIMIT for none) from the machine's current state. */
SmsViewerResult sms_viewer_run(SmsViewerRig *rig, const SmsViewerHost *host, SmsPacer *pacer, uint64_t max_frames);

#ifdef __cplusplus
}
#endif
#endif
