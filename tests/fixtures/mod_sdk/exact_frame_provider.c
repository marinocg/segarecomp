/* SEG-152-T003/T004 (ADR 0320): a synthetic, project-authored ABI 2 Genesis provider that records what the exact-frame view and the VDP helpers show it. Strict C11, warnings as errors in the test build.
 * It is a pure observer: no guest writes, no output. The host test reads `g_exact_obs` after each callback. */
#include <segarecomp_mod_genesis.h>

#include <string.h>

#include "exact_frame_observation.h"


ExactObservation g_exact_obs;
unsigned g_exact_required_minor = 1U;
static SegaModProvider g_provider;

static uint64_t fnv(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = UINT64_C(1469598103934665603);
  size_t i;
  for (i = 0; i < n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}

static int provider_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM && host->platform_features != NULL ? 0 : 1;
}

static void provider_phase(void *state, const SegaModHost *host) {
  const SegaGenesisFeatures *features = (const SegaGenesisFeatures *)host->platform_features;
  const SegaGenesisFrameView *view;
  (void)state;
  if (host->phase != SEGA_MOD_PHASE_RENDER) {
    ++g_exact_obs.other_phase_calls;
    if (features->frame != NULL) ++g_exact_obs.frame_ptr_outside_render;
    return;
  }
  ++g_exact_obs.render_calls;
  g_exact_obs.last_render_frame = host->frame;
  if (g_exact_obs.light) return;
  view = features->frame;
  g_exact_obs.kept = view;
  g_exact_obs.vdp_helpers_present = features->vdp != NULL;
  g_exact_obs.view_seen = view != NULL;
  if (g_exact_obs.fault_next) {
    uint32_t v = 0U;
    g_exact_obs.fault_next = 0;
    (void)host->read(SEGA_GEN_SPACE_VDP_VRAM, 0U, 1U, &v);   /* the VDP spaces are never readable through read(): a fault of the callback */
    return;
  }
  if (view == NULL) return;
  g_exact_obs.size = view->size;
  g_exact_obs.width = view->width;
  g_exact_obs.height = view->height;
  g_exact_obs.side = view->side;
  g_exact_obs.requested = view->requested;
  g_exact_obs.flags = view->flags;
  g_exact_obs.line_count = view->line_count;
  g_exact_obs.work_ram_present = view->work_ram != NULL;
  g_exact_obs.ram_hash = view->work_ram != NULL ? fnv(view->work_ram, 65536U) : 0U;
  g_exact_obs.vram_hash = view->vram != NULL ? fnv(view->vram, 65536U) : 0U;
  g_exact_obs.lines_hash = view->lines != NULL ? fnv(view->lines, (size_t)view->line_count * sizeof(SegaGenesisFrameLine)) : 0U;
  g_exact_obs.vram_ptr = view->vram;
  g_exact_obs.ram_read_status = host->read(SEGA_GEN_SPACE_M68K_WORK_RAM, 0x10U, 1U, &g_exact_obs.ram_read_value);
  if (features->vdp != NULL && view->lines != NULL && view->line_count > 0U && view->vram != NULL) {   /* T004: the helpers on this frame's own line 0 */
    const SegaGenesisFrameLine *l0 = &view->lines[0];
    g_exact_obs.hscroll_a = features->vdp->read_hscroll(view->vram, l0->registers, 0U, 0U, &g_exact_obs.hscroll_a_value);
    g_exact_obs.hscroll_b = features->vdp->read_hscroll(view->vram, l0->registers, 1U, 0U, &g_exact_obs.hscroll_b_value);
    g_exact_obs.vscroll_a = features->vdp->read_vscroll(l0->vsram, l0->registers, 0U, 0U, &g_exact_obs.vscroll_a_value);
    g_exact_obs.vscroll_b = features->vdp->read_vscroll(l0->vsram, l0->registers, 1U, 0U, &g_exact_obs.vscroll_b_value);
    g_exact_obs.per_column = features->vdp->vscroll_is_per_column(l0->registers);
    g_exact_obs.rgb_status = features->vdp->cram_rgb(l0->cram, 5U, g_exact_obs.rgb);
  }
}

/* The test chooses required_minor (1 = the exact-frame provider; 0 = a provider written before the minor existed). */
const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  memset(&g_provider, 0, sizeof g_provider);
  g_provider.size = sizeof(SegaModProvider);
  g_provider.abi_major = SEGA_MOD_ABI_MAJOR;
  g_provider.required_minor = g_exact_required_minor;
  g_provider.platform = SEGA_MOD_PLATFORM;
  g_provider.name = "synthetic-exact-frame";
  g_provider.implementation_version = 1U;
  g_provider.init = provider_init;
  g_provider.phase = provider_phase;
  if (state_out != NULL) *state_out = NULL;
  return &g_provider;
}

const SegaModProvider *exact_frame_provider(void **state_out) { return segarecomp_mod_provider(state_out); }
