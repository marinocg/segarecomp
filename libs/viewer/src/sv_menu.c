#include "sv_menu.h"

#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src) {
  size_t n = src == NULL ? 0u : strlen(src);
  if (n >= cap) n = cap - 1u;
  if (n != 0u) memcpy(dst, src, n);
  dst[n] = '\0';
}

void sv_menu_reset(SvMenu *menu) { menu->depth = 0; }

SvPage *sv_menu_top(SvMenu *menu) { return menu->depth > 0 ? &menu->stack[menu->depth - 1] : NULL; }

SvPage *sv_menu_push(SvMenu *menu, int page_id, const char *title) {
  SvPage *page;
  if (menu->depth >= SV_MENU_MAX_DEPTH) return NULL;
  page = &menu->stack[menu->depth++];
  memset(page, 0, sizeof *page);
  page->page_id = page_id;
  copy_text(page->title, sizeof page->title, title);
  return page;
}

void sv_menu_pop(SvMenu *menu) { if (menu->depth > 0) --menu->depth; }

SvItem *sv_page_add(SvPage *page, SvItemKind kind, int id, const char *label) {
  SvItem *item;
  if (page->count >= SV_MENU_MAX_ITEMS) return NULL;
  item = &page->items[page->count++];
  memset(item, 0, sizeof *item);
  item->kind = kind;
  item->id = id;
  item->enabled = kind != SV_ITEM_LABEL;
  item->step = 1;
  copy_text(item->label, sizeof item->label, label);
  return item;
}

void sv_item_set_text(SvItem *item, const char *text) { copy_text(item->text, sizeof item->text, text); }
void sv_item_set_hint(SvItem *item, const char *hint) { copy_text(item->hint, sizeof item->hint, hint); }
void sv_item_disable(SvItem *item, const char *reason) { item->enabled = 0; copy_text(item->hint, sizeof item->hint, reason); }

static int focusable(const SvItem *item) { return item->enabled && item->kind != SV_ITEM_LABEL; }

void sv_page_focus_first(SvPage *page) {
  int i;
  page->focus = 0;
  for (i = 0; i < page->count; ++i)
    if (focusable(&page->items[i])) { page->focus = i; return; }
}

void sv_page_restore_focus(SvPage *page, int item_id) {
  int i;
  for (i = 0; i < page->count; ++i)
    if (page->items[i].id == item_id && focusable(&page->items[i])) { page->focus = i; return; }
  if (page->focus >= page->count || !focusable(&page->items[page->focus])) sv_page_focus_first(page);
}

static void move_focus(SvPage *page, int direction) {
  int i = page->focus, tries;
  for (tries = 0; tries < page->count; ++tries) {
    i += direction;
    if (i < 0) i = page->count - 1;
    if (i >= page->count) i = 0;
    if (focusable(&page->items[i])) { page->focus = i; return; }
  }
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

SvMenuEvent sv_menu_navigate(SvMenu *menu, SvNav nav) {
  SvMenuEvent ev = {SV_EVENT_NONE, 0, 0};
  SvPage *page = sv_menu_top(menu);
  SvItem *item;
  if (page == NULL || nav == SV_NAV_NONE) return ev;
  if (nav == SV_NAV_BACK) { ev.kind = SV_EVENT_BACK; return ev; }
  if (page->count == 0) return ev;
  if (nav == SV_NAV_UP) { move_focus(page, -1); return ev; }
  if (nav == SV_NAV_DOWN) { move_focus(page, 1); return ev; }
  item = &page->items[page->focus];
  if (!focusable(item)) return ev;
  ev.item_id = item->id;
  switch (item->kind) {
    case SV_ITEM_BUTTON: case SV_ITEM_BINDING:
      if (nav == SV_NAV_ACCEPT) ev.kind = SV_EVENT_ACTIVATE;
      break;
    case SV_ITEM_TOGGLE:
      if (nav == SV_NAV_ACCEPT || nav == SV_NAV_LEFT || nav == SV_NAV_RIGHT) { item->value = !item->value; ev.kind = SV_EVENT_CHANGED; ev.value = item->value; }
      break;
    case SV_ITEM_CHOICE: {
      const int count = item->max - item->min + 1;
      int next = item->value;
      if (count < 2) break;
      if (nav == SV_NAV_LEFT) next = item->value > item->min ? item->value - 1 : item->max;
      else if (nav == SV_NAV_RIGHT || nav == SV_NAV_ACCEPT) next = item->value < item->max ? item->value + 1 : item->min;
      if (next != item->value) { item->value = next; ev.kind = SV_EVENT_CHANGED; ev.value = next; }
      break;
    }
    case SV_ITEM_SLIDER: {
      int next = item->value;
      if (nav == SV_NAV_LEFT) next = clamp(item->value - item->step, item->min, item->max);
      else if (nav == SV_NAV_RIGHT) next = clamp(item->value + item->step, item->min, item->max);
      if (next != item->value) { item->value = next; ev.kind = SV_EVENT_CHANGED; ev.value = next; }
      break;
    }
    case SV_ITEM_LABEL: break;
  }
  return ev;
}

SvMenuEvent sv_menu_pick(SvMenu *menu, int index, int activate) {
  SvMenuEvent ev = {SV_EVENT_NONE, 0, 0};
  SvPage *page = sv_menu_top(menu);
  if (page == NULL || index < 0 || index >= page->count || !focusable(&page->items[index])) return ev;
  page->focus = index;
  if (activate) {
    const SvItem *item = &page->items[index];
    if (item->kind == SV_ITEM_BUTTON || item->kind == SV_ITEM_BINDING || item->kind == SV_ITEM_TOGGLE || item->kind == SV_ITEM_CHOICE) return sv_menu_navigate(menu, SV_NAV_ACCEPT);
  }
  return ev;
}

void sv_menu_ensure_visible(SvPage *page, int visible) {
  if (visible < 1) visible = 1;
  if (page->focus < page->scroll) page->scroll = page->focus;
  if (page->focus >= page->scroll + visible) page->scroll = page->focus - visible + 1;
  if (page->scroll < 0) page->scroll = 0;
  if (page->count <= visible) page->scroll = 0;
}
