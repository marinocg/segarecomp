/* SEG-139-T004: the pure geometry declaration parser, the range/coverage decision and the stable JSON report (no SDL, no host). Strict C11. */
#include <stdio.h>
#include <string.h>

#include "sv_layout.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)
static SvDisplayRatio R(int n, int d) { SvDisplayRatio r; r.num = n; r.den = d; return r; }

int main(void) {
  SvGeomDecl d;
  SvCovAccount acc;
  char json[4096], tiny[16];
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
      "{\"schema\":\"segarecomp.geometry-coverage/1\",\"declared\":true,\"aspect_min\":\"4:3\",\"aspect_max\":\"16:9\",\"frames\":4,\"outside_range_frames\":1,\"last_aspect\":\"16:9\",\"last_status\":\"inside declared range\",\"scenes\":["
      "{\"id\":\"stage-1\",\"components\":{"
      "\"scenery\":{\"declared\":true,\"covered\":2,\"missing\":0,\"undeclared\":0,\"outside_range\":1},"
      "\"hud\":{\"declared\":true,\"covered\":1,\"missing\":1,\"undeclared\":0,\"outside_range\":1},"
      "\"actors\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":1,\"outside_range\":1}}},"
      "{\"id\":\"*\",\"components\":{"
      "\"scenery\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":0,\"outside_range\":0},"
      "\"hud\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":1,\"outside_range\":0},"
      "\"actors\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":0,\"outside_range\":0}}}]}") == 0, "the exact stable JSON shape");
  /* snprintf-like truncation and determinism */
  CHECK(sv_cov_report_json(&acc, tiny, sizeof tiny) == need && strlen(tiny) == sizeof tiny - 1u, "a short buffer is truncated and the needed length returned");
  CHECK(sv_cov_report_json(&acc, NULL, 0) == need, "a NULL buffer measures");
  { char again[4096]; (void)sv_cov_report_json(&acc, again, sizeof again); CHECK(strcmp(again, json) == 0, "the report is deterministic"); }
  /* empty account: the same shape */
  sv_cov_init(&acc, NULL);
  (void)sv_cov_report_json(&acc, json, sizeof json);
  CHECK(strstr(json, "\"declared\":false") != NULL && strstr(json, "\"last_status\":\"no frames\"") != NULL && strstr(json, "{\"id\":\"*\"") != NULL, "an empty undeclared account keeps the shape");
  (void)sv_cov_frame(&acc, R(32, 9), NULL, SV_COV_BIT(SV_COV_SCENERY));
  (void)sv_cov_report_json(&acc, json, sizeof json);
  CHECK(strstr(json, "\"last_status\":\"no declaration\"") != NULL && strstr(json, "\"undeclared\":1") != NULL && strstr(json, "\"outside_range_frames\":0") != NULL, "no declaration: counted as undeclared, never outside");
  printf(failures == 0 ? "sv_coverage_test: all checks passed\n" : "sv_coverage_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
