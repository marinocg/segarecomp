#ifndef SEGARECOMP_MASTER_SYSTEM_SMS_MOD_SCHEDULER_H
#define SEGARECOMP_MASTER_SYSTEM_SMS_MOD_SCHEDULER_H

/*
 * SEG-147-T004 (ADR 0325): the ONE Master System mod phase scheduler, shared by native C11 (ABI 2) providers and the Luau adapter (a Luau program defines the same `segarecomp_mod_provider` entry).
 *
 * It implements docs/architecture/mod-execution-phases.md for `reset`, `frame-begin`, `input`, `post-input`, `frame-end` and `render` (no function hooks: no Z80 hook spike exists). It is the Master
 * System realization of that contract, deliberately not a core shared with another platform's scheduler (mod-platform-adapters.md section 4). All callbacks run from the machine's frame hooks at the frame start of frame N
 * (T = N x 59,736), in the order frame-end(N-1), render(N-1), frame-begin(N), [the scripted input of frame N is applied], input(N), post-input(N); `reset` once before frame-begin(0). A
 * `sms_run_until_frame(N)` call returns after post-input(N): that boundary is the save-state barrier. Callbacks take no guest time.
 *
 * Every host service is checked against the capability, the phase and the declared range. A violation is a fault: the callback's staged writes, input, log and presentation are discarded, the mod is
 * disabled for the rest of the session, one `E_MOD_FAULT` line is emitted and the guest continues unmodified. Without a scheduler attached (`SmsMachine::frame_hooks == NULL`, `SmsPad::intercept ==
 * NULL`) the machine is exactly what it was.
 *
 * Strict C11, no allocation outside state restore, no wall clock, no host RNG. One scheduler per process (the ABI 2 service functions carry no context pointer).
 */
#include <stddef.h>
#include <stdint.h>

#include <segarecomp_mod_sms.h>

#include "sms_machine.h"
#include "sms_mod_present.h"
#include "sms_mod_state.h"
#include "sms_pad.h"
#include "sms_vdp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_MOD_WRITE_OPS_MAX 1024U /* per callback (phases section 4) */
#define SMS_MOD_WRITE_BYTES_MAX 16384U
#define SMS_MOD_LOG_LINES_MAX 16U
#define SMS_MOD_LOG_LINE_BYTES 256U
#define SMS_MOD_RANGES_MAX 16U
#define SMS_MOD_ACCESS_READ 1U
#define SMS_MOD_ACCESS_WRITE 2U

typedef struct SmsModRange {
  uint32_t space, offset, length, modes; /* SegaSmsSpace; offsets are inside the space (0 = $C000 for work RAM; the cart RAM space is the 32 KiB array) */
} SmsModRange;

typedef void (*SmsModLogSink)(void *context, const char *line, uint32_t length);

typedef struct SmsModConfig {
  const char *mod_id;                 /* 1..64 printable ASCII */
  uint8_t identity_sha256[32];        /* the package identity */
  uint64_t required[2], optional[2];  /* capability bitsets by id */
  const SegaModProvider *provider;
  void *provider_state;
  uint32_t phase_mask;                /* bit 1 << SegaModPhase; at least one of the six phases, no hook phase */
  const SmsModRange *ranges;
  uint32_t range_count;
  uint32_t state_schema;              /* 0 = the mod declares none */
  SmsModLogSink log_sink;
  void *log_context;
  /* Optional observer: frame `frame` is finished (frame-end and render have run, frame-begin of the next one has not). Called whether or not the mod is enabled. It observes only. */
  void (*frame_done)(void *context, uint64_t frame);
  void *frame_done_context;
} SmsModConfig;

typedef struct SmsModFault {
  uint32_t faulted, phase;
  uint64_t frame;
  char cause[32];
  char text[192]; /* E_MOD_FAULT cause=.. mod=.. phase=.. frame=.. */
} SmsModFault;

typedef struct SmsModStagedWrite {
  uint32_t space, offset, width, value;
} SmsModStagedWrite;

