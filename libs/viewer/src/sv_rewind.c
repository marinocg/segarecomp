#include "sv_rewind.h"

#include <stdio.h>
#include <string.h>

void sv_rewind_model_init(SvRewindModel *model, int history) {
  model->history = history < 0 ? 0 : history;
  model->selected = model->history;   /* the surface opens on NOW: the picture the player was looking at */
}

int sv_rewind_select(SvRewindModel *model, int index) {
  if (index < 0 || index > model->history || index == model->selected) return 0;
  model->selected = index;
  return 1;
}

int sv_rewind_move(SvRewindModel *model, int delta) {
  long target = (long)model->selected + (long)delta;
  if (target < 0) target = 0;
  if (target > model->history) target = model->history;   /* no wrapping from the oldest moment to the newest */
  if ((int)target == model->selected) return 0;
  model->selected = (int)target;
  return 1;
}

int sv_rewind_selection_is_moment(const SvRewindModel *model) { return model->selected >= 0 && model->selected < model->history; }

void sv_rewind_label(int64_t age_ms, int now, char *out, size_t cap) {
  if (now) { snprintf(out, cap, "NOW"); return; }
  if (age_ms < 0) age_ms = 0;
  if (age_ms < 10000) { snprintf(out, cap, "-%d.%ds", (int)(age_ms / 1000), (int)((age_ms % 1000) / 100)); return; }
  {
    const int64_t seconds = (age_ms + 500) / 1000;
    if (seconds < 100) snprintf(out, cap, "-%ds", (int)seconds);
    else snprintf(out, cap, "-%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
  }
}

static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int max_int(int a, int b) { return a > b ? a : b; }

void sv_rewind_layout(SvRewindLayout *out, int out_w, int out_h, int u, const SvRewindModel *model, int pic_w, int pic_h) {
  const int total = model->history + 1;   /* moments + NOW */
  int margin, header_h, footer_h, usable, pad, info_h, label_h, card_h, card_w, sel_w, sel_h, gap, avail_w, content_w, x_start, i, center_rel, first = -1, last = -1;
  SvRect fit;
  memset(out, 0, sizeof *out);
  if (u < 1) u = 1;
  if (out_w < 1) out_w = 1;
  if (out_h < 1) out_h = 1;
  out->out_w = out_w; out->out_h = out_h; out->u = u;
  margin = clamp_int(out_w / 40, 4, 40 * u);
  header_h = clamp_int(out_h * 8 / 100, 28, 70 * u);
  footer_h = clamp_int(out_h * 6 / 100, 28, 50 * u);
  if (footer_h < 35 && out_h >= 360) footer_h = 35;
  usable = out_h - header_h - footer_h;
  if (usable < 40) usable = 40;
  pad = max_int(usable * 2 / 100, 2);
  info_h = max_int(usable * 8 / 100, 14 * u);
  out->header = (SvRect){0, 0, out_w, header_h};
  out->footer = (SvRect){0, out_h - footer_h, out_w, footer_h};
  out->preview_area = (SvRect){margin, header_h + pad, out_w - 2 * margin, usable * 56 / 100};
  out->timeline = (SvRect){0, header_h + pad + out->preview_area.h + info_h, out_w, usable * 28 / 100};
  if (out->timeline.y + out->timeline.h > out_h - footer_h) out->timeline.h = max_int(out_h - footer_h - out->timeline.y, 12);
  /* the preview keeps the picture's native aspect: letterboxed, never stretched */
  fit = sv_layout_dest(out->preview_area.w, out->preview_area.h, pic_w > 0 ? pic_w : 320, pic_h > 0 ? pic_h : 224, SV_ASPECT_ORIGINAL, SV_SCALE_FIT);
  out->preview = (SvRect){out->preview_area.x + fit.x, out->preview_area.y + fit.y, fit.w, fit.h};
  /* cards: the selected one is ~18% larger; every card keeps the 160:112 proportion of the stored thumbnails */
  gap = 8 * u;
  label_h = 13 * u + 6 * u;
  out->label_h = label_h;
  card_h = (out->timeline.h - label_h - 6 * u) * 100 / 118;
  if (card_h < 8) card_h = 8;
  card_w = card_h * 160 / 112;
  sel_w = card_w * 118 / 100;
  sel_h = card_h * 118 / 100;
  avail_w = out_w - 2 * margin;
  {  /* at least five cards when the width allows: shrink the cards before dropping any */
    const int want = total < 5 ? total : 5;
    const int need = (want - 1) * card_w + sel_w + (want - 1) * gap;
    if (need > avail_w && avail_w > 0) {
      card_w = card_w * avail_w / need;
      gap = gap * avail_w / need;
      if (card_w < 4) card_w = 4;
      card_h = card_w * 112 / 160;
      sel_w = card_w * 118 / 100;
      sel_h = card_h * 118 / 100;
    }
  }
  content_w = total * card_w + (total - 1) * gap + (sel_w - card_w);
  center_rel = model->selected * (card_w + gap) + sel_w / 2;   /* the selected card's centre in content coordinates */
  if (content_w <= avail_w) {
    x_start = (out_w - content_w) / 2;
  } else {
    x_start = out_w / 2 - center_rel;                       /* selection centered ... */
    if (x_start > margin) x_start = margin;                 /* ... until the oldest card reaches the left margin */
    if (x_start + content_w < out_w - margin) x_start = out_w - margin - content_w;   /* ... or NOW the right margin */
  }
  {
    const int row_center = out->timeline.y + (out->timeline.h - label_h) / 2;
    out->label_y = out->timeline.y + out->timeline.h - label_h;
    for (i = 0; i < total; ++i) {
      const int sel = i == model->selected;
      const int w = sel ? sel_w : card_w, h = sel ? sel_h : card_h;
      const int x = x_start + i * (card_w + gap) + (i > model->selected ? sel_w - card_w : 0);
      if (x < margin || x + w > out_w - margin) continue;   /* a card is either fully visible or not shown: nothing is clipped */
      if (first < 0) first = i;
      last = i;
      if (out->card_count < SV_REWIND_MAX_CARDS) {
        SvRewindCard *c = &out->cards[out->card_count++];
        c->index = i;
        c->selected = sel;
        c->rect = (SvRect){x, row_center - h / 2, w, h};
      }
    }
  }
  out->more_left = first > 0;
  out->more_right = last >= 0 && last < total - 1;
  {  /* footer buttons for the mouse */
    const int bh = footer_h * 3 / 4, by = out_h - footer_h + (footer_h - bh) / 2, bw = 64 * u > out_w / 4 ? out_w / 4 : 64 * u;
    out->restore_button = (SvRect){out_w - margin - bw, by, bw, bh};
    out->cancel_button = (SvRect){out_w - margin - 2 * bw - gap, by, bw, bh};
  }
}

int sv_rewind_hit(const SvRewindLayout *layout, int x, int y) {
  int i;
  for (i = 0; i < layout->card_count; ++i) {
    const SvRect *r = &layout->cards[i].rect;
    if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return layout->cards[i].index;
  }
  return -1;
}
