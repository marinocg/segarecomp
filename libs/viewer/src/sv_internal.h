#ifndef SEGARECOMP_VIEWER_SV_INTERNAL_H
#define SEGARECOMP_VIEWER_SV_INTERNAL_H

#include <SDL3/SDL.h>

#include "sv_host.h"
#include "sv_font.h"
#include "sv_input_diag.h"
#include "sv_layout.h"
#include "sv_menu.h"

#define SV_MAX_PADS 4

typedef struct SvPadSlot {
  SDL_JoystickID id;
  char name[64];
  uint32_t buttons;
  uint32_t latched;            /* presses since the previous sample (a tap shorter than a sample still counts for Pause) */
  int ax, ay;
} SvPadSlot;

/* One set of working textures of the CRT pipeline (the game picture at window size, and the live preview of the Video page at its own size). */
typedef struct SvCrtState {
  SDL_Texture *rt;
  int rt_w, rt_h;
  SDL_Texture *scan_tex;
  int scan_h, scan_src_h, scan_strength;
  SDL_Texture *mask_tex;
  int mask_w, mask_strength;
} SvCrtState;

typedef enum SvListenKind { SV_LISTEN_NONE = 0, SV_LISTEN_KEY, SV_LISTEN_PAD, SV_LISTEN_MENU_KEY } SvListenKind;

struct SvHost {
  SvHostConfig cfg;
  SvSettings settings;
  char settings_path[1024];
  int settings_dirty;
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *frame_tex;      /* RGB24 streaming picture */
  int tex_w, tex_h;
  uint8_t *last_rgb;           /* copy of the last presented picture (tightly packed RGB24) */
  int last_w, last_h;
  uint64_t frames_presented;
  SvCrtState crt, crt_preview;   /* CRT working textures: the picture, and the Video page's live preview */
  SDL_Texture *font_tex;          /* the 1-bit bitmap font atlas as an RGBA texture (built on first use) */
  SDL_Texture *wordmark;          /* assets/wordmark.png when available (the header falls back to Silkscreen text) */
  int wordmark_tried;
  int last_ui_scale;
  /* input */
  uint8_t key_held[512];
  uint8_t key_latch[512];
  SvPadSlot pads[SV_MAX_PADS];
  SvDiagHid hid[SV_DIAG_MAX_HID];
  int hid_count, hid_scanned;  /* the last on-demand OS-level Nintendo HID scan */
  SvDiag *diag;                /* the one detection engine (shared with the launcher): owns the opened gamepads */
  int pad_count;
  int quit;
  int reset;
  /* audio */
  SDL_AudioStream *audio;
  /* overlay */
  SvMenu menu;
  int menu_open;
  int last_input_pad;          /* hint footer shows gamepad cues after pad input */
  int bind_player;             /* controls page: player being edited */
  struct { SvListenKind kind; int player; SvAction action; Uint64 started; Uint64 after_ts; } listen;
  unsigned char bind_col[16];  /* Controls: the focused column (0 keyboard, 1 gamepad) per binding row, kept across page rebuilds */
  char toast[96];
  char conflict_msg[120];     /* Controls page: the last binding conflict, shown until the next change */
  SDL_Texture *icons[24];
  char icon_names[24][16];
  int icon_count;
  Uint64 session_start;
  Uint64 toast_until;
  /* pacing statistics */
  Uint64 fps_window_start;
  unsigned fps_frames;
  double fps, frame_ms;
  Uint64 last_present_ns;
  char last_screenshot[512];
  int ui_item_rect[SV_MENU_MAX_ITEMS][4]; /* x,y,w,h of the drawn rows, for the mouse */
  int ui_item_index[SV_MENU_MAX_ITEMS];   /* page item index of each drawn row */
  int ui_preset_rect[4][4];               /* the Video page's preset buttons (x,y,w,h), w == 0 when not drawn */
  int ui_rows;
  float mouse_x, mouse_y;
  int stop_screen;
  Uint64 event_ts;             /* timestamp of the event being handled (a binding listener ignores anything not strictly newer) */
  int (*hook)(void *ctx, SvHost *host, int iteration); /* test/automation: called with the finished overlay in the back buffer */
  void *hook_ctx;
};

