#ifndef SEGARECOMP_VIEWER_SV_SESSION_H
#define SEGARECOMP_VIEWER_SV_SESSION_H

/*
 * SEG-146-T006: platform-neutral guest-frame session recording and replay (format `segarecomp-session 1`).
 *
 * A session file is ASCII (LF; CR is tolerated on read). It holds only header hashes, button masks keyed by guest frame, periodic checkpoint
 * digests and an explicit end marker: no image bytes, saves, audio or video.
 *
 *   segarecomp-session 1
 *   platform genesis
 *   image <64 hex>            the effective (possibly patched) image the session ran on
 *   variant <64 hex|vanilla>  the patch/package identity (vanilla = none)
 *   digest <profile>          which machine-state digest the checkpoints use (e.g. machine-0003); checkpoints are enforced only when the replaying machine uses the same profile
 *   <frame> <player> <mask>   player 1|2; mask 0..255 decimal: that player's pad held from <frame> on (a pad starts released)
 *   checkpoint <frame> <64 hex>  also at the END frame: the final checkpoint always lies on the end marker, so a divergence anywhere before it cannot go unseen
 *   end <frame>               the recording lasted until <frame>; a file without it is refused on replay
 *
 * The adapter stamps the pad at each guest-frame boundary, before the guest reads input; replay applies it at the same boundary, so a
 * recording replays identically in the viewer and headless regardless of how a host slices execution. Recording streams every line
 * (a crash keeps a valid prefix without end marker, which replay refuses). Header-only and free of SDL and of any machine type: a console
 * adapter supplies the frame number, the pad and the checkpoint digest. Strict C11.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SV_SESSION_FORMAT "segarecomp-session 1"
#define SV_SESSION_MAX_EVENTS 4000000u        /* explicit cap: a recording reaching it is ended with an error, never truncated silently */
#define SV_SESSION_MAX_CHECKPOINTS 400000u
#define SV_SESSION_CHECKPOINT_INTERVAL 600u   /* guest frames between checkpoint digests (10 s at 60 Hz) */
#define SV_SESSION_MAX_LINE 160

typedef struct SvSessionIdentity {
  char platform[16];       /* "genesis", "master-system", "sega-cd" */
  uint8_t image[32];       /* effective image SHA-256 */
  uint8_t variant[32];     /* package identity; all zero = vanilla */
  char digest_profile[40]; /* the checkpoint digest profile of the machine (no spaces) */
} SvSessionIdentity;

typedef struct SvSessionEvent { uint64_t frame; uint8_t player; uint8_t mask; } SvSessionEvent;
typedef struct SvSessionCheckpoint { uint64_t frame; uint8_t digest[32]; } SvSessionCheckpoint;

static inline void sv_session_hex(const uint8_t *bytes, char out[65]) {
  static const char digits[] = "0123456789abcdef";
  int i;
  for (i = 0; i < 32; ++i) { out[2 * i] = digits[bytes[i] >> 4]; out[2 * i + 1] = digits[bytes[i] & 15]; }
  out[64] = '\0';
}

static inline int sv_session_unhex(const char *text, uint8_t out[32]) {
  int i;
  if (text == NULL) return 0;
  for (i = 0; i < 64; ++i) {
    const char c = text[i];
    const int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (v < 0) return 0;
    if ((i & 1) == 0) out[i / 2] = (uint8_t)(v << 4); else out[i / 2] = (uint8_t)(out[i / 2] | v);
  }
  return text[64] == '\0';
}

static inline int sv_session_all_zero(const uint8_t *b) {
  int i;
  for (i = 0; i < 32; ++i) if (b[i] != 0U) return 0;
  return 1;
}

/* ------------------------------------------------------------------------------------------------------------------ recording */

typedef enum SvSessionRecordResult { SV_SESSION_RECORD_OK = 0, SV_SESSION_RECORD_CAPACITY, SV_SESSION_RECORD_ERROR, SV_SESSION_RECORD_CLOSED } SvSessionRecordResult;

typedef struct SvSessionRecorder {
  FILE *file;
  uint32_t events;
  uint32_t max_events;        /* SV_SESSION_MAX_EVENTS unless a test lowers it */
  uint8_t last[2];
  uint64_t next_checkpoint;
  int finished;               /* the end marker was written and the file closed */
  char message[200];          /* why the recording ended early (capacity, I/O), or the final summary */
} SvSessionRecorder;

