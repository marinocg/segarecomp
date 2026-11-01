#include "sms_viewer_sdl3.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "sv_host.h"
#include "sv_layout.h"

/* Audio latency policy: at most ~100 ms (4,410 samples) may be queued in the SDL stream. A chunk that would exceed it is
 * dropped (overrun, counted); when the device drains faster than the viewer produces, SDL plays silence (underrun).
 * Neither direction feeds back into guest time or state: the stream is fed from the PCM the guest already produced. */
#define AUDIO_QUEUE_LIMIT_BYTES (4410 * 2)

struct SmsSdl3Viewer {
  SvHost *host; /* the shared viewer host (libs/viewer): window, input, settings, pause overlay, presenter */
  SDL_AudioStream *audio;
  uint64_t overruns;
  uint64_t frames;
  /* SEG-120 (ADR 0263): presentation-only Level 2. The target is adapter-owned and preallocated; the renderer fills it per line. */
  SmsExtension ext;
  const uint64_t *sequence; /* the renderer's frames_completed: the identity of the frame being presented */
  int ext_attached;
  SvAspect plan_aspect;     /* sampled with the requested mode once per guest frame */
  SvWidescreenSupport l2_support; /* the outcome of the latest frame produced under a request */
  SvWidescreenSupport l3_support; /* SEG-121: whether the latest Level 3 request produced the sprite spill (never Available) */
};

#define EXT_SURFACE_W (SMS_FB_WIDTH + 2u * SMS_EXT_MAX_SIDE)
static uint8_t g_ext_rgb[EXT_SURFACE_W * SMS_FB_MAX_HEIGHT * 3u];
static const char k_wrapped_note[] = "Background repeats existing scenery; this does not reveal more of the game world.";
static const char k_sprite_note[] = "Background repeats existing scenery. Only sprites crossing the screen edges can extend; this does not expand the game world.";

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
  if (v->ext_attached) { /* the requested mode is sampled here, between guest frames: a frame is produced wholly under one value */
    const SvVideoSettings *video = &sv_host_settings(v->host)->video;
    const int on = video->widescreen != SV_WIDESCREEN_ORIGINAL;
    v->ext.request_sprites = (uint8_t)(video->widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY);
    v->plan_aspect = video->aspect;
    v->ext.request_side_192 = on ? (uint32_t)sv_layout_extension_side((int)SMS_FB_WIDTH, 192, video->aspect) : 0u;
    v->ext.request_side_224 = on ? (uint32_t)sv_layout_extension_side((int)SMS_FB_WIDTH, 224, video->aspect) : 0u;
  }
  in->p1 = pad_of(s.pad[0]);
  in->p2 = pad_of(s.pad[1]);
  in->pause = (uint8_t)((s.pad[0] & SV_ACTION_BIT(SV_ACT_PAUSE)) != 0);
  in->reset = (uint8_t)s.reset;
  in->quit = (uint8_t)s.quit;
  return 0;
}

static void put_rgb(uint8_t *dst, uint8_t cram) {
  const uint32_t c = sms_render_rgb888(cram);
  dst[0] = (uint8_t)(c >> 16); dst[1] = (uint8_t)(c >> 8); dst[2] = (uint8_t)c;
}

