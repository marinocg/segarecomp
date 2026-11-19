#ifndef SEGARECOMP_VIEWER_GENESIS_SESSION_H
#define SEGARECOMP_VIEWER_GENESIS_SESSION_H

/*
 * SEG-146-T006: the Genesis adapter of the shared session recorder/replayer (libs/viewer/include/sv_session.h, format `segarecomp-session 1`).
 *
 * Both pads are stamped (record) or applied (replay) at every guest-frame boundary through the runtime's host-owned frame-hook seam (`GenesisFrameHooks`): `start`
 * for frame 0 and `after_snapshot` for every later frame, i.e. on a dispatch boundary, before the guest executes another instruction of that frame. The pad the guest
 * reads is latched through the runtime's `mod_pad_*` override, so mid-frame host polls (the viewer samples per slice) can never change what the guest sees; the viewer
 * and the headless capture therefore replay one file identically however they slice execution. If a mod scheduler already owns the hooks it is wrapped, not replaced
 * (this adapter must be installed after the scheduler is attached).
 *
 * A session ends ON a guest-frame boundary. A replay stops at its end marker's boundary: the hook asks the runner to yield right after the dispatch that crossed it
 * (`GenesisRuntime::host_yield_request`), so no instruction beyond that boundary runs whatever the host's slice size. A recording ends at the boundary where "Stop recording"
 * was honoured (`genesis_session_request_end`), or, when the program is quit, at the last boundary reached (what ran after it is not part of the recording).
 *
 * Checkpoints hash the WHOLE guest machine through a host-supplied `GenesisSessionDigest` (genesis_session_machine.h: the snapshot container digest covering CPU, work RAM, VDP,
 * I/O, cartridge state, Z80, PSG, YM2612, mixer and mod state). A host without one gets the "basic" profile (work RAM, guest time, frame counter); the profile is written into
 * the recording and checkpoints are enforced only against the same profile.
 *
 * Header-only; nothing here links SDL. Strict C11.
 *
 * Environment (the generated argv ABI is unchanged; the launcher/driver maps --record/--replay/--allow-variant onto it):
 *   SEGARECOMP_RECORD=<file>     record the session from power-on
 *   SEGARECOMP_REPLAY=<file>     replay a recording (viewer or headless capture)
 *   SEGARECOMP_ALLOW_VARIANT=1   replay against a different effective image / package
 * Identity: SEGARECOMP_ROM_SHA256 (the effective image) and SEGARECOMP_SAVE_VARIANT (package identity or "vanilla"), both supplied by the trusted launcher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "sv_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GenesisSessionMode { GENESIS_SESSION_NONE = 0, GENESIS_SESSION_RECORD, GENESIS_SESSION_REPLAY } GenesisSessionMode;

/* The checkpoint digest of a host: `fn` returns 0 and fills `out`, nonzero when the machine cannot be digested (the session then fails, loudly). */
typedef int (*GenesisSessionDigestFn)(void *ctx, const GenesisRuntime *runtime, uint8_t out[32]);
typedef struct GenesisSessionDigest GenesisSessionDigest;
struct GenesisSessionDigest {
  const char *profile;   /* no spaces, at most 39 characters; identifies what the digest covers */
  GenesisSessionDigestFn fn;
  void *ctx;
  void (*prepare)(void *ctx, GenesisSessionDigest *self);   /* optional: called once when the session is created, after a mod scheduler attached (refreshes `profile`) */
};

typedef struct GenesisSession {
  GenesisSessionMode mode;
  GenesisFrameHooks hooks;
  const GenesisFrameHooks *inner;   /* what was installed before (a mod scheduler), or NULL */
  GenesisRuntime *runtime;
  GenesisSessionDigest digest;
  char profile[40];
  SvSessionRecorder recorder;
  SvSessionReplay replay;
  int started;
  int latched;                      /* this adapter set runtime->mod_pad_active */
  int done;                         /* replay reached its end marker, or the recording ended */
  int failed;                       /* divergence or recording error */
  int end_requested;                /* record: end at the next boundary ("Stop recording") */
  int ended_cleanly;                /* replay: the end marker was reached (not quit early) */
  uint64_t frames;                  /* guest frame of the last boundary (the frame the session ended at once done) */
  uint32_t checkpoints_checked;
  uint8_t end_digest[32];           /* the state digest at the boundary where the session ended */
  char message[320];
} GenesisSession;

