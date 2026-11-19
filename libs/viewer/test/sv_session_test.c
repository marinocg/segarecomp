/* SEG-146-T006: the shared session format (record, replay, identity, capacity, end marker, checkpoints, privacy). Pure C, no SDL, no machine. */
#include <stdio.h>
#include <string.h>

#include "sv_session.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { ++failures; fprintf(stderr, "FAIL: %s (line %d)\n", m, __LINE__); } } while (0)

static void ident(SvSessionIdentity *id, uint8_t image, uint8_t variant) {
  memset(id, 0, sizeof *id);
  strcpy(id->platform, "genesis");
  strcpy(id->digest_profile, "machine-0001");
  memset(id->image, image, 32);
  memset(id->variant, variant, 32);
}

static void write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  fputs(text, f);
  fclose(f);
}

int main(void) {
  const char *path = "sv_session_test.rec";
  SvSessionIdentity a, b, c, other_platform;
  SvSessionRecorder rec;
  SvSessionReplay rp;
  char err[400];
  uint8_t pad[2], digest[32], wrong[32];
  uint8_t held[2];
  ident(&a, 0x11, 0);
  ident(&b, 0x22, 0);
  ident(&c, 0x11, 0x33);
  other_platform = a;
  strcpy(other_platform.platform, "master-system");
  memset(digest, 0xAB, 32);
  memset(wrong, 0xCD, 32);

  /* record: only changes are written, checkpoints and the explicit end marker follow */
  CHECK(sv_session_record_open(&rec, path, &a, err, sizeof err) == 0, "open");
  held[0] = 0; held[1] = 0;
  CHECK(sv_session_record_pads(&rec, 0, held) == SV_SESSION_RECORD_OK && rec.events == 0U, "released pads at frame 0 write nothing");
  held[0] = 0x41;
  CHECK(sv_session_record_pads(&rec, 3, held) == SV_SESSION_RECORD_OK && rec.events == 1U, "a change is one event");
  CHECK(sv_session_record_pads(&rec, 4, held) == SV_SESSION_RECORD_OK && rec.events == 1U, "no change, no event");
  held[1] = 0x10;
  CHECK(sv_session_record_pads(&rec, 9, held) == SV_SESSION_RECORD_OK && rec.events == 2U, "player 2 is independent");
  CHECK(sv_session_record_checkpoint_due(&rec, 600), "a checkpoint is due on the interval");
  CHECK(!sv_session_record_checkpoint_due(&rec, 601), "not off the interval");
  sv_session_record_checkpoint(&rec, 600, digest);
  held[0] = 0;
  CHECK(sv_session_record_pads(&rec, 700, held) == SV_SESSION_RECORD_OK, "release");
  CHECK(sv_session_record_end(&rec, 1000) == SV_SESSION_RECORD_OK, "end marker");

  /* replay against the same identity */
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) == 0, "replay loads");
  CHECK(rp.count == 3U && rp.end_frame == 1000U, "events and the end marker are read");
  sv_session_replay_pads(&rp, 2, pad);
  CHECK(pad[0] == 0 && pad[1] == 0, "nothing before the first event");
  sv_session_replay_pads(&rp, 3, pad);
  CHECK(pad[0] == 0x41 && pad[1] == 0, "applied at the exact frame");
  sv_session_replay_pads(&rp, 9, pad);
  CHECK(pad[0] == 0x41 && pad[1] == 0x10, "player 2 applied");
  sv_session_replay_pads(&rp, 800, pad);
  CHECK(pad[0] == 0 && pad[1] == 0x10, "player 1 released");
  CHECK(sv_session_replay_checkpoint(&rp, 600, digest, err, sizeof err) == 0, "equal checkpoint passes");
  CHECK(!sv_session_replay_finished(&rp, 999) && sv_session_replay_finished(&rp, 1000), "end frame");
  sv_session_replay_free(&rp);
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) == 0, "reload");
  CHECK(sv_session_replay_checkpoint(&rp, 600, wrong, err, sizeof err) == 1 && strstr(err, "diverged") != NULL && strstr(err, "frame 600") != NULL, "a divergent digest is reported at its frame");
  sv_session_replay_free(&rp);

  /* identity: refused unless --allow-variant, always for another platform */
  CHECK(sv_session_replay_load(&rp, path, &b, 0, err, sizeof err) != 0 && strstr(err, "different image") != NULL && strstr(err, "--allow-variant") != NULL, "image mismatch is refused with the flag named");
  CHECK(sv_session_replay_load(&rp, path, &c, 0, err, sizeof err) != 0 && strstr(err, "different image or version") != NULL, "variant mismatch is refused");
  CHECK(sv_session_replay_load(&rp, path, &c, 1, err, sizeof err) == 0 && rp.variant_mismatch && strstr(rp.note, "not enforced") != NULL, "--allow-variant admits it and reports the mismatch");
  CHECK(sv_session_replay_checkpoint(&rp, 600, wrong, err, sizeof err) == 0, "checkpoints are not enforced against a variant");
  sv_session_replay_free(&rp);
  CHECK(sv_session_replay_load(&rp, path, &other_platform, 1, err, sizeof err) != 0 && strstr(err, "is for the genesis") != NULL, "another platform is never admitted");

  /* missing end marker (a crash keeps a valid prefix that replay refuses) */
  CHECK(sv_session_record_open(&rec, path, &a, err, sizeof err) == 0, "open prefix");
  held[0] = 1; held[1] = 0;
  CHECK(sv_session_record_pads(&rec, 5, held) == SV_SESSION_RECORD_OK, "event");
  fflush(rec.file);
  CHECK(sv_session_replay_load(&rp, path, &a, 1, err, sizeof err) != 0 && strstr(err, "no end marker") != NULL, "unfinished recording is refused even with --allow-variant");
  (void)sv_session_record_end(&rec, 6);

  /* capacity: ended with an explicit marker, never truncated silently */
  CHECK(sv_session_record_open(&rec, path, &a, err, sizeof err) == 0, "open cap");
  rec.max_events = 2U;
  held[0] = 1; held[1] = 0;
  CHECK(sv_session_record_pads(&rec, 1, held) == SV_SESSION_RECORD_OK, "event 1");
  held[0] = 2;
  CHECK(sv_session_record_pads(&rec, 2, held) == SV_SESSION_RECORD_OK, "event 2");
  held[0] = 3;
  CHECK(sv_session_record_pads(&rec, 3, held) == SV_SESSION_RECORD_CAPACITY && strstr(rec.message, "capacity was reached") != NULL && strstr(rec.message, "frame 3") != NULL, "capacity is reported");
  CHECK(!sv_session_record_active(&rec), "recording stopped");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) == 0 && rp.count == 2U && rp.end_frame == 3U, "the capped recording is complete and replayable");
  sv_session_replay_free(&rp);

  /* malformed input */
  write_text(path, "not a session\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "not a segarecomp session recording") != NULL, "bad header");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 11\nvariant vanilla\ndigest machine-0001\nend 1\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "malformed image hash") != NULL, "bad hash");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\ndigest machine-0001\n5 1 7\n4 1 0\nend 9\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "goes back in time") != NULL, "events out of order");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\ndigest machine-0001\n5 3 7\nend 9\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "malformed") != NULL, "bad player");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\ndigest machine-0001\n   \n5 1 256\nend 9\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "malformed") != NULL, "mask out of range");


  /* checkpoints: strictly increasing, within the end marker, never skipped, all compared by the end */
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\ndigest machine-0001\n"
                   "checkpoint 600 abababababababababababababababababababababababababababababababab\ncheckpoint 300 abababababababababababababababababababababababababababababababab\nend 900\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "checkpoint out of order") != NULL, "out-of-order checkpoints are refused, not skipped");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\ndigest machine-0001\n"
                   "checkpoint 600 abababababababababababababababababababababababababababababababab\ncheckpoint 600 abababababababababababababababababababababababababababababababab\nend 900\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "out of order") != NULL, "a duplicate checkpoint frame is refused");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\ndigest machine-0001\n"
                   "checkpoint 1200 abababababababababababababababababababababababababababababababab\nend 900\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "beyond the end marker") != NULL, "a checkpoint beyond the end marker is refused");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\nend 900\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) != 0 && strstr(err, "header is incomplete") != NULL, "a recording without a digest profile line is refused");
  write_text(path, SV_SESSION_FORMAT "\nplatform genesis\nimage 1111111111111111111111111111111111111111111111111111111111111111\nvariant vanilla\ndigest machine-0001\n"
                   "checkpoint 600 abababababababababababababababababababababababababababababababab\ncheckpoint 1200 abababababababababababababababababababababababababababababababab\nend 1200\n");
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) == 0, "two ordered checkpoints load");
  CHECK(sv_session_replay_checkpoint(&rp, 600, digest, err, sizeof err) == 0, "first compared");
  CHECK(sv_session_replay_checkpoint(&rp, 1201, digest, err, sizeof err) == 1 && strstr(err, "without comparing") != NULL, "passing a checkpoint without comparing it is an error");
  sv_session_replay_free(&rp);
  CHECK(sv_session_replay_load(&rp, path, &a, 0, err, sizeof err) == 0, "reload");
  CHECK(sv_session_replay_checkpoints_complete(&rp, err, sizeof err) == 1 && strstr(err, "frame 600") != NULL, "a replay that ends before its checkpoints were compared is reported");
  sv_session_replay_free(&rp);
  /* a different digest profile: loads, but checkpoints are informational and the mismatch is reported */
  {
    SvSessionIdentity basic = a;
    strcpy(basic.digest_profile, "basic");
    CHECK(sv_session_replay_load(&rp, path, &basic, 0, err, sizeof err) == 0 && rp.variant_mismatch && strstr(rp.note, "machine digest") != NULL, "another digest profile: not enforced, and said so");
    sv_session_replay_free(&rp);
  }
