/* SEG-152-T003 (ADR 0320): the ABI 1 twin of the exact-frame probe, a FABRICATED ABI 1 native provider. Its `prepare` logs (stderr, `ABI1 ...`) the same digests of the SAME inputs the ABI 2 probe logs from its
 * exact-frame view, computed with the same functions, so the end-to-end test compares the real producer's ABI 1 frame inputs with what the ABI 2 provider observed. It accepts the HUD and scenery
 * components (scenery left to the generic extension) and draws nothing. Test infrastructure, not an example of provider style. */
#include <segarecomp_genesis_patch.h>

#include <stdio.h>

static unsigned calls;

static unsigned long long fnv(const void *data, unsigned n) {
  const uint8_t *p = (const uint8_t *)data;
  unsigned long long h = 1469598103934665603ULL;
  unsigned i;
  for (i = 0U; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
  return h;
}

static void prepare(void *state, const GenesisCompatFrame *f, GenesisCompatDecision *out) {
  int32_t hs = 0;
  int hs_ok = 1;
  unsigned camera = 0U, tile_word = 0U;
  (void)state;
  out->accepted = f->requested & (GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY);
  if (f->work_ram != NULL) camera = ((unsigned)f->work_ram[0x12] << 8U) | f->work_ram[0x13];
  tile_word = ((unsigned)f->vram[0x800] << 8U) | f->vram[0x801];
  if (f->line_count > 0U) hs_ok = f->host->read_hscroll(f->vram, f->lines[0].registers, 0U, 0U, &hs);
  ++calls;
  fprintf(stderr, "ABI1 n=%u s=%u lc=%u w=%u h=%u q=%u r=%016llx v=%016llx l=%016llx a=%016llx b=%016llx hs=%d cam=%u t=%u\n", calls, f->side, f->line_count, f->width, f->height, f->requested,
          f->work_ram != NULL ? fnv(f->work_ram, 65536U) : 0ULL, fnv(f->vram, 65536U), fnv(f->lines, f->line_count * (unsigned)sizeof(GenesisCompatLine)),
          f->line_count > 50U ? fnv(&f->lines[50], (unsigned)sizeof(GenesisCompatLine)) : 0ULL, f->line_count > 150U ? fnv(&f->lines[150], (unsigned)sizeof(GenesisCompatLine)) : 0ULL,
          hs_ok == 0 ? (int)hs : 99999, camera, tile_word);
}

static int scenery_word(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word) {
  (void)state; (void)plane; (void)row; (void)x; (void)hscroll; (void)vscroll; (void)word;
  return GENESIS_COMPAT_WORD_GENERIC;
}

static int hud(void *state, GenesisCompatOverlay *out) {
  (void)state; (void)out;
  return 0;
}

static const GenesisNativeCompat table = {GENESIS_NATIVE_COMPAT_ABI, "exact-frame-twin", 1U, GENESIS_COMPAT_HUD | GENESIS_COMPAT_SCENERY, prepare, scenery_word, hud};

const GenesisNativeCompat *segarecomp_patch_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &table;
}