/* The "basic" profile: work RAM, guest master tick and frame counter. */
static inline int genesis_session_basic_digest(void *ctx, const GenesisRuntime *rt, uint8_t out[32]) {
  GenesisSha256 sha;
  uint8_t tail[16];
  int i;
  (void)ctx;
  for (i = 0; i < 8; ++i) {
    tail[i] = (uint8_t)(rt->scheduler.master_ticks >> (8 * i));
    tail[8 + i] = (uint8_t)(rt->virtual_frame_count >> (8 * i));
  }
  genesis_sha256_init(&sha);
  genesis_sha256_update(&sha, rt->work_ram, (uint32_t)sizeof rt->work_ram);
  genesis_sha256_update(&sha, tail, 16U);
  genesis_sha256_final(&sha, out);
  return 0;
}

static inline int genesis_session_digest(GenesisSession *s, const GenesisRuntime *rt, uint8_t out[32]) {
  if (s->digest.fn != NULL) return s->digest.fn(s->digest.ctx, rt, out);
  return genesis_session_basic_digest(NULL, rt, out);
}

static inline void genesis_session_fail(GenesisSession *s, GenesisRuntime *rt, const char *message) {
  snprintf(s->message, sizeof s->message, "%s", message);
  s->done = 1;
  s->failed = 1;
  rt->host_yield_request = 1U;   /* stop at once: a failed session never keeps running unobserved */
}

/* One guest-frame boundary. Order (identical for recording and replay, so a checkpoint digest sees the same machine in both): this frame's pads are stamped / applied FIRST,
 * then the checkpoint digest is taken, then the session may end. */
static inline void genesis_session_boundary(GenesisSession *s, GenesisRuntime *rt) {
  const uint64_t frame = rt->virtual_frame_count;
  uint8_t digest[32];
  uint8_t pad[2];
  if (s->done) return;
  s->frames = frame;
  if (s->mode == GENESIS_SESSION_RECORD) {
    SvSessionRecordResult r;
    const int due = sv_session_record_checkpoint_due(&s->recorder, frame);
    pad[0] = rt->pad1;
    pad[1] = rt->pad2;
    r = sv_session_record_pads(&s->recorder, frame, pad);
    if (r != SV_SESSION_RECORD_OK) {   /* capacity (the recording ended with an explicit marker; play on) or a write error */
      snprintf(s->message, sizeof s->message, "%s", s->recorder.message);
      s->done = 1;
      s->failed = r != SV_SESSION_RECORD_CAPACITY;
      if (s->failed) rt->host_yield_request = 1U;
      return;
    }
    if ((due || s->end_requested) && genesis_session_digest(s, rt, digest) != 0) { genesis_session_fail(s, rt, "the machine state could not be digested for a checkpoint"); return; }
    if (due) {
      r = sv_session_record_checkpoint(&s->recorder, frame, digest);
      if (r != SV_SESSION_RECORD_OK) { genesis_session_fail(s, rt, s->recorder.message); return; }
    }
    if (s->end_requested) {   /* "Stop recording": the recording ends on this boundary */
      memcpy(s->end_digest, digest, 32);
      if (sv_session_record_end(&s->recorder, frame) != SV_SESSION_RECORD_OK) { genesis_session_fail(s, rt, s->recorder.message); return; }
      snprintf(s->message, sizeof s->message, "recording saved (%u pad change(s), %llu guest frame(s))", (unsigned)s->recorder.events, (unsigned long long)frame);
      s->done = 1;
      rt->host_yield_request = 1U;
    }
  } else if (s->mode == GENESIS_SESSION_REPLAY) {
    sv_session_replay_pads(&s->replay, frame, pad);
    rt->pad1 = pad[0];
    rt->pad2 = pad[1];
    if (genesis_session_digest(s, rt, digest) != 0) { genesis_session_fail(s, rt, "the machine state could not be digested for a checkpoint"); return; }
    if (sv_session_replay_checkpoint(&s->replay, frame, digest, s->message, sizeof s->message) != 0) { s->done = 1; s->failed = 1; rt->host_yield_request = 1U; return; }
    s->checkpoints_checked = s->replay.checkpoint_next;
    if (sv_session_replay_finished(&s->replay, frame)) {
      if (!s->replay.variant_mismatch && sv_session_replay_checkpoints_complete(&s->replay, s->message, sizeof s->message) != 0) { s->done = 1; s->failed = 1; rt->host_yield_request = 1U; return; }
      memcpy(s->end_digest, digest, 32);
      snprintf(s->message, sizeof s->message, "replay finished at frame %llu (%u checkpoint(s) matched)", (unsigned long long)frame, (unsigned)s->replay.checkpoint_next);
      s->ended_cleanly = 1;
      s->done = 1;
      rt->host_yield_request = 1U;   /* no instruction beyond this boundary runs, whatever the host's slice size */
    }
  }
}

