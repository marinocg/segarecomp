#ifndef SEGARECOMP_VIEWER_SV_LAYOUT_H
#define SEGARECOMP_VIEWER_SV_LAYOUT_H

/* Pure presentation math (SDL-free): destination rectangle of the game picture, and the CRT pixel profiles. */
#include <stddef.h>
#include <stdint.h>

#include "sv_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SvRect { int x, y, w, h; } SvRect;

/* Largest rectangle for a `src_w` x `src_h` picture inside a `win_w` x `win_h` output, centred.
 *  SV_ASPECT_ORIGINAL: square pixels (the picture keeps src_w:src_h).
 *  SV_ASPECT_4_3: the picture is stretched horizontally to a 4:3 display aspect (the TV picture of both consoles).
 *  SV_SCALE_FIT: any size that fits.  SV_SCALE_INTEGER: the vertical scale is a whole multiple (horizontal follows the aspect);
 *  when the output is smaller than 1x it falls back to fit. A zero or negative size returns an empty rectangle. */
SvRect sv_layout_dest(int win_w, int win_h, int src_w, int src_h, SvAspect aspect, SvScaleMode scale);

/* ---- SEG-118: presentation-only 16:9 extension geometry (pure; no SDL, no platform knowledge) ----
 * The extended surface is the native picture plus `side` source pixels of extra scenery on EACH side (always symmetric, so the native picture is
 * exactly centred). The display aspect is 16:9; a window of another shape only letterboxes it. Native pixels keep the aspect policy: SV_ASPECT_ORIGINAL
 * shows square pixels, SV_ASPECT_4_3 stretches the native picture to 4:3 and the extension gets the same pixel shape (4:3 content is never stretched to 16:9). */
#define SV_EXT_LEGACY_SIDE 256 /* SEG-139: the clamp the original 4:3..16:9 functions below always applied (kept bit-for-bit for existing settings) */
#define SV_EXT_MAX_SIDE 672    /* SEG-139: hard bound of one side's extension in source pixels = SV_GEOM_MAX_SIDE (the planner below); documents the maximum allocation */

/* Source pixels of extension per side for a `src_w` x `src_h` native picture so the whole surface displays at 16:9, clamped to [0, SV_EXT_LEGACY_SIDE].
 * 0 for an invalid size or a native picture already at least as wide as 16:9 (extension adds nothing). Depends only on the native geometry and the
 * aspect policy, never on the window, so a window resize does not change what the producer must make. */
int sv_layout_extension_side(int src_w, int src_h, SvAspect aspect);
/* SEG-123 amendment: VISIBLE side width per side in source pixels for the user's target aspect `width_pct` (0 = 4:3 .. 100 = 16:9, sv_widescreen_width_ratio),
 * never more than `produced_side` (what the validated producer supplied) and never more than SV_EXT_LEGACY_SIDE. 0 when the native display aspect is already at least as
 * wide as the target (the picture is never shrunk or stretched; report the effective aspect with sv_ratio_milli). At width 100 and produced_side >= the 16:9 side this is
 * exactly sv_layout_extension_side. Symmetric by construction. */
int sv_layout_visible_side(int src_w, int src_h, SvAspect aspect, int width_pct, int produced_side);
/* Largest extended surface any picture up to `max_w` x `max_h` can need: width max_w + 2 * SV_EXT_MAX_SIDE, height max_h (0 for invalid input). */
void sv_layout_extension_max(int max_w, int max_h, int *width_out, int *height_out);

typedef struct SvExtendedLayout {
  SvRect surface; /* where the whole extended surface is drawn inside the window (centred, letterboxed) */
  SvRect native;  /* where the native picture lies inside the window (centred on `surface`, same scale) */
} SvExtendedLayout;
/* Placement of an extended surface (`side` pixels each side of a `src_w` x `src_h` native picture) in a `win_w` x `win_h` output. Same scale rules as
 * sv_layout_dest (fit, or whole-number vertical scale with the fit fallback below 1x), applied to the extended surface so the native picture keeps the
 * very scale it would have had with the extension. Returns 0, or -1 (both rectangles empty) for any invalid size or side. */