#ifdef __linux__
  /* a checkpoint that cannot be written ends the recording with an error (no end marker, so replay refuses the file) */
  CHECK(sv_session_record_open(&rec, path, &a, err, sizeof err) == 0, "open for write failure");
  fclose(rec.file);
  rec.file = fopen("/dev/full", "wb");
  if (rec.file != NULL) {
    CHECK(sv_session_record_checkpoint(&rec, 600, digest) == SV_SESSION_RECORD_ERROR, "checkpoint write failure is reported");
    CHECK(!sv_session_record_active(&rec) && strstr(rec.message, "checkpoint could not be written") != NULL, "and the recording is over, with the reason");
  }
#endif

  /* privacy: a recording holds the header hashes, masks and frames only */
  CHECK(sv_session_record_open(&rec, path, &a, err, sizeof err) == 0, "open privacy");
  held[0] = 0x80; held[1] = 0;
  (void)sv_session_record_pads(&rec, 1, held);
  (void)sv_session_record_end(&rec, 2);
  {
    FILE *f = fopen(path, "rb");
    char text[512];
    size_t n = fread(text, 1, sizeof text - 1, f), i;
    int ok = 1;
    fclose(f);
    text[n] = '\0';
    for (i = 0; i < n; ++i) if ((unsigned char)text[i] > 0x7E || ((unsigned char)text[i] < 0x20 && text[i] != '\n')) ok = 0;
    CHECK(ok && n < 300U, "the file is short printable text with no payload bytes");
  }
  remove(path);
  if (failures == 0) puts("sv_session_test: ok");
  return failures == 0 ? 0 : 1;
}
