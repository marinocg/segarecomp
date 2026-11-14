/* SEG-139-T004: the pure geometry declaration parser, the range/coverage decision and the stable JSON report (no SDL, no host). Strict C11. */
#include <stdio.h>
#include <string.h>

#include "sv_layout.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)
static SvDisplayRatio R(int n, int d) { SvDisplayRatio r; r.num = n; r.den = d; return r; }

int main(void) {
  SvGeomDecl d;
  SvCovAccount acc, acc_t;
  char json[40960], tiny[16];
  size_t need;
  int i;

  /* no declaration: nothing is claimed and nothing is restricted */
  CHECK(sv_geomdecl_parse(NULL, &d) == 0 && !d.present && sv_geomdecl_in_range(&d, R(32, 9)), "NULL text = no declaration, every ratio allowed");
  CHECK(sv_geomdecl_parse("", &d) == 0 && !d.present, "empty text = no declaration");
  CHECK(sv_geomdecl_check(&d, R(32, 9), "any", SV_COV_SCENERY, 1) == SV_COV_UNDECLARED, "delivered without a declaration is undeclared, never support");
  CHECK(sv_geomdecl_check(&d, R(32, 9), NULL, SV_COV_HUD, 0) == SV_COV_ABSENT, "nothing declared, nothing delivered");

  /* a declared 16:9 maximum */
  CHECK(sv_geomdecl_parse("max=16:9", &d) == 0 && d.present && !d.invalid && d.has_range && d.min.num == 4 && d.min.den == 3 && d.max.num == 16 && d.max.den == 9, "max alone: min defaults to 4:3");
  CHECK(sv_geomdecl_in_range(&d, R(4, 3)) && sv_geomdecl_in_range(&d, R(16, 9)) && sv_geomdecl_in_range(&d, R(400, 225)), "4:3 and 16:9 (also as 400:225) are inside");
  CHECK(!sv_geomdecl_in_range(&d, R(21, 9)) && !sv_geomdecl_in_range(&d, R(32, 9)) && !sv_geomdecl_in_range(&d, R(161, 90)), "21:9, 32:9 and a hair above 16:9 are outside (exact arithmetic)");
  CHECK(sv_geomdecl_check(&d, R(32, 9), NULL, SV_COV_SCENERY, 1) == SV_COV_OUTSIDE_RANGE, "16:9 declared, shown at 32:9: outside declared range");
  CHECK(strcmp(sv_cov_status_name(SV_COV_OUTSIDE_RANGE), "outside declared range") == 0, "the stable status text");

  /* scenes and components */
  CHECK(sv_geomdecl_parse("min=16:9;max=21:9;scene=boss:actors;scene=stage-1:hud+scenery", &d) == 0 && d.scene_count == 2, "two scenes");
  CHECK(!sv_geomdecl_in_range(&d, R(4, 3)) && sv_geomdecl_in_range(&d, R(21, 9)), "min is honoured");
  CHECK(sv_geomdecl_check(&d, R(16, 9), "stage-1", SV_COV_SCENERY, 1) == SV_COV_COVERED, "declared and delivered = covered");
  CHECK(sv_geomdecl_check(&d, R(16, 9), "stage-1", SV_COV_HUD, 0) == SV_COV_MISSING, "declared but not delivered = missing");
  CHECK(sv_geomdecl_check(&d, R(16, 9), "stage-1", SV_COV_ACTORS, 1) == SV_COV_UNDECLARED, "delivered but not declared (background coverage is not gameplay support)");
  CHECK(sv_geomdecl_check(&d, R(16, 9), "boss", SV_COV_SCENERY, 0) == SV_COV_ABSENT, "neither");
  CHECK(sv_geomdecl_check(&d, R(16, 9), "unknown-scene", SV_COV_SCENERY, 1) == SV_COV_UNDECLARED, "an undeclared scene claims nothing");
  CHECK(sv_geomdecl_check(&d, R(16, 9), NULL, SV_COV_SCENERY, 1) == SV_COV_UNDECLARED, "unscoped claims nothing");

  /* malformed text fails closed */
  {
    static const char *bad[] = {"min=16:9", "scene=a:hud", "max=16:9;min=4:3", "max=5:4", "max=33:9", "max=0:9", "max=16:9;max=16:9", "min=21:9;max=16:9",
                                "max=16:9;scene=a", "max=16:9;scene=a:", "max=16:9;scene=a:hud+hud", "max=16:9;scene=a:sky", "max=16:9;scene=B:hud", "max=16:9;scene=b:hud;scene=a:hud",
                                "max=16:9;scene=a:hud;scene=a:hud", "max=16:9;", "max=16:9;;", "garbage", "max=16:9 ", "max=16:9;scene=a:hud;extra=1", "max=16:9;scene=a:hud+",
                                "max=0x10:9", "max=-16:9", "MAX=16:9"};
    for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); ++i) {
      CHECK(sv_geomdecl_parse(bad[i], &d) == -1 && d.present && d.invalid, bad[i]);
      CHECK(!sv_geomdecl_in_range(&d, R(16, 9)) && sv_geomdecl_check(&d, R(16, 9), NULL, SV_COV_SCENERY, 1) == SV_COV_OUTSIDE_RANGE, "an invalid declaration puts every frame outside");
    }
    {
      char longtext[2100];
      memset(longtext, 'a', sizeof longtext - 1u); longtext[sizeof longtext - 1u] = '\0';
      CHECK(sv_geomdecl_parse(longtext, &d) == -1 && d.invalid, "an over-long text is rejected");
    }
    {   /* the longest LEGAL declaration (what the schema allows: min, max, 32 scenes with 32-character ids and all three components) must parse */
      char big[2200] = "min=4:3;max=32:9";
      int k;
      for (k = 0; k < SV_COV_MAX_SCENES; ++k) {
        char one[80];
        snprintf(one, sizeof one, ";scene=%c%c%030d:actors+hud+scenery", 'a' + k / 10, 'a' + k % 10, 0);
        strcat(big, one);
      }
      CHECK(strlen(big) > 1024 && strlen(big) <= 1884 + 4, "the maximal declaration is longer than the old 1024 limit");
      CHECK(sv_geomdecl_parse(big, &d) == 0 && d.scene_count == SV_COV_MAX_SCENES && d.scenes[31].declared == (SV_COV_BIT(0) | SV_COV_BIT(1) | SV_COV_BIT(2)), "the maximal legal declaration parses");
    }
    {
      char many[SV_COV_MAX_SCENES * 24 + 64] = "max=16:9";
      int k;
      for (k = 0; k <= SV_COV_MAX_SCENES; ++k) { char one[24]; snprintf(one, sizeof one, ";scene=s%02d:hud", k); strcat(many, one); }
      CHECK(sv_geomdecl_parse(many, &d) == -1, "more than the maximum number of scenes is rejected");
    }
  }

  /* accounting and the stable JSON */
  (void)sv_geomdecl_parse("max=16:9;scene=stage-1:hud+scenery", &d);
  sv_cov_init(&acc, &d);
  CHECK(sv_cov_frame(&acc, R(16, 9), "stage-1", SV_COV_BIT(SV_COV_SCENERY) | SV_COV_BIT(SV_COV_HUD)) == 1, "an in-range frame");
  CHECK(sv_cov_frame(&acc, R(16, 9), "stage-1", SV_COV_BIT(SV_COV_SCENERY) | SV_COV_BIT(SV_COV_ACTORS)) == 1, "an in-range frame with missing hud and undeclared actors");
  CHECK(sv_cov_frame(&acc, R(32, 9), "stage-1", SV_COV_BIT(SV_COV_SCENERY)) == 0, "an outside frame");
  CHECK(sv_cov_frame(&acc, R(16, 9), "nope", SV_COV_BIT(SV_COV_HUD)) == 1, "an unknown scene goes to the unscoped bucket");
  need = sv_cov_report_json(&acc, json, sizeof json);
  CHECK(need > 0 && need < sizeof json && strlen(json) == need, "the report fits and its length is exact");
  CHECK(strcmp(json,
      "{\"schema\":\"segarecomp.geometry-coverage/1\",\"declared\":true,\"aspect_min\":\"4:3\",\"aspect_max\":\"16:9\",\"frames\":4,\"outside_range_frames\":1,\"last_aspect\":\"16:9\",\"last_status\":\"inside declared range\","
      "\"last_reason\":\"undeclared\",\"last_vsprite_reason\":0,\"reason_frames\":{\"unverified_state\":0,\"unsupported_mode\":0,\"over_declared_geometry\":1,\"undeclared\":2,\"virtual_sprite_rejected\":0,\"virtual_sprite_unpresented\":0,\"invalid_surface\":0},\"scenes\":["
      "{\"id\":\"stage-1\",\"components\":{"
      "\"scenery\":{\"declared\":true,\"covered\":2,\"missing\":0,\"undeclared\":0,\"outside_range\":1,\"requested\":2,\"offered\":3,\"accepted\":3,\"presented\":2,\"rejected\":0},"
      "\"hud\":{\"declared\":true,\"covered\":1,\"missing\":1,\"undeclared\":0,\"outside_range\":1,\"requested\":2,\"offered\":1,\"accepted\":1,\"presented\":1,\"rejected\":0},"
      "\"actors\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":1,\"outside_range\":1,\"requested\":0,\"offered\":1,\"accepted\":1,\"presented\":1,\"rejected\":0}}},"
      "{\"id\":\"*\",\"components\":{"
      "\"scenery\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":0,\"outside_range\":0,\"requested\":0,\"offered\":0,\"accepted\":0,\"presented\":0,\"rejected\":0},"
      "\"hud\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":1,\"outside_range\":0,\"requested\":0,\"offered\":1,\"accepted\":1,\"presented\":1,\"rejected\":0},"
      "\"actors\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":0,\"outside_range\":0,\"requested\":0,\"offered\":0,\"accepted\":0,\"presented\":0,\"rejected\":0}}}]}") == 0, "the exact stable JSON shape");
  /* snprintf-like truncation and determinism */
  CHECK(sv_cov_report_json(&acc, tiny, sizeof tiny) == need && strlen(tiny) == sizeof tiny - 1u, "a short buffer is truncated and the needed length returned");
  CHECK(sv_cov_report_json(&acc, NULL, 0) == need, "a NULL buffer measures");
  { char again[40960]; (void)sv_cov_report_json(&acc, again, sizeof again); CHECK(strcmp(again, json) == 0, "the report is deterministic"); }
  /* empty account: the same shape */
  sv_cov_init(&acc, NULL);
  (void)sv_cov_report_json(&acc, json, sizeof json);
  CHECK(strstr(json, "\"declared\":false") != NULL && strstr(json, "\"last_status\":\"no frames\"") != NULL && strstr(json, "{\"id\":\"*\"") != NULL, "an empty undeclared account keeps the shape");
  (void)sv_cov_frame(&acc, R(32, 9), NULL, SV_COV_BIT(SV_COV_SCENERY));
  (void)sv_cov_report_json(&acc, json, sizeof json);
  CHECK(acc.last_reason == SV_REJ_NONE && acc.reason_frames[SV_REJ_UNDECLARED] == 0, "no declaration: undeclared delivery is counted but never a rejection reason (existing packages unchanged)");
  CHECK(strstr(json, "\"last_status\":\"no declaration\"") != NULL && strstr(json, "\"undeclared\":1") != NULL && strstr(json, "\"outside_range_frames\":0") != NULL, "no declaration: counted as undeclared, never outside");
  /* SEG-140-T005: the actor signal. Only a COMPOSED set with a known provenance (and a verified snapshot for game-state art) is a delivery; every earlier stage is evidence of nothing. */
  {
    SvExtendedSurface e;
    unsigned off, acc, why;
    const unsigned A = SV_COV_BIT(SV_COV_ACTORS);
    static const struct { int stage, prov, state; unsigned off, acc, why; } T[] = {
      {SV_ACT_NONE, 0, 0, 0, 0, 0},
      {SV_ACT_SUBMITTED, 2, 0, 1, 0, 1u << SV_REJ_VSPRITE_UNPRESENTED},
      {SV_ACT_VALIDATED, 2, 1, 1, 0, 1u << SV_REJ_VSPRITE_UNPRESENTED},
      {SV_ACT_REJECTED, 2, 1, 1, 0, 1u << SV_REJ_VSPRITE_REJECTED},
      {SV_ACT_COMPOSED, 2, 0, 1, 1, 0},
      {SV_ACT_COMPOSED, 1, 1, 1, 1, 0},
      {SV_ACT_COMPOSED, 1, 0, 1, 0, 1u << SV_REJ_UNVERIFIED_STATE},
      {SV_ACT_COMPOSED, 0, 1, 1, 0, 1u << SV_REJ_UNSUPPORTED_MODE},
      {SV_ACT_COMPOSED, 3, 1, 1, 0, 1u << SV_REJ_UNSUPPORTED_MODE},
      {99, 2, 1, 1, 0, 1u << SV_REJ_UNSUPPORTED_MODE},
    };
    for (i = 0; i < (int)(sizeof T / sizeof T[0]); ++i) {
      memset(&e, 0, sizeof e);
      e.actor_stage = (uint8_t)T[i].stage; e.actor_provenance = (uint8_t)T[i].prov; e.actor_state_verified = (uint8_t)T[i].state;
      sv_actor_signal(&e, &off, &acc, &why);
      CHECK(off == (T[i].off ? A : 0u) && acc == (T[i].acc ? A : 0u) && why == T[i].why, "the actor signal credits only a composed set with its required provenance gate");
    }
    sv_actor_signal(NULL, &off, &acc, &why);
    CHECK(off == 0 && acc == 0 && why == 0, "no surface, no actor signal");
  }
  /* accepted is a subset of offered; a refused frame records reasons without a frame or a counter; a frame with no reason clears the last reason */
  (void)sv_geomdecl_parse("max=16:9;scene=s:actors+hud", &d);
  sv_cov_init(&acc_t, &d);
  CHECK(sv_cov_frame_ex(&acc_t, R(16, 9), "s", 0u, SV_COV_BIT(SV_COV_HUD), 0u, 0u) == 1 && acc_t.count[0][SV_COV_HUD][6] == 0 && acc_t.count[0][SV_COV_HUD][1] == 1, "accepted without offered is nothing");
  sv_cov_reject(&acc_t, 1u << SV_REJ_UNVERIFIED_STATE);
  CHECK(acc_t.frames == 1 && acc_t.last_reason == SV_REJ_UNVERIFIED_STATE && acc_t.reason_frames[SV_REJ_UNVERIFIED_STATE] == 1, "a refused frame records a reason only");
  (void)sv_cov_frame_ex(&acc_t, R(16, 9), "s", SV_COV_BIT(SV_COV_HUD), SV_COV_BIT(SV_COV_HUD), 0u, 0u);
  CHECK(acc_t.last_reason == SV_REJ_NONE && acc_t.count[0][SV_COV_HUD][0] == 1, "a clean frame clears the last reason");
  CHECK(strcmp(sv_reject_reason_name(SV_REJ_OVER_DECLARED), "over_declared_geometry") == 0 && strcmp(sv_reject_reason_name(SV_REJ_COUNT), "none") == 0, "stable reason names");
  /* the longest legal declaration with saturated counters still fits the viewer's 36,864-byte report buffer */
  {
    static char big[2100], rep[40960];
    int k, c, n;
    SvCovAccount full;
    strcpy(big, "max=32:9");
    for (k = 0; k < SV_COV_MAX_SCENES; ++k) { char one[64]; snprintf(one, sizeof one, ";scene=s%030d:actors+hud+scenery", k); strcat(big, one); }
    CHECK(sv_geomdecl_parse(big, &d) == 0, "the longest legal declaration parses");
    sv_cov_init(&full, &d);
    for (k = 0; k <= SV_COV_MAX_SCENES; ++k) for (c = 0; c < SV_COV_COMPONENTS; ++c) for (n = 0; n < SV_COV_COUNTERS; ++n) full.count[k][c][n] = UINT64_MAX;
    for (k = 1; k < SV_REJ_COUNT; ++k) full.reason_frames[k] = UINT64_MAX;
    full.frames = full.outside_frames = UINT64_MAX; full.last = R(32, 9);
    need = sv_cov_report_json(&full, rep, sizeof rep);
    CHECK(need < 36864u && strlen(rep) == need, "the worst-case report fits the viewer buffer");
  }
  printf(failures == 0 ? "sv_coverage_test: all checks passed\n" : "sv_coverage_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
