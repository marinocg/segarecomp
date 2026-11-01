#ifndef SEGARECOMP_VIEWER_SV_HOST_H
#define SEGARECOMP_VIEWER_SV_HOST_H

/*
 * The shared SDL3 host of the in-game viewers (Genesis and Master System).
 *
 * It owns the window, the event pump, persistent settings, gamepads, the video presenter (layout, nearest/integer scaling, CRT presets),
 * the pause overlay, screenshots and playback-only audio gain. It does NOT own any guest execution: the console adapters keep their own
 * run loops and call sv_host_service() at their existing deterministic sampling points and sv_host_present() with a finished RGB frame.
 * While the pause menu is open the call to sv_host_service() simply does not return, so the guest does not advance.
 *
 * Headless targets never link this library.
 */
#include <stddef.h>
#include <stdint.h>

#include "sv_layout.h"
#include "sv_settings.h"

struct SDL_AudioStream;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SvHost SvHost;

typedef struct SvDiagnostics {
  int audio_active;            /* an audio device is open */
  uint32_t audio_queued_ms;    /* playback queue, 0 when unknown */
  uint64_t audio_dropped;      /* chunks the adapter had to drop */
  uint64_t guest_frames;       /* frames the adapter has run (0 = not reported) */
} SvDiagnostics;
typedef void (*SvDiagnosticsFn)(void *ctx, SvDiagnostics *out);

typedef struct SvHostConfig {
  SvSystem system;
  const char *title;           /* window title */
  int picture_w, picture_h;    /* nominal picture size (initial window size) */
  int max_w, max_h;            /* largest picture the adapter will present */
  int default_scale;           /* initial window = picture * scale when no window size was saved; 0 = 3 */
  int can_reset;               /* 1 only where the adapter has a proven safe reset seam */
  const char *rom_sha256;      /* optional, 64 hex characters, shown in System */
  /* SEG-123: per-game widescreen profile identity. `platform_id` ([a-z0-9-], e.g. "genesis", "master-system", "sega-cd") and `profile_id` (the full 64-hex SHA-256
   * of the game; NULL falls back to rom_sha256). Without a valid pair there is no per-game profile and the widescreen preferences are the global ones. */
  const char *platform_id;
  const char *profile_id;
  SvDiagnosticsFn diagnostics; /* optional */
  void *diagnostics_ctx;
  /* Optional (NULL = the platform extends nothing: both enhanced levels are Unavailable and the picture stays Original). Called from the
   * menu/presentation thread between guest slices; it only reports what the viewer can currently do and must not touch guest state. */
  void (*widescreen_caps)(void *ctx, SvWidescreenCaps *out);
  void *widescreen_ctx;
} SvHostConfig;

/* Opens SDL video (+ gamepad when available), the window and the renderer, and loads the persistent settings. NULL on failure. */
SvHost *sv_host_open(const SvHostConfig *config);
/* Saves settings if changed and tears everything down (including SDL_Quit). */
void sv_host_close(SvHost *host);

typedef struct SvSample {
  uint16_t pad[SV_PLAYERS]; /* SV_ACTION_BIT masks; the SMS Pause bit is true for a held press or a tap since the previous sample */
  int quit;                 /* the window was closed or Quit/Return was confirmed */
  int reset;                /* a confirmed reset (only when config.can_reset) */
  int menu_was_open;        /* the pause overlay ran during this call (the guest was not advanced meanwhile) */
} SvSample;

/* Pumps events and samples input. Opens the pause overlay (blocking) on the menu key / gamepad Back button. */
void sv_host_service(SvHost *host, SvSample *out);
/* Presents one finished frame (RGB24, `pitch` bytes per row). Returns 0 on success. */
int sv_host_present(SvHost *host, const uint8_t *rgb24, int width, int height, int pitch);
/* SEG-118: presents one finished frame like sv_host_present, optionally with a borrowed extended surface built around it (Level 2). The native picture
 * is always the authority: when `ext` is NULL or fails sv_extended_surface_check against this frame (identity, geometry, pitch, size, status, mode) the
 * original picture is presented unchanged for this frame and a bounded rejection counter is incremented; nothing from a rejected surface is read beyond
 * the check. The surface is read only during this call. CRT presets, fit/integer scaling, fullscreen and resize apply to the extended picture exactly as
 * to the original, and the native pixels in the centre are always taken from `rgb24`, never from the surface. Returns 0 on success. */