/* After the inner hooks ran (a scheduler's `input` phase may have set the guest pad): make sure the guest reads exactly the stamped physical pad for this frame. */
static inline void genesis_session_latch(GenesisSession *s, GenesisRuntime *rt) {
  const int active = s->mode == GENESIS_SESSION_RECORD ? (sv_session_record_active(&s->recorder) && !s->done) : (s->mode == GENESIS_SESSION_REPLAY && !s->done);   /* symmetric at the end boundary */
  if (!active) {  /* a recording that ended early hands the pad back to the host */
    if (s->latched) { rt->mod_pad_active = 0U; s->latched = 0; }
    return;
  }
  if (rt->mod_pad_active == 0U || s->latched) {
    rt->mod_pad_active = 1U;
    rt->mod_pad1 = rt->pad1;
    rt->mod_pad2 = rt->pad2;
    s->latched = 1;
  }
}

static void genesis_session_hook_start(void *ctx, GenesisRuntime *rt) {
  GenesisSession *s = (GenesisSession *)ctx;
  const int first = !s->started;   /* `start` runs at the head of EVERY genesis_runtime_run slice; only the very first is a frame boundary (power-on, frame 0) */
  if (first) {
    s->started = 1;
    genesis_session_boundary(s, rt);
  }
  if (s->inner != NULL && s->inner->start != NULL) s->inner->start(s->inner->ctx, rt);
  if (first) genesis_session_latch(s, rt);
}

static void genesis_session_hook_frame_end(void *ctx, GenesisRuntime *rt) {
  GenesisSession *s = (GenesisSession *)ctx;
  if (s->inner != NULL && s->inner->frame_end != NULL) s->inner->frame_end(s->inner->ctx, rt);
}

static void genesis_session_hook_after_snapshot(void *ctx, GenesisRuntime *rt) {
  GenesisSession *s = (GenesisSession *)ctx;
  genesis_session_boundary(s, rt);
  if (s->inner != NULL && s->inner->after_snapshot != NULL) s->inner->after_snapshot(s->inner->ctx, rt);
  genesis_session_latch(s, rt);
}

static inline void genesis_session_install(GenesisSession *s, GenesisRuntime *rt) {
  s->runtime = rt;
  s->inner = rt->frame_hooks;
  s->hooks.ctx = s;
  s->hooks.start = genesis_session_hook_start;
  s->hooks.frame_end = genesis_session_hook_frame_end;
  s->hooks.after_snapshot = genesis_session_hook_after_snapshot;
  rt->frame_hooks = &s->hooks;
}

static inline int genesis_session_identity(SvSessionIdentity *id, const char *profile, char *error, size_t cap) {
  const char *image = getenv("SEGARECOMP_ROM_SHA256");
  const char *variant = getenv("SEGARECOMP_SAVE_VARIANT");
  memset(id, 0, sizeof *id);
  snprintf(id->platform, sizeof id->platform, "genesis");
  snprintf(id->digest_profile, sizeof id->digest_profile, "%s", profile);
  if (image == NULL || !sv_session_unhex(image, id->image)) { snprintf(error, cap, "this game has no verified identity (SEGARECOMP_ROM_SHA256), so a session cannot be recorded or replayed"); return -1; }
  if (variant != NULL && *variant != '\0' && strcmp(variant, "vanilla") != 0 && !sv_session_unhex(variant, id->variant)) {
    snprintf(error, cap, "the version of this game has no valid identity (SEGARECOMP_SAVE_VARIANT)");
    return -1;
  }
  return 0;
}