typedef struct SmsModScheduler {
  SmsModConfig config;
  SmsModRange ranges[SMS_MOD_RANGES_MAX];
  char mod_id[65];
  SegaModHost host;
  SegaMasterSystemFeatures features;
  SegaSmsRenderApi render_api;
  SmsMachine *machine;
  SmsPad *pad;
  const SmsVdp *vdp;
  SmsFrameHooks hooks;
  SmsInputState guest_input;   /* what the guest reads while `intercepting` */
  uint8_t intercepting;
  uint8_t started, disabled, in_callback;
  uint8_t in_set; /* SEG-141-T007: a member of an SmsModSet: the set owns the machine hooks and the pad interception */
  uint32_t phase;
  uint64_t frame;
  uint8_t final_pad[2], previous_final[2], sampled[2];
  SmsModStagedWrite staged[SMS_MOD_WRITE_OPS_MAX];
  uint32_t staged_count, staged_bytes;
  uint8_t staged_pad[2], staged_pad_set[2];
  char staged_log[SMS_MOD_LOG_LINES_MAX][SMS_MOD_LOG_LINE_BYTES + 1U];
  uint32_t staged_log_count;
  int fault_status;
  const char *external_cause;
  SmsModFault fault;
  uint8_t render_ram[SMS_RAM_SIZE];
  uint8_t render_ram_valid;
  uint8_t view_vram[SMS_VDP_VRAM_SIZE], view_cram[SMS_VDP_CRAM_SIZE], view_registers[SMS_VDP_REGISTERS];
  SegaSmsVdpView view;
  SmsModPresentation staged_presentation, presentation; /* `presentation`: the committed output of the last successful render */
  uint8_t presentation_valid;
  uint64_t presentation_frame;
  SmsSha256 write_stream;
  uint64_t write_total, callbacks_run;
} SmsModScheduler;

/* The capability ids this host implements (bitset by id). */
void sms_mod_scheduler_supported(uint64_t out[2]);

/* Validates the configuration and negotiates; on success installs the machine's frame hooks. Attach BEFORE the machine's first run and before `sms_machine_reset` of the session (the reset hook starts the
 * session over). `vdp` may be NULL (then a required frame.sms_vdp is refused). Returns 0, or non-zero with `*rejection` a stable class. */
int sms_mod_scheduler_attach(SmsModScheduler *s, SmsMachine *machine, SmsPad *pad, const SmsVdp *vdp, const SmsModConfig *config, const char **rejection);
void sms_mod_scheduler_detach(SmsModScheduler *s);

/* The presentation committed by the last successful `render`, or NULL (not rendered, faulted, restored, reset): composing NULL is the Vanilla picture. */
const SmsModPresentation *sms_mod_scheduler_presentation(const SmsModScheduler *s);
/* SHA-256 over every committed guest write since the last machine reset (frame, phase, space, offset, width, value): the replay witness. */
void sms_mod_write_log_digest(const SmsModScheduler *s, uint8_t out[32]);
/* A host adapter reports a failure of the callback in flight that the services did not see (a script error, a budget fault). `cause` is a static string. */
void sms_mod_fault_current(const char *cause);
/* Fills the snapshot seam of this scheduler. */
void sms_mod_scheduler_state(SmsModScheduler *s, SmsModState *out);

/*
 * SEG-141-T007 (ADR 0327): several mods on ONE Master System machine, with the ordering, input-chain, isolation and combined-state rules of ADR 0327 (there are no function hooks here). Every
 * member is an ordinary SmsModScheduler; the set owns the machine's frame hooks and the one pad interception. MEMBER INDEX IS THE COMPOSITE ORDER. Each phase runs for all members before the next; a
 * later member observes earlier members' committed writes; `input` chains the pad; a faulted member is disabled alone and the interception is released only when no live member has `input`.
 * `frame_done` is the set's one observer, called after every member's frame-end and render.
 */
#define SMS_MOD_SET_MEMBERS_MAX 16U

typedef struct SmsModSet {
  SmsModScheduler *members[SMS_MOD_SET_MEMBERS_MAX]; /* caller storage */
  uint32_t count;
  SmsMachine *machine;
  SmsPad *pad;
  SmsFrameHooks hooks;
  SmsInputState guest_input;
  uint8_t identity[32]; /* the composite identity: a saved state restores only into a set with exactly this identity */
  void (*frame_done)(void *context, uint64_t frame);
  void *frame_done_context;
} SmsModSet;

/* Attaches schedulers[i] with configs[i] (every rule of sms_mod_scheduler_attach; a member's own frame_done is ignored). Returns 0, or 1 with *rejection a stable class and *member the index concerned
 * (`count` for a set-level refusal): E_SET_SIZE, E_SET_IDENTITY, E_SET_DUPLICATE_MOD or a member's own class. Nothing stays attached on rejection. */
int sms_mod_set_attach(SmsModSet *set, SmsMachine *machine, SmsPad *pad, const SmsVdp *vdp, SmsModScheduler *const *schedulers, const SmsModConfig *configs, uint32_t count, const uint8_t identity[32],
                       void (*frame_done)(void *context, uint64_t frame), void *frame_done_context, const char **rejection, uint32_t *member);
void sms_mod_set_detach(SmsModSet *set);
/* One MODSTATE section bound to the composite identity, holding every member's state in order. The members' blobs share the section's one bound. Restore is all-or-nothing for the guest. */
void sms_mod_set_state(SmsModSet *set, SmsModState *out);

#ifdef __cplusplus
}
#endif
#endif