int sv_host_present_extended(SvHost *host, const uint8_t *rgb24, int width, int height, int pitch, uint64_t sequence, const SvExtendedSurface *ext);
/* Extension of the last presented frame in source pixels per side (0 = the original picture was shown), and bounded counters: surfaces accepted,
 * surfaces offered but rejected, and frame-texture (re)creations (constant at unchanged geometry). Test/diagnostic seams. */
int sv_host_extension_side(const SvHost *host);
void sv_host_extension_stats(const SvHost *host, uint32_t *accepted, uint32_t *rejected, uint32_t *texture_creations);
/* SEG-123: what the last presented frame shows at its sides: PASSTHROUGH (original picture, Raw, or an AUTHORED surface under Comfort), COMFORT or BORDERS;
 * and the provenance of the extension on screen (GENERIC when there is none). The treatment follows `video.widescreen_side_style` and the provenance
 * the producer declared, never the mode, the status or the pixels. Applied at once when the style changes; presentation only. */
SvSideTreatment sv_host_side_treatment(const SvHost *host);
/* Visible side width of the frame on screen in source pixels per side (<= the produced `sv_host_extension_side`), from `video.widescreen_width` and the native geometry. */
int sv_host_visible_side(const SvHost *host);
/* 1 when this game has a valid identity (widescreen preferences are saved per game); path of its profile file (NULL when none). */
int sv_host_profile_active(const SvHost *host);
const char *sv_host_profile_path(const SvHost *host);
SvExtSource sv_host_extension_source(const SvHost *host);
/* Re-applies the side style to the frame already shown (also done by sv_host_apply_settings and by the menu). */
void sv_host_restyle(SvHost *host);
/* Registers the adapter's playback stream so volume/mute (gain only) and pause apply; NULL unregisters. */
void sv_host_set_audio(SvHost *host, struct SDL_AudioStream *stream);
/* Shows the final state of a stopped guest instead of a blank window and waits for a key/button/close. Returns immediately on a
 * headless video driver or when SEGARECOMP_VIEWER_NO_STOP_SCREEN is set. */
void sv_host_stop_screen(SvHost *host, const char *title, const char *detail);
/* Transient on-screen message (screenshot saved, binding changed, ...). */
void sv_host_toast(SvHost *host, const char *text);
const SvSettings *sv_host_settings(const SvHost *host);
/* Widescreen. REQUESTED is `sv_host_settings(host)->video.widescreen` (the saved preference, kept even when unsupported); CAPABILITY is what
 * the platform reports per level; EFFECTIVE is the fallback Level 3 -> Level 2 -> Original of the two. None of them is visible to the guest. */
void sv_host_widescreen_caps(const SvHost *host, SvWidescreenCaps *out);
SvWidescreenMode sv_host_widescreen_effective(const SvHost *host);
/* Bounded diagnostic: how many times a requested mode was selected that the platform could not honour (saturating). */
uint32_t sv_host_widescreen_fallbacks(const SvHost *host);
/* Replaces the settings as if they were edited in the menu (applies fullscreen/VSync/gain, sanitizes, persists). For automation and tests. */
void sv_host_apply_settings(SvHost *host, const SvSettings *settings);
/* Automation seam: `hook` is called once per frame while the pause overlay (or stop screen) is shown, with the finished overlay in the
 * back buffer (sv_host_capture_output reads it). Returning nonzero closes the overlay. NULL removes it. */
void sv_host_set_loop_hook(SvHost *host, int (*hook)(void *ctx, SvHost *host, int iteration), void *ctx);
/* Test/automation seams. */
const char *sv_host_renderer_name(const SvHost *host);
/* The drawn rectangle (x, y, w, h) of the Video page's preset button `index` (0..3); returns 0 when that page is not on screen. */
int sv_host_ui_preset_rect(const SvHost *host, int index, int rect[4]);
/* The whole-number UI scale used for an output of this size in pixels (text is 13 px * scale). */
int sv_host_ui_scale(int out_w, int out_h);
int sv_host_menu_open(const SvHost *host);
/* Renders the current frame + overlays into the back buffer and returns the pixels of the output (caller frees with SDL_DestroySurface). */
struct SDL_Surface *sv_host_capture_output(SvHost *host);
/* Saves the last presented game picture (game pixels only) as PNG (BMP on older SDL); fills `path_out`. 0 == ok. */
int sv_host_screenshot(SvHost *host, char *path_out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
