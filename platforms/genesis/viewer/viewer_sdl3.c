#include "viewer_sdl3.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_audio_format.h"
#include "sv_host.h"
#include "sv_layout.h"
#include "vdp_render.h"

struct GenesisSdl3Viewer {
  SvHost *host;            /* the shared viewer host (libs/viewer): window, input, settings, pause overlay, presenter */
  SDL_AudioStream *audio;
  GenesisAudioSink sink;
  SvSample sample;         /* the input sampled by the latest window_closed() call (once per host slice, as before) */
  const uint64_t *audio_dropped;
  /* SEG-118: Level 2 extension (see viewer_sdl3.h). The surface is preallocated for the widest geometry; no per-frame allocation. */
  GenesisFrameExtension ext;
  const uint64_t *frame_sequence;
  int ext_attached;
  SvAspect plan_aspect;               /* sampled with the requested mode once per host slice */
  SvWidescreenSupport l2_support;     /* the outcome of the latest frame produced under a request */
  SvWidescreenSupport l3_support;     /* SEG-121: whether the latest Level 3 request produced sprites (never Available: games may place none beside the picture) */
};

/* the runtime's and the viewer library's bounds must agree (the runtime does not depend on the viewer) */
typedef char genesis_ext_side_bound_matches_layout[(GENESIS_EXT_MAX_SIDE == (unsigned)SV_EXT_MAX_SIDE) ? 1 : -1];

static uint8_t g_extension_surface[GENESIS_EXT_SURFACE_BYTES];

/* The saved mode as the typed producer request (0 none, 1 Level 2, 2 Level 3); sampled once per host slice. */
static uint8_t request_of(SvWidescreenMode mode) {
  return mode == SV_WIDESCREEN_ENHANCED_GAMEPLAY ? (uint8_t)GENESIS_EXT_MODE_SPRITES : (mode == SV_WIDESCREEN_EXTENDED_BACKGROUNDS ? (uint8_t)GENESIS_EXT_MODE_BACKGROUNDS : 0U);
}

static const char k_sprite_note[] = "Sprites appear beside the picture only where the game places them; this does not widen the game world.";

static uint64_t sdl_now(void *ctx) { (void)ctx; return SDL_GetTicksNS(); }
static void sdl_sleep(void *ctx, uint64_t ns) { (void)ctx; SDL_DelayNS(ns); }

/* The host's once-per-slice poll: pumps events, samples the mapped input and, when the user opens the pause overlay, blocks here -- so
 * the guest does not advance while the menu is open and the host sleeps no wall time into guest state. */
static int sdl_closed(void *ctx) {
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)ctx;
  sv_host_service(v->host, &v->sample);
  if (v->ext_attached) { /* the requested mode is sampled here, between guest slices: a frame is produced wholly under one value */
    const SvVideoSettings *video = &sv_host_settings(v->host)->video;
    v->ext.request = request_of(video->widescreen);
    v->plan_aspect = video->aspect;
  }
  return v->sample.quit;
}

/* Console mapping of the shared action masks: BUTTON1/2/3 are the Genesis A/B/C buttons. */
static uint8_t pad_of(uint16_t actions) {
  GenesisViewerKeys k;
  k.up = (actions & SV_ACTION_BIT(SV_ACT_UP)) != 0; k.down = (actions & SV_ACTION_BIT(SV_ACT_DOWN)) != 0;
  k.left = (actions & SV_ACTION_BIT(SV_ACT_LEFT)) != 0; k.right = (actions & SV_ACTION_BIT(SV_ACT_RIGHT)) != 0;
  k.a = (actions & SV_ACTION_BIT(SV_ACT_BUTTON1)) != 0; k.b = (actions & SV_ACTION_BIT(SV_ACT_BUTTON2)) != 0;
  k.c = (actions & SV_ACTION_BIT(SV_ACT_BUTTON3)) != 0; k.start = (actions & SV_ACTION_BIT(SV_ACT_START)) != 0;
  return genesis_viewer_pad_from_keys(&k);
}
static uint8_t sdl_pad1(void *ctx) { return pad_of(((GenesisSdl3Viewer *)ctx)->sample.pad[0]); }
static uint8_t sdl_pad2(void *ctx) { return pad_of(((GenesisSdl3Viewer *)ctx)->sample.pad[1]); }

/* SEG-055: the artifact carries its own geometry and per-row palettes; the shared presenter fits whatever geometry arrives (a TV does the
 * same for H32 and interlace-mode-2 pictures). The RGB conversion is the one the headless PPM exporter uses
 * (genesis_vdp_frame_row_rgb888), so the window and captures agree by construction. */