int sv_layout_extended_dest(int win_w, int win_h, int src_w, int src_h, int side, SvAspect aspect, SvScaleMode scale, SvExtendedLayout *out);

/* The borrowed extended surface a platform adapter hands to the host for ONE frame (never retained, never written by the host). The native picture sits
 * `side` pixels in from the left edge and `side` from the right edge; `sequence` is the frame identity the producer stamped, `mode` the Widescreen
 * mode that was actually produced for that frame (Level 2 = background only, Level 3 = background + sprites; never merely the saved preference), `status` the typed outcome (UNAVAILABLE = ignore the surface). */
/* SEG-123: where the extra side pixels come from, for PRESENTATION only (it never grants memory access or relaxes sv_extended_surface_check).
 * GENERIC (0, the default of a zero-initialised surface): hardware-derived Level 2/3 data whose visual coherence is not guaranteed.
 * AUTHORED: extended content explicitly supplied by a verified, opt-in widescreen compatibility implementation. Never inferred from mode, status,
 * pixels, size or the title; only such an implementation declares it. */
typedef enum SvExtSource { SV_EXTSRC_GENERIC = 0, SV_EXTSRC_AUTHORED = 1 } SvExtSource;

typedef struct SvExtendedSurface {
  const uint8_t *rgb24;
  size_t size;      /* readable bytes at rgb24 */
  int width, height, pitch;
  int side;
  uint64_t sequence;
  SvWidescreenMode mode;
  SvWidescreenSupport status;
  SvExtSource source; /* SEG-123: zero-initialised callers stay GENERIC */
  /* SEG-124 (ADR 0267): an optional overlay supplied by a verified native compatibility implementation: the game's HUD, relocated (it replaces the game's own). RGBA8, tightly packed
   * (pitch overlay_w * 4), at most SV_OVERLAY_MAX x SV_OVERLAY_MAX. Its rows are picture rows 1:1 (overlay row y is picture row y). The host draws it after the side treatment with its
   * left edge at the left edge of the VISIBLE side (so only the horizontal position differs from where the game drew it), clipped to the surface; when there is no visible side
   * (4:3, Original) it is drawn inside the picture with its top-left at (overlay_fx, overlay_fy), the place of the HUD it replaces. Fully transparent pixels leave the picture alone (no
   * panel). It is independent of `source`; it is also drawn when the extension itself is not shown (Original). NULL / 0 = none. */
  const uint8_t *overlay_rgba;
  int overlay_w, overlay_h;
  int overlay_fx, overlay_fy;
  /* SEG-140-T004 (ADR 0303): optional positioned, layered overlays (see SvOverlayLayer below), drawn after the fixed HUD above. The array and the pixels are read during the present call and
   * copied; NULL / 0 = none (a zero-initialised surface presents exactly as before). Like the HUD they are independent of `source` and also drawn when the extension itself is not shown. */
  const struct SvOverlayLayer *layers;
  int layer_count;
} SvExtendedSurface;
#define SV_OVERLAY_MAX 256

typedef enum SvExtCheck {
  SV_EXT_OK = 0,
  SV_EXT_NO_SURFACE,  /* absent surface/buffer */
  SV_EXT_STATUS,      /* status UNAVAILABLE (or unknown) */
  SV_EXT_MODE,        /* the frame was produced as Original (or an unknown mode) */
  SV_EXT_SEQUENCE,    /* identity differs from the frame being presented (stale/mismatched) */
  SV_EXT_GEOMETRY,    /* size/side inconsistent with the native picture, not centred, or beyond the bound */
  SV_EXT_PITCH,
  SV_EXT_SIZE         /* the readable bytes do not cover the surface */
} SvExtCheck;
/* Validates `surface` as an extension of the `native_w` x `native_h` picture with identity `native_sequence`, for hosts that accept at most `max_w` native
 * pixels per row. SV_EXT_OK means every pixel the host will read is inside `size`. Pure; no overflow for any input. */