/* Page and item identifiers shared by the page builder (sv_pages.c) and the drawer (sv_draw.c). */
enum {
  PAGE_MAIN = 1, PAGE_QUICK, PAGE_VIDEO, PAGE_CONTROLS, PAGE_AUDIO, PAGE_SYSTEM, PAGE_PADSTATUS, PAGE_CONFIRM_QUIT, PAGE_CONFIRM_RESET, PAGE_CONFIRM_VIDEO, PAGE_CONFIRM_CONTROLS
};
enum {
  I_RESUME = 1, I_GO_QUICK, I_GO_VIDEO, I_GO_CONTROLS, I_GO_AUDIO, I_GO_SYSTEM, I_QUIT,
  I_FULLSCREEN, I_SCALE, I_ASPECT, I_PRESET, I_VSYNC, I_FPS, I_SCANLINES, I_MASK, I_CURVE, I_BLOOM, I_BRIGHT, I_VIDEO_DEFAULTS,
  I_VOLUME, I_MUTE, I_AUDIO_NOTE,
  I_SHOT, I_RESET, I_INFO,
  I_PLAYER, I_PAD, I_PAD_ASSIGNED, I_DEADZONE, I_MENUKEY, I_CTRL_DEFAULTS, I_GO_PADSTATUS, I_PAD_RESCAN, I_PAD_HID,
  I_YES, I_NO,
  I_BIND = 100 /* + action (one row per action: keyboard and gamepad columns) */
};


/* sv_pages.c */
void sv_pages_open_main(SvHost *host);
void sv_pages_rebuild(SvHost *host);
void sv_pages_apply(SvHost *host, SvMenuEvent ev);
void sv_pages_draw(SvHost *host, int out_w, int out_h);
int sv_pages_wants_preview(const SvHost *host); /* true on the Video page: the picture stays undimmed */
void sv_pages_listen_key(SvHost *host, int scancode);
void sv_pages_listen_pad(SvHost *host, int button);
void sv_pages_listen_cancel(SvHost *host, const char *why);
void sv_pages_back(SvHost *host);
void sv_pages_close(SvHost *host);
int sv_pages_click(SvHost *host, float x, float y); /* handles a click on a non-row control (the preset buttons); 1 when consumed */
void sv_draw_stop_screen(SvHost *host, int out_w, int out_h, const char *title, const char *detail);

/* sv_host.c */
void sv_host_mark_dirty(SvHost *host);
void sv_host_apply_video(SvHost *host);
void sv_host_apply_audio(SvHost *host);
void sv_host_release_inputs(SvHost *host);
int sv_host_pad_assigned(const SvHost *host, int player); /* slot index or -1 */
void sv_host_toggle_fullscreen(SvHost *host);
void sv_host_take_screenshot(SvHost *host);
void sv_host_draw_text(SvHost *host, float x, float y, int scale, SDL_Color color, const char *text); /* BODY face at an integer scale (legacy entry point) */
/* sv_ui.c: bitmap-font text and stepped-frame primitives. (x, y) is the top-left of the text line box; widths come from the real glyph advances. */
void sv_ui_text(SvHost *host, SvFontId face, int scale, int x, int y, SDL_Color color, const char *text);
int sv_ui_text_w(SvFontId face, int scale, const char *text);
int sv_ui_line_h(SvFontId face, int scale);
void sv_ui_text_shadow(SvHost *host, SvFontId face, int scale, int x, int y, SDL_Color color, const char *text); /* readable over any picture */
void sv_ui_chamfer(SvHost *host, int x, int y, int w, int h, int k, SDL_Color color);   /* rectangle with k-pixel stepped corners */
void sv_ui_frame(SvHost *host, int x, int y, int w, int h, int u, SDL_Color fill, SDL_Color edge, SDL_Color light, int glow);
SDL_Texture *sv_ui_wordmark(SvHost *host);
void sv_ui_destroy(SvHost *host);
void sv_host_fill(SvHost *host, int x, int y, int w, int h, SDL_Color color);
void sv_host_bevel(SvHost *host, int x, int y, int w, int h, int step, SDL_Color fill, SDL_Color edge, SDL_Color highlight, SDL_Color shadow);
void sv_host_round_fill(SvHost *host, float x, float y, float w, float h, float r, SDL_Color color);
void sv_host_round_stroke(SvHost *host, float x, float y, float w, float h, float r, int thickness, SDL_Color color);
void sv_host_icon_draw(SvHost *host, const char *name, float x, float y, float size, SDL_Color tint);
void sv_host_draw_picture(SvHost *host, float x, float y, float w, float h); /* the last picture, aspect-fitted, no effects */
void sv_host_draw_preview(SvHost *host, int x, int y, int w, int h);        /* a centre crop of the picture with the CURRENT video settings applied (live) */
SDL_Texture *sv_host_load_asset(SvHost *host, const char *relative);

#endif