/* Builds the borrowed extended surface for the frame being presented; returns 1 when `out` carries a usable extension. */
static int build_extension(SmsSdl3Viewer *v, const uint8_t *rgb, uint32_t height, SvExtendedSurface *out) {
  const SmsExtResult *r = &v->ext.result;
  uint32_t y, i;
  memset(out, 0, sizeof *out);
  out->mode = SV_WIDESCREEN_EXTENDED_BACKGROUNDS;
  out->status = SV_WS_UNAVAILABLE;
  out->sequence = r->sequence;
  if (r->cls == SMS_EXT_NONE) return 0;
  if (r->sequence != *v->sequence) { /* stale result: offered with no pixels so the shared check rejects (and counts) it; nothing is read */
    out->status = SV_WS_PARTIAL;
    return 1;
  }
  if (r->height != height) return 1; /* geometry the adapter will not offer: Original for this frame */
  if (r->cls == SMS_EXT_UNAVAILABLE) {
    v->l2_support = SV_WS_UNAVAILABLE;
    if (v->ext.request_sprites != 0u) v->l3_support = SV_WS_UNAVAILABLE; /* no background, so no sprite spill either */
    return 1;
  }
  if ((r->cls != SMS_EXT_WRAPPED && r->cls != SMS_EXT_DISTINCT) || r->side == 0u || r->side > SMS_EXT_MAX_SIDE) return 1;
  {
    const uint32_t w = SMS_FB_WIDTH + 2u * r->side;
    for (y = 0; y < height; ++y) {
      uint8_t *row = &g_ext_rgb[(size_t)y * w * 3u];
      for (i = 0; i < r->side; ++i) put_rgb(row + (size_t)i * 3u, v->ext.left[y][i]);
      memcpy(row + (size_t)r->side * 3u, rgb + (size_t)y * SMS_FB_WIDTH * 3u, (size_t)SMS_FB_WIDTH * 3u);
      for (i = 0; i < r->side; ++i) put_rgb(row + ((size_t)r->side + SMS_FB_WIDTH + i) * 3u, v->ext.right[y][i]);
    }
    out->rgb24 = g_ext_rgb;
    out->size = sizeof g_ext_rgb;
    out->width = (int)w;
    out->height = (int)height;
    out->pitch = (int)(w * 3u);
    out->side = (int)r->side;
    /* WRAPPED is real tilemap data that repeats the native picture: Partial, never Available. DISTINCT (no Mode 4 case) would be Available. */
    out->status = r->cls == SMS_EXT_DISTINCT ? SV_WS_AVAILABLE : SV_WS_PARTIAL;
    v->l2_support = out->status;
    /* the identity of the content is the producer's own: Level 3 only when this frame's result says it carries the sprite spill (never Available) */
    out->mode = r->sprites ? SV_WIDESCREEN_ENHANCED_GAMEPLAY : SV_WIDESCREEN_EXTENDED_BACKGROUNDS;
    if (v->ext.request_sprites != 0u) v->l3_support = r->sprites ? SV_WS_PARTIAL : SV_WS_UNAVAILABLE;
  }
  return 1;
}

static int sdl_present(void *ctx, const uint8_t *fb, uint32_t height) {
  SmsSdl3Viewer *v = (SmsSdl3Viewer *)ctx;
  static uint8_t rgb[SMS_FB_WIDTH * SMS_FB_MAX_HEIGHT * 3];
  uint32_t i;
  if (height == 0u || height > SMS_FB_MAX_HEIGHT) return -1;
  for (i = 0; i < SMS_FB_WIDTH * height; ++i) put_rgb(&rgb[i * 3u], fb[i]);
  v->frames++;
  /* The picture is 256 x 192 or 256 x 224 lines; the shared presenter fits the active height with the right aspect. */
  if (v->ext_attached) {
    SvExtendedSurface surface;
    if (build_extension(v, rgb, height, &surface))
      return sv_host_present_extended(v->host, rgb, (int)SMS_FB_WIDTH, (int)height, (int)SMS_FB_WIDTH * 3, *v->sequence, &surface);
  }
  return sv_host_present(v->host, rgb, (int)SMS_FB_WIDTH, (int)height, (int)SMS_FB_WIDTH * 3);
}

/* What the producer last achieved. Master System Level 2 can be at best Partial (the name table wraps); Level 3 adds only the sprites that cross the screen edges, also at best Partial. */
static void sdl_widescreen_caps(void *ctx, SvWidescreenCaps *out) {
  const SmsSdl3Viewer *v = (const SmsSdl3Viewer *)ctx;
  out->extended = v->ext_attached ? v->l2_support : SV_WS_UNAVAILABLE;
  out->enhanced = v->ext_attached && v->l2_support != SV_WS_UNAVAILABLE ? v->l3_support : SV_WS_UNAVAILABLE;
  out->partial_note = k_wrapped_note;
  out->enhanced_note = k_sprite_note;
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
  cfg.widescreen_caps = sdl_widescreen_caps;
  cfg.widescreen_ctx = v;
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

int sms_sdl3_viewer_attach_extension(SmsSdl3Viewer *v, SmsViewerRig *rig) {
  if (v == NULL || rig == NULL) return -1;
  memset(&v->ext, 0, sizeof v->ext);
  v->sequence = &rig->renderer.frames_completed;
  v->l2_support = SV_WS_PARTIAL; /* capable (at best Partial) until a produced frame says otherwise */
  v->l3_support = SV_WS_PARTIAL;
  v->plan_aspect = sv_host_settings(v->host)->video.aspect;
  v->ext_attached = 1;
  sms_viewer_rig_set_extension(rig, &v->ext);
  return 0;
}

SmsExtension *sms_sdl3_viewer_extension(SmsSdl3Viewer *v) { return v != NULL && v->ext_attached ? &v->ext : NULL; }

struct SvHost *sms_sdl3_viewer_sv_host(SmsSdl3Viewer *v) { return v != NULL ? v->host : NULL; }

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
