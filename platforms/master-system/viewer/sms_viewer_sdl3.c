#include "sms_viewer_sdl3.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"

/* Audio latency policy: at most ~100 ms (4,410 samples) may be queued in the SDL stream. A chunk that would exceed it is
 * dropped (overrun, counted); when the device drains faster than the viewer produces, SDL plays silence (underrun).
 * Neither direction feeds back into guest time or state: the stream is fed from the PCM the guest already produced. */
#define AUDIO_QUEUE_LIMIT_BYTES (4410 * 2)

struct SmsSdl3Viewer {
  SvHost *host; /* the shared viewer host (libs/viewer): window, input, settings, pause overlay, presenter */
  SDL_AudioStream *audio;
  uint64_t overruns;
  uint64_t frames;
};

static uint64_t sdl_now(void *ctx) { (void)ctx; return SDL_GetTicksNS(); }
static void sdl_sleep(void *ctx, uint64_t ns) { (void)ctx; SDL_DelayNS(ns); }

/* Console mapping of the shared action masks: BUTTON1/2 are SMS buttons 1/2; PAUSE is the console's hardware Pause button (an NMI edge on
 * a press), which is distinct from the host pause menu (Esc / gamepad Back). */
static uint8_t pad_of(uint16_t actions) {
  SmsViewerKeys k;
  k.up = (actions & SV_ACTION_BIT(SV_ACT_UP)) != 0; k.down = (actions & SV_ACTION_BIT(SV_ACT_DOWN)) != 0;
  k.left = (actions & SV_ACTION_BIT(SV_ACT_LEFT)) != 0; k.right = (actions & SV_ACTION_BIT(SV_ACT_RIGHT)) != 0;
  k.button1 = (actions & SV_ACTION_BIT(SV_ACT_BUTTON1)) != 0; k.button2 = (actions & SV_ACTION_BIT(SV_ACT_BUTTON2)) != 0;
  return sms_viewer_pad_from_keys(&k);
}

/* Once per guest frame, before the frame's input is recorded: the pause overlay (if opened) blocks here, so the guest does not advance. */
static int sdl_poll(void *ctx, SmsViewerInput *in) {
  SmsSdl3Viewer *v = (SmsSdl3Viewer *)ctx;
  SvSample s;
  sv_host_service(v->host, &s);
  in->p1 = pad_of(s.pad[0]);
  in->p2 = pad_of(s.pad[1]);
  in->pause = (uint8_t)((s.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE)) != 0);
  in->reset = (uint8_t)s.reset;
  in->quit = (uint8_t)s.quit;
  return 0;
}

static int sdl_present(void *ctx, const uint8_t *fb, uint32_t height) {
  SmsSdl3Viewer *v = (SmsSdl3Viewer *)ctx;
  static uint8_t rgb[SMS_FB_WIDTH * SMS_FB_MAX_HEIGHT * 3];
  uint32_t i;
  if (height == 0u || height > SMS_FB_MAX_HEIGHT) return -1;
  for (i = 0; i < SMS_FB_WIDTH * height; ++i) {
    const uint32_t c = sms_render_rgb888(fb[i]);
    rgb[i * 3u] = (uint8_t)(c >> 16); rgb[i * 3u + 1u] = (uint8_t)(c >> 8); rgb[i * 3u + 2u] = (uint8_t)c;
  }
  v->frames++;
  /* The picture is 256 x 192 or 256 x 224 lines; the shared presenter fits the active height with the right aspect. */
  return sv_host_present(v->host, rgb, (int)SMS_FB_WIDTH, (int)height, (int)SMS_FB_WIDTH * 3);
}

static int sdl_audio(void *ctx, const int16_t *samples, uint32_t count) {
  SmsSdl3Viewer *v = (SmsSdl3Viewer *)ctx;
  if (v->audio == NULL) return -1;
  if (SDL_GetAudioStreamQueued(v->audio) > AUDIO_QUEUE_LIMIT_BYTES) { v->overruns++; return -1; }
  return SDL_PutAudioStreamData(v->audio, samples, (int)(count * 2u)) ? 0 : -1;
}

static void sdl_diagnostics(void *ctx, SvDiagnostics *out) {
  const SmsSdl3Viewer *v = (const SmsSdl3Viewer *)ctx;
  out->audio_active = v->audio != NULL;
  if (v->audio != NULL) {
    const int bytes = SDL_GetAudioStreamQueued(v->audio);
    out->audio_queued_ms = bytes > 0 ? (uint32_t)((uint64_t)bytes * 1000u / (SMS_VIEWER_PCM_RATE * 2u)) : 0u;
  }
  out->audio_dropped = v->overruns;
  out->guest_frames = v->frames;
}

SmsSdl3Viewer *sms_sdl3_viewer_open(const char *title, int scale, int want_audio) {
  SmsSdl3Viewer *v;
  SvHostConfig cfg;
  const char *sha = SDL_getenv("SEGARECOMP_ROM_SHA256");
  if (scale < 1) scale = 3;
  if (scale > INT_MAX / (int)SMS_FB_WIDTH || scale > INT_MAX / (int)SMS_FB_MAX_HEIGHT) return NULL;
  v = (SmsSdl3Viewer *)calloc(1, sizeof *v);
  if (v == NULL) return NULL;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_SMS;
  cfg.title = title ? title : "segarecomp";
  cfg.picture_w = (int)SMS_FB_WIDTH;
  cfg.picture_h = 192;
  cfg.max_w = (int)SMS_FB_WIDTH;
  cfg.max_h = (int)SMS_FB_MAX_HEIGHT;
  cfg.default_scale = scale;
  cfg.can_reset = 1; /* sms_viewer_rig_reset is the proven deterministic reset seam (it restarts the input recording) */
  cfg.rom_sha256 = sha != NULL && strlen(sha) == 64u ? sha : NULL;
  cfg.diagnostics = sdl_diagnostics;
  cfg.diagnostics_ctx = v;
  v->host = sv_host_open(&cfg);
  if (v->host == NULL) { free(v); return NULL; }
  if (want_audio && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16; spec.channels = 1; spec.freq = (int)SMS_VIEWER_PCM_RATE;
    v->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (v->audio != NULL) { SDL_ResumeAudioStreamDevice(v->audio); sv_host_set_audio(v->host, v->audio); } /* a failed open leaves the viewer silent */
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

void sms_sdl3_viewer_stop_screen(SmsSdl3Viewer *v, const char *title, const char *detail) {
  if (v != NULL) sv_host_stop_screen(v->host, title, detail);
}

void sms_sdl3_viewer_close(SmsSdl3Viewer *v) {
  if (v == NULL) return;
  if (v->audio) { sv_host_set_audio(v->host, NULL); SDL_DestroyAudioStream(v->audio); }
  sv_host_close(v->host); /* includes SDL_Quit */
  free(v);
}
