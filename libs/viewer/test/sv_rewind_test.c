/* SEG-131 Checkpoint F (ADR 0278): the pure model and geometry of the rewind surface, with no window and no SDL: selection bounds (no wrapping), the labels, and the
 * layout at the review sizes (720x520 minimum, 1080x740, 1440x1040, 2560x1440): every region and card inside the output, the documented proportions, at least five
 * cards when the width allows, the selected card larger and centered where the timeline is long enough. Strict C11 like the code under test. */
#include <stdio.h>
#include <string.h>

#include "sv_rewind.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static int inside(const SvRect *r, int w, int h) { return r->w > 0 && r->h > 0 && r->x >= 0 && r->y >= 0 && r->x + r->w <= w && r->y + r->h <= h; }

static void labels(void) {
  char out[24];
  sv_rewind_label(0, 1, out, sizeof out);
  CHECK(strcmp(out, "NOW") == 0, "NOW label");
  sv_rewind_label(3500, 0, out, sizeof out);
  CHECK(strcmp(out, "-3.5s") == 0, "-3.5s");
  sv_rewind_label(500, 0, out, sizeof out);
  CHECK(strcmp(out, "-0.5s") == 0, "-0.5s");
  sv_rewind_label(12000, 0, out, sizeof out);
  CHECK(strcmp(out, "-12s") == 0, "-12s");
  sv_rewind_label(28400, 0, out, sizeof out);
  CHECK(strcmp(out, "-28s") == 0, "-28s");
  sv_rewind_label(65000, 0, out, sizeof out);
  CHECK(strcmp(out, "-65s") == 0, "-65s");
  sv_rewind_label(125000, 0, out, sizeof out);
  CHECK(strcmp(out, "-2:05") == 0, "-2:05");
  sv_rewind_label(-5, 0, out, sizeof out);
  CHECK(strcmp(out, "-0.0s") == 0, "a negative age is clamped");
}

static void model(void) {
  SvRewindModel m;
  sv_rewind_model_init(&m, 10);
  CHECK(m.selected == 10 && !sv_rewind_selection_is_moment(&m), "opens on NOW");
  CHECK(sv_rewind_move(&m, -1) == 1 && m.selected == 9 && sv_rewind_selection_is_moment(&m), "left selects the previous moment");
  CHECK(sv_rewind_move(&m, 1) == 1 && m.selected == 10, "right returns to NOW");
  CHECK(sv_rewind_move(&m, 1) == 0 && m.selected == 10, "no wrapping past NOW to the oldest moment");
  CHECK(sv_rewind_move(&m, -SV_REWIND_JUMP) == 1 && m.selected == 5, "a jump moves several entries");
  CHECK(sv_rewind_move(&m, -100) == 1 && m.selected == 0, "a long jump stops at the oldest moment");
  CHECK(sv_rewind_move(&m, -1) == 0 && m.selected == 0, "no wrapping past the oldest moment to NOW");
  CHECK(sv_rewind_select(&m, 7) == 1 && m.selected == 7 && sv_rewind_select(&m, 11) == 0 && sv_rewind_select(&m, -1) == 0 && m.selected == 7, "select validates its index");
  sv_rewind_model_init(&m, 0);
  CHECK(m.selected == 0 && !sv_rewind_selection_is_moment(&m) && sv_rewind_move(&m, -1) == 0, "an empty history has only NOW");
  sv_rewind_model_init(&m, -3);
  CHECK(m.history == 0, "a negative history is clamped");
}

static void check_layout(int w, int h, int u, int history, int selected, const char *what) {
  SvRewindModel m;
  SvRewindLayout L;
  char msg[160];
  int i;
  sv_rewind_model_init(&m, history);
  sv_rewind_select(&m, selected);
  sv_rewind_layout(&L, w, h, u, &m, 320, 224);
  snprintf(msg, sizeof msg, "%s: header, preview, timeline, footer and buttons lie inside the output", what);
  CHECK(inside(&L.header, w, h) && inside(&L.preview, w, h) && inside(&L.timeline, w, h) && inside(&L.footer, w, h) && inside(&L.restore_button, w, h) && inside(&L.cancel_button, w, h), msg);
  snprintf(msg, sizeof msg, "%s: header above preview above timeline above footer", what);
  CHECK(L.header.y + L.header.h <= L.preview.y && L.preview.y + L.preview.h <= L.timeline.y && L.timeline.y + L.timeline.h <= L.footer.y, msg);
  snprintf(msg, sizeof msg, "%s: the preview keeps the native 320:224 aspect (letterboxed, never stretched)", what);
  CHECK(L.preview.w * 224 >= L.preview.h * 320 - 320 && L.preview.w * 224 <= L.preview.h * 320 + 320, msg);
  snprintf(msg, sizeof msg, "%s: the Cancel and Restore buttons do not overlap", what);
  CHECK(L.cancel_button.x + L.cancel_button.w <= L.restore_button.x, msg);
  {
    int previous_right = -1, selected_seen = 0, selected_w = 0, other_w = 0;
    for (i = 0; i < L.card_count; ++i) {
      const SvRect *r = &L.cards[i].rect;
      snprintf(msg, sizeof msg, "%s: card %d is fully inside the output and inside the timeline band", what, i);
      CHECK(inside(r, w, h) && r->y >= L.timeline.y && r->y + r->h <= L.timeline.y + L.timeline.h, msg);
      snprintf(msg, sizeof msg, "%s: cards are ordered left to right and never overlap", what);
      CHECK(r->x > previous_right, msg);
      previous_right = r->x + r->w;
      if (L.cards[i].selected) { selected_seen = 1; selected_w = r->w; } else other_w = r->w;
    }
    snprintf(msg, sizeof msg, "%s: the selected card is visible and larger than the others", what);
    CHECK(selected_seen && (L.card_count == 1 || selected_w > other_w), msg);
  }
  if (history + 1 >= 5 && w >= 480) {
    snprintf(msg, sizeof msg, "%s: at least five cards are shown (%d)", what, L.card_count);
    CHECK(L.card_count >= 5, msg);
  }
  {
    int sum = 0, expected = 0;
    for (i = 0; i < L.card_count; ++i) sum += L.cards[i].index;
    (void)sum;
    expected = history + 1;
    snprintf(msg, sizeof msg, "%s: the chevrons say whether more cards exist (visible %d of %d)", what, L.card_count, expected);
    CHECK((L.card_count == expected) == (!L.more_left && !L.more_right), msg);
  }
}