SvExtCheck sv_extended_surface_check(const SvExtendedSurface *surface, int native_w, int native_h, uint64_t native_sequence, int max_w);

/* ---- SEG-123: side-area treatment (pure, SDL-free, deterministic, no allocation) ----
 * The native picture is never read for writing and never changed; only the two side regions of an accepted extension are replaced. */
typedef enum SvSideTreatment {
  SV_TREAT_PASSTHROUGH = 0, /* the surface is displayed exactly as supplied */
  SV_TREAT_COMFORT,         /* side regions replaced by a smoothed ambient fill that fades to a dark neutral */
  SV_TREAT_BORDERS          /* side regions replaced by a flat neutral panel */
} SvSideTreatment;
/* The decision table. RAW: passthrough for both sources. COMFORT: generic -> comfort, authored -> passthrough. BORDERS: borders for both (the user explicitly asked). */
SvSideTreatment sv_side_treatment(SvSideStyle style, SvExtSource source);
/* Bytes of scratch sv_side_fill needs for a native picture `h` rows high (0 for an invalid height). */
size_t sv_side_scratch_bytes(int h);
#define SV_SIDE_MAX_ROWS 480
/* Fills `left` and `right` (each `side` x `h` RGB24 pixels, tightly packed, pitch side*3) for the COMFORT or BORDERS treatment from the native picture.
 * `scratch` must hold sv_side_scratch_bytes(h) bytes (suitably aligned for int32). Returns 0, or -1 for invalid input or SV_TREAT_PASSTHROUGH. */
int sv_side_fill(SvSideTreatment treatment, const uint8_t *native, int w, int h, int pitch, int side, uint8_t *left, uint8_t *right, void *scratch);

/* ---- SEG-139: the checked widescreen surface planner (pure, SDL-free, no platform knowledge, no allocation) ----
 * One function turns the native picture, its pixel-aspect policy and a requested rational display aspect into the number of extra source pixels per side and
 * the size of the resulting surface, or into a typed rejection. It NEVER clamps: a request that does not fit the supported surface is rejected so no producer,
 * composer buffer or texture is ever sized from a silently reduced value. Legacy 4:3..16:9 settings keep their historical meaning through
 * sv_geom_resolve_production / sv_geom_resolve_visible (the legacy functions above, bit-for-bit). */
#define SV_GEOM_MAX_NATIVE 4096
#define SV_GEOM_MAX_SIDE SV_EXT_MAX_SIDE
/* Largest surface of the supported Genesis frame (320 x 448 plus 2 * SV_GEOM_MAX_SIDE): 1664 x 448 RGB24. */
#define SV_GEOM_GENESIS_MAX_SURFACE_BYTES ((size_t)(320 + 2 * SV_GEOM_MAX_SIDE) * 448u * 3u)

typedef enum SvGeomStatus {
  SV_GEOM_OK = 0,
  SV_GEOM_BAD_NATIVE = 1,   /* native size not in 1..SV_GEOM_MAX_NATIVE */
  SV_GEOM_BAD_ASPECT = 2,   /* ratio unset, a term out of 1..SV_RATIO_MAX_TERM, or outside 4:3 .. 32:9 */
  SV_GEOM_TOO_WIDE = 3      /* the required side exceeds SV_GEOM_MAX_SIDE: rejected, never clamped */
} SvGeomStatus;

typedef struct SvGeomPlan {
  SvGeomStatus status;
  int native_w, native_h;
  int side;            /* extra source pixels on EACH side (0 = nothing to add: the native picture is already at least as wide as the request) */
  int surface_w;       /* native_w + 2 * side */
  size_t surface_bytes; /* surface_w * native_h * 3 (RGB24), 0 unless OK */
} SvGeomPlan;

/* Plans the surface so the whole surface displays at `target` for a `native_w` x `native_h` picture with the pixel policy `aspect`. Always fills `out`
 * (non-OK plans have side 0, surface_w 0 and surface_bytes 0). Exact integer arithmetic; deterministic; monotonic: a wider target never needs a smaller side. */
