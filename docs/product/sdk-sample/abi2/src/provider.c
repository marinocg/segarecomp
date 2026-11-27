/*
 * Sample Segarecomp Genesis native provider, ABI 2 (a SYNTHETIC "game": project-authored, no commercial content). It is the ABI 2 counterpart of ../../provider.c (ABI 1) and the worked example of
 * docs/product/native-abi-migration.md, which maps every ABI 1 facility to the ABI 2 call used here. Strict C11 against platforms/genesis/sdk/segarecomp_mod_genesis.h only.
 * Like the ABI 1 sample it declines every frame whose state it cannot verify, answers a solid pattern 1 stripe on plane B (plane A blank) beside the picture, and draws a small white HUD square.
 */
#include <segarecomp_mod_genesis.h>

#define MAGIC_OFFSET 0x0102U /* work RAM: the byte that identifies the one screen this sample accepts */
#define MAGIC 0x5AU
#define MAGIC_WORD 0x5A00U   /* the same byte as the high half of a big-endian word */
#define WINDOW_COLS 128U     /* one window's widest allowed width; two per plane */
#define WINDOW_ROWS 64U
#define WINDOW_WORDS 4096U
#define HUD_SIDE 8U

/* ESSENTIAL cross-frame state: it is maintained at `frame-end`, which runs for every guest frame whether or not a frame is presented, and saved through state.mod. An ABI 1 provider that updated a
 * static in `prepare` must move it here (or prove it is a cache whose absence never changes the output). */
static uint32_t frames_on_screen;

static uint16_t stripe[WINDOW_WORDS], blank[WINDOW_WORDS];
static uint8_t hud_rgba[HUD_SIDE * HUD_SIDE * 4U];
static GenesisVirtualSpriteSet sprites;

static int32_t floor_cell(int32_t pixels) { return pixels >= 0 ? pixels / 8 : -((7 - pixels) / 8); }

