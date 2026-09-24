#include "viewer_sdl3.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>

#include "vdp_render.h"

struct GenesisSdl3Viewer {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  int closed;
};

static uint64_t sdl_now(void *ctx) { (void)ctx; return SDL_GetTicksNS(); }
static void sdl_sleep(void *ctx, uint64_t ns) { (void)ctx; SDL_DelayNS(ns); }

static int sdl_closed(void *ctx) {
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)ctx;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) v->closed = 1;
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) v->closed = 1;
  }
  return v->closed;
}

static uint8_t sdl_pad1(void *ctx) {
  (void)ctx;
  const bool *ks = SDL_GetKeyboardState(NULL);
  GenesisViewerKeys k;
  k.up = ks[SDL_SCANCODE_UP]; k.down = ks[SDL_SCANCODE_DOWN];
  k.left = ks[SDL_SCANCODE_LEFT]; k.right = ks[SDL_SCANCODE_RIGHT];
  k.a = ks[SDL_SCANCODE_Z]; k.b = ks[SDL_SCANCODE_X]; k.c = ks[SDL_SCANCODE_C];
  k.start = ks[SDL_SCANCODE_RETURN];
  return genesis_viewer_pad_from_keys(&k);
}

static int sdl_present(void *ctx, const GenesisFrameArtifact *frame) {
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)ctx;
  static uint8_t rgb[GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT * 3];
  for (unsigned i = 0; i < GENESIS_FRAME_WIDTH * GENESIS_FRAME_HEIGHT; ++i) {
    GenesisRgb888 c;
    if (genesis_vdp_decode_cram_entry(frame->palette_snapshot, frame->pixels[i], &c) != 0) return -1;
    rgb[i * 3] = c.r; rgb[i * 3 + 1] = c.g; rgb[i * 3 + 2] = c.b;
  }
  if (!SDL_UpdateTexture(v->texture, NULL, rgb, GENESIS_FRAME_WIDTH * 3)) return -1;
  if (!SDL_RenderClear(v->renderer)) return -1;
  if (!SDL_RenderTexture(v->renderer, v->texture, NULL, NULL)) return -1;
  if (!SDL_RenderPresent(v->renderer)) return -1;
  return 0;
}

GenesisSdl3Viewer *genesis_sdl3_viewer_open(const char *title, int scale) {
  if (scale < 1) scale = 2;
  if (scale > INT_MAX / (int)GENESIS_FRAME_WIDTH || scale > INT_MAX / (int)GENESIS_FRAME_HEIGHT) return NULL;
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)calloc(1, sizeof(*v));
  if (v == NULL) return NULL;
  if (!SDL_Init(SDL_INIT_VIDEO)) { free(v); return NULL; }
  if (!SDL_CreateWindowAndRenderer(title ? title : "segarecomp", (int)GENESIS_FRAME_WIDTH * scale,
                                   (int)GENESIS_FRAME_HEIGHT * scale, 0, &v->window, &v->renderer)) {
    SDL_Quit(); free(v); return NULL;
  }
  v->texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                 (int)GENESIS_FRAME_WIDTH, (int)GENESIS_FRAME_HEIGHT);
  if (v->texture == NULL) { genesis_sdl3_viewer_close(v); return NULL; }
  SDL_SetTextureScaleMode(v->texture, SDL_SCALEMODE_NEAREST);
  return v;
}

void genesis_sdl3_viewer_host(GenesisSdl3Viewer *viewer, GenesisViewerHost *host) {
  host->ctx = viewer;
  host->now_ns = sdl_now;
  host->sleep_ns = sdl_sleep;
  host->present = sdl_present;
  host->window_closed = sdl_closed;
  host->pad1 = sdl_pad1;
}

void genesis_sdl3_viewer_close(GenesisSdl3Viewer *v) {
  if (v == NULL) return;
  if (v->texture) SDL_DestroyTexture(v->texture);
  if (v->renderer) SDL_DestroyRenderer(v->renderer);
  if (v->window) SDL_DestroyWindow(v->window);
  SDL_Quit();
  free(v);
}