SvGeomStatus sv_geom_plan(int native_w, int native_h, SvAspect aspect, SvDisplayRatio target, SvGeomPlan *out);

/* Side the PRODUCER must make for the user's video settings: the legacy rule when no exact ratio is set (16:9, clamped as before, rejection-free), else the
 * planner at the exact ratio. Returns the plan status; `*side_out` is 0 on any non-OK status. */
SvGeomStatus sv_geom_resolve_production(const SvVideoSettings *video, int native_w, int native_h, int *side_out);
/* Side the HOST may show of a frame whose producer supplied `produced_side` per side: legacy sv_layout_visible_side without an exact ratio, else the planner's
 * side limited by what was produced (a rejected plan shows nothing). */
int sv_geom_resolve_visible(const SvVideoSettings *video, int native_w, int native_h, int produced_side);

const char *sv_geom_status_name(SvGeomStatus status); /* stable lowercase diagnostic name */

/* ---- SEG-139-T004: a package's geometry declaration and the host's per-scene, per-component coverage accounting (pure, SDL-free, no allocation) ----
 * A package declares the display-aspect range it supports and, per named scene, which components it covers (scenery, hud, actors). The host compares that
 * declaration with what a frame actually delivered and reports the result; it never infers support. The declaration reaches the host as one compact text,
 * `min=N:M;max=N:M;scene=ID:comp+comp;scene=...` (every part optional, `max` required when `min` or a scene is given; components `scenery`, `hud`, `actors`),
 * produced by the package tooling from the schema 3 `geometry` object. Nothing here knows a platform or a title. */
#define SV_COV_MAX_SCENES 32
#define SV_COV_ID_MAX 32
typedef enum SvCovComponent { SV_COV_SCENERY = 0, SV_COV_HUD = 1, SV_COV_ACTORS = 2, SV_COV_COMPONENTS = 3 } SvCovComponent;
#define SV_COV_BIT(c) (1u << (unsigned)(c))
const char *sv_cov_component_name(SvCovComponent component); /* "scenery", "hud", "actors" */

typedef struct SvGeomScene { char id[SV_COV_ID_MAX + 1]; unsigned declared; /* mask of SV_COV_BIT */ } SvGeomScene;
typedef struct SvGeomDecl {
  int present;        /* a declaration was given (even an invalid one) */
  int invalid;        /* the text could not be read: fail closed, no frame is in range */
  int has_range;      /* min/max declared */
  SvDisplayRatio min, max;
  int scene_count;
  SvGeomScene scenes[SV_COV_MAX_SCENES];
} SvGeomDecl;
/* Parses the compact text. 0 on success; -1 on anything malformed (out is then present = 1, invalid = 1). NULL or an empty text is present = 0 (no declaration: nothing is claimed
 * and nothing is restricted). Ids are tokens of [a-z0-9_-], at most SV_COV_ID_MAX, strictly ascending, unique; at most 2048 characters (the longest legal declaration is 1884); ratios use sv_ratio_parse; min <= max. */
int sv_geomdecl_parse(const char *text, SvGeomDecl *out);
/* 1 when `requested` lies inside the declared range, or when nothing is declared (an undeclared package is not restricted). 0 for an invalid declaration or an out-of-range ratio. */
int sv_geomdecl_in_range(const SvGeomDecl *decl, SvDisplayRatio requested);

typedef enum SvCovStatus {
  SV_COV_ABSENT = 0,        /* neither declared nor delivered */
  SV_COV_COVERED = 1,       /* declared for this scene, inside the range, and delivered this frame */
  SV_COV_MISSING = 2,       /* declared for this scene, inside the range, but not delivered this frame */
  SV_COV_UNDECLARED = 3,    /* delivered without being declared for this scene (never counted as support) */
  SV_COV_OUTSIDE_RANGE = 4  /* the requested aspect is outside the declared range: the generic presentation is shown instead */
} SvCovStatus;
const char *sv_cov_status_name(SvCovStatus status); /* "absent", "covered", "missing", "undeclared", "outside declared range" */
/* The status of one component of one frame. `scene` NULL or not declared = the unscoped bucket (nothing declared). */
SvCovStatus sv_geomdecl_check(const SvGeomDecl *decl, SvDisplayRatio requested, const char *scene, SvCovComponent component, int delivered);

