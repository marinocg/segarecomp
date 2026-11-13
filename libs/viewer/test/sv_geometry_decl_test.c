/* SEG-139-T004: the host compares a package's declared geometry with what a frame delivered. A provider that declares 16:9 and is shown at 32:9 reports
 * "outside declared range", is presented through the generic path (not as authored, no relocated HUD) with a visible diagnostic, and the coverage report stays
 * truthful per scene and component. Dummy video driver, software renderer, no ROM. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "../src/sv_internal.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define W 320
#define H 224
static char g_settings[700];
static uint8_t g_native[W * H * 3], g_ext[(W + 2 * 238) * H * 3], g_ovl[8 * 8 * 4];
static const int SIDE = 238; /* the 32:9 side of 320 x 224: produced once, the widest request */

static SvDisplayRatio R(int n, int d) { SvDisplayRatio r; r.num = n; r.den = d; return r; }
static SvHost *open_host(const char *decl) {
  SvHostConfig cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS; cfg.title = "decl test"; cfg.picture_w = W; cfg.picture_h = H; cfg.max_w = 320; cfg.max_h = 448; cfg.default_scale = 3;
  cfg.geometry_declaration = decl;
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", g_settings, 1);
  return sv_host_open(&cfg);
}
static void set(SvHost *h, SvDisplayRatio ratio, int width) {
  SvSettings s = *sv_host_settings(h);
  s.video.widescreen = SV_WIDESCREEN_ENHANCED_GAMEPLAY; s.video.widescreen_side_style = SV_SIDE_COMFORT; s.video.widescreen_width = width; s.video.widescreen_ratio = ratio;
  s.video.aspect = SV_ASPECT_ORIGINAL;
  sv_host_apply_settings(h, &s);
}
static int present(SvHost *h, uint64_t seq, int with_overlay) {
  SvExtendedSurface e;
  memset(&e, 0, sizeof e);
  e.rgb24 = g_ext; e.size = sizeof g_ext; e.width = W + 2 * SIDE; e.height = H; e.pitch = (W + 2 * SIDE) * 3; e.side = SIDE; e.sequence = seq;
  e.mode = SV_WIDESCREEN_ENHANCED_GAMEPLAY; e.status = SV_WS_PARTIAL; e.source = SV_EXTSRC_AUTHORED;
  if (with_overlay) { e.overlay_rgba = g_ovl; e.overlay_w = 8; e.overlay_h = 8; }
  return sv_host_present_extended(h, g_native, W, H, W * 3, seq, &e);
}
static int toast_has(const SvHost *h, const char *needle) {
  int i;
  for (i = 0; i < h->toast_count; ++i) if (strstr(h->toasts[i].text, needle) != NULL) return 1;
  return 0;
}
static char *json(const SvHost *h) { static char buf[4096]; (void)sv_host_coverage_json(h, buf, sizeof buf); return buf; }