static inline int sv_session_record_open(SvSessionRecorder *r, const char *path, const SvSessionIdentity *id, char *error, size_t cap) {
  char image[65], variant[65];
  memset(r, 0, sizeof *r);
  r->max_events = SV_SESSION_MAX_EVENTS;
  r->file = fopen(path, "wb");
  if (r->file == NULL) { snprintf(error, cap, "cannot create the recording file '%s'", path); return -1; }
  sv_session_hex(id->image, image);
  if (sv_session_all_zero(id->variant)) strcpy(variant, "vanilla"); else sv_session_hex(id->variant, variant);
  if (fprintf(r->file, SV_SESSION_FORMAT "\nplatform %s\nimage %s\nvariant %s\ndigest %s\n", id->platform, image, variant, id->digest_profile[0] != '\0' ? id->digest_profile : "none") < 0 || fflush(r->file) != 0) {
    snprintf(error, cap, "cannot write the recording file '%s'", path);
    fclose(r->file);
    r->file = NULL;
    return -1;
  }
  return 0;
}

static inline int sv_session_record_active(const SvSessionRecorder *r) { return r != NULL && r->file != NULL && !r->finished; }

static inline SvSessionRecordResult sv_session_record_end(SvSessionRecorder *r, uint64_t frame) {
  int ok;
  if (r == NULL || r->file == NULL) return SV_SESSION_RECORD_CLOSED;
  ok = fprintf(r->file, "end %llu\n", (unsigned long long)frame) > 0 && fflush(r->file) == 0;
  ok = (fclose(r->file) == 0) && ok;
  r->file = NULL;
  r->finished = 1;
  if (!ok) { snprintf(r->message, sizeof r->message, "the recording could not be completed (write error)"); return SV_SESSION_RECORD_ERROR; }
  return SV_SESSION_RECORD_OK;
}

/* Stamps the pads held from `frame` on. Only changes are written. At capacity the recording is ended at `frame` with an explicit marker (a valid, replayable
 * prefix) and SV_SESSION_RECORD_CAPACITY is returned; the caller reports r->message and stops recording. */
static inline SvSessionRecordResult sv_session_record_pads(SvSessionRecorder *r, uint64_t frame, const uint8_t pad[2]) {
  int p;
  if (!sv_session_record_active(r)) return SV_SESSION_RECORD_CLOSED;
  for (p = 0; p < 2; ++p) {
    if (pad[p] == r->last[p]) continue;
    if (r->events >= r->max_events) {
      snprintf(r->message, sizeof r->message, "recording stopped at frame %llu: the %u-event capacity was reached (the recording up to this frame is complete and replayable)",
               (unsigned long long)frame, (unsigned)r->max_events);
      (void)sv_session_record_end(r, frame);
      return SV_SESSION_RECORD_CAPACITY;
    }
    if (fprintf(r->file, "%llu %d %u\n", (unsigned long long)frame, p + 1, (unsigned)pad[p]) < 0 || fflush(r->file) != 0) {
      snprintf(r->message, sizeof r->message, "recording stopped at frame %llu: the file could not be written", (unsigned long long)frame);
      fclose(r->file);
      r->file = NULL;
      r->finished = 1;
      return SV_SESSION_RECORD_ERROR;
    }
    r->last[p] = pad[p];
    ++r->events;
  }
  return SV_SESSION_RECORD_OK;
}

static inline int sv_session_record_checkpoint_due(const SvSessionRecorder *r, uint64_t frame) {
  return sv_session_record_active(r) && frame >= r->next_checkpoint && frame % SV_SESSION_CHECKPOINT_INTERVAL == 0U;
}

/* A checkpoint that cannot be written ends the recording with an error (the file is closed without an end marker, so replay refuses it): never a silently gappy recording. */
static inline SvSessionRecordResult sv_session_record_checkpoint(SvSessionRecorder *r, uint64_t frame, const uint8_t digest[32]) {
  char hex[65];
  if (!sv_session_record_active(r)) return SV_SESSION_RECORD_CLOSED;
  sv_session_hex(digest, hex);
  if (fprintf(r->file, "checkpoint %llu %s\n", (unsigned long long)frame, hex) < 0 || fflush(r->file) != 0) {
    snprintf(r->message, sizeof r->message, "recording stopped at frame %llu: a checkpoint could not be written", (unsigned long long)frame);
    fclose(r->file);
    r->file = NULL;
    r->finished = 1;
    return SV_SESSION_RECORD_ERROR;
  }
  r->next_checkpoint = frame + 1U;
  return SV_SESSION_RECORD_OK;
}

/* ------------------------------------------------------------------------------------------------------------------ replay */

