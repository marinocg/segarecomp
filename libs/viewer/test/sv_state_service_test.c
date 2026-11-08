/* SEG-132 (ADR 0279): the platform-neutral save-state service against a FAKE machine (a counter and a guest clock), with no SDL and no console: the six slot files
 * (quick + 1..5), atomic replacement, damaged and foreign files, unavailability, and the bounded in-memory rewind history (guest-time cadence, age and byte eviction,
 * the retained-span note, branch truncation on a restore, cancel changes nothing, no disk writes, a latched stop is skipped, a failing capture turns rewind off).
 * Strict C11 like the code under test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sv_state_service.h"
#include "sv_state_store.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define DIR "sv_state_service_test_tmp"
#define BLOB 4096U

typedef struct Fake {
  uint32_t counter;
  uint64_t ms;
  uint8_t identity;
  int skip_capture, fail_capture, restores, after_restore_calls;
} Fake;

/* container: "FAKE" id counter ms thumb(8x8 rgb565) + padding to BLOB (so budgets are easy to reason about) */
static int f_capture(void *ctx, uint8_t **data, uint32_t *size, char *why, size_t cap) {
  Fake *f = (Fake *)ctx;
  uint8_t *b;
  if (f->skip_capture) return SV_CAPTURE_SKIP;
  if (f->fail_capture) { snprintf(why, cap, "the fake machine broke"); return SV_CAPTURE_FAILED; }
  b = (uint8_t *)calloc(1U, BLOB);
  memcpy(b, "FAKE", 4U);
  b[4] = f->identity;
  memcpy(b + 8, &f->counter, 4U);
  memcpy(b + 16, &f->ms, 8U);
  b[24] = 8; b[25] = 8;
  memset(b + 32, (int)(f->counter & 0xFFU), 128U);
  *data = b;
  *size = BLOB;
  return SV_CAPTURE_OK;
}
static int f_peek(void *ctx, const uint8_t *d, size_t n, SvStateInfo *info) {
  (void)ctx;
  if (n != BLOB || memcmp(d, "FAKE", 4U) != 0) return -1;
  memset(info, 0, sizeof *info);
  memcpy(&info->guest_ms, d + 16, 8U);
  info->thumb_w = 8; info->thumb_h = 8; info->thumb = d + 32;
  return 0;
}
static int f_validate(void *ctx, const uint8_t *d, size_t n, SvStateInfo *info, char *why, size_t cap) {
  Fake *f = (Fake *)ctx;
  SvStateInfo local;
  if (f_peek(ctx, d, n, info != NULL ? info : &local) != 0) { snprintf(why, cap, "the state file is corrupt"); return -1; }
  if (d[4] != f->identity) { snprintf(why, cap, "the state belongs to a different game or version"); return -1; }
  return 0;
}
static int f_restore(void *ctx, const uint8_t *d, size_t n, char *why, size_t cap) {
  Fake *f = (Fake *)ctx;
  if (f_validate(ctx, d, n, NULL, why, cap) != 0) return -1;
  memcpy(&f->counter, d + 8, 4U);
  memcpy(&f->ms, d + 16, 8U);
  ++f->restores;
  return 0;
}
static void f_after(void *ctx) { ++((Fake *)ctx)->after_restore_calls; }
static uint64_t f_ms(void *ctx) { return ((Fake *)ctx)->ms; }

static SvStateService *make(Fake *f, const char *dir, const char *reason, uint32_t budget, uint32_t interval, uint32_t target) {
  SvStateServiceConfig c;
  memset(&c, 0, sizeof c);
  c.ops.ctx = f; c.ops.capture = f_capture; c.ops.peek = f_peek; c.ops.validate = f_validate; c.ops.restore = f_restore; c.ops.after_restore = f_after; c.ops.guest_ms = f_ms;
  c.state_dir = dir; c.unavailable_reason = reason; c.max_file_bytes = 1U << 20;
  c.rewind_budget_bytes = budget; c.rewind_interval_ms = interval; c.rewind_target_ms = target;
  return sv_state_service_create(&c);
}

static void advance(SvStateService *s, Fake *f, uint64_t ms_step, int steps) {
  int i;
  for (i = 0; i < steps; ++i) { f->ms += ms_step; ++f->counter; sv_state_service_after_slice(s); }
}

static void cleanup(void) {
  char path[256];
  int i;
  const char *names[] = {"quick.sgstate", "slot-1.sgstate", "slot-2.sgstate", "slot-3.sgstate", "slot-4.sgstate", "slot-5.sgstate", "quick.sgstate.next", "quick.sgstate.prev"};
  for (i = 0; i < 8; ++i) { snprintf(path, sizeof path, "%s/states/%s", DIR, names[i]); (void)remove(path); }
}