/* Reads the environment. `digest` is the host's machine digest (NULL = the "basic" profile). Returns 0 = no session requested, 1 = installed (record or replay),
 * -1 = refused (`error` says why; the caller reports it and exits). */
static inline int genesis_session_from_environment(GenesisSession *s, GenesisRuntime *rt, const GenesisSessionDigest *digest, char *error, size_t cap) {
  const char *record = getenv("SEGARECOMP_RECORD");
  const char *replay = getenv("SEGARECOMP_REPLAY");
  SvSessionIdentity id;
  memset(s, 0, sizeof *s);
  if ((record == NULL || *record == '\0') && (replay == NULL || *replay == '\0')) return 0;
  if (record != NULL && *record != '\0' && replay != NULL && *replay != '\0') { snprintf(error, cap, "--record and --replay cannot be used together"); return -1; }
  if (digest != NULL) {
    s->digest = *digest;
    if (s->digest.prepare != NULL) s->digest.prepare(s->digest.ctx, &s->digest);
  }
  if (s->digest.fn != NULL && s->digest.profile != NULL && s->digest.profile[0] != '\0') {
    snprintf(s->profile, sizeof s->profile, "%.39s", s->digest.profile);
  } else {
    s->digest.fn = NULL;
    snprintf(s->profile, sizeof s->profile, "basic");
  }
  if (genesis_session_identity(&id, s->profile, error, cap) != 0) return -1;
  if (record != NULL && *record != '\0') {
    if (sv_session_record_open(&s->recorder, record, &id, error, cap) != 0) return -1;
    s->mode = GENESIS_SESSION_RECORD;
  } else {
    const char *allow = getenv("SEGARECOMP_ALLOW_VARIANT");
    if (sv_session_replay_load(&s->replay, replay, &id, allow != NULL && strcmp(allow, "1") == 0, error, cap) != 0) return -1;
    s->mode = GENESIS_SESSION_REPLAY;
    if (s->replay.note[0] != '\0') fprintf(stderr, "session: %s\n", s->replay.note);
  }
  genesis_session_install(s, rt);
  return 1;
}

/* "Stop recording": the recording ends at the next guest-frame boundary (within one frame), where the runner is asked to yield. */
static inline int genesis_session_request_end(GenesisSession *s) {
  if (s->mode != GENESIS_SESSION_RECORD || s->done || !sv_session_record_active(&s->recorder)) return -1;
  s->end_requested = 1;
  return 0;
}

/* Ends the session and removes the adapter. A recording still open (the program was quit) ends at the last boundary reached: what ran after it is not part of the recording. */
static inline void genesis_session_finish(GenesisSession *s) {
  if (s->mode == GENESIS_SESSION_NONE) return;
  if (s->runtime != NULL && s->runtime->frame_hooks == &s->hooks) s->runtime->frame_hooks = s->inner;
  if (s->runtime != NULL && s->latched) { s->runtime->mod_pad_active = 0U; s->latched = 0; }
  if (s->mode == GENESIS_SESSION_RECORD) {
    if (sv_session_record_active(&s->recorder)) {
      if (sv_session_record_end(&s->recorder, s->frames) != SV_SESSION_RECORD_OK) { s->failed = 1; snprintf(s->message, sizeof s->message, "%s", s->recorder.message); }
      else snprintf(s->message, sizeof s->message, "recording saved (%u pad change(s), %llu guest frame(s))", (unsigned)s->recorder.events, (unsigned long long)s->frames);
    }
  } else {
    sv_session_replay_free(&s->replay);
  }
  s->done = 1;
  s->mode = GENESIS_SESSION_NONE;
}

#ifdef __cplusplus
}
#endif
#endif
