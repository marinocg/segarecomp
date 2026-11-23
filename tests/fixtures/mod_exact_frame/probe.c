/* SEG-152-T003/T004 (ADR 0320): the exact-frame probe, a FABRICATED native ABI 2 provider for the end-to-end producer test (tests/mod_exact_frame_abi2_test.py) on the mid-frame scenery fixture.
 * It requires frame.vdp, so its `render` runs at the producer seam with an immutable view of the frame being produced. Per callback it logs (host.log, one line, no stdio) digests of everything the view
 * carries and the horizontal scroll read through the host's own helper; it also DRAWS from the view, so the presented frame shows whose data it was built from:
 *   - HUD: a rect whose colour is a function of the frame number and the essential state;
 *   - scenery: windows for plane A/B built from the scroll of THIS frame and the placement (left = side / 2) declared for THIS frame's margin;
 *   - one authored virtual sprite whose x depends on the frame number.
 * State classes (ADR 0320 decision 3): `ess` (frame-end phases counted: guest time, essential, saved through state.mod) and `memo` (the last window words, a pure cache: cleared on load, output never depends on it). */
#include <segarecomp_mod_genesis.h>

#define COLS 112U
#define ROWS 28U

static uint32_t ess;                                  /* essential cross-frame state: frame-end phases seen */
static uint32_t memo_camera = 0xFFFFFFFFU, memo_left = 0xFFFFFFFFU; /* pure cache key */
static uint16_t words[COLS * ROWS], blank[COLS * ROWS];
static GenesisVirtualSpriteSet vs;