static void layouts(void) {
  static const int sizes[][3] = {{720, 520, 1}, {1080, 740, 1}, {1440, 1040, 2}, {2560, 1440, 3}};
  size_t i;
  for (i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
    char what[64];
    const int w = sizes[i][0], h = sizes[i][1], u = sizes[i][2];
    snprintf(what, sizeof what, "%dx%d", w, h);
    check_layout(w, h, u, 0, 0, what);        /* only NOW */
    check_layout(w, h, u, 1, 0, what);        /* one moment, selected */
    check_layout(w, h, u, 3, 3, what);        /* short history, NOW selected */
    check_layout(w, h, u, 119, 119, what);    /* a full minute: NOW */
    check_layout(w, h, u, 119, 60, what);     /* middle */
    check_layout(w, h, u, 119, 0, what);      /* oldest */
    check_layout(w, h, u, 119, 118, what);    /* newest moment */
  }
  {  /* the documented proportions at 1080x740 */
    SvRewindModel m;
    SvRewindLayout L;
    int usable;
    sv_rewind_model_init(&m, 40);
    sv_rewind_layout(&L, 1080, 740, 1, &m, 320, 224);
    usable = 740 - L.header.h - L.footer.h;
    CHECK(L.header.h >= 50 && L.header.h <= 65, "1080x740: the header is 50-65 px");
    CHECK(L.footer.h >= 35 && L.footer.h <= 50, "1080x740: the footer is 35-50 px");
    CHECK(L.preview_area.h * 100 >= usable * 55 && L.preview_area.h * 100 <= usable * 60, "1080x740: the preview takes the upper 55-60% of the usable height");
    CHECK(L.timeline.h * 100 >= usable * 25 && L.timeline.h * 100 <= usable * 30, "1080x740: the timeline takes 25-30%");
  }
  {  /* the selection is centered when the timeline is long enough, and the strip stops sliding at its ends */
    SvRewindModel m;
    SvRewindLayout L;
    int i, center = -1;
    sv_rewind_model_init(&m, 119);
    sv_rewind_select(&m, 60);
    sv_rewind_layout(&L, 1080, 740, 1, &m, 320, 224);
    for (i = 0; i < L.card_count; ++i) if (L.cards[i].selected) center = L.cards[i].rect.x + L.cards[i].rect.w / 2;
    CHECK(center >= 1080 / 2 - 3 && center <= 1080 / 2 + 3, "a selection in the middle of a long timeline is centered");
    sv_rewind_select(&m, 0);
    sv_rewind_layout(&L, 1080, 740, 1, &m, 320, 224);
    for (i = 0; i < L.card_count; ++i) if (L.cards[i].selected) center = L.cards[i].rect.x + L.cards[i].rect.w / 2;
    CHECK(center < 1080 / 2 && L.cards[0].index == 0 && !L.more_left && L.more_right, "the oldest moment sits at the left edge, nothing hidden on its left");
    sv_rewind_select(&m, 119);
    sv_rewind_layout(&L, 1080, 740, 1, &m, 320, 224);
    CHECK(L.cards[L.card_count - 1].index == 119 && L.more_left && !L.more_right, "NOW sits at the right edge, nothing hidden on its right");
    CHECK(sv_rewind_hit(&L, L.cards[0].rect.x + 1, L.cards[0].rect.y + 1) == L.cards[0].index && sv_rewind_hit(&L, -5, -5) == -1, "hit testing picks the card under the pointer");
  }
  {  /* a very small output never produces an out-of-range layout */
    SvRewindModel m;
    SvRewindLayout L;
    sv_rewind_model_init(&m, 30);
    sv_rewind_layout(&L, 200, 120, 1, &m, 320, 224);
    CHECK(inside(&L.footer, 200, 120) && L.card_count >= 1, "a tiny output still has a footer and a card, inside the output");
    sv_rewind_layout(&L, 0, 0, 0, &m, 0, 0);
    CHECK(L.out_w == 1 && L.out_h == 1, "degenerate sizes are clamped");
  }
}

int main(void) {
  labels();
  model();
  layouts();
  printf(failures == 0 ? "sv_rewind_test: ok\n" : "sv_rewind_test: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
