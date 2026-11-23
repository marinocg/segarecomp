#include "viewer_sdl3.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_audio_format.h"
#include "sv_host.h"
#include "sv_perf.h"
#include "sv_layout.h"
#include "vdp_render.h"
#include "virtual_sprite.h"

struct GenesisSdl3Viewer {
  SvHost *host;            /* the shared viewer host (libs/viewer): window, input, settings, pause overlay, presenter */
  SDL_AudioStream *audio;
  GenesisAudioSink sink;
  struct SvPerf *perf;     /* SEG-125: the collector (NULL = none) */
  SvSample sample;         /* the input sampled by the latest window_closed() call (once per host slice, as before) */
  const uint64_t *audio_dropped;
  /* SEG-118: Level 2 extension (see viewer_sdl3.h). The surface is preallocated for the widest geometry; no per-frame allocation. */
  GenesisFrameExtension ext;
  const uint64_t *frame_sequence;
  int ext_attached;
  SvVideoSettings plan_video;         /* SEG-139: the geometry-relevant video settings (pixel aspect, exact ratio, legacy width), sampled with the requested mode once per host slice */
  SvWidescreenSupport l2_support;     /* the outcome of the latest frame produced under a request */
  /* SEG-122 automation seam (like the Sega CD viewer's SEGARECOMP_VIEWER_KEY_SCRIPT): SEGARECOMP_VIEWER_PAD_SCRIPT names a file of `<frame> <buttons>` lines (buttons: any of
   * U D L R A B C S, or `-` for none) that replaces player 1's sampled pad from that guest frame on (the published frame number). Deterministic input for repeatable real-game comparisons; unset in every shipped use. */
  struct PadLine { uint64_t poll; uint16_t actions; } pad_script[1024];
  size_t pad_count, pad_next;
  uint64_t polls;
  GenesisNativeCompatStats compat_stats;   /* SEG-124 */
  unsigned compat_enabled;                 /* SEG-124: the components the verified descriptor enables */
  SvWidescreenSupport l3_support;     /* SEG-121: whether the latest Level 3 request produced sprites (never Available: games may place none beside the picture) */
};

/* the runtime's and the viewer library's bounds must agree (the runtime does not depend on the viewer) */
typedef char genesis_ext_side_bound_matches_layout[(GENESIS_EXT_MAX_SIDE == (unsigned)SV_EXT_MAX_SIDE) ? 1 : -1];

static uint8_t g_extension_surface[GENESIS_EXT_SURFACE_BYTES];
static uint8_t g_extension_classes[GENESIS_EXT_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT];   /* ADR 0312: the producer's winner class of every extension pixel of the frame being produced (one byte per surface pixel) */
static uint8_t g_compat_ram[GENESIS_COMPAT_RAM_BYTES];          /* SEG-124: work-RAM snapshot for the verified provider (written by the runtime at each publication) */
static uint8_t g_compat_overlay[GENESIS_COMPAT_OVERLAY_BYTES];  /* SEG-124: the provider's HUD overlay for the frame being presented */
static uint8_t g_compat_layers[GENESIS_COMPAT_LAYER_BYTES];     /* SEG-140-T004: the provider's positioned overlay images for the frame being presented */
static SvOverlayLayer g_layer_desc[GENESIS_COMPAT_LAYERS_MAX];
/* SEG-140-T008 (ADR 0306): the virtual-sprite storage of the frame being produced: the provider's set, the auxiliary render the compositor needs and the composed picture. Static, one per process, reused every
 * frame; nothing is allocated per frame, and the auxiliary render only runs for a frame whose provider submitted commands. */