typedef struct SvSessionReplay {
  SvSessionIdentity identity;     /* as recorded */
  SvSessionEvent *events;
  uint32_t count, next;
  SvSessionCheckpoint *checkpoints;
  uint32_t checkpoint_count, checkpoint_next;
  uint32_t compared;              /* checkpoints actually compared and equal (never counts a checkpoint that was not enforced) */
  uint64_t end_frame;
  int variant_mismatch;           /* identity differed and --allow-variant admitted it, or the digest profile differs: checkpoints are NOT enforced, so the replay is never reported as verified */
  uint8_t pad[2];                 /* the pads currently held */
  char note[240];                 /* the reported mismatch when one was admitted */
} SvSessionReplay;

static inline void sv_session_replay_free(SvSessionReplay *p) {
  if (p == NULL) return;
  free(p->events);
  free(p->checkpoints);
  memset(p, 0, sizeof *p);
}

static inline int sv_session_parse_u64(const char *s, uint64_t *out) {
  uint64_t v = 0U;
  if (s == NULL || *s == '\0') return 0;
  for (; *s != '\0'; ++s) {
    if (*s < '0' || *s > '9' || v > (UINT64_MAX - 9U) / 10U) return 0;
    v = v * 10U + (uint64_t)(*s - '0');
  }
  *out = v;
  return 1;
}

/* Loads and checks `path` against `expect`. 0 = ready; nonzero fills `error` (stable, user-facing messages). A different platform is always refused; a different
 * image or variant is refused unless `allow_variant`, in which case the mismatch is reported in `note` and checkpoints are not enforced. */
