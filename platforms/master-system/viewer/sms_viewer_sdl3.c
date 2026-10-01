#include "sms_viewer_sdl3.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>

/* Audio latency policy: at most ~100 ms (4,410 samples) may be queued in the SDL stream. A chunk that would exceed it is
 * dropped (overrun, counted); when the device drains faster than the viewer produces, SDL plays silence (underrun).
 * Neither direction feeds back into guest time or state: the stream is fed from the PCM the guest already produced. */
#define AUDIO_QUEUE_LIMIT_BYTES (4410 * 2)

struct SmsSdl3Viewer {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  SDL_AudioStream *audio;
  uint64_t overruns;
  uint32_t height; /* logical presentation height in effect */
  int closed, pause_latched, reset_latched;
};

static uint64_t sdl_now(void *ctx) { (void)ctx; return SDL_GetTicksNS(); }
static void sdl_sleep(void *ctx, uint64_t ns) { (void)ctx; SDL_DelayNS(ns); }

static int sdl_poll(void *ctx, SmsViewerInput *in) {
  SmsSdl3Viewer *v = (SmsSdl3Viewer *)ctx;
  SDL_Event e;
  const bool *ks;
  SmsViewerKeys k;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) v->closed = 1;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
      if (e.key.key == SDLK_ESCAPE) v->closed = 1;
      if (e.key.key == SDLK_P) v->pause_latched = 1; /* a tap shorter than a frame still presses pause for one frame */
      if (e.key.key == SDLK_R) v->reset_latched = 1;
    }
  }
  ks = SDL_GetKeyboardState(NULL);
  k.up = ks[SDL_SCANCODE_UP]; k.down = ks[SDL_SCANCODE_DOWN]; k.left = ks[SDL_SCANCODE_LEFT];
  k.right = ks[SDL_SCANCODE_RIGHT]; k.button1 = ks[SDL_SCANCODE_Z]; k.button2 = ks[SDL_SCANCODE_X];
  in->p1 = sms_viewer_pad_from_keys(&k);
  in->pause = (uint8_t)(v->pause_latched || ks[SDL_SCANCODE_P]);
  v->pause_latched = 0;
  in->reset = (uint8_t)v->reset_latched;
  v->reset_latched = 0;
  in->quit = (uint8_t)v->closed;
  return 0;
}

static int sdl_present(void *ctx, const uint8_t *fb, uint32_t height) {
  SmsSdl3Viewer *v = (SmsSdl3Viewer *)ctx;
  static uint8_t rgb[SMS_FB_WIDTH * SMS_FB_MAX_HEIGHT * 3];
  SDL_FRect src;
  uint32_t i;
  if (height == 0u || height > SMS_FB_MAX_HEIGHT) return -1;
  for (i = 0; i < SMS_FB_WIDTH * height; ++i) {
    const uint32_t c = sms_render_rgb888(fb[i]);
    rgb[i * 3u] = (uint8_t)(c >> 16); rgb[i * 3u + 1u] = (uint8_t)(c >> 8); rgb[i * 3u + 2u] = (uint8_t)c;
  }
  if (!SDL_UpdateTexture(v->texture, NULL, rgb, (int)SMS_FB_WIDTH * 3)) return -1;
  if (height != v->height) { /* aspect-preserving letterbox of the active area (192 or 224 lines) */
    if (!SDL_SetRenderLogicalPresentation(v->renderer, (int)SMS_FB_WIDTH, (int)height, SDL_LOGICAL_PRESENTATION_LETTERBOX)) return -1;
    v->height = height;
  }
  src.x = 0.0f; src.y = 0.0f; src.w = (float)SMS_FB_WIDTH; src.h = (float)height;
  if (!SDL_RenderClear(v->renderer)) return -1;
  if (!SDL_RenderTexture(v->renderer, v->texture, &src, NULL)) return -1;
  return SDL_RenderPresent(v->renderer) ? 0 : -1;
}

static int sdl_audio(void *ctx, const int16_t *samples, uint32_t count) {
  SmsSdl3Viewer *v = (SmsSdl3Viewer *)ctx;
  if (v->audio == NULL) return -1;
  if (SDL_GetAudioStreamQueued(v->audio) > AUDIO_QUEUE_LIMIT_BYTES) { v->overruns++; return -1; }
  return SDL_PutAudioStreamData(v->audio, samples, (int)(count * 2u)) ? 0 : -1;
}

SmsSdl3Viewer *sms_sdl3_viewer_open(const char *title, int scale, int want_audio) {
  SmsSdl3Viewer *v;
  if (scale < 1) scale = 3;
  if (scale > INT_MAX / (int)SMS_FB_WIDTH || scale > INT_MAX / (int)SMS_FB_MAX_HEIGHT) return NULL;
  v = (SmsSdl3Viewer *)calloc(1, sizeof *v);
  if (v == NULL) return NULL;
  if (!SDL_Init(SDL_INIT_VIDEO | (want_audio ? SDL_INIT_AUDIO : 0))) {
    if (!want_audio || !SDL_Init(SDL_INIT_VIDEO)) { free(v); return NULL; } /* no audio subsystem: silent viewer */
    want_audio = 0;
  }
  if (!SDL_CreateWindowAndRenderer(title ? title : "segarecomp", (int)SMS_FB_WIDTH * scale, (int)SMS_FB_MAX_HEIGHT * scale,
                                   SDL_WINDOW_RESIZABLE, &v->window, &v->renderer)) {
    SDL_Quit(); free(v); return NULL;
  }
  v->texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, (int)SMS_FB_WIDTH, (int)SMS_FB_MAX_HEIGHT);
  if (v->texture == NULL) { sms_sdl3_viewer_close(v); return NULL; }
  SDL_SetTextureScaleMode(v->texture, SDL_SCALEMODE_NEAREST);
  if (want_audio) {
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16; spec.channels = 1; spec.freq = (int)SMS_VIEWER_PCM_RATE;
    v->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (v->audio != NULL) SDL_ResumeAudioStreamDevice(v->audio); /* a failed open leaves the viewer silent */
  }
  return v;
}

void sms_sdl3_viewer_host(SmsSdl3Viewer *viewer, SmsViewerHost *host) {
  host->ctx = viewer;
  host->now_ns = sdl_now;
  host->sleep_ns = sdl_sleep;
  host->poll = sdl_poll;
  host->present = sdl_present;
  host->audio = viewer->audio != NULL ? sdl_audio : NULL; /* muted or no device: the core only drains the ring */
}

int sms_sdl3_viewer_audio_active(const SmsSdl3Viewer *v, uint64_t *overruns) {
  if (overruns != NULL) *overruns = v->overruns;
  return v->audio != NULL;
}

void sms_sdl3_viewer_close(SmsSdl3Viewer *v) {
  if (v == NULL) return;
  if (v->audio) SDL_DestroyAudioStream(v->audio);
  if (v->texture) SDL_DestroyTexture(v->texture);
  if (v->renderer) SDL_DestroyRenderer(v->renderer);
  if (v->window) SDL_DestroyWindow(v->window);
  SDL_Quit();
  free(v);
}