static GenesisVirtualSpriteSet g_vsprite_set;
static GenesisVdpFrameAux g_vsprite_aux;
static GenesisFrameArtifact g_vsprite_scratch;
static uint8_t g_vsprite_rgb[GENESIS_FRAME_MAX_WIDTH * GENESIS_FRAME_MAX_HEIGHT * 3U];   /* the composed native picture */
static uint8_t g_vsprite_wide[GENESIS_EXT_SURFACE_BYTES];                               /* the composed extension surface (picture and presented columns), SEG-140-T009 */
static uint8_t g_vsprite_wide_valid;                                                     /* the wide buffer holds THIS frame's composition (extension pixels were drawn) */

/* SEG-140-T008 (ADR 0306): the adapter's compositor, called by the producer for a frame whose provider submitted at least one command, while the frame's raster state and VRAM are valid. It runs the VDP's
 * own auxiliary render (identical artifact, plus per-pixel layer classes and per-row sprite demand) and the F14 compositor, and keeps the composed picture in a static buffer the presenter reads. */
static int viewer_vsprite_compose(void *ctx, const GenesisVirtualSpriteSet *set, unsigned host_flags, const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster,
                                  const GenesisFrameArtifact *native, const uint8_t *surface, const uint8_t *classes, unsigned side, int shift, GenesisVirtualSpriteOutcome *out) {
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)ctx;
  GenesisVirtualSpriteReport report;
  GenesisVirtualSpriteSurface target;
  const unsigned width = GENESIS_FRAME_ARTIFACT_WIDTH(native), height = GENESIS_FRAME_ARTIFACT_HEIGHT(native);
  const SvSettings *settings = sv_host_settings(v->host);
  unsigned visible = 0U;
  g_vsprite_wide_valid = 0U;
  if (width == 0U || width > GENESIS_FRAME_MAX_WIDTH || height == 0U || height > GENESIS_FRAME_MAX_HEIGHT) return -1;
  if (surface != NULL && side > 0U && settings->video.widescreen != SV_WIDESCREEN_ORIGINAL) {   /* ADR 0307: only the columns the host will present for the CURRENT aspect are ever drawn */
    const int vis = sv_geom_resolve_visible(&settings->video, (int)width, (int)height, (int)side);
    visible = vis > 0 ? (unsigned)vis : 0U;
    if (shift != 0 && visible != side) visible = 0U;   /* ADR 0315: an asymmetric surface is presented whole or not at all, and only then are its extension columns composed */
  }
  target.shift = shift;
  target.rgb = surface;
  target.side = side;
  target.visible = visible;
  target.layer = classes;   /* ADR 0312: NULL unless the producer recorded this frame's classes */
  if (genesis_vdp_produce_frame_raster_aux(vram, raster, &g_vsprite_scratch, &g_vsprite_aux) != 0) return -1;
  if (side > 0U && surface != NULL) {   /* the composition keeps the extension's layout; the native picture is its centre */
    if (genesis_virtual_sprites_compose_wide(set, host_flags, vram, native, &g_vsprite_aux, &target, g_vsprite_wide, sizeof g_vsprite_wide, &report) != 0) return -1;
    for (unsigned row = 0U; row < height; ++row) memcpy(&g_vsprite_rgb[(size_t)row * width * 3U], &g_vsprite_wide[((size_t)row * (width + 2U * side) + (size_t)((int)side + shift)) * 3U], (size_t)width * 3U);
    g_vsprite_wide_valid = (uint8_t)(report.pixels_left_verified + report.pixels_left_authored + report.pixels_right_verified + report.pixels_right_authored > 0U);
  } else if (genesis_virtual_sprites_compose(set, host_flags, vram, native, &g_vsprite_aux, g_vsprite_rgb, sizeof g_vsprite_rgb, &report) != 0) {
    return -1;
  }
  out->space = set->space;
  out->pixels_left_verified = report.pixels_left_verified;
  out->pixels_left_authored = report.pixels_left_authored;
  out->pixels_right_verified = report.pixels_right_verified;
  out->pixels_right_authored = report.pixels_right_authored;
  out->status = report.status;
  out->reason = report.reason;
  out->index = report.reject_index;
  out->commands = report.commands;
  out->pixels_verified = report.pixels_drawn_verified;
  out->pixels_authored = report.pixels_drawn_authored;
  out->rows_over_sprite_limit = report.rows_over_sprite_limit;
  out->rows_over_cell_budget = report.rows_over_cell_budget;
  out->rows_dropped = report.sprite_rows_dropped;
  out->rows_truncated = report.sprite_rows_truncated;
  if (report.status == (uint8_t)GENESIS_VSPRITE_STATUS_COMPOSED) {
    const unsigned n = set->count < GENESIS_VSPRITE_MAX ? (unsigned)set->count : GENESIS_VSPRITE_MAX;
    for (unsigned i = 0U; i < n; ++i) out->provenance = (uint8_t)(out->provenance | (set->sprites[i].provenance == (uint8_t)GENESIS_VSPRITE_GAME_STATE_VERIFIED ? 1U : 2U));
  }
  return 0;
}

