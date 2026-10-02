#include "viewer_sdl3.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_audio_format.h"
#include "vdp_render.h"

struct GenesisSdl3Viewer {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  SDL_AudioStream *audio;
  GenesisAudioSink sink;
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

/* SEG-032-T009 (ADR 0075): a one-way sink. Queue, overrun, underrun and mute policy lives in genesis_audio_present.c. */
static uint32_t sdl_audio_queued(void *ctx) {
  const GenesisSdl3Viewer *v = (const GenesisSdl3Viewer *)ctx;
  const int bytes = v->audio != NULL ? SDL_GetAudioStreamQueued(v->audio) : 0;
  return bytes > 0 ? (uint32_t)bytes / (GENESIS_MIXER_CHANNELS * 2U) : 0U;
}

static int sdl_audio_put(void *ctx, const int16_t *frames, uint32_t count) {
  const GenesisSdl3Viewer *v = (const GenesisSdl3Viewer *)ctx;
  if (v->audio == NULL || count > (uint32_t)INT_MAX / (GENESIS_MIXER_CHANNELS * 2U)) return -1;
  return SDL_PutAudioStreamData(v->audio, frames, (int)(count * GENESIS_MIXER_CHANNELS * 2U)) ? 0 : -1;
}

const GenesisAudioSink *genesis_sdl3_viewer_audio_open(GenesisSdl3Viewer *v) {
  SDL_AudioSpec spec;
  if (v == NULL || v->audio != NULL) return v != NULL && v->audio != NULL ? &v->sink : NULL;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return NULL;
  SDL_zero(spec);
  spec.format = SDL_AUDIO_S16LE;
  spec.channels = (int)GENESIS_MIXER_CHANNELS;
  spec.freq = (int)GENESIS_MIXER_RATE_HZ;
  v->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (v->audio == NULL) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return NULL;
  }
  SDL_ResumeAudioStreamDevice(v->audio);
  v->sink.context = v;
  v->sink.queued_frames = sdl_audio_queued;
  v->sink.put = sdl_audio_put;
  return &v->sink;
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
  memset(host, 0, sizeof(*host));  /* every optional callback defaults to NULL (after_slice is installed by the hook) */
  host->ctx = viewer;
  host->now_ns = sdl_now;
  host->sleep_ns = sdl_sleep;
  host->present = sdl_present;
  host->window_closed = sdl_closed;
  host->pad1 = sdl_pad1;
}

void genesis_sdl3_viewer_close(GenesisSdl3Viewer *v) {
  if (v == NULL) return;
  if (v->audio) SDL_DestroyAudioStream(v->audio);
  if (v->texture) SDL_DestroyTexture(v->texture);
  if (v->renderer) SDL_DestroyRenderer(v->renderer);
  if (v->window) SDL_DestroyWindow(v->window);
  SDL_Quit();
  free(v);
}
