/* SEG-152-T005/T006 (ADR 0320 decisions 6 and 7): a synthetic, project-authored exact-frame ABI 2 Genesis provider that replays a SCRIPT of render-API calls written by the host test (a bitmap, a layer, a clip, a
 * fallback, a decline, a rect, a text, a scene, a scenery window or a virtual-sprite set), so the test can drive the real viewer adapter with exactly the call sequence it wants to check. Strict C11. */
#include <segarecomp_mod_genesis.h>

#include <string.h>

enum { OP_RECT = 1, OP_TEXT, OP_BITMAP, OP_TARGET, OP_LAYER, OP_LAYER_BITMAP, OP_CLIP, OP_FALLBACK, OP_DECLINE, OP_SCENE, OP_WINDOW, OP_SPRITES, OP_LAYER_LEGACY_RAW };

typedef struct ScriptOp {
  uint32_t kind;
  int32_t a[8];
  const uint8_t *px; /* bitmap pixels / text / scene bytes */
  uint32_t len;
} ScriptOp;

typedef struct OverlayScript {
  unsigned count, calls, ran;
  uint64_t last_frame;
  uint32_t has_render_api, has_frame;
  int statuses[96];
  ScriptOp ops[96];
} OverlayScript;
OverlayScript g_script;

static GenesisVirtualSpriteSet g_set;

static void script_phase(void *state, const SegaModHost *host) {
  const SegaGenesisFeatures *f = (const SegaGenesisFeatures *)host->platform_features;
  const SegaGenesisRenderApi *r;
  unsigned i;
  (void)state;
  if (host->phase != SEGA_MOD_PHASE_RENDER) return;
  ++g_script.calls;
  g_script.last_frame = host->frame;
  r = f != NULL ? f->render : NULL;
  g_script.has_render_api = r != NULL;
  g_script.has_frame = f != NULL && f->frame != NULL;
  if (r == NULL) return;
  g_script.ran = 0U;
  for (i = 0U; i < g_script.count; ++i) {
    const ScriptOp *o = &g_script.ops[i];
    int st = 0;
    switch (o->kind) {
      case OP_RECT: st = r->overlay_rect(o->a[0], o->a[1], o->a[2], o->a[3], (uint32_t)o->a[4]); break;
      case OP_TEXT: st = r->overlay_text(o->a[0], o->a[1], (const char *)o->px, o->len, (uint32_t)o->a[2]); break;
      case OP_BITMAP: st = r->overlay_bitmap(o->a[0], o->a[1], (uint32_t)o->a[2], (uint32_t)o->a[3], o->px, (uint32_t)o->a[4]); break;
      case OP_TARGET: st = r->overlay_target((uint32_t)o->a[0]); break;
      case OP_LAYER: st = r->overlay_layer((uint32_t)o->a[0], (uint32_t)o->a[1], (uint32_t)o->a[2], o->a[3], o->a[4], (uint32_t)o->a[5], (uint32_t)o->a[6], (uint32_t)o->a[7]); break;
      case OP_LAYER_BITMAP: st = r->overlay_layer_bitmap((uint32_t)o->a[0], (uint32_t)o->a[1], (uint32_t)o->a[2], o->a[3], o->a[4], (uint32_t)o->a[5], (uint32_t)o->a[6], (uint32_t)o->a[7]); break;
      case OP_CLIP: st = r->overlay_layer_clip((uint32_t)o->a[0], (uint32_t)o->a[1], (uint32_t)o->a[2], (uint32_t)o->a[3], (uint32_t)o->a[4]); break;
      case OP_FALLBACK: st = r->overlay_fallback((uint32_t)o->a[0], (uint32_t)o->a[1]); break;
      case OP_DECLINE: st = r->decline((uint32_t)o->a[0], (uint32_t)o->a[1]); break;
      case OP_SCENE: st = r->scene((const char *)o->px, o->len); break;
      case OP_WINDOW: {
        uint16_t word = (uint16_t)o->a[3];
        st = r->scenery_window((uint32_t)o->a[0], o->a[1], o->a[2], 1U, 1U, &word);
        break;
      }
      case OP_SPRITES:
        memset(&g_set, 0, sizeof g_set);
        g_set.count = 1U;
        g_set.sprites[0].provenance = GENESIS_VSPRITE_MOD_AUTHORED;
        g_set.sprites[0].width_cells = 1U;
        g_set.sprites[0].height_cells = 1U;
        st = r->virtual_sprites(&g_set);
        break;
      default: break;
    }
    g_script.statuses[i] = st;
    g_script.ran = i + 1U;
    if (st != 0) return;
  }
}

static int script_init(void *state, const SegaModHost *host) {
  (void)state;
  return host->platform == SEGA_MOD_PLATFORM ? 0 : 1;
}

static const SegaModProvider provider = {sizeof(SegaModProvider), SEGA_MOD_ABI_MAJOR, 1U, SEGA_MOD_PLATFORM, "synthetic-overlay-script", 1U, script_init, script_phase, NULL, NULL, NULL,
                                         {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

const SegaModProvider *segarecomp_mod_provider(void **state_out) {
  if (state_out != NULL) *state_out = NULL;
  return &provider;
}