/* The saved mode as the typed producer request (0 none, 1 Level 2, 2 Level 3); sampled once per host slice. */
static uint8_t request_of_compat(SvWidescreenMode mode, unsigned compat_hud) {
  /* a verified native HUD replaces the game's own (which the patch removed): the producer must also run for Original, so the HUD is drawn (inside the picture there) */
  if (mode == SV_WIDESCREEN_ORIGINAL && compat_hud != 0U) return (uint8_t)GENESIS_EXT_MODE_BACKGROUNDS;
  return mode == SV_WIDESCREEN_ENHANCED_GAMEPLAY ? (uint8_t)GENESIS_EXT_MODE_SPRITES : (mode == SV_WIDESCREEN_EXTENDED_BACKGROUNDS ? (uint8_t)GENESIS_EXT_MODE_BACKGROUNDS : 0U);
}
static uint8_t request_of(SvWidescreenMode mode) {
  return mode == SV_WIDESCREEN_ENHANCED_GAMEPLAY ? (uint8_t)GENESIS_EXT_MODE_SPRITES : (mode == SV_WIDESCREEN_EXTENDED_BACKGROUNDS ? (uint8_t)GENESIS_EXT_MODE_BACKGROUNDS : 0U);
}

static const char k_sprite_note[] = "Sprites appear beside the picture only where the game places them; this does not widen the game world.";

static uint64_t sdl_now(void *ctx) { (void)ctx; return SDL_GetTicksNS(); }
static void sdl_sleep(void *ctx, uint64_t ns) { (void)ctx; SDL_DelayNS(ns); }

/* SEG-131 automation: a key press as a queued SDL event, exactly what the keyboard would deliver. */
static void push_script_key(SDL_Scancode scancode) {
  SDL_Event e;
  int down;
  for (down = 1; down >= 0; --down) {
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.common.timestamp = SDL_GetTicksNS();
    e.key.scancode = scancode;
    e.key.down = down != 0;
    SDL_PushEvent(&e);
  }
}

/* The host's once-per-slice poll: pumps events, samples the mapped input and, when the user opens the pause overlay, blocks here -- so
 * the guest does not advance while the menu is open and the host sleeps no wall time into guest state. */