static int sdl_present(void *ctx, const GenesisFrameArtifact *frame) {
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)ctx;
  static uint8_t rgb[GENESIS_FRAME_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT * 3];
  const unsigned width = GENESIS_FRAME_ARTIFACT_WIDTH(frame);
  const unsigned height = GENESIS_FRAME_ARTIFACT_HEIGHT(frame);
  if (width == 0U || width > GENESIS_FRAME_MAX_WIDTH || height == 0U || height > GENESIS_FRAME_MAX_HEIGHT) return -1;
  for (unsigned row = 0; row < height; ++row) {
    if (genesis_vdp_frame_row_rgb888(frame, row, &rgb[(size_t)row * width * 3U]) != 0) return -1;
  }
  if (v->ext_attached && v->frame_sequence != NULL) {
    const GenesisExtensionResult *r = &v->ext.result;
    const uint64_t sequence = *v->frame_sequence;
    SvExtendedSurface surface;
    memset(&surface, 0, sizeof surface);
    if (r->sequence == sequence && (r->status == GENESIS_EXT_AVAILABLE || r->status == GENESIS_EXT_PARTIAL)) {
      v->l2_support = r->status == GENESIS_EXT_AVAILABLE ? SV_WS_AVAILABLE : SV_WS_PARTIAL;
      if (v->ext.request >= GENESIS_EXT_MODE_SPRITES) v->l3_support = r->mode == GENESIS_EXT_MODE_SPRITES ? SV_WS_PARTIAL : SV_WS_UNAVAILABLE;
    } else if (r->sequence == sequence && r->status == GENESIS_EXT_UNAVAILABLE) {
      v->l2_support = SV_WS_UNAVAILABLE;
      if (v->ext.request >= GENESIS_EXT_MODE_SPRITES) v->l3_support = SV_WS_UNAVAILABLE;
    }
    if (r->status != GENESIS_EXT_NONE) {
      surface.rgb24 = v->ext.pixels;
      surface.size = v->ext.capacity;
      surface.width = (int)r->native_w + 2 * (int)r->side;
      surface.height = (int)r->native_h;
      surface.pitch = (int)r->pitch;
      surface.side = (int)r->side;
      surface.sequence = r->sequence;
      /* the produced frame's own identity, never the saved preference */
      surface.mode = r->mode == GENESIS_EXT_MODE_SPRITES ? SV_WIDESCREEN_ENHANCED_GAMEPLAY
                     : (r->mode == GENESIS_EXT_MODE_BACKGROUNDS ? SV_WIDESCREEN_EXTENDED_BACKGROUNDS : SV_WIDESCREEN_ORIGINAL);
      surface.status = r->status == GENESIS_EXT_AVAILABLE ? SV_WS_AVAILABLE : (r->status == GENESIS_EXT_PARTIAL ? SV_WS_PARTIAL : SV_WS_UNAVAILABLE);
      return sv_host_present_extended(v->host, rgb, (int)width, (int)height, (int)(width * 3U), sequence, &surface);
    }
  }
  return sv_host_present(v->host, rgb, (int)width, (int)height, (int)(width * 3U));
}

/* Reports what the producer last achieved: Level 2 scenery, and Level 3 (Partial at best) when the sprite layer was produced. */
static void sdl_widescreen_caps(void *ctx, SvWidescreenCaps *out) {
  const GenesisSdl3Viewer *v = (const GenesisSdl3Viewer *)ctx;
  out->extended = v->ext_attached ? v->l2_support : SV_WS_UNAVAILABLE;
  out->enhanced = v->ext_attached && v->l2_support != SV_WS_UNAVAILABLE ? v->l3_support : SV_WS_UNAVAILABLE;
  out->enhanced_note = k_sprite_note;
}

static int sdl_extension_plan(void *ctx, unsigned native_w, unsigned native_h, unsigned *side_out) {
  const GenesisSdl3Viewer *v = (const GenesisSdl3Viewer *)ctx;
  const int side = sv_layout_extension_side((int)native_w, (int)native_h, v->plan_aspect);
  if (side <= 0) return -1;
  *side_out = (unsigned)side;
  return 0;
}

GenesisFrameExtension *genesis_sdl3_viewer_extension(GenesisSdl3Viewer *v, const uint64_t *frame_sequence) {
  int w = 0, h = 0;
  if (v == NULL || frame_sequence == NULL) return NULL;
  sv_layout_extension_max((int)GENESIS_FRAME_MAX_WIDTH, (int)GENESIS_FRAME_MAX_HEIGHT, &w, &h);
  if ((size_t)w * (size_t)h * 3U > sizeof g_extension_surface) return NULL;
  memset(&v->ext, 0, sizeof v->ext);
  v->ext.producer = genesis_vdp_produce_extension;
  v->ext.plan = sdl_extension_plan;
  v->ext.plan_ctx = v;
  v->ext.pixels = g_extension_surface;
  v->ext.capacity = sizeof g_extension_surface;
  v->frame_sequence = frame_sequence;
  v->ext_attached = 1;
  v->l2_support = SV_WS_AVAILABLE; /* capable until a produced frame says otherwise */
  v->l3_support = SV_WS_PARTIAL;
  v->plan_aspect = sv_host_settings(v->host)->video.aspect;
  v->ext.request = request_of(sv_host_settings(v->host)->video.widescreen);
  return &v->ext;
}