static int provider_init(void *state, const SegaModHost *host) {
  uint32_t i;
  (void)state;
  for (i = 0U; i < WINDOW_WORDS; ++i) stripe[i] = 1U; /* pattern 1, palette line 0, no flips */
  for (i = 0U; i < HUD_SIDE * HUD_SIDE * 4U; ++i) hud_rgba[i] = 255U;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

/* ABI 1 `prepare`: verify every input the frame will use. */
static uint32_t verify(const SegaGenesisFrameView *frame) {
  if (frame->work_ram == NULL || frame->lines == NULL || frame->line_count == 0U || (frame->flags & SEGA_GEN_FRAME_VRAM_FRAME_START) == 0U) return SEGA_GEN_REASON_BAD_STATE;
  if (frame->work_ram[MAGIC_OFFSET] != MAGIC) return SEGA_GEN_REASON_UNSUPPORTED_SCENE; /* not the screen this provider was written for */
  return 0U;
}

/* ABI 1 `scenery_word`: instead of answering cells one at a time, submit a window of name-table words in unwrapped plane-cell coordinates that covers every cell the host may look up. */
static int submit_scenery(const SegaGenesisFeatures *features, const SegaGenesisRenderApi *render, const SegaGenesisFrameView *frame) {
  uint32_t plane;
  for (plane = 0U; plane < 2U; ++plane) {
    int32_t low_x = 0, high_x = 0, low_y = 0, high_y = 0, x0, y0;
    uint32_t line, cols, rows, first;
    for (line = 0U; line < frame->line_count; ++line) {
      int32_t h = 0, v = 0;
      /* The ABI 1 GenesisCompatHost helpers, with the same arguments and results. */
      if (features->vdp->vscroll_is_per_column(frame->lines[line].registers) != 0) return 1;
      if (features->vdp->read_hscroll(frame->vram, frame->lines[line].registers, plane, line, &h) != 0) return 1;
      if (features->vdp->read_vscroll(frame->lines[line].vsram, frame->lines[line].registers, plane, 0U, &v) != 0) return 1;
      if (line == 0U || h < low_x) low_x = h;
      if (line == 0U || h > high_x) high_x = h;
      if (line == 0U || v < low_y) low_y = v;
      if (line == 0U || v > high_y) high_y = v;
    }
    x0 = floor_cell(-(int32_t)(2U * frame->side) - high_x); /* wide enough for any view placement: the whole planned total may lie on one side */
    cols = (uint32_t)(floor_cell((int32_t)(frame->width + 2U * frame->side) - low_x) - x0) + 1U;
    y0 = floor_cell(low_y);
    rows = (uint32_t)(floor_cell((int32_t)frame->height + high_y) - y0) + 1U;
    if (cols > 2U * WINDOW_COLS || rows > WINDOW_ROWS) return 1;
    first = cols > WINDOW_COLS ? WINDOW_COLS : cols;
    if (first * rows > WINDOW_WORDS || (cols - first) * rows > WINDOW_WORDS) return 1;
    (void)render->scenery_window(plane, x0, y0, first, rows, plane == 1U ? stripe : blank);
    if (cols > first) (void)render->scenery_window(plane, x0 + (int32_t)first, y0, cols - first, rows, plane == 1U ? stripe : blank);
  }
  return 0;
}

/* Optional scenery facilities, shown with data that leaves this sample's output unchanged. */
static void submit_row_facilities(const SegaGenesisRenderApi *render) {
  static const uint8_t no_generic_rows[28] = {0U};
  static const int16_t no_origin_delta[224] = {0};
  /* ABI 1: scenery_word answered 1 with the VDP's own word on some lines. ABI 2: set those screen rows' bits. */
  (void)render->scenery_generic_rows(0U, no_generic_rows, 224U, 28U);
  /* ABI 1: scenery_word used a world origin that depends on the line. ABI 2: a signed whole-cell delta per screen row. */
  (void)render->scenery_row_origin(1U, no_origin_delta, 224U, 448U);
}

/* ABI 1 `hud`: the overlay, its fallback position, a positioned layer with a clip, and virtual sprites. */
static void submit_hud(const SegaGenesisRenderApi *render) {
  (void)render->overlay_target(0U);
  (void)render->overlay_bitmap(0, 0, HUD_SIDE, HUD_SIDE, hud_rgba, HUD_SIDE * 4U); /* out->rgba, out->width, out->height */
  (void)render->overlay_fallback(4U, 4U);                                          /* out->fallback_x, out->fallback_y */
  (void)render->overlay_layer_bitmap(1U, 2U /* top_right */, 0U /* picture */, -8, 8, HUD_SIDE, HUD_SIDE, 1U); /* GenesisCompatLayer */
  (void)render->overlay_target(1U);
  (void)render->overlay_bitmap(0, 0, HUD_SIDE, HUD_SIDE, hud_rgba, HUD_SIDE * 4U);
  (void)render->overlay_layer_clip(1U, 0U, 0U, HUD_SIDE, HUD_SIDE / 2U); /* has_clip and clip_* */
  sprites.count = 0U;                                                    /* out->vsprites: fill the same GenesisVirtualSpriteSet type, then submit it once */
  (void)render->virtual_sprites(&sprites);
}

static void render_frame(const SegaGenesisFeatures *features) {
  const SegaGenesisRenderApi *render = features->render;
  const SegaGenesisFrameView *frame = features->frame;
  const uint32_t reason = verify(frame);
  if (reason != 0U) { /* accepted = 0 with out->reason: a refusal, never a fault */
    (void)render->decline(SEGA_GEN_COMPONENT_HUD | SEGA_GEN_COMPONENT_SCENERY | SEGA_GEN_COMPONENT_SPRITES, reason);
    return;
  }
  (void)render->scene("main", 4U); /* decision->scene */
  if ((frame->requested & SEGA_GEN_COMPONENT_SCENERY) != 0U && frame->side != 0U) {
    if (submit_scenery(features, render, frame) != 0) (void)render->decline(SEGA_GEN_COMPONENT_SCENERY, SEGA_GEN_REASON_BAD_SCROLL);
    else submit_row_facilities(render);
  }
  /* decision->left_px + has_left_px: here the centred placement; windows and placement use the same margin. */
  if (frame->side != 0U) (void)render->view_left(frame->side, frame->side);
  if ((frame->requested & SEGA_GEN_COMPONENT_HUD) != 0U) submit_hud(render);
}

static void provider_phase(void *state, const SegaModHost *host) {
  (void)state;
  if (host->phase == SEGA_MOD_PHASE_RESET) {
    frames_on_screen = 0U;
  } else if (host->phase == SEGA_MOD_PHASE_FRAME_END) {
    uint32_t value = 0U;
    if (host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, MAGIC_OFFSET, 2U, &value) == SEGA_MOD_OK)
      frames_on_screen = (value & 0xFF00U) == MAGIC_WORD ? frames_on_screen + 1U : 0U;
  } else if (host->phase == SEGA_MOD_PHASE_RENDER) {
    const SegaGenesisFeatures *features = (const SegaGenesisFeatures *)host->platform_features;
    /* A host reports its own table sizes; read the appended members only when they are covered. Requiring frame.vdp makes `render` the exact-frame callback. */
    if (features == NULL || features->size < sizeof(SegaGenesisFeatures) || features->render == NULL || features->frame == NULL || features->vdp == NULL) return;
    if (features->render->size < sizeof(SegaGenesisRenderApi)) return;
    render_frame(features);
  }
}

static int provider_save(void *state, uint8_t *out, uint32_t cap, uint32_t *len) {
  uint32_t i;
  (void)state;
  if (cap < 4U) return 1;
  for (i = 0U; i < 4U; ++i) out[i] = (uint8_t)(frames_on_screen >> (24U - 8U * i));
  *len = 4U;
  return 0;
}

static int provider_load(void *state, const uint8_t *in, uint32_t len) {
  (void)state;
  if (len != 4U) return 1;
  frames_on_screen = ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) | ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
  return 0;
}

/* required_minor = 3 matches abi.minor in mod.toml (scenery_row_origin). */
static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 3U, SEGA_MOD_PLATFORM, "sample.synthetic.abi2", 1U, provider_init, provider_phase, NULL, provider_save, provider_load,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

/* ABI 1: segarecomp_patch_provider. */
const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = &frames_on_screen;
  return &provider;
}