static int sdl_closed(void *ctx) {
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)ctx;
  sv_host_service(v->host, &v->sample);
  const uint64_t clock = v->frame_sequence != NULL ? *v->frame_sequence : v->polls;  /* guest frames: identical input timing in every build */
  while (v->pad_next < v->pad_count && v->pad_script[v->pad_next].poll <= clock) {
    v->sample.pad[0] = v->pad_script[v->pad_next].actions;  /* persists until the next line */
    /* SEG-131 automation: a line may press the quick-save (K) / quick-load (M) hotkey once, at this snapshot barrier, exactly as F5 / F8 do */
    if ((v->sample.pad[0] & 0x4000U) != 0U) (void)sv_host_quick_save(v->host);
    if ((v->sample.pad[0] & 0x2000U) != 0U) (void)sv_host_quick_load(v->host);
    /* SEG-131 automation: Y = open the rewind surface, go back two moments with Left, confirm with Return; Z = go back three and cancel with Escape. The keys are
     * real queued key events consumed by the real surface loop, so the whole keyboard path is exercised. */
    if ((v->sample.pad[0] & 0x1000U) != 0U) { push_script_key(SDL_SCANCODE_LEFT); push_script_key(SDL_SCANCODE_LEFT); push_script_key(SDL_SCANCODE_RETURN); (void)sv_host_open_rewind(v->host); }
    if ((v->sample.pad[0] & 0x0800U) != 0U) { push_script_key(SDL_SCANCODE_LEFT); push_script_key(SDL_SCANCODE_LEFT); push_script_key(SDL_SCANCODE_LEFT); push_script_key(SDL_SCANCODE_ESCAPE); (void)sv_host_open_rewind(v->host); }
    v->pad_script[v->pad_next].actions = (uint16_t)(v->pad_script[v->pad_next].actions & 0x87FFU);
    v->sample.pad[0] = (uint16_t)(v->sample.pad[0] & 0x87FFU);
    v->pad_next += 1U;
  }
  if (v->pad_count != 0U && v->pad_next > 0U) v->sample.pad[0] = v->pad_script[v->pad_next - 1U].actions;
  if ((v->sample.pad[0] & 0x8000U) != 0U) { v->sample.pad[0] = (uint16_t)(v->sample.pad[0] & 0x7FFFU); v->sample.quit = 1; }
  v->polls += 1U;
  if (v->ext_attached) { /* the requested mode is sampled here, between guest slices: a frame is produced wholly under one value */
    const SvVideoSettings *video = &sv_host_settings(v->host)->video;
    v->ext.request = request_of_compat(video->widescreen, v->compat_enabled & GENESIS_COMPAT_HUD);
    v->ext.extension_shown = video->widescreen != SV_WIDESCREEN_ORIGINAL;   /* ADR 0315: in Original nothing of the extension is presented, whatever the producer makes for the HUD */
    if (v->ext.compat != NULL) v->ext.compat_components = video->widescreen == SV_WIDESCREEN_ORIGINAL ? (v->compat_enabled & (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SPRITES)) : v->compat_enabled;   /* Original: the HUD (and the virtual sprites drawn in the picture) only */
    v->plan_video = *video;
  }
  return v->sample.quit;
}

