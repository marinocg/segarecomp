#ifndef SEGARECOMP_GENESIS_PATCH_H
#define SEGARECOMP_GENESIS_PATCH_H

/*
 * Segarecomp Genesis native patch SDK, ABI version 1 (ADR 0267).
 *
 * A `.segapatch` package may carry ONE portable C11 source file (`provider.c`) that implements a native compatibility provider for one game. This header is the entire
 * public interface of that source: it needs nothing but <stdint.h> and <stddef.h>, and it is the only segarecomp header the build puts on the provider's include path.
 * The segarecomp product contains no game-specific provider; the package owns all of it (game identity, RAM offsets, level formats, camera interpretation, HUD layout).
 *
 * The provider is compiled into the generated-native executable of the separate, explicitly selected Patched build and called through the table below. It receives READ-ONLY views:
 *   frame.work_ram   a 64 KiB copy of guest work RAM taken at the VBlank onset that preceded the frame being produced (the state the game used to prepare that frame),
 *   frame.vdp        the frame's VRAM as its first line saw it and the per-line registers / CRAM / VSRAM,
 * and it may contribute
 *   scenery.words    the name-table word of the world cell beneath an off-screen column (the host's VDP code still decodes patterns, palettes, flips, priority and sprites), and
 *   overlay.rgba     a bounded RGBA overlay the viewer draws after side treatment.
 * A package declares the capabilities it needs in its manifest; the host grants exactly those and refuses a package that needs one it does not have.
 * There is no API to write guest memory, control a device or reach the host: the interface is read-only by construction. THIS IS NOT A SANDBOX: a provider is native code
 * compiled into the program and must come from a trusted source (the launcher asks the user to approve each exact package).
 *
 * The provider exports `segarecomp_patch_provider`. A frame the provider does not verify must be declined in `prepare`; it is then presented by the generic path.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_NATIVE_COMPAT_ABI 1U

/* Host capabilities a package may require (manifest `native.capabilities`, by name: "frame.work_ram", "frame.vdp", "scenery.words", "overlay.rgba"). */
#define GENESIS_CAP_FRAME_WORK_RAM 1U
#define GENESIS_CAP_FRAME_VDP 2U
#define GENESIS_CAP_SCENERY_WORDS 4U
#define GENESIS_CAP_OVERLAY_RGBA 8U

#define GENESIS_COMPAT_RAM_BYTES 65536U
#define GENESIS_COMPAT_VRAM_BYTES 65536U
#define GENESIS_COMPAT_REGISTERS 24U
#define GENESIS_COMPAT_CRAM_BYTES 128U
#define GENESIS_COMPAT_VSRAM_BYTES 80U
#define GENESIS_COMPAT_MAX_LINES 224U
#define GENESIS_COMPAT_OVERLAY_MAX_W 128U
#define GENESIS_COMPAT_OVERLAY_MAX_H 256U
#define GENESIS_COMPAT_OVERLAY_BYTES (GENESIS_COMPAT_OVERLAY_MAX_W * GENESIS_COMPAT_OVERLAY_MAX_H * 4U)

/* Component bits of one frame (derived from the granted capabilities): scenery.words -> SCENERY, overlay.rgba -> HUD. */
#define GENESIS_COMPAT_HUD 1U
#define GENESIS_COMPAT_SCENERY 2U

/* VDP plane selector for scroll queries and scenery words. */
#define GENESIS_COMPAT_PLANE_A 0U
#define GENESIS_COMPAT_PLANE_B 1U

/* Why a provider declined a frame (diagnostic only; a declined frame is served by the generic path with its own accurate status). */
typedef enum GenesisCompatReason {
  GENESIS_COMPAT_OK = 0,
  GENESIS_COMPAT_STALE_STATE = 1,       /* host-side: no snapshot taken exactly one frame earlier */
  GENESIS_COMPAT_UNSUPPORTED_SCENE = 2, /* a game mode, level or screen this provider is not verified for */
  GENESIS_COMPAT_BAD_STATE = 3,         /* the game state is inconsistent or out of range */
  GENESIS_COMPAT_BAD_SCROLL = 4,        /* the VDP scroll/plane configuration is not the verified one, or disagrees with the game state */
  GENESIS_COMPAT_FAULT = 5,             /* the provider failed to answer for a cell it accepted (fails the frame's authored claim) */
  GENESIS_COMPAT_MISMATCH = 6           /* the frame's own picture disagrees with the game state: a fade, a transition or another level */
} GenesisCompatReason;

/* The VDP state of one output line as the frame saw it (layout is fixed by the ABI). */
typedef struct GenesisCompatLine {
  uint16_t registers[GENESIS_COMPAT_REGISTERS];
  uint8_t cram[GENESIS_COMPAT_CRAM_BYTES];
  uint8_t vsram[GENESIS_COMPAT_VSRAM_BYTES];
} GenesisCompatLine;