typedef struct SvCovAccount {
  SvGeomDecl decl;
  uint64_t frames, outside_frames;
  SvDisplayRatio last;
  /* [scene (declared ones, then the unscoped bucket)][component][covered, missing, undeclared, outside_range] */
  uint64_t count[SV_COV_MAX_SCENES + 1][SV_COV_COMPONENTS][4];
} SvCovAccount;
void sv_cov_init(SvCovAccount *account, const SvGeomDecl *decl);
/* Accounts one frame: `delivered` is a mask of SV_COV_BIT components the producer/provider actually supplied for it. Returns 1 when the frame is inside the declared range (or nothing is declared). */
int sv_cov_frame(SvCovAccount *account, SvDisplayRatio requested, const char *scene, unsigned delivered);
/* Stable JSON (fixed key order, integers only, declared scenes in declaration order then the unscoped scene "*"), schema "segarecomp.geometry-coverage/1". Writes at most `cap` bytes
 * including the terminator and returns the length it needs (like snprintf), so a result >= cap was truncated. */
size_t sv_cov_report_json(const SvCovAccount *account, char *buf, size_t cap);

/* ---- SEG-140-T004 (ADR 0303): bounded, positioned, layered overlays (pure, SDL-free, deterministic, no allocation) ----
 * A generalisation of the fixed HUD overlay above: up to SV_OVL_LAYERS_MAX RGBA images, each anchored to a named point of a reference rectangle of the PRESENTATION geometry
 * (the native picture, or the whole visible surface: picture plus the visible side on both sides) with a signed offset, ordered by `z`, optionally clipped to a rectangle of its own
 * image. The fixed HUD path is NOT one of these layers: it is drawn first, exactly as before, and a surface that supplies no layers is presented bit for bit as it always was.
 * Rules (each violation rejects that LAYER only, with a stable reason; the others are still drawn): each image must be 1..SV_OVERLAY_MAX per side; the running total of SUBMITTED image
 * pixels (every image of valid size counts, accepted or not) must stay within SV_OVL_BUDGET_PIXELS; the image must be present; anchor, reference and z must be known; |offset| <= SV_OVL_OFFSET_MAX; the placed rectangle must lie WHOLLY inside its reference rectangle
 * (never silently moved or clipped to fit); a clip rectangle must lie inside the image. Layers beyond SV_OVL_LAYERS_MAX are rejected as COUNT. */
