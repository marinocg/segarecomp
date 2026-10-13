#ifndef SEGARECOMP_VIEWER_SV_MENU_H
#define SEGARECOMP_VIEWER_SV_MENU_H

/*
 * Pure, SDL-free menu model of the viewer. The host builds a page from the current settings, feeds navigation events, and applies the
 * returned event to the settings; this file only knows about focus movement, value stepping and the page stack. That keeps keyboard and
 * gamepad navigation testable with synthetic events.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define SV_MENU_MAX_ITEMS 40
#define SV_MENU_MAX_DEPTH 6

typedef enum SvItemKind { SV_ITEM_BUTTON = 0, SV_ITEM_TOGGLE, SV_ITEM_CHOICE, SV_ITEM_SLIDER, SV_ITEM_LABEL, SV_ITEM_BINDING } SvItemKind;

typedef struct SvItem {
  SvItemKind kind;
  int id;               /* host-defined identifier */
  int enabled;          /* disabled items are skipped by focus movement and say why in `hint` */
  int value;            /* toggle 0/1, choice index, slider value */
  int min, max, step;   /* choice: 0..count-1; slider range */
  char label[40];
  char text[48];        /* value text shown at the right (choice name, binding, label text) */
  char hint[112];       /* one-line explanation shown while focused (always filled for a disabled item) */
} SvItem;

typedef struct SvPage {
  int page_id;
  char title[32];
  SvItem items[SV_MENU_MAX_ITEMS];
  int count;
  int focus;
  int scroll;           /* first visible item (kept by the drawer through sv_menu_ensure_visible) */
} SvPage;

typedef enum SvNav { SV_NAV_NONE = 0, SV_NAV_UP, SV_NAV_DOWN, SV_NAV_LEFT, SV_NAV_RIGHT, SV_NAV_ACCEPT, SV_NAV_BACK } SvNav;

typedef enum SvEventKind { SV_EVENT_NONE = 0, SV_EVENT_CHANGED, SV_EVENT_ACTIVATE, SV_EVENT_BACK } SvEventKind;
typedef struct SvMenuEvent { SvEventKind kind; int item_id; int value; } SvMenuEvent;

typedef struct SvMenu {
  SvPage stack[SV_MENU_MAX_DEPTH];
  int depth; /* number of pages on the stack, 0 == closed */
} SvMenu;

void sv_menu_reset(SvMenu *menu);
SvPage *sv_menu_top(SvMenu *menu);
/* Pushes an empty page (returns it) or NULL when the stack is full. */
SvPage *sv_menu_push(SvMenu *menu, int page_id, const char *title);
void sv_menu_pop(SvMenu *menu);
/* Appends an item to a page; returns it, or NULL when full. */
SvItem *sv_page_add(SvPage *page, SvItemKind kind, int id, const char *label);
void sv_item_set_text(SvItem *item, const char *text);
void sv_item_set_hint(SvItem *item, const char *hint);
void sv_item_disable(SvItem *item, const char *reason);
/* Keeps the same logical focus (by item id) across a rebuild of the page. */
void sv_page_restore_focus(SvPage *page, int item_id);
/* Moves the first focus onto an enabled item. */
void sv_page_focus_first(SvPage *page);
/* Applies one navigation step to the top page. Slider and choice items change on LEFT/RIGHT; toggles on LEFT/RIGHT/ACCEPT; ACCEPT on
 * a button or binding activates; BACK reports SV_EVENT_BACK (the host decides whether that pops or closes). The new value is written
 * into the item and reported so the host can persist it. */
SvMenuEvent sv_menu_navigate(SvMenu *menu, SvNav nav);
/* Mouse/touch: focus (and optionally activate) the item at `index`. */
SvMenuEvent sv_menu_pick(SvMenu *menu, int index, int activate);
/* Scroll so that the focused item lies in a window of `visible` rows. */
void sv_menu_ensure_visible(SvPage *page, int visible);

#ifdef __cplusplus
}
#endif
#endif
