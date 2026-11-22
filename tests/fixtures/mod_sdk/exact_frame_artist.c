/* SEG-152-T003 (ADR 0320): a synthetic, project-authored exact-frame ABI 2 Genesis provider that DRAWS from the frame view, so a host test can prove that what it submitted belongs to the very frame it saw:
 * the overlay colour, the scenery word and the placement are all functions of the view's first VRAM byte and `side`. Strict C11. */
#include <segarecomp_mod_genesis.h>

#include <string.h>

typedef struct ArtistState {
  unsigned calls;
  uint64_t last_frame;
  uint32_t last_side, margin_seen, view_seen;
  uint32_t last_requested, last_flags;
  int fault_next;          /* an invalid render-API call (a range fault of the callback) */
  int decline_placement;   /* declare view_left for a margin that is NOT the frame's: the host must withhold it and the windows */
} ArtistState;
ArtistState g_artist;

static void artist_phase(void *state, const SegaModHost *host) {
  const SegaGenesisFeatures *f = (const SegaGenesisFeatures *)host->platform_features;
  const SegaGenesisFrameView *v;
  uint32_t margin = 0U;
  uint16_t word;
  (void)state;
  if (host->phase != SEGA_MOD_PHASE_RENDER) return;
  ++g_artist.calls;
  g_artist.last_frame = host->frame;
  v = f->frame;
  g_artist.view_seen = v != NULL;
  if (v == NULL || f->render == NULL) return;
  g_artist.last_side = v->side;
  g_artist.last_requested = v->requested;
  g_artist.last_flags = v->flags;
  if (g_artist.fault_next) {
    g_artist.fault_next = 0;
    (void)f->render->overlay_layer(99U, 0U, 0U, 0, 0, 8U, 8U, 0U);   /* layer index out of range: a range fault */
    return;
  }
  (void)f->render->view_margin(&margin);
  g_artist.margin_seen = margin;
  (void)f->render->overlay_rect(0, 0, 8, 8, ((uint32_t)v->vram[0] << 24U) | ((uint32_t)v->vram[1] << 16U) | 0x0000FFU);   /* colour = this frame's VRAM bytes 0 and 1 */
  (void)f->render->scene("exact", 5U);
  word = (uint16_t)(0x2000U | v->vram[0]);
  (void)f->render->scenery_window(0U, -1, 0, 1U, 1U, &word);   /* plane cell (-1, 0): screen x -8..-1 on rows 0..7 with no scroll */
  if (v->side > 0U) (void)f->render->view_left(g_artist.decline_placement ? v->side + 8U : v->side / 2U, g_artist.decline_placement ? v->side + 8U : v->side);
}

static int artist_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM ? 0 : 1;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 1U, SEGA_MOD_PLATFORM, "synthetic-exact-artist", 1U, artist_init, artist_phase, NULL, NULL, NULL,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &provider;
}