#define SV_OVL_LAYERS_MAX 4
#define SV_OVL_BUDGET_PIXELS 65536
#define SV_OVL_OFFSET_MAX 2048
typedef enum SvOvAnchor {
  SV_OVA_TOP_LEFT = 0, SV_OVA_TOP_CENTER, SV_OVA_TOP_RIGHT,
  SV_OVA_MIDDLE_LEFT, SV_OVA_CENTER, SV_OVA_MIDDLE_RIGHT,
  SV_OVA_BOTTOM_LEFT, SV_OVA_BOTTOM_CENTER, SV_OVA_BOTTOM_RIGHT,
  SV_OVA_COUNT
} SvOvAnchor;
typedef enum SvOvReference { SV_OVREF_PICTURE = 0, SV_OVREF_PRESENTATION = 1, SV_OVREF_COUNT } SvOvReference;
typedef enum SvOvReject {
  SV_OVR_OK = 0,
  SV_OVR_COUNT,         /* more than SV_OVL_LAYERS_MAX layers */
  SV_OVR_NO_IMAGE,      /* no pixel data */
  SV_OVR_SIZE,          /* a side is outside 1..SV_OVERLAY_MAX */
  SV_OVR_BUDGET,        /* the running pixel total would exceed SV_OVL_BUDGET_PIXELS */
  SV_OVR_ANCHOR,
  SV_OVR_REFERENCE,
  SV_OVR_Z,             /* z outside 0..SV_OVL_LAYERS_MAX - 1 */
  SV_OVR_OFFSET,
  SV_OVR_OUT_OF_BOUNDS, /* the placed rectangle is not wholly inside the reference rectangle */
  SV_OVR_CLIP           /* the clip rectangle is empty or not inside the image */
} SvOvReject;
const char *sv_ovl_reject_name(SvOvReject reason); /* "ok", "count", "no_image", "size", "budget", "anchor", "reference", "z", "offset", "out_of_bounds", "clip" */
typedef struct SvOverlayLayer {
  const uint8_t *rgba;      /* w * h * 4, tightly packed, straight alpha */
  int w, h;
  int anchor;               /* SvOvAnchor */
  int reference;            /* SvOvReference */
  int z;                    /* draw order, 0 first (lowest) */
  int dx, dy;               /* added to the anchored position (positive = right / down) */
  int has_clip;
  int clip_x, clip_y, clip_w, clip_h;   /* image pixels */
} SvOverlayLayer;
typedef struct SvOverlayPlacement {
  int index;                /* index in the submitted array */
  int x, y;                 /* top-left in presentation coordinates (0,0 = the left edge of the visible surface) */
  int w, h;
  int cx0, cy0, cx1, cy1;   /* the drawn part of the image: [cx0, cx1) x [cy0, cy1) in image pixels */
} SvOverlayPlacement;
typedef struct SvOverlayPlan {
  int placed;
  SvOverlayPlacement place[SV_OVL_LAYERS_MAX];   /* in draw order (z ascending, then submission order) */
  int rejected;
  struct { int index; SvOvReject reason; } reject[SV_OVL_LAYERS_MAX + 1];   /* the first SV_OVL_LAYERS_MAX + 1 rejections; `rejected` is the exact total */
  int pixels;               /* image pixels of the accepted (placed) layers */
} SvOverlayPlan;
/* True when a further image of w x h pixels fits the budget after `used` pixels. Shared by the host's copy and the planner so they cannot disagree. */
int sv_ovl_budget_ok(int used, int w, int h);
/* Plans `count` layers (a negative or NULL input plans nothing) for a native picture of pic_w x pic_h with `visible_side` columns of surface on each side (0 at 4:3 / Original).
 * Pure; never reads image pixels. */
void sv_overlay_plan(const SvOverlayLayer *layers, int count, int pic_w, int pic_h, int visible_side, SvOverlayPlan *out);
/* Blends the planned layers, in plan order, over the RGB24 presentation surface `rgb` (width x height, row pitch `pitch` bytes) whose width is pic_w + 2 * visible_side. Source-over with
 * the same rounding as the fixed overlay; fully transparent pixels leave the surface unchanged. */
void sv_overlay_compose(uint8_t *rgb, int width, int height, int pitch, const SvOverlayLayer *layers, const SvOverlayPlan *plan);

/* Barrel distortion of a normalized coordinate pair in [-1, 1] (curvature 0..100). Maps the undistorted position of a mesh vertex to its
 * position on the curved tube; 0 is the identity. `vignette` receives a 0..1 brightness factor for the corner falloff. */
void sv_crt_warp(float nx, float ny, int curvature, float *out_x, float *out_y, float *vignette);

/* One scanline-gap alpha (0..255) for output row `row` when each source line spans `period` output rows: 0 on the bright part of the
 * line, rising to `strength` percent of 255 at the dark gap between lines. Smooth, periodic, pure. */
uint8_t sv_crt_scanline_alpha(int row, float period, int strength);

/* Aperture-mask multiplier (0..255) for output column `col` and channel 0..2 (R,G,B): 255 on the channel's stripe, reduced elsewhere. */
uint8_t sv_crt_mask_value(int col, int channel, int strength);

/* True when the settings describe the untouched nearest-neighbour picture (no overlay, no warp, no brightness change). */
int sv_crt_is_identity(const SvVideoSettings *video);

#ifdef __cplusplus
}
#endif
#endif