static void slots(void) {
  Fake f;
  SvStateService *s;
  SvStateProvider p;
  char err[160];
  int i;
  SvStateSlotInfo info;
  memset(&f, 0, sizeof f);
  f.identity = 7;
  cleanup();
  s = make(&f, DIR "/states", NULL, 0U, 0U, 0U);
  sv_state_service_provider(s, &p);
  CHECK(p.states_available(p.ctx, err, sizeof err) == 1, "slots are available with a save folder");
  for (i = 0; i < SV_STATE_SLOTS; ++i) {
    f.counter = 100U + (uint32_t)i; f.ms = 1000U * (uint64_t)(i + 1);
    CHECK(p.save_slot(p.ctx, i, err, sizeof err) == 0, "save into each of the six slots (quick + 1..5)");
  }
  CHECK(p.save_slot(p.ctx, 6, err, sizeof err) != 0 && p.save_slot(p.ctx, -1, err, sizeof err) != 0, "slots outside 0..5 are refused");
  for (i = 0; i < SV_STATE_SLOTS; ++i) {
    memset(&info, 0, sizeof info);
    CHECK(p.slot_info(p.ctx, i, &info) == 0 && info.occupied && !info.damaged && info.guest_ms == 1000U * (uint64_t)(i + 1) && info.thumb.w == 8, "slot info: occupied, guest time and the thumbnail of the snapshot itself");
    CHECK(info.thumb.rgb565 != NULL && info.thumb.rgb565[0] == (uint8_t)(100 + i), "the thumbnail is the slot's own");
  }
  for (i = 0; i < SV_STATE_SLOTS; ++i) {
    f.counter = 0; f.ms = 0;
    CHECK(p.load_slot(p.ctx, i, err, sizeof err) == 0 && f.counter == 100U + (uint32_t)i, "load each slot restores its own state");
  }
  CHECK(f.after_restore_calls == SV_STATE_SLOTS, "after_restore ran after every successful load");
  /* a foreign state is refused and the machine is unchanged */
  f.identity = 9; f.counter = 5;
  CHECK(p.load_slot(p.ctx, 1, err, sizeof err) != 0 && f.counter == 5U && strstr(err, "different") != NULL, "a state of another game is refused with its reason, the machine unchanged");
  memset(&info, 0, sizeof info);
  CHECK(p.slot_info(p.ctx, 1, &info) == 0 && !info.occupied && info.damaged && info.note[0] != '\0', "the foreign slot is shown damaged with a note, not loadable");
  f.identity = 7;
  /* a corrupt file */
  {
    FILE *fp = fopen(DIR "/states/slot-2.sgstate", "r+b");
    CHECK(fp != NULL, "open slot 2");
    if (fp) { fseek(fp, 0, SEEK_SET); fputc('X', fp); fclose(fp); }
  }
  f.counter = 6;
  CHECK(p.load_slot(p.ctx, 2, err, sizeof err) != 0 && f.counter == 6U, "a corrupt file is refused and the machine is unchanged");
  CHECK(p.load_slot(p.ctx, 0, err, sizeof err) == 0, "other slots still load");
  /* an empty slot */
  cleanup();
  CHECK(p.load_slot(p.ctx, 3, err, sizeof err) != 0 && strstr(err, "empty") != NULL, "an empty slot says so");
  CHECK(p.save_slot(p.ctx, 0, err, sizeof err) == 0 && p.save_slot(p.ctx, 0, err, sizeof err) == 0, "a slot can be overwritten");
  {
    FILE *fp = fopen(DIR "/states/quick.sgstate.next", "rb");
    CHECK(fp == NULL, "no candidate file is left behind by the atomic replacement");
    if (fp) fclose(fp);
  }
  sv_state_service_destroy(s);
  cleanup();
  /* unavailable: every state feature says why, nothing is written */
  s = make(&f, NULL, "No save location is configured.", 0U, 0U, 0U);
  sv_state_service_provider(s, &p);
  CHECK(p.states_available(p.ctx, err, sizeof err) == 0 && strstr(err, "save location") != NULL, "unavailable reason is reported");
  CHECK(p.save_slot(p.ctx, 0, err, sizeof err) != 0 && p.load_slot(p.ctx, 0, err, sizeof err) != 0, "no save and no load when unavailable");
  sv_state_service_destroy(s);
  /* a file where the folder must be: the save fails and is reported, never claimed */
  {
    FILE *fp = fopen(DIR "/blocker", "wb");
    if (fp) { fputs("x", fp); fclose(fp); }
    s = make(&f, DIR "/blocker/states", NULL, 0U, 0U, 0U);
    sv_state_service_provider(s, &p);
    CHECK(p.save_slot(p.ctx, 0, err, sizeof err) != 0, "a save that cannot be written reports failure");
    sv_state_service_destroy(s);
    (void)remove(DIR "/blocker");
  }
}