struct SvHost *genesis_sdl3_viewer_sv_host(GenesisSdl3Viewer *v) { return v != NULL ? v->host : NULL; }

static void sdl_diagnostics(void *ctx, SvDiagnostics *out) {
  const GenesisSdl3Viewer *v = (const GenesisSdl3Viewer *)ctx;
  out->audio_active = v->audio != NULL;
  if (v->audio != NULL) {
    const int bytes = SDL_GetAudioStreamQueued(v->audio);
    out->audio_queued_ms = bytes > 0 ? (uint32_t)((uint64_t)bytes * 1000U / (GENESIS_MIXER_CHANNELS * 2U * GENESIS_MIXER_RATE_HZ)) : 0U;
  }
  out->audio_dropped = v->audio_dropped != NULL ? *v->audio_dropped : 0U;
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
  sv_host_set_audio(v->host, v->audio); /* volume/mute are playback gain on this stream; the mixer output is untouched */
  v->sink.context = v;
  v->sink.queued_frames = sdl_audio_queued;
  v->sink.put = sdl_audio_put;
  return &v->sink;
}

GenesisSdl3Viewer *genesis_sdl3_viewer_open(const char *title, int scale) { return genesis_sdl3_viewer_open_platform(title, scale, "genesis"); }

GenesisSdl3Viewer *genesis_sdl3_viewer_open_platform(const char *title, int scale, const char *platform_id) {
  SvHostConfig cfg;
  const char *sha = SDL_getenv("SEGARECOMP_ROM_SHA256");
  const char *profile = SDL_getenv("SEGARECOMP_PROFILE_ID");
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)calloc(1, sizeof(*v));
  if (v == NULL) return NULL;
  memset(&cfg, 0, sizeof cfg);
  cfg.system = SV_SYSTEM_GENESIS;
  cfg.title = title ? title : "segarecomp";
  cfg.picture_w = (int)GENESIS_FRAME_WIDTH;
  cfg.picture_h = (int)GENESIS_FRAME_HEIGHT;
  cfg.max_w = (int)GENESIS_FRAME_MAX_WIDTH;
  cfg.max_h = (int)GENESIS_FRAME_MAX_HEIGHT;
  cfg.default_scale = scale >= 1 ? scale : 3;
  cfg.can_reset = 0; /* the Genesis viewer has no proven safe reset seam */
  cfg.rom_sha256 = sha != NULL && strlen(sha) == 64U ? sha : NULL;
  /* SEG-123: the per-game widescreen profile key is platform + full SHA-256; a Sega CD launch supplies its disc identity (never shown as a ROM digest) */
  cfg.platform_id = platform_id;
  cfg.profile_id = cfg.rom_sha256 != NULL ? cfg.rom_sha256 : (profile != NULL && strlen(profile) == 64U ? profile : NULL);
  cfg.diagnostics = sdl_diagnostics;
  cfg.diagnostics_ctx = v;
  cfg.widescreen_caps = sdl_widescreen_caps;
  cfg.widescreen_ctx = v;
  v->host = sv_host_open(&cfg);
  if (v->host == NULL) { free(v); return NULL; }
  return v;
}

void genesis_sdl3_viewer_report_audio_drops(GenesisSdl3Viewer *v, const uint64_t *overrun_dropped) {
  if (v != NULL) v->audio_dropped = overrun_dropped;
}

void genesis_sdl3_viewer_stop_screen(GenesisSdl3Viewer *v, const char *title, const char *detail) {
  if (v != NULL) sv_host_stop_screen(v->host, title, detail);
}

void genesis_sdl3_viewer_host(GenesisSdl3Viewer *viewer, GenesisViewerHost *host) {
  memset(host, 0, sizeof(*host));  /* every optional callback defaults to NULL (after_slice is installed by the hook) */
  host->ctx = viewer;
  host->now_ns = sdl_now;
  host->sleep_ns = sdl_sleep;
  host->present = sdl_present;
  host->window_closed = sdl_closed;
  host->pad1 = sdl_pad1;
  host->pad2 = sdl_pad2;
}

void genesis_sdl3_viewer_close(GenesisSdl3Viewer *v) {
  if (v == NULL) return;
  if (v->audio) { sv_host_set_audio(v->host, NULL); SDL_DestroyAudioStream(v->audio); }
  sv_host_close(v->host); /* includes SDL_Quit */
  free(v);
}