/* Host helper functions (the same code the host's own renderer uses, so a provider never clones VDP decoding). All are pure and read-only. Return 0 on success. */
typedef struct GenesisCompatHost {
  uint32_t size;                         /* sizeof(GenesisCompatHost) of the host */
  /* Signed horizontal scroll of `plane` (GENESIS_COMPAT_PLANE_*) on output `line` for the given registers and VRAM (plane_x = screen_x - hscroll). */
  int (*read_hscroll)(const uint8_t vram[GENESIS_COMPAT_VRAM_BYTES], const uint16_t registers[GENESIS_COMPAT_REGISTERS], unsigned plane, unsigned line, int32_t *value);
  /* Signed vertical scroll of `plane` for screen cell column `cell_column` (column 0 for full-screen vertical scroll). */
  int (*read_vscroll)(const uint8_t vsram[GENESIS_COMPAT_VSRAM_BYTES], const uint16_t registers[GENESIS_COMPAT_REGISTERS], unsigned plane, unsigned cell_column, int32_t *value);
  /* Non-zero when the registers select per-2-cell-column vertical scroll. */
  int (*vscroll_is_per_column)(const uint16_t registers[GENESIS_COMPAT_REGISTERS]);
  /* RGB888 of CRAM entry `index` (0-63; palette line * 16 + colour) exactly as the host presents it. */
  int (*cram_rgb)(const uint8_t cram[GENESIS_COMPAT_CRAM_BYTES], unsigned index, uint8_t rgb[3]);
} GenesisCompatHost;

typedef struct GenesisCompatFrame {
  const GenesisCompatHost *host;
  const uint8_t *work_ram;              /* GENESIS_COMPAT_RAM_BYTES, read-only; NULL unless the package declared frame.work_ram */
  const uint8_t *vram;                  /* GENESIS_COMPAT_VRAM_BYTES: the VRAM as the frame's first line saw it; NULL unless frame.vdp */
  const GenesisCompatLine *lines;       /* per output line state; NULL unless frame.vdp */
  unsigned line_count;                  /* valid entries of `lines` (the picture height, at most GENESIS_COMPAT_MAX_LINES) */
  unsigned width, height, side;         /* the native picture and the extension width per side the host plans to produce */
  unsigned requested;                   /* GENESIS_COMPAT_* components the host will use if accepted */
} GenesisCompatFrame;

#define GENESIS_COMPAT_SCENE_MAX 32
typedef struct GenesisCompatDecision {
  unsigned accepted;                    /* GENESIS_COMPAT_* components the provider serves for THIS frame (subset of `requested`; 0 = decline the whole frame) */
  uint8_t reason;                       /* GenesisCompatReason when nothing was accepted */
  /* Optional (SEG-139-T004, additive to ABI 1: the host zero-initialises the whole struct, so a provider built before this field existed simply leaves it empty): the NAME of the scene this frame
   * belongs to, `[a-z0-9_-]`, at most GENESIS_COMPAT_SCENE_MAX characters, NUL-terminated. The host never infers a scene; it uses this one only to account declared-versus-delivered coverage
   * per scene against the package's geometry declaration. A scene the declaration does not list is accounted as undeclared, never as covered. It grants nothing. */
  char scene[GENESIS_COMPAT_SCENE_MAX + 1];
} GenesisCompatDecision;

typedef struct GenesisCompatOverlay {
  uint8_t *rgba;                        /* host-owned, GENESIS_COMPAT_OVERLAY_BYTES; rows are picture rows 1:1, left edge anchored by the host */
  size_t capacity;
  uint16_t width, height;               /* written by the provider; 0 = nothing to draw */
  uint16_t fallback_x, fallback_y;      /* picture pixels where the overlay's top-left sits when the host draws it inside the picture (no visible side) */
} GenesisCompatOverlay;

typedef struct GenesisNativeCompat {
  uint32_t abi_version;                 /* GENESIS_NATIVE_COMPAT_ABI */
  const char *name;                     /* the provider's own identity (informational) */
  uint32_t implementation_version;      /* the provider's own version (informational) */
  unsigned components;                  /* GENESIS_COMPAT_* the implementation can serve */
  /* Validates every input the frame will use; accepts a subset of `frame->requested` (0 = decline the whole frame). Called once per produced frame. */
  void (*prepare)(void *state, const GenesisCompatFrame *frame, GenesisCompatDecision *out);
  /* Plane (GENESIS_COMPAT_PLANE_*) name-table word for the world cell under screen column `x` (outside [0, width)) of output line `row`, given the horizontal/vertical scroll the VDP
   * applies to that line (plane_x = x - hscroll, plane_y = row + vscroll). Returns 1 with a word (0 = blank cell). Only called for a scenery component accepted by `prepare`. */
  int (*scenery_word)(void *state, unsigned plane, unsigned row, int x, int hscroll, int vscroll, uint16_t *word);
  /* Draws the overlay (RGBA8, tightly packed). Returns 1 when something is drawn. Only called for a HUD component accepted by `prepare`. */
  int (*hud)(void *state, GenesisCompatOverlay *out);
} GenesisNativeCompat;

/* The one entry point a provider source exports. Returns the provider table and its state storage (static storage in the provider), or NULL to decline to run. */
const GenesisNativeCompat *segarecomp_patch_provider(void **state_out);

#ifdef __cplusplus
}
#endif
#endif