static void load_pad_script(GenesisSdl3Viewer *v) {
  const char *path = SDL_getenv("SEGARECOMP_VIEWER_PAD_SCRIPT");
  FILE *f = path != NULL ? fopen(path, "r") : NULL;
  char text[96];
  if (f == NULL) return;
  while (v->pad_count < sizeof v->pad_script / sizeof v->pad_script[0] && fgets(text, sizeof text, f) != NULL) {
    unsigned long long poll = 0U;
    char keys[32] = "";
    uint16_t actions = 0U;
    if (sscanf(text, "%llu %31s", &poll, keys) != 2) continue;
    for (const char *c = keys; *c != '\0'; ++c) {
      switch (*c) {
        case 'U': actions |= SV_ACTION_BIT(SV_ACT_UP); break;
        case 'D': actions |= SV_ACTION_BIT(SV_ACT_DOWN); break;
        case 'L': actions |= SV_ACTION_BIT(SV_ACT_LEFT); break;
        case 'R': actions |= SV_ACTION_BIT(SV_ACT_RIGHT); break;
        case 'A': actions |= SV_ACTION_BIT(SV_ACT_BUTTON1); break;
        case 'B': actions |= SV_ACTION_BIT(SV_ACT_BUTTON2); break;
        case 'C': actions |= SV_ACTION_BIT(SV_ACT_BUTTON3); break;
        case 'S': actions |= SV_ACTION_BIT(SV_ACT_START); break;
        case 'Y': actions |= 0x1000U; break;  /* SEG-131: rewind two moments and confirm */
        case 'Z': actions |= 0x0800U; break;  /* SEG-131: browse three moments back and cancel */
        case 'K': actions |= 0x4000U; break;  /* SEG-131: quick save (F5) */
        case 'M': actions |= 0x2000U; break;  /* SEG-131: quick load (F8) */
        case 'Q': actions |= 0x8000U; break;  /* end the run (the window's close): lets a scripted capture finish without a wall-clock timeout */
        default: break;
      }
    }
    v->pad_script[v->pad_count].poll = (uint64_t)poll;
    v->pad_script[v->pad_count].actions = actions;
    v->pad_count += 1U;
  }
  fclose(f);
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
  {
    const uint64_t t_rgb = sv_perf_span_begin(v->perf);
    for (unsigned row = 0; row < height; ++row) {
      if (genesis_vdp_frame_row_rgb888(frame, row, &rgb[(size_t)row * width * 3U]) != 0) return -1;
    }
    sv_perf_span_end(v->perf, SV_SPAN_RGB_CONVERT, t_rgb);
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
      surface.shift = (int)r->shift;   /* ADR 0315: where the picture sits in the planned total; validated again by the host */
      surface.hud_rows = (int)r->hud_rows;   /* ADR 0318: the window-plane HUD band at the bottom; the host keeps it centred and black-bordered */
      surface.sequence = r->sequence;
      /* the produced frame's own identity, never the saved preference */
      surface.mode = r->mode == GENESIS_EXT_MODE_SPRITES ? SV_WIDESCREEN_ENHANCED_GAMEPLAY
                     : (r->mode == GENESIS_EXT_MODE_BACKGROUNDS ? SV_WIDESCREEN_EXTENDED_BACKGROUNDS : SV_WIDESCREEN_ORIGINAL);
      surface.status = r->status == GENESIS_EXT_AVAILABLE ? SV_WS_AVAILABLE : (r->status == GENESIS_EXT_PARTIAL ? SV_WS_PARTIAL : SV_WS_UNAVAILABLE);
      if (v->ext.compat != NULL && r->sequence == sequence) {   /* SEG-124: only the frame's own provider verdict; never inferred from the title, mode or pixels */
        v->compat_stats.frames += 1U;
        /* ADR 0313: an unanswered scenery cell (reason FAULT) fails only the frame's authored-scenery claim (`authored` is 0 and the unanswered cells fell back to the generic extension); the provider's HUD, positioned
         * layers and virtual sprites are independent contributions the provider already produced for this very frame, so they are still delivered. Every other reason (stale state, unsupported scene, bad state or scroll,
         * mismatch) and a frame the provider did not accept at all still deliver nothing. */
        if (r->compat_accepted != 0U && r->compat_reason == (uint8_t)GENESIS_COMPAT_FAULT) v->compat_stats.declined[GENESIS_COMPAT_FAULT] += 1U;   /* still counted, so the scenery failures stay observable */
        if (r->compat_accepted != 0U && (r->compat_reason == 0U || r->compat_reason == (uint8_t)GENESIS_COMPAT_FAULT)) {
          if (r->authored) { surface.source = SV_EXTSRC_AUTHORED; v->compat_stats.authored += 1U; }
          else if (r->authored_row_count > 0U) { surface.authored_rows = r->authored_rows; surface.authored_row_count = (int)r->authored_row_count; }   /* W04: row provenance of a partial frame */
          if (r->overlay_w > 0U && r->overlay_h > 0U && r->overlay_w <= GENESIS_COMPAT_OVERLAY_MAX_W && r->overlay_h <= GENESIS_COMPAT_OVERLAY_MAX_H) {
            surface.overlay_rgba = v->ext.overlay_rgba;
            surface.overlay_w = (int)r->overlay_w;
            surface.overlay_h = (int)r->overlay_h;
            surface.overlay_fx = (int)r->overlay_fx;
            surface.overlay_fy = (int)r->overlay_fy;
            v->compat_stats.hud_frames += 1U;
          }
          if (r->vsprite.status != 0U) {   /* SEG-140-T008: the compositor's own result for THIS frame, mapped verbatim; a set composed with no drawn pixel is only "validated" (it delivers nothing) */
            const uint32_t drawn = r->vsprite.pixels_verified + r->vsprite.pixels_authored;
            if (r->vsprite.status == (uint8_t)GENESIS_VSPRITE_STATUS_COMPOSED) {
              surface.actor_stage = drawn > 0U ? (uint8_t)SV_ACT_COMPOSED : (uint8_t)SV_ACT_VALIDATED;
              surface.actor_provenance = (r->vsprite.provenance & 1U) != 0U ? 1U : 2U;
              surface.actor_state_verified = r->vsprite.state_verified;
              surface.actor_pixels_verified = r->vsprite.pixels_verified;
              surface.actor_pixels_authored = r->vsprite.pixels_authored;
              surface.actor_left_verified = r->vsprite.pixels_left_verified;
              surface.actor_left_authored = r->vsprite.pixels_left_authored;
              surface.actor_right_verified = r->vsprite.pixels_right_verified;
              surface.actor_right_authored = r->vsprite.pixels_right_authored;
              if (drawn > 0U) surface.actor_picture = g_vsprite_rgb;
              if (g_vsprite_wide_valid != 0U) surface.rgb24 = g_vsprite_wide;   /* the extension columns THIS frame will show carry the sprites (composed over the surface that is displayed) */
            } else {
              surface.actor_stage = (uint8_t)SV_ACT_REJECTED;
              surface.actor_reject = r->vsprite.reason;
            }
          }
          if (r->layer_count > 0U) {   /* SEG-140-T004: the host validates each layer against the live geometry; this only maps the transport */
            for (unsigned i = 0U; i < r->layer_count && i < GENESIS_COMPAT_LAYERS_MAX; ++i) {
              const GenesisCompatLayer *l = &r->layers[i];
              SvOverlayLayer *d = &g_layer_desc[i];
              memset(d, 0, sizeof *d);
              d->rgba = (r->layer_bad >> i & 1U) != 0U ? NULL : g_compat_layers + l->offset;   /* a flagged descriptor never forms a pointer: the host rejects it (no_image / size) */
              d->w = (int)l->width; d->h = (int)l->height; d->anchor = (int)l->anchor; d->reference = (int)l->reference; d->z = (int)l->z; d->dx = (int)l->dx; d->dy = (int)l->dy;
              d->has_clip = l->has_clip != 0U; d->clip_x = (int)l->clip_x; d->clip_y = (int)l->clip_y; d->clip_w = (int)l->clip_w; d->clip_h = (int)l->clip_h;
            }
            surface.layers = g_layer_desc;
            surface.layer_count = (int)r->layer_count;   /* beyond four: the host reports the extras as rejected by count */
          }
        } else if (r->compat_reason < 8U) {
          v->compat_stats.declined[r->compat_reason] += 1U;
        }
      }
      sv_host_set_scene(v->host, v->ext.compat != NULL && r->sequence == sequence && r->compat_accepted != 0U ? r->scene : ""); /* SEG-139-T004: the provider's own scene name for THIS frame, never inferred */
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
  GenesisSdl3Viewer *v = (GenesisSdl3Viewer *)ctx;
  int side = 0;
  if (sv_geom_resolve_production(&v->plan_video, (int)native_w, (int)native_h, &side) != SV_GEOM_OK) return -1; /* a rejected plan produces nothing (never a clamped surface) */
  if (side <= 0) {
    /* ADR 0315: an exact aspect that needs no extension (4:3) plans nothing, and the producer then returns before it serves any render contribution, so a verified provider's indicator layer would vanish at exactly
     * 4:3 although it is shown in Original. As in Original (where a verified HUD also makes the producer run on the legacy plan) such a provider is served on the legacy side and the extension is marked not shown:
     * its contributions are presented, nothing extended is. Without a provider nothing is produced, as before. */
    if ((v->compat_enabled & GENESIS_COMPAT_HUD) == 0U) return -1;   /* also in Original with an exact aspect retained (a zero plan must not drop the provider there either) */
    side = sv_layout_extension_side((int)native_w, (int)native_h, v->plan_video.aspect);
    if (side <= 0) return -1;
    v->ext.extension_shown = 0U;
  } else {
    v->ext.extension_shown = v->plan_video.widescreen != SV_WIDESCREEN_ORIGINAL;
  }
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
  v->ext.vsprite_set = &g_vsprite_set;
  v->ext.vsprite_compose = viewer_vsprite_compose;
  v->ext.vsprite_ctx = v;
  v->ext.capacity = sizeof g_extension_surface;
  v->ext.classes = g_extension_classes;   /* ADR 0312 */
  v->ext.classes_capacity = sizeof g_extension_classes;
  v->frame_sequence = frame_sequence;
  v->ext_attached = 1;
  v->l2_support = SV_WS_AVAILABLE; /* capable until a produced frame says otherwise */
  v->l3_support = SV_WS_PARTIAL;
  v->plan_video = sv_host_settings(v->host)->video;
  v->ext.request = request_of(sv_host_settings(v->host)->video.widescreen);
  return &v->ext;
}

struct SvHost *genesis_sdl3_viewer_sv_host(GenesisSdl3Viewer *v) { return v != NULL ? v->host : NULL; }

int genesis_sdl3_viewer_attach_native_compat(GenesisSdl3Viewer *v, GenesisFrameExtension *ext, const GenesisNativeCompat *compat, void *state, unsigned caps) {
  const unsigned known = GENESIS_CAP_FRAME_WORK_RAM | GENESIS_CAP_FRAME_VDP | GENESIS_CAP_SCENERY_WORDS | GENESIS_CAP_OVERLAY_RGBA;
  const unsigned components = ((caps & GENESIS_CAP_SCENERY_WORDS) != 0U ? GENESIS_COMPAT_SCENERY : 0U) | ((caps & GENESIS_CAP_OVERLAY_RGBA) != 0U ? GENESIS_COMPAT_HUD : 0U) |
                              ((caps & GENESIS_CAP_OVERLAY_RGBA) != 0U && compat != NULL && (compat->components & GENESIS_COMPAT_SPRITES) != 0U ? GENESIS_COMPAT_SPRITES : 0U);   /* virtual sprites: declared by the provider AND output-only under overlay.rgba */
  if (v == NULL || ext == NULL || compat == NULL || ext != &v->ext || compat->abi_version != GENESIS_NATIVE_COMPAT_ABI || compat->prepare == NULL || ((components & GENESIS_COMPAT_SCENERY) != 0U && compat->scenery_word == NULL) || (caps & ~known) != 0U ||
      components == 0U || (components & compat->components) == 0U)
    return -1;
  ext->compat = compat;
  ext->compat_state = state;
  ext->compat_caps = caps;
  ext->compat_private_words = 0U;   /* SEG-152-T007: never enabled here; only the trusted attach of the scheduled-mod adapter (viewer_main_hook.c) sets it after this call */
  ext->compat_components = components & compat->components;
  v->compat_enabled = ext->compat_components;
  ext->request = request_of_compat(sv_host_settings(v->host)->video.widescreen, ext->compat_components & GENESIS_COMPAT_HUD);
  ext->extension_shown = sv_host_settings(v->host)->video.widescreen != SV_WIDESCREEN_ORIGINAL;
  ext->compat_ram = g_compat_ram;
  ext->compat_ram_stamp = 0U;
  ext->overlay_rgba = g_compat_overlay;
  ext->layer_rgba = g_compat_layers;
  return 0;
}

int genesis_sdl3_viewer_attach_scheduled_mod_compat(GenesisSdl3Viewer *v, GenesisFrameExtension *ext, const GenesisNativeCompat *compat, void *state, unsigned caps) {
  if (genesis_sdl3_viewer_attach_native_compat(v, ext, compat, state, caps) != 0) return -1;
  ext->compat_private_words = 1U;
  return 0;
}

void genesis_sdl3_viewer_native_compat_stats(const GenesisSdl3Viewer *v, GenesisNativeCompatStats *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof *out);
  if (v != NULL) *out = v->compat_stats;
}

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