int main(void) {
  char dir[512];
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : (SDL_getenv("TEMP") ? SDL_getenv("TEMP") : "/tmp");
  SvHost *h;
  uint64_t seq = 100;
  size_t i;
  snprintf(dir, sizeof dir, "%s/sv_geometry_decl_test_%lld_%d", tmp, (long long)time(NULL), (int)SDL_GetTicks());
  SDL_CreateDirectory(dir);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
  snprintf(g_settings, sizeof g_settings, "%s/d1.cfg", dir);
  for (i = 0; i < sizeof g_native; ++i) g_native[i] = (uint8_t)(i * 7u);
  for (i = 0; i < sizeof g_ext; ++i) g_ext[i] = (uint8_t)(i * 13u + 5u);
  for (i = 0; i < sizeof g_ovl; ++i) g_ovl[i] = 255;

  /* a provider declaring 16:9 */
  h = open_host("max=16:9;scene=stage-1:hud+scenery");
  CHECK(h != NULL, "host opens");
  if (h == NULL) return 1;
  sv_host_set_scene(h, "stage-1");
  set(h, R(16, 9), 100);
  CHECK(present(h, ++seq, 1) == 0, "frame at 16:9");
  CHECK(!sv_host_geometry_fallback(h) && sv_host_side_treatment(h) == SV_TREAT_PASSTHROUGH && sv_host_extension_source(h) == SV_EXTSRC_AUTHORED && h->ovl_w == 8, "inside the range: authored scenery and the relocated HUD are shown as supplied");
  CHECK(strstr(json(h), "\"last_status\":\"inside declared range\"") != NULL && strstr(json(h), "\"scenery\":{\"declared\":true,\"covered\":1,") != NULL && strstr(json(h), "\"hud\":{\"declared\":true,\"covered\":1,") != NULL, "covered scenery and hud are counted");
  CHECK(strstr(json(h), "\"actors\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":1,") != NULL, "Level 3 actors delivered but never declared are not support");

  set(h, R(32, 9), 100);
  sv_host_toast(h, "");
  CHECK(present(h, ++seq, 1) == 0, "frame at 32:9");
  CHECK(sv_host_geometry_fallback(h), "shown at 32:9: outside the declared range");
  CHECK(sv_host_side_treatment(h) == SV_TREAT_COMFORT && sv_host_extension_source(h) == SV_EXTSRC_GENERIC && h->ovl_w == 0, "fallback: the generic presentation, nothing authored, no relocated HUD");
  CHECK(sv_host_extension_side(h) == SIDE, "the extension itself is still shown (generic), not removed");
  CHECK(toast_has(h, "outside declared range"), "a visible diagnostic names the reason");
  CHECK(strstr(json(h), "\"last_status\":\"outside declared range\"") != NULL && strstr(json(h), "\"outside_range_frames\":1") != NULL && strstr(json(h), "\"last_aspect\":\"32:9\"") != NULL, "the report says outside declared range");
  CHECK(strstr(json(h), "\"scenery\":{\"declared\":true,\"covered\":1,\"missing\":0,\"undeclared\":0,\"outside_range\":1}") != NULL, "the outside frame is not counted as covered");

  sv_host_toast(h, "");
  CHECK(present(h, ++seq, 1) == 0 && h->toast_count == 0, "the diagnostic is raised on entering the state, not on every frame");
  set(h, R(16, 9), 100);
  CHECK(present(h, ++seq, 1) == 0 && !sv_host_geometry_fallback(h) && sv_host_side_treatment(h) == SV_TREAT_PASSTHROUGH && h->ovl_w == 8, "back inside: authored again");
  /* a missing component is reported, an undeclared scene claims nothing */
  CHECK(present(h, ++seq, 0) == 0 && strstr(json(h), "\"hud\":{\"declared\":true,\"covered\":2,\"missing\":1,") != NULL, "a declared but undelivered component is missing");
  sv_host_set_scene(h, "no-such-scene");
  CHECK(present(h, ++seq, 1) == 0 && strstr(json(h), "{\"id\":\"*\",\"components\":{\"scenery\":{\"declared\":false,\"covered\":0,\"missing\":0,\"undeclared\":1,") != NULL, "an undeclared scene is accounted as unscoped, undeclared");
  /* legacy widths map to the exact rational (300+width)/225: 16:9 at 100 is inside, the same frame with a 4:3 declaration is outside */
  sv_host_close(h);
  remove(g_settings);

  h = open_host("max=4:3");
  set(h, R(0, 0), 100);
  CHECK(present(h, ++seq, 0) == 0 && sv_host_geometry_fallback(h), "the legacy width 100 (16:9) against a 4:3 declaration is outside");
  set(h, R(0, 0), 0);
  CHECK(present(h, ++seq, 0) == 0 && !sv_host_geometry_fallback(h), "the legacy width 0 (4:3) is inside");
  sv_host_close(h);
  remove(g_settings);

  /* an unreadable declaration fails closed; no declaration restricts nothing */
  h = open_host("max=16:9;scene=Bad Id:hud");
  set(h, R(16, 9), 100);
  CHECK(present(h, ++seq, 1) == 0 && sv_host_geometry_fallback(h) && h->ovl_w == 0 && toast_has(h, "invalid") && sv_host_extension_source(h) == SV_EXTSRC_GENERIC, "an invalid declaration never shows provider content as authored");
  CHECK(strstr(json(h), "\"declared\":false") != NULL, "an invalid declaration is not reported as declared");
  sv_host_close(h);
  remove(g_settings);
  h = open_host(NULL);
  set(h, R(32, 9), 100);
  CHECK(present(h, ++seq, 1) == 0 && !sv_host_geometry_fallback(h) && sv_host_extension_source(h) == SV_EXTSRC_AUTHORED && h->ovl_w == 8, "no declaration: behaviour unchanged");
  CHECK(strstr(json(h), "\"declared\":false") != NULL && strstr(json(h), "\"last_status\":\"no declaration\"") != NULL && strstr(json(h), "\"outside_range_frames\":0") != NULL, "no declaration is reported as such");
  sv_host_close(h);
  remove(g_settings);
  printf(failures == 0 ? "sv_geometry_decl_test: all checks passed\n" : "sv_geometry_decl_test: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