static void history_tests(void) {
  Fake f;
  SvStateService *s;
  SvStateProvider p;
  SvStateServiceStats st;
  SvRewindEntry e;
  char err[160];
  int n, i;
  memset(&f, 0, sizeof f);
  f.identity = 1;
  /* cadence: one moment per 500 ms of GUEST time, whatever the slice length */
  s = make(&f, NULL, "no folder", 8U * 1024U * 1024U, 500U, 60000U);
  sv_state_service_provider(s, &p);
  CHECK(p.rewind_available(p.ctx, err, sizeof err) == 1, "rewind works without a save folder (it is in memory)");
  advance(s, &f, 100, 4);
  CHECK(p.history_count(p.ctx) == 0, "no moment before a full interval passed");
  advance(s, &f, 100, 1);
  CHECK(p.history_count(p.ctx) == 1, "the first moment at 500 ms");
  advance(s, &f, 100, 20);
  CHECK(p.history_count(p.ctx) == 5, "one moment per 500 ms (5 after 2.5 s)");
  CHECK(p.history_info(p.ctx, 0, &e) == 0 && e.age_ms == 2000 && e.thumb.w == 8, "oldest first, ages in guest ms, thumbnail present");
  CHECK(p.history_info(p.ctx, 4, &e) == 0 && e.age_ms == 0, "the newest moment is the present");
  CHECK(p.history_info(p.ctx, 5, &e) != 0, "no entry beyond the last");
  /* cancel changes nothing */
  {
    const uint32_t counter = f.counter;
    const uint64_t ms = f.ms;
    p.rewind_opened(p.ctx);
    p.rewind_cancelled(p.ctx);
    CHECK(f.counter == counter && f.ms == ms && f.restores == 0 && p.history_count(p.ctx) == 5, "browsing and cancelling mutates nothing");
  }
  /* restore truncates the newer moments: a new branch */
  CHECK(p.restore_entry(p.ctx, 2, err, sizeof err) == 0 && f.restores == 1 && f.after_restore_calls == 1, "restore a moment");
  CHECK(p.history_count(p.ctx) == 3, "no moment newer than the chosen one survives");
  advance(s, &f, 100, 4);
  CHECK(p.history_count(p.ctx) == 3, "the cadence restarts from the restored time");
  advance(s, &f, 100, 1);
  CHECK(p.history_count(p.ctx) == 4, "the branch grows again");
  CHECK(p.restore_entry(p.ctx, 99, err, sizeof err) != 0, "a vanished moment is refused");
  sv_state_service_stats(s, &st);
  CHECK(st.captures >= 6 && st.restores == 1 && st.last_capture_bytes == BLOB && st.history_entries == 4, "statistics are exact counts");
  sv_state_service_destroy(s);

  /* age eviction: target 3 s keeps at most ~7 moments of 500 ms */
  memset(&f, 0, sizeof f);
  f.identity = 1;
  s = make(&f, NULL, NULL, 8U * 1024U * 1024U, 500U, 3000U);
  sv_state_service_provider(s, &p);
  advance(s, &f, 500, 40);
  n = p.history_count(p.ctx);
  CHECK(n >= 6 && n <= 7, "age eviction bounds the history to the target");
  CHECK(p.history_info(p.ctx, 0, &e) == 0 && e.age_ms <= 3500, "the oldest retained moment is within the target");
  CHECK(p.history_note == NULL || p.history_note(p.ctx, err, sizeof err) == 0, "age eviction is not a memory shortage: no note");
  sv_state_service_destroy(s);

  /* byte-budget eviction: a budget for 10 containers, a 60 s target: the real span is reported */
  memset(&f, 0, sizeof f);
  f.identity = 1;
  s = make(&f, NULL, NULL, 400U * 1024U, 500U, 60000U);   /* 400 KiB = 100 containers = 50 s of 500 ms moments: memory, not age, limits the span */
  sv_state_service_provider(s, &p);
  advance(s, &f, 500, 400);
  sv_state_service_stats(s, &st);
  CHECK(st.history_entries <= 100U && st.history_bytes <= 400U * 1024U, "the byte budget bounds the history");
  CHECK(st.evictions > 0 && p.history_note(p.ctx, err, sizeof err) == 1 && strstr(err, "Memory limit") != NULL, "the retained span is reported honestly when memory limits it");
  sv_state_service_destroy(s);

  /* a container that does not fit the budget at all turns rewind off, the game is unaffected */
  memset(&f, 0, sizeof f);
  s = make(&f, NULL, NULL, 1024U, 100U, 60000U);
  sv_state_service_provider(s, &p);
  advance(s, &f, 100, 3);
  CHECK(p.rewind_available(p.ctx, err, sizeof err) == 0 && strstr(err, "memory") != NULL && p.history_count(p.ctx) == 0, "an over-budget moment makes rewind unavailable with a reason");
  sv_state_service_destroy(s);

  /* a latched stop is skipped quietly; a failing capture turns rewind off with the reason */
  memset(&f, 0, sizeof f);
  s = make(&f, NULL, NULL, 1U << 20, 100U, 60000U);
  sv_state_service_provider(s, &p);
  f.skip_capture = 1;
  advance(s, &f, 100, 3);
  CHECK(p.history_count(p.ctx) == 0 && p.rewind_available(p.ctx, err, sizeof err) == 1, "a latched stop is skipped without disabling rewind");
  f.skip_capture = 0;
  advance(s, &f, 100, 2);
  CHECK(p.history_count(p.ctx) >= 1, "capturing resumes");
  f.fail_capture = 1;
  advance(s, &f, 100, 2);
  CHECK(p.rewind_available(p.ctx, err, sizeof err) == 0 && strstr(err, "broke") != NULL && p.history_count(p.ctx) == 0, "a failing capture turns rewind off and says why");
  sv_state_service_destroy(s);

  /* rewind disabled by policy, and by a session reason */
  memset(&f, 0, sizeof f);
  s = make(&f, NULL, NULL, 0U, 100U, 60000U);
  sv_state_service_provider(s, &p);
  advance(s, &f, 100, 5);
  CHECK(p.history_count(p.ctx) == 0 && p.rewind_available(p.ctx, err, sizeof err) == 0, "a zero budget is rewind off");
  sv_state_service_destroy(s);

  /* loading a slot starts a fresh history; rewind never writes the disk */
  memset(&f, 0, sizeof f);
  f.identity = 3;
  cleanup();
  s = make(&f, DIR "/states", NULL, 1U << 20, 100U, 60000U);
  sv_state_service_provider(s, &p);
  advance(s, &f, 100, 6);
  CHECK(p.history_count(p.ctx) >= 5, "history exists");
  for (i = 0; i < SV_STATE_SLOTS; ++i) {
    char name[96];
    FILE *fp;
    snprintf(name, sizeof name, DIR "/states/%s", i == 0 ? "quick.sgstate" : (i == 1 ? "slot-1.sgstate" : "slot-2.sgstate"));
    fp = fopen(name, "rb");
    if (i < 3) CHECK(fp == NULL, "nothing was written by the rewind history");
    if (fp) fclose(fp);
  }
  CHECK(p.save_slot(p.ctx, 1, err, sizeof err) == 0, "manual save works beside rewind");
  advance(s, &f, 100, 3);
  CHECK(p.load_slot(p.ctx, 1, err, sizeof err) == 0 && p.history_count(p.ctx) == 0, "loading a manual slot starts a fresh history");
  sv_state_service_destroy(s);
  cleanup();
}

static void thumb(void) {
  int w, h;
  uint8_t px[2];
  sv_state_thumb_size(256, 192, &w, &h);
  CHECK(w == 149 && h == 111, "256x192 fits 160x112 keeping the aspect");
  sv_state_thumb_size(320, 224, &w, &h);
  CHECK(w == 160 && h == 112, "320x224 -> 160x112");
  sv_state_thumb_size(0, 10, &w, &h);
  CHECK(w == 0 && h == 0, "an empty source has no thumbnail");
  sv_state_put_rgb565(px, 255, 255, 255);
  CHECK(px[0] == 0xFF && px[1] == 0xFF, "white");
  sv_state_put_rgb565(px, 255, 0, 0);
  CHECK(px[0] == 0x00 && px[1] == 0xF8, "red is little-endian 0xF800");
}

int main(void) {
  (void)sv_store_ensure_dir(DIR);
  slots();
  history_tests();
  thumb();
  if (failures != 0) { printf("sv_state_service_test: %d failure(s)\n", failures); return 1; }
  printf("sv_state_service_test: ok\n");
  return 0;
}