void genesis_sdl3_viewer_audio_clear(GenesisSdl3Viewer *v) {
  if (v != NULL && v->audio != NULL) SDL_ClearAudioStream(v->audio);
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
  /* SEG-122: only the launcher sets these, and only for a verified, selected Patched program (the host validates every value and ignores the lot when one is invalid) */
  cfg.patch_id = SDL_getenv("SEGARECOMP_PATCH_ID");
  cfg.patch_widescreen = SDL_getenv("SEGARECOMP_PATCH_WIDESCREEN");
  cfg.patch_side_style = SDL_getenv("SEGARECOMP_PATCH_SIDE_STYLE");
#ifdef SEGARECOMP_GEOMETRY_DECLARATION
  cfg.geometry_declaration = SEGARECOMP_GEOMETRY_DECLARATION; /* SEG-139-T004: compiled in by the build from the verified package; the environment cannot loosen it */
#else
  cfg.geometry_declaration = SDL_getenv("SEGARECOMP_PATCH_GEOMETRY"); /* development override for a program built without a declaration */
#endif
  /* SEG-122: an explicit identity wins over the image digest: a Patched program was built from a different image than the library game its preferences belong to */
  cfg.profile_id = profile != NULL && strlen(profile) == 64U ? profile : cfg.rom_sha256;
  cfg.diagnostics = sdl_diagnostics;
  cfg.diagnostics_ctx = v;
  cfg.widescreen_caps = sdl_widescreen_caps;
  cfg.widescreen_ctx = v;
  load_pad_script(v);
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

void genesis_sdl3_viewer_set_perf(GenesisSdl3Viewer *v, struct SvPerf *perf) {
  if (v == NULL) return;
  v->perf = perf;
  sv_host_set_perf(v->host, perf);
}

const char *genesis_sdl3_viewer_renderer_name(const GenesisSdl3Viewer *v) { return v != NULL && v->host != NULL ? sv_host_renderer_name(v->host) : NULL; }

void genesis_sdl3_viewer_close(GenesisSdl3Viewer *v) {
  if (v == NULL) return;
  if (v->audio) { sv_host_set_audio(v->host, NULL); SDL_DestroyAudioStream(v->audio); }
  { /* SEG-139-T004: SEGARECOMP_COVERAGE_REPORT=<file> writes the declared-versus-delivered coverage report (stable JSON, aggregate counters only) when the viewer closes */
    const char *path = SDL_getenv("SEGARECOMP_COVERAGE_REPORT");
    if (path != NULL && *path != '\0') {
      char json[36864]; /* 33 buckets of 3 components with every counter need about 24 KB (asserted in sv_coverage_test) */
      const size_t n = sv_host_coverage_json(v->host, json, sizeof json);
      FILE *f = n > 0 && n < sizeof json ? fopen(path, "wb") : NULL;
      if (f != NULL) { (void)fwrite(json, 1, n, f); (void)fputc('\n', f); (void)fclose(f); }
    }
  }
  sv_host_close(v->host); /* includes SDL_Quit */
  free(v);
}