static inline int sv_session_replay_load(SvSessionReplay *p, const char *path, const SvSessionIdentity *expect, int allow_variant, char *error, size_t cap) {
  FILE *f = fopen(path, "rb");
  char line[SV_SESSION_MAX_LINE + 2];
  unsigned long line_no = 0U;
  int have_platform = 0, have_image = 0, have_variant = 0, have_digest = 0, have_end = 0;
  uint64_t last_checkpoint = 0U;
  int have_checkpoint = 0;
  uint64_t last_frame = 0U;
  memset(p, 0, sizeof *p);
  if (f == NULL) { snprintf(error, cap, "cannot open the recording '%s'", path); return -1; }
  while (fgets(line, sizeof line, f) != NULL) {
    size_t n = strlen(line);
    char a[SV_SESSION_MAX_LINE], b[SV_SESSION_MAX_LINE], c[SV_SESSION_MAX_LINE];
    ++line_no;
    a[0] = b[0] = c[0] = '\0';
    if (n > SV_SESSION_MAX_LINE || (n > 0U && line[n - 1U] != '\n' && !feof(f))) { snprintf(error, cap, "recording line %lu is too long", line_no); goto fail; }
    while (n > 0U && (line[n - 1U] == '\n' || line[n - 1U] == '\r')) line[--n] = '\0';
    if (line_no == 1U) {
      if (strcmp(line, SV_SESSION_FORMAT) != 0) { snprintf(error, cap, "'%s' is not a segarecomp session recording (expected the header '%s')", path, SV_SESSION_FORMAT); goto fail; }
      continue;
    }
    if (n == 0U || line[0] == '#') continue;
    if (have_end) { snprintf(error, cap, "recording line %lu follows the end marker", line_no); goto fail; }
    if (sscanf(line, "%159s %159s %159s", a, b, c) >= 1 && strcmp(a, "platform") == 0) {
      if (sscanf(line, "platform %15s", p->identity.platform) != 1) { snprintf(error, cap, "recording line %lu is malformed", line_no); goto fail; }
      have_platform = 1;
    } else if (strcmp(a, "image") == 0) {
      if (sscanf(line, "image %159s", b) != 1 || !sv_session_unhex(b, p->identity.image)) { snprintf(error, cap, "recording line %lu has a malformed image hash", line_no); goto fail; }
      have_image = 1;
    } else if (strcmp(a, "variant") == 0) {
      if (sscanf(line, "variant %159s", b) != 1) { snprintf(error, cap, "recording line %lu is malformed", line_no); goto fail; }
      if (strcmp(b, "vanilla") == 0) memset(p->identity.variant, 0, 32); else if (!sv_session_unhex(b, p->identity.variant)) { snprintf(error, cap, "recording line %lu has a malformed variant hash", line_no); goto fail; }
      have_variant = 1;
    } else if (strcmp(a, "digest") == 0) {
      if (sscanf(line, "digest %39s", p->identity.digest_profile) != 1) { snprintf(error, cap, "recording line %lu is malformed", line_no); goto fail; }
      have_digest = 1;
    } else if (strcmp(a, "checkpoint") == 0) {
      uint64_t frame;
      uint8_t digest[32];
      if (sscanf(line, "checkpoint %159s %159s", b, c) != 2 || !sv_session_parse_u64(b, &frame) || !sv_session_unhex(c, digest)) { snprintf(error, cap, "recording line %lu has a malformed checkpoint", line_no); goto fail; }
      if (have_checkpoint && frame <= last_checkpoint) { snprintf(error, cap, "recording line %lu has a checkpoint out of order (frame %llu after %llu)", line_no, (unsigned long long)frame, (unsigned long long)last_checkpoint); goto fail; }
      have_checkpoint = 1;
      last_checkpoint = frame;
      if (p->checkpoint_count >= SV_SESSION_MAX_CHECKPOINTS) { snprintf(error, cap, "the recording has more than %u checkpoints", SV_SESSION_MAX_CHECKPOINTS); goto fail; }
      if (p->checkpoint_count % 256U == 0U) {
        void *grown = realloc(p->checkpoints, ((size_t)p->checkpoint_count + 256U) * sizeof *p->checkpoints);
        if (grown == NULL) { snprintf(error, cap, "out of memory loading the recording"); goto fail; }
        p->checkpoints = (SvSessionCheckpoint *)grown;
      }
      p->checkpoints[p->checkpoint_count].frame = frame;
      memcpy(p->checkpoints[p->checkpoint_count].digest, digest, 32);
      ++p->checkpoint_count;
    } else if (strcmp(a, "end") == 0) {
      if (sscanf(line, "end %159s", b) != 1 || !sv_session_parse_u64(b, &p->end_frame)) { snprintf(error, cap, "recording line %lu has a malformed end marker", line_no); goto fail; }
      if (p->end_frame < last_frame) { snprintf(error, cap, "the end marker (frame %llu) precedes an event (frame %llu)", (unsigned long long)p->end_frame, (unsigned long long)last_frame); goto fail; }
      have_end = 1;
    } else {
      uint64_t frame, player, mask;
      char extra[8];
      if (sscanf(line, "%159s %159s %159s %7s", a, b, c, extra) != 3 || !sv_session_parse_u64(a, &frame) || !sv_session_parse_u64(b, &player) || !sv_session_parse_u64(c, &mask) ||
          (player != 1U && player != 2U) || mask > 255U) { snprintf(error, cap, "recording line %lu is malformed", line_no); goto fail; }
      if (frame < last_frame) { snprintf(error, cap, "recording line %lu goes back in time (frame %llu after %llu)", line_no, (unsigned long long)frame, (unsigned long long)last_frame); goto fail; }
      if (p->count >= SV_SESSION_MAX_EVENTS) { snprintf(error, cap, "the recording has more than %u events", SV_SESSION_MAX_EVENTS); goto fail; }
      if (p->count % 1024U == 0U) {
        void *grown = realloc(p->events, ((size_t)p->count + 1024U) * sizeof *p->events);
        if (grown == NULL) { snprintf(error, cap, "out of memory loading the recording"); goto fail; }
        p->events = (SvSessionEvent *)grown;
      }
      p->events[p->count].frame = frame;
      p->events[p->count].player = (uint8_t)player;
      p->events[p->count].mask = (uint8_t)mask;
      ++p->count;
      last_frame = frame;
    }
  }
  fclose(f);
  f = NULL;
  if (line_no == 0U) { snprintf(error, cap, "'%s' is not a segarecomp session recording (it is empty)", path); goto fail; }
  if (p->checkpoint_count != 0U && p->checkpoints[p->checkpoint_count - 1U].frame > p->end_frame && have_end) { snprintf(error, cap, "a checkpoint (frame %llu) lies beyond the end marker (frame %llu)", (unsigned long long)p->checkpoints[p->checkpoint_count - 1U].frame, (unsigned long long)p->end_frame); goto fail; }
  if (!have_platform || !have_image || !have_variant || !have_digest) { snprintf(error, cap, "the recording header is incomplete"); goto fail; }
  if (have_end && (p->checkpoint_count == 0U || p->checkpoints[p->checkpoint_count - 1U].frame != p->end_frame)) {
    snprintf(error, cap, "the recording has no checkpoint at its end marker (frame %llu): the state it ended in cannot be verified", (unsigned long long)p->end_frame);
    goto fail;
  }
  if (!have_end) { snprintf(error, cap, "the recording has no end marker: it was not finished (the game or the viewer stopped before 'Stop recording') and cannot be replayed"); goto fail; }
  if (strcmp(p->identity.platform, expect->platform) != 0) {
    snprintf(error, cap, "the recording is for the %s, this program is for the %s", p->identity.platform, expect->platform);
    goto fail;
  }
  if (memcmp(p->identity.image, expect->image, 32) != 0 || memcmp(p->identity.variant, expect->variant, 32) != 0) {
    char rec_image[65], run_image[65], rec_var[65], run_var[65];
    sv_session_hex(p->identity.image, rec_image);
    sv_session_hex(expect->image, run_image);
    if (sv_session_all_zero(p->identity.variant)) strcpy(rec_var, "vanilla"); else sv_session_hex(p->identity.variant, rec_var);
    if (sv_session_all_zero(expect->variant)) strcpy(run_var, "vanilla"); else sv_session_hex(expect->variant, run_var);
    if (!allow_variant) {
      snprintf(error, cap, "the recording was made on a different image or version (recorded image %.12s variant %.12s, running image %.12s variant %.12s); pass --allow-variant to replay it anyway",
               rec_image, rec_var, run_image, run_var);
      goto fail;
    }
    p->variant_mismatch = 1;
    snprintf(p->note, sizeof p->note, "replaying against a different image or version (recorded image %.12s variant %.12s, running image %.12s variant %.12s): checkpoint digests are not enforced",
             rec_image, rec_var, run_image, run_var);
  }
  if (strcmp(p->identity.digest_profile, expect->digest_profile[0] != '\0' ? expect->digest_profile : "none") != 0 && !p->variant_mismatch) {
    p->variant_mismatch = 1;
    snprintf(p->note, sizeof p->note, "the recording's checkpoints use the '%.39s' machine digest and this program uses '%.39s': checkpoint digests are not enforced", p->identity.digest_profile, expect->digest_profile[0] != '\0' ? expect->digest_profile : "none");
  }
  return 0;
fail:
  if (f != NULL) fclose(f);
  sv_session_replay_free(p);
  return -1;
}

