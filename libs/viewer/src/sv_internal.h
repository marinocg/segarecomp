#ifndef SEGARECOMP_VIEWER_SV_INTERNAL_H
#define SEGARECOMP_VIEWER_SV_INTERNAL_H

#include <SDL3/SDL.h>

#include "sv_host.h"
#include "sv_layout.h"
#include "sv_menu.h"

#define SV_MAX_PADS 4

typedef struct SvPadSlot {
  SDL_JoystickID id;
  SDL_Gamepad *pad;
  char name[64];
  uint32_t buttons;
  uint32_t latched;            /* presses since the previous sample (a tap shorter than a sample still counts for Pause) */
  int ax, ay;
} SvPadSlot;

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
  /* CRT working textures */
  SDL_Texture *rt;
  int rt_w, rt_h;
  SDL_Texture *scan_tex;
  int scan_h, scan_src_h, scan_strength;
  SDL_Texture *mask_tex;
  int mask_w, mask_strength;
  /* input */
  uint8_t key_held[512];
  uint8_t key_latch[512];
  SvPadSlot pads[SV_MAX_PADS];
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
  char toast[96];
  Uint64 toast_until;
  /* pacing statistics */
  Uint64 fps_window_start;
  unsigned fps_frames;
  double fps, frame_ms;
  Uint64 last_present_ns;
  char last_screenshot[512];
  int ui_item_rect[SV_MENU_MAX_ITEMS][4]; /* x,y,w,h of the drawn rows, for the mouse */
  int ui_rows;
  float mouse_x, mouse_y;
  int stop_screen;
  Uint64 event_ts;             /* timestamp of the event being handled (a binding listener ignores anything not strictly newer) */
  int (*hook)(void *ctx, SvHost *host, int iteration); /* test/automation: called with the finished overlay in the back buffer */
  void *hook_ctx;
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
void sv_draw_stop_screen(SvHost *host, int out_w, int out_h, const char *title, const char *detail);

/* sv_host.c */
void sv_host_mark_dirty(SvHost *host);
void sv_host_apply_video(SvHost *host);
void sv_host_apply_audio(SvHost *host);
void sv_host_release_inputs(SvHost *host);
int sv_host_pad_assigned(const SvHost *host, int player); /* slot index or -1 */
void sv_host_toggle_fullscreen(SvHost *host);
void sv_host_take_screenshot(SvHost *host);
void sv_host_draw_text(SvHost *host, float x, float y, int scale, SDL_Color color, const char *text);
void sv_host_fill(SvHost *host, int x, int y, int w, int h, SDL_Color color);
void sv_host_bevel(SvHost *host, int x, int y, int w, int h, int step, SDL_Color fill, SDL_Color edge, SDL_Color highlight, SDL_Color shadow);
int sv_host_ui_scale(int out_w, int out_h);

#endif