static uint64_t fnv(const void *data, uint32_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = UINT64_C(1469598103934665603);
  uint32_t i;
  for (i = 0U; i < n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}

static uint32_t put_str(char *o, uint32_t n, const char *s) {
  while (*s != '\0') o[n++] = *s++;
  return n;
}
static uint32_t put_dec(char *o, uint32_t n, int64_t v) {
  char rev[24];
  uint32_t k = 0U, d;
  uint64_t u = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
  if (v < 0) o[n++] = '-';
  do { rev[k++] = (char)('0' + u % 10U); u /= 10U; } while (u != 0U);
  for (d = 0U; d < k; ++d) o[n++] = rev[k - 1U - d];
  return n;
}
static uint32_t put_hex(char *o, uint32_t n, uint64_t v) {
  int i;
  for (i = 60; i >= 0; i -= 4) o[n++] = "0123456789abcdef"[(v >> (unsigned)i) & 15U];
  return n;
}
static uint32_t field(char *o, uint32_t n, const char *name, int64_t v) { n = put_str(o, n, name); n = put_dec(o, n, v); return put_str(o, n, " "); }
static uint32_t hexfield(char *o, uint32_t n, const char *name, uint64_t v) { n = put_str(o, n, name); n = put_hex(o, n, v); return put_str(o, n, " "); }

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static void render(const SegaModHost *host, const SegaGenesisFeatures *f) {
  const SegaGenesisFrameView *v = f->frame;
  const SegaGenesisRenderApi *render_api = f->render;
  char line[250];
  uint32_t n = 0U, camera = 0U, left = 0U, tile_word = 0U, c, r, hs_ok = 1U;
  int32_t hs = 0, x0, bx0 = -40;
  if (v == NULL) {   /* an ordinary (non exact-frame) render: it must never happen for this provider */
    n = put_str(line, 0U, "NOVIEW ");
    n = field(line, n, "f=", (int64_t)host->frame);
    host->log(line, n);
    return;
  }
  if (v->work_ram != NULL) camera = ((uint32_t)v->work_ram[0x12] << 8U) | v->work_ram[0x13];
  if (v->vram != NULL) tile_word = ((uint32_t)v->vram[0x800] << 8U) | v->vram[0x801];
  if (f->vdp != NULL && v->line_count > 0U) hs_ok = (uint32_t)f->vdp->read_hscroll(v->vram, v->lines[0].registers, 0U, 0U, &hs);
  n = put_str(line, 0U, "E ");
  n = field(line, n, "f=", (int64_t)host->frame);
  n = field(line, n, "s=", v->side);
  n = field(line, n, "fl=", v->flags);
  n = field(line, n, "lc=", v->line_count);
  n = field(line, n, "w=", v->width);
  n = field(line, n, "h=", v->height);
  n = field(line, n, "q=", v->requested);
  n = hexfield(line, n, "r=", v->work_ram != NULL ? fnv(v->work_ram, 65536U) : 0U);
  n = hexfield(line, n, "v=", fnv(v->vram, 65536U));
  n = hexfield(line, n, "l=", fnv(v->lines, v->line_count * (uint32_t)sizeof(SegaGenesisFrameLine)));
  n = hexfield(line, n, "a=", v->line_count > 50U ? fnv(&v->lines[50], (uint32_t)sizeof(SegaGenesisFrameLine)) : 0U);
  n = hexfield(line, n, "b=", v->line_count > 150U ? fnv(&v->lines[150], (uint32_t)sizeof(SegaGenesisFrameLine)) : 0U);
  n = field(line, n, "hs=", hs_ok == 0U ? hs : 99999);
  n = field(line, n, "cam=", camera);
  n = field(line, n, "t=", tile_word);
  n = field(line, n, "ess=", ess);
  host->log(line, n);
  if (render_api == NULL || hs_ok != 0U) return;
  /* the camera of THIS frame is its own scroll (the previous-onset RAM camera lags it by one frame): the windows are built from the view */
  camera = (uint32_t)(-hs) / 8U;
  left = v->side / 2U;
  x0 = (int32_t)camera - (int32_t)(left / 8U) - 3;
  bx0 = -(int32_t)(left / 8U) - 3;
  if (v->side > 0U && (memo_camera != camera || memo_left != left)) {   /* the cache: rebuilt when its key changes, output is the same warm or cold */
    for (r = 0U; r < ROWS; ++r)
      for (c = 0U; c < COLS; ++c) {
        const int32_t cell = x0 + (int32_t)c;
        const int inside = cell >= 24 && cell < 64;
        words[r * COLS + c] = inside ? ((cell & 1) == 0 ? 1U : 3U) : ((cell & 1) == 0 ? 0x2002U : 0x2003U);
      }
    memo_camera = camera;
    memo_left = left;
  }
  (void)render_api->scene("main", 4U);
  if (v->side > 0U) {
    (void)render_api->scenery_window(0U, x0, 0, COLS, ROWS, words);
    (void)render_api->scenery_window(1U, bx0, 0, COLS, ROWS, blank);
    (void)render_api->view_left(left, v->side);
  }
  (void)render_api->overlay_rect(0, 0, 24, 8, ((ess & 0xFFU) << 24) | (((uint32_t)host->frame & 0xFFU) << 16) | 0x0000FFU);   /* colour = essential state and the frame number */
  vs.count = 1U;
  vs.sprites[0].provenance = GENESIS_VSPRITE_MOD_AUTHORED;
  vs.sprites[0].layer = GENESIS_VSPRITE_LAYER_ABOVE_ALL;
  vs.sprites[0].width_cells = 1U;
  vs.sprites[0].height_cells = 1U;
  vs.sprites[0].palette = 0U;
  vs.sprites[0].tile_index = 1U;
  vs.sprites[0].x = (int16_t)(16U + ((uint32_t)host->frame % 16U) * 8U);   /* where the sprite is drawn names the frame it was built for */
  vs.sprites[0].y = 40;
  for (c = 0U; c < 32U; ++c) vs.authored_vram[32U + c] = 0x11U;
  vs.authored_cram[2] = 0x0EU;   /* palette 0 entry 1 = 0x0E0E (magenta: a colour the picture and the authored columns never use) */
  vs.authored_cram[3] = 0x0EU;
  (void)render_api->virtual_sprites(&vs);
}

static void provider_phase(void *state, const SegaModHost *host) {
  (void)state;
  if (host->phase == SEGA_MOD_PHASE_FRAME_END) ++ess;
  else if (host->phase == SEGA_MOD_PHASE_RENDER) render(host, (const SegaGenesisFeatures *)host->platform_features);
}

static int provider_save(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  (void)state;
  if (cap < 4U) return 1;
  out[0] = (uint8_t)(ess >> 24U); out[1] = (uint8_t)(ess >> 16U); out[2] = (uint8_t)(ess >> 8U); out[3] = (uint8_t)ess;
  *len = 4U;
  return 0;
}
static int provider_load(void *state, const uint8_t *in, uint32_t len) {
  (void)state;
  if (len != 4U) return 1;
  ess = ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) | ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
  memo_camera = memo_left = 0xFFFFFFFFU;   /* the cache is invalidated on restore, never saved */
  return 0;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 1U, SEGA_MOD_PLATFORM, "exact-frame-probe", 1U, provider_init, provider_phase, NULL, provider_save, provider_load,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = &ess;
  return &provider;
}
