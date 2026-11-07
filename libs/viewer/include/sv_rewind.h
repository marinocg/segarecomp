#ifndef SEGARECOMP_VIEWER_SV_REWIND_H
#define SEGARECOMP_VIEWER_SV_REWIND_H

/*
 * SEG-131 (ADR 0278): the pure (SDL-free) model and layout of the visual rewind surface -- a Switch-style timeline of real historical pictures.
 *
 * The timeline holds `history` moments (index 0 = oldest ... history-1 = newest) followed by the NOW card (index `history`), left to right. The model owns the
 * selection and its bounds (no wrapping), the labels (guest-time offsets, never wall-clock) and the geometry of every region and visible card for a given output
 * size, so the layout is testable at 720x520, 1080x740, 1440x1040 and 2560x1440 without a window. Drawing and input live in the host.
 */
#include <stddef.h>
#include <stdint.h>

#include "sv_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SV_REWIND_MAX_CARDS 16
#define SV_REWIND_JUMP 5

typedef struct SvRewindModel {
  int history;   /* moments older than now */
  int selected;  /* 0..history; `history` is the NOW card */
} SvRewindModel;

void sv_rewind_model_init(SvRewindModel *model, int history);
/* Moves the selection by `delta` cards, clamped to [0, history] (the timeline never wraps from the oldest moment to the newest). Returns 1 when it changed. */
int sv_rewind_move(SvRewindModel *model, int delta);
/* Selects `index` when it is a valid card; returns 1 when the selection changed. */
int sv_rewind_select(SvRewindModel *model, int index);
/* The selection is a historical moment (not NOW): confirming it restores. */
int sv_rewind_selection_is_moment(const SvRewindModel *model);

/* "NOW", "-0.5s", "-3.5s", "-12s", "-1:05" for an age in guest milliseconds (ASCII only; the pixel font has no U+2212). `now` selects "NOW". */
void sv_rewind_label(int64_t age_ms, int now, char *out, size_t cap);

typedef struct SvRewindCard {
  int index;      /* timeline index (history = NOW) */
  SvRect rect;    /* the card picture */
  int selected;
} SvRewindCard;

typedef struct SvRewindLayout {
  int out_w, out_h, u;
  SvRect header, preview_area, preview, timeline, footer;
  int label_y, label_h;                 /* labels sit under the cards, inside the timeline region */
  SvRewindCard cards[SV_REWIND_MAX_CARDS];
  int card_count;
  int more_left, more_right;            /* cards exist beyond the visible window on that side (chevrons) */
  SvRect restore_button, cancel_button; /* footer buttons for the mouse */
} SvRewindLayout;

/* Geometry for an output of out_w x out_h pixels and UI scale `u`, for the model's state, with the preview showing a picture of pic_w x pic_h (aspect only).
 * Header 50-65 px (scaled), preview in the upper ~58% of the usable height, timeline in the lower ~28%, footer 35-50 px; at least five cards when the width
 * allows (cards shrink before they would be dropped), the selected card larger, the selection centered where the timeline is long enough. Nothing is placed
 * outside the output. */
void sv_rewind_layout(SvRewindLayout *out, int out_w, int out_h, int u, const SvRewindModel *model, int pic_w, int pic_h);
/* The card under (x, y), or -1. */
int sv_rewind_hit(const SvRewindLayout *layout, int x, int y);

#ifdef __cplusplus
}
#endif
#endif