/* The pads held at `frame` (events with frame <= `frame` applied in file order). */
static inline void sv_session_replay_pads(SvSessionReplay *p, uint64_t frame, uint8_t pad[2]) {
  while (p->next < p->count && p->events[p->next].frame <= frame) {
    p->pad[p->events[p->next].player - 1U] = p->events[p->next].mask;
    ++p->next;
  }
  pad[0] = p->pad[0];
  pad[1] = p->pad[1];
}

static inline int sv_session_replay_finished(const SvSessionReplay *p, uint64_t frame) { return frame >= p->end_frame; }

/* Compares the digest the machine computed at `frame` with the recorded one, if the recording has a checkpoint at exactly that frame. 0 = nothing to compare / equal /
 * not enforced; 1 = divergence (`error` says where). */
static inline int sv_session_replay_checkpoint(SvSessionReplay *p, uint64_t frame, const uint8_t digest[32], char *error, size_t cap) {
  if (p->checkpoint_next < p->checkpoint_count && p->checkpoints[p->checkpoint_next].frame < frame) {   /* a checkpoint that was never compared is not skipped silently */
    snprintf(error, cap, "replay passed the checkpoint at frame %llu without comparing it", (unsigned long long)p->checkpoints[p->checkpoint_next].frame);
    return 1;
  }
  if (p->checkpoint_next >= p->checkpoint_count || p->checkpoints[p->checkpoint_next].frame != frame) return 0;
  if (p->variant_mismatch) { ++p->checkpoint_next; return 0; }   /* not enforced: consumed, never counted as compared */
  if (memcmp(p->checkpoints[p->checkpoint_next].digest, digest, 32) != 0) {
    snprintf(error, cap, "replay diverged from the recording at frame %llu (checkpoint digest mismatch)", (unsigned long long)frame);
    return 1;
  }
  ++p->checkpoint_next;
  ++p->compared;
  return 0;
}

/* At the end marker every recorded checkpoint must have been compared (or deliberately not enforced). 0 = yes; 1 = one was never reached. */
static inline int sv_session_replay_checkpoints_complete(const SvSessionReplay *p, char *error, size_t cap) {
  if (p->checkpoint_next < p->checkpoint_count) {
    snprintf(error, cap, "replay ended before the checkpoint at frame %llu was compared", (unsigned long long)p->checkpoints[p->checkpoint_next].frame);
    return 1;
  }
  return 0;
}

#ifdef __cplusplus
}
#endif
#endif
