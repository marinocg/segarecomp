/* SEG-119 (ADR 0262): Level 2 extended backgrounds in the Sega CD viewer, read back from the rendered output.
 *
 * A real ScdMachine (BIOS-image profile, device level: no processor program, no disc) is driven through the runtime's own bus ABI exactly as in
 * tests/tools/sega_cd_live_table.c: VDP registers and CRAM by control/data-port writes, background tiles by the graphics ASIC (stamp rendering into 2M
 * Word RAM) followed by a Word RAM -> VRAM DMA, name tables by DMA. The Sega CD viewer's own registered live-driver callbacks (poll, extension hook,
 * present: platforms/sega-cd/viewer/scd_viewer.c, included below so its static host state is reachable) run unmodified over the real Genesis SDL3
 * adapter and shared SvHost presenter (dummy video driver, software renderer). The frame loop here mirrors one iteration of scd_live_loop.
 *
 * Every expected colour comes from an independent model of the scene (tile/cell/stamp arithmetic written below), never from the renderer or from the
 * producer. Independent of the Genesis extension tests: this file builds its own machine and scenes. Strict C11. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scd_viewer.c" /* the unit under test: ScdSdlHost g_host, fill_live_host, sdl_* callbacks */
#include "sv_host.h"
#include "sv_layout.h"
#include "vdp_render.h"

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define MASTER UINT64_C(500000)
#define LINE_TICKS (UINT64_C(3420) * MASTER)
#define FRAME_TICKS (LINE_TICKS * 262U)
#define ONSET(k) (SCD_BIOS_FIRST_ONSET_MCLK * MASTER + ((uint64_t)(k)-1U) * FRAME_TICKS)

/* ---------------------------------------------------------------- the machine ------------------------------------------------------------ */
static ScdMachine *m;
static uint8_t bios[0x20000];
static ScdLiveHost live;
static uint64_t frame_no; /* the onset number of the next frame to complete */

static GenesisRuntime *vdp_host(void) { return (GenesisRuntime *)m->vdp_host; }

static void fresh(ScdMachine *machine) {
  if (machine->vdp_host != NULL) scd_vdp_release(machine);
  memset(machine, 0, sizeof *machine);
  machine->bios = bios;
  machine->bios_size = sizeof bios;
  machine->cpu[SCD_MAIN].cycle_ticks = SCD_MAIN_CYCLE_TICKS;
  machine->cpu[SCD_SUB].cycle_ticks = SCD_SUB_CYCLE_TICKS;
  if (scd_vdp_init(machine) != 0) exit(2);
}

static void run_to(ScdMachine *machine, uint64_t t) {
  for (;;) {
    const uint64_t e = scd_devices_next_event(machine);
    if (e > t) break;
    machine->now = e;
    scd_devices_advance(machine, e);
  }
  machine->now = t;
}

static int port(ScdMachine *machine, uint32_t address, uint32_t width, uint32_t value) {
  const int status = scd_write(machine, SCD_MAIN, address, width, value, 0xFF0100U);
  if (status != 0) { printf("      stop %s\n", scd_stop_class_name(machine->stop.stop_class)); machine->stop.stop_class = SCD_STOP_NONE; }
  return status;
}
static void vdp_reg(ScdMachine *machine, unsigned reg, unsigned value) { port(machine, 0xC00004U, 2U, 0x8000U | (reg << 8) | (value & 0xFFU)); }
static void cram_word(ScdMachine *machine, unsigned index, unsigned r, unsigned g, unsigned b) {
  port(machine, 0xC00004U, 4U, 0xC0000000U | ((index * 2U) << 16));
  port(machine, 0xC00000U, 2U, (b << 9) | (g << 5) | (r << 1));
}

/* ---------------------------------------------------------------- the scene model (independent of every renderer) ---------------------- */
/* A tile of the model: EMPTY (all index 0), SOLID (every pixel `idx`) or HALF (left four columns index 0, right four `idx`). */
typedef enum { T_EMPTY = 0, T_SOLID, T_HALF } TileKind;
typedef struct { TileKind kind; unsigned idx; } TileDef;
typedef struct { unsigned tile, pal, pri; } Entry;
typedef struct {
  TileDef tiles[128];
  Entry a[32][64], b[32][64];
  int hs_a, hs_b;
  unsigned rgb[64][3];   /* 3-bit channels per CRAM entry */
} Scene;
static Scene scene;

static unsigned tile_pixel(const TileDef *t, unsigned fx) {
  if (t->kind == T_SOLID) return t->idx;
  if (t->kind == T_HALF) return fx >= 4U ? t->idx : 0U;
  return 0U;
}
/* The (cram index, opaque, priority) of a plane at screen column x, scene row r: hscroll shifts the picture right. */
static int plane_at(const Entry grid[32][64], const Scene *s, int hs, int x, unsigned cell_row, unsigned *cram, unsigned *pri) {
  int px = (x - hs) % 512;
  unsigned cell, fx, idx;
  const Entry *e;
  if (px < 0) px += 512;
  cell = (unsigned)px / 8U;
  fx = (unsigned)px % 8U;
  e = &grid[cell_row % 32U][cell];
  idx = tile_pixel(&s->tiles[e->tile], fx);
  if (idx == 0U) return 0;
  *cram = e->pal * 16U + idx;
  *pri = e->pri;
  return 1;
}
static void ref_rgb(const Scene *s, int x, unsigned row, uint8_t out[3]) {
  unsigned ca = 0, cb = 0, pa = 0, pb = 0, idx = 0;
  const unsigned cell_row = row / 8U;
  const int a = plane_at(s->a, s, s->hs_a, x, cell_row, &ca, &pa);
  const int b = plane_at(s->b, s, s->hs_b, x, cell_row, &cb, &pb);
  if (a && pa) idx = ca; else if (b && pb) idx = cb; else if (a) idx = ca; else if (b) idx = cb; else idx = 0U;
  for (int c = 0; c < 3; ++c) out[c] = (uint8_t)(s->rgb[idx][c] * 255U / 7U);
}

static void put_be16(uint8_t *vram, unsigned addr, unsigned v) { vram[addr & 0xFFFFU] = (uint8_t)(v >> 8); vram[(addr + 1U) & 0xFFFFU] = (uint8_t)v; }
static unsigned entry_word(const Entry *e) { return (e->pri << 15) | (e->pal << 13) | e->tile; }

/* Writes tile `t`'s pattern (from the model) into VRAM tile slot `slot`. */
static void write_tile(uint8_t *vram, unsigned slot, const TileDef *t) {
  for (unsigned row = 0; row < 8; ++row)
    for (unsigned col = 0; col < 8; col += 2) {
      const unsigned hi = tile_pixel(t, col), lo = tile_pixel(t, col + 1U);
      vram[slot * 32U + row * 4U + col / 2U] = (uint8_t)((hi << 4) | lo);
    }
}

static void set_palette(void) {
  static const unsigned c[16][3] = {{0, 0, 0}, {7, 0, 0}, {0, 7, 0}, {0, 0, 7}, {7, 7, 0}, {7, 0, 7}, {0, 7, 7}, {3, 3, 3},
                                    {5, 1, 2}, {1, 5, 2}, {2, 1, 5}, {6, 6, 1}, {1, 6, 6}, {6, 1, 6}, {4, 4, 7}, {7, 4, 4}};
  for (unsigned i = 0; i < 64; ++i) {
    const unsigned *q = c[i % 16U];
    unsigned k = i / 16U;
    scene.rgb[i][0] = (q[0] + k) % 8U; scene.rgb[i][1] = (q[1] + 2U * k) % 8U; scene.rgb[i][2] = (q[2] + 3U * k) % 8U;
    if (i % 16U == 0U) scene.rgb[i][0] = scene.rgb[i][1] = scene.rgb[i][2] = 0U; /* transparent index 0 of any line shows the backdrop colour 0 */
    cram_word(m, i, scene.rgb[i][0], scene.rgb[i][1], scene.rgb[i][2]);
  }
}

/* The layout common to every scene: Plane A 0xC000, window 0xD000, Plane B 0xE000, sprites 0xF800, H-scroll table 0xFC00 (full-screen scroll), 64 x 32. */
static void setup_registers(int h40, unsigned reg1_extra) {
  vdp_reg(m, 0, 0x04); vdp_reg(m, 1, 0x44 | reg1_extra); vdp_reg(m, 2, 0x30); vdp_reg(m, 3, 0x34); vdp_reg(m, 4, 0x07); vdp_reg(m, 5, 0x7C);
  vdp_reg(m, 7, 0x00); vdp_reg(m, 11, 0x00); vdp_reg(m, 12, h40 ? 0x81 : 0x00); vdp_reg(m, 13, 0x3F); vdp_reg(m, 15, 0x02); vdp_reg(m, 16, 0x01);
}

static void write_scroll(void) {
  uint8_t *vram = vdp_host()->devices.vdp.vram;
  put_be16(vram, 0xFC00U, (unsigned)scene.hs_a & 0x3FFU);
  put_be16(vram, 0xFC02U, (unsigned)scene.hs_b & 0x3FFU);
}
static void write_names(void) {
  uint8_t *vram = vdp_host()->devices.vdp.vram;
  for (unsigned r = 0; r < 32; ++r)
    for (unsigned c = 0; c < 64; ++c) {
      put_be16(vram, 0xC000U + (r * 64U + c) * 2U, entry_word(&scene.a[r][c]));
      put_be16(vram, 0xE000U + (r * 64U + c) * 2U, entry_word(&scene.b[r][c]));
    }
}

/* ----- scene 1 (direct VRAM): small solid/half tiles; Plane B columns 1 + c % 4, Plane A priority and transparency cases ----- */
static void build_direct_scene(int h40) {
  uint8_t *vram = vdp_host()->devices.vdp.vram;
  memset(&scene, 0, sizeof scene);
  memset(vram, 0, GENESIS_VDP_VRAM_BYTES);
  set_palette();
  setup_registers(h40, 0);
  for (unsigned t = 1; t <= 4; ++t) { scene.tiles[t].kind = T_SOLID; scene.tiles[t].idx = t; }   /* tiles 1..4: palette-0 indexes 1..4 */
  scene.tiles[5].kind = T_SOLID; scene.tiles[5].idx = 5;                                         /* tile 5: solid index 5 (palette line given by the entry) */
  scene.tiles[6].kind = T_HALF;  scene.tiles[6].idx = 2;                                         /* tile 6: left half transparent */
  for (unsigned t = 1; t <= 6; ++t) write_tile(vram, t, &scene.tiles[t]);
  for (unsigned r = 0; r < 32; ++r)
    for (unsigned c = 0; c < 64; ++c) {
      scene.b[r][c].tile = 1U + (c % 4U);
      switch (c % 8U) { /* Plane A cases by cell column */
      case 0: scene.a[r][c].tile = 5; scene.a[r][c].pal = 1; break;                            /* A over B (low priority both) */
      case 1: scene.a[r][c].tile = 5; scene.a[r][c].pal = 2; scene.a[r][c].pri = 1; break;    /* A high priority */
      case 2: scene.a[r][c].tile = 5; scene.a[r][c].pal = 1; scene.b[r][c].pri = 1; break;     /* B high priority beats low A */
      case 3: scene.a[r][c].tile = 6; scene.a[r][c].pal = 3; break;                            /* half-transparent A over B */
      default: break;                                                                          /* A empty: B alone */
      }
    }
  write_names();
  write_scroll();
}

/* ----- scene 2 (the graphics ASIC + DMA): tiles from stamp rendering, name tables from Word RAM, both reaching VRAM by DMA ----- */
#define WR_STAMPS 0x00000U   /* stamp 1 (16x16, 128 bytes) at byte 0x80, stamp 2 at 0x100 (stamp number << 7) */
#define WR_MAP 0x04000U      /* stamp map (map_base 0x1000 -> byte 0x4000) */
#define WR_TRACE 0x05000U    /* trace vector table (trace base 0x1400 -> byte 0x5000) */
#define WR_BUFFER 0x10000U   /* image buffer (buffer_origin 0x4000 -> nibble 0x20000) */
#define WR_NAMES 0x20000U    /* name tables as the guest would stage them: Plane B entries */

static void stamp_regs(void) {
  uint32_t v;
  v = 0x0000; scd_gfx_register_access(m, 0x58U, 2U, 1, &v, 0); /* 16x16 stamps, 16x16 stamp map, no repeat */
  v = 0x1000; scd_gfx_register_access(m, 0x5AU, 2U, 1, &v, 0);
  v = 0x0007; scd_gfx_register_access(m, 0x5CU, 2U, 1, &v, 0); /* image buffer 8 cells high */
  v = 0x4000; scd_gfx_register_access(m, 0x5EU, 2U, 1, &v, 0);
  v = 0x0000; scd_gfx_register_access(m, 0x60U, 2U, 1, &v, 0);
  v = 0x0040; scd_gfx_register_access(m, 0x62U, 2U, 1, &v, 0); /* 64 dots */
  v = 0x0040; scd_gfx_register_access(m, 0x64U, 2U, 1, &v, 0); /* 64 lines */
}

static void put_wr16(uint32_t at, unsigned v) { m->word_ram[at] = (uint8_t)(v >> 8); m->word_ram[at + 1U] = (uint8_t)v; }

/* The model of what the stamp renders: cell column cx (0..7) holds solid index 3 (cx 0,1,4,5) or 5 (cx 2,3,6,7) -- stamp 1 / stamp 2 alternate each 16
 * dots along the first map row -- and cells are stored column-major (cell number = cx * 8 + cy), 32 bytes each, the VDP's own 4bpp tile format. */
static unsigned stamp_cell_index(unsigned cx) { return ((cx >> 1) & 1U) ? 5U : 3U; }

static void dma_words(uint32_t wr_byte, unsigned vram_dest, unsigned words) {
  /* Word RAM -> VRAM: the first destination word is the stale bus word (not written) and the source stream follows: destination = dest - 2 */
  const uint32_t src = (0x200000U + wr_byte) >> 1;
  const unsigned length = words + 1U;
  const unsigned dest = (vram_dest - 2U) & 0xFFFFU;
  vdp_reg(m, 15, 0x02);
  vdp_reg(m, 19, length & 0xFFU); vdp_reg(m, 20, (length >> 8) & 0xFFU);
  vdp_reg(m, 21, src & 0xFFU); vdp_reg(m, 22, (src >> 8) & 0xFFU); vdp_reg(m, 23, (src >> 16) & 0x7FU);
  port(m, 0xC00004U, 4U, ((0x4000U | (dest & 0x3FFFU)) << 16) | 0x80U | ((dest >> 14) & 3U));
}

static int build_stamp_dma_scene(int h40) {
  uint32_t v;
  const uint64_t first = frame_no;
  memset(&scene, 0, sizeof scene);
  memset(vdp_host()->devices.vdp.vram, 0, GENESIS_VDP_VRAM_BYTES);
  memset(m->word_ram, 0, sizeof m->word_ram);
  set_palette();
  setup_registers(h40, 0x10); /* DMA enabled */
  /* project-authored stamp data: stamp 1 = 16x16 pixels of index 3, stamp 2 = index 5 (4bpp packed) */
  memset(&m->word_ram[WR_STAMPS + 0x80U], 0x33, 128U);
  memset(&m->word_ram[WR_STAMPS + 0x100U], 0x55, 128U);
  for (unsigned x = 0; x < 16; ++x) put_wr16(WR_MAP + 2U * x, (x & 1U) ? 2U : 1U); /* map row 0: stamps 1, 2, 1, 2 ... */
  for (unsigned line = 0; line < 64; ++line) {                                     /* trace: x0 = 0, y0 = 0, x step = 1 dot (0x0800), y step = 0 */
    put_wr16(WR_TRACE + line * 8U + 0U, 0); put_wr16(WR_TRACE + line * 8U + 2U, 0);
    put_wr16(WR_TRACE + line * 8U + 4U, 0x0800U); put_wr16(WR_TRACE + line * 8U + 6U, 0);
  }
  /* the sub processor owns 2M Word RAM (mode 0): run the ASIC to the end of its 64 lines */
  m->ga.mode = 0U;
  stamp_regs();
  v = 0x1400U;
  if (scd_gfx_register_access(m, 0x66U, 2U, 1, &v, 0) != 0) return -1;
  run_to(m, ONSET(frame_no) / 1U);
  if (m->gfx.busy || m->gfx.operations != 1U || m->gfx.lines_rendered != 64U) { printf("      gfx busy=%d ops=%llu lines=%llu\n", m->gfx.busy, (unsigned long long)m->gfx.operations, (unsigned long long)m->gfx.lines_rendered); return -2; }
  (void)first;
  /* the model of the stamp output as a precondition of the DMA test: the first cell of every 16-dot group holds the stamp's colour */
  for (unsigned cx = 0; cx < 8; ++cx)
    for (unsigned cy = 0; cy < 8; ++cy) {
      const uint8_t want = (uint8_t)((stamp_cell_index(cx) << 4) | stamp_cell_index(cx));
      const uint8_t *cell = &m->word_ram[WR_BUFFER + (cx * 8U + cy) * 32U];
      for (unsigned b = 0; b < 32; ++b) if (cell[b] != want) return -3;
    }
  /* model for the guest's staging of the name table: Plane B entry (row r, column c) = tile 1 + (c % 8) * 8 + (r % 8) */
  for (unsigned t = 1; t <= 64; ++t) { scene.tiles[t].kind = T_SOLID; scene.tiles[t].idx = stamp_cell_index((t - 1U) / 8U); }
  for (unsigned r = 0; r < 32; ++r)
    for (unsigned c = 0; c < 64; ++c) {
      scene.b[r][c].tile = 1U + (c % 8U) * 8U + (r % 8U);
      put_wr16(WR_NAMES + (r * 64U + c) * 2U, entry_word(&scene.b[r][c]));
    }
  put_wr16(WR_NAMES + 2U * 2048U, 0); /* the DMA's ignored trailing source word */
  m->ga.mode = SCD_MODE_RET;            /* Word RAM handed to the main processor (2M) */
  run_to(m, ONSET(frame_no + 1U));
  ++frame_no;
  dma_words(WR_BUFFER, 32U, 1024U);     /* 64 cells x 16 words = tiles 1..64 */
  run_to(m, ONSET(frame_no + 1U));
  ++frame_no;
  dma_words(WR_NAMES, 0xE000U, 2048U);
  run_to(m, ONSET(frame_no + 1U));
  ++frame_no;
  /* the DMA reached VRAM as the Word RAM held it (documented source-delay layout), tile by tile */
  {
    const uint8_t *vram = vdp_host()->devices.vdp.vram;
    if (memcmp(&vram[32], &m->word_ram[WR_BUFFER], 2048U) != 0) return -4;
    for (unsigned i = 0; i < 4096U; ++i) if (vram[0xE000U + i] != m->word_ram[WR_NAMES + i]) return -5;
  }
  return 0;
}

/* ---------------------------------------------------------------- the viewer driver loop (one scd_live_loop iteration) ------------------ */
static const GenesisFrameArtifact *last_native; /* the native artifact of the latest presented frame */
static uint64_t last_onset;
static unsigned presented_frames;

static GenesisFrameExtension *adapter(void) { return g_host.extension; }
static SvHost *sv(void) { return genesis_sdl3_viewer_sv_host(g_host.sdl); }

static int use_viewer = 1; /* 0: a machine that never meets the viewer (the guest-invariance reference) */

static int frame_begin(void) {
  ScdLivePoll poll;
  if (!use_viewer) return 0;
  memset(&poll, 0, sizeof poll);
  if (live.poll(live.ctx, frame_no, &poll) != 0 || poll.quit) return -1;
  (void)scd_vdp_present_set_extension(m, live.extension != NULL ? live.extension(live.ctx) : NULL);
  return 0;
}
static int frame_finish(void) {
  uint64_t onset = 0;
  const GenesisFrameArtifact *frame;
  run_to(m, ONSET(frame_no + 1U));
  ++frame_no;
  frame = scd_vdp_present_take(m, &onset);
  if (frame == NULL) return 1;
  last_native = frame;
  last_onset = onset;
  ++presented_frames;
  if (!use_viewer) return 0;
  return live.present(live.ctx, frame, onset) != 0 ? -2 : 0;
}
static int frame(void) { return frame_begin() != 0 ? -1 : frame_finish(); }

static void set_mode(SvWidescreenMode mode, SvAspect aspect, SvScaleMode scale) {
  SvSettings s = *sv_host_settings(sv());
  s.video.widescreen = mode; s.video.aspect = aspect; s.video.scale = scale;
  sv_host_apply_settings(sv(), &s);
}

static int pixel(SDL_Surface *a, int x, int y, uint8_t out[3]) {
  Uint8 r = 0, g = 0, b = 0, al = 0;
  if (!SDL_ReadSurfacePixel(a, x, y, &r, &g, &b, &al)) return 0;
  out[0] = r; out[1] = g; out[2] = b;
  return 1;
}
static uint64_t surface_hash(SDL_Surface *s) {
  uint64_t h = UINT64_C(1469598103934665603);
  uint8_t p[3];
  for (int y = 0; y < s->h; ++y) for (int x = 0; x < s->w; ++x) { pixel(s, x, y, p); h = (h ^ (uint64_t)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16))) * UINT64_C(1099511628211); }
  return h;
}
static uint64_t capture_hash(void) {
  SDL_Surface *a;
  uint64_t h = 0;
  sv_host_toast(sv(), "");
  a = sv_host_capture_output(sv());
  if (a != NULL) { h = surface_hash(a); SDL_DestroySurface(a); }
  return h;
}

/* Probes the rendered output against the scene model: the side columns (`want_sides`) and the native centre; native centre also against the artifact. */
static int check_output(const char *name, int native_w, int side, int want_sides) {
  SvExtendedLayout lay;
  SDL_Surface *a;
  static const unsigned rows[] = {4, 60, 111, 200, 219};
  int ok_sides = 1, ok_centre = 1, ok_native = 1, scale;
  char what[160];
  sv_host_toast(sv(), "");
  a = sv_host_capture_output(sv());
  snprintf(what, sizeof what, "%s: output captured", name);
  CHECK(a != NULL, what);
  if (a == NULL) return 0;
  if (!want_sides) { /* plain picture: compare the centre only through the fit layout of the original picture */
    SDL_DestroySurface(a);
    return 1;
  }
  snprintf(what, sizeof what, "%s: layout", name);
  CHECK(sv_layout_extended_dest(a->w, a->h, native_w, 224, side, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, what);
  scale = lay.native.w / native_w;
  snprintf(what, sizeof what, "%s: whole-number scale and exact centre", name);
  CHECK(scale >= 1 && lay.native.w == native_w * scale && lay.native.x == lay.surface.x + side * scale && lay.surface.w == (native_w + 2 * side) * scale, what);
  for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
    uint8_t native_row[GENESIS_FRAME_MAX_WIDTH * 3U];
    const int sy = lay.surface.y + (int)rows[i] * scale + scale / 2;
    if (genesis_vdp_frame_row_rgb888(last_native, rows[i], native_row) != 0) { ok_native = 0; continue; }
    for (int x = -side; x < native_w + side; x += 3) {
      uint8_t got[3], want[3];
      const int sx = lay.native.x + x * scale + scale / 2;
      ref_rgb(&scene, x, rows[i], want);
      if (!pixel(a, sx, sy, got)) { ok_sides = 0; continue; }
      if (x >= 0 && x < native_w) {
        if (memcmp(got, &native_row[(size_t)x * 3U], 3) != 0) ok_native = 0;   /* the centre IS the native picture */
        if (memcmp(got, want, 3) != 0) ok_centre = 0;                           /* ... and the native picture matches the model */
      } else if (memcmp(got, want, 3) != 0) {
        ok_sides = 0;
      }
    }
  }
  snprintf(what, sizeof what, "%s: scenery on both sides matches the independent model", name);
  CHECK(ok_sides, what);
  snprintf(what, sizeof what, "%s: native centre equals the native artifact", name);
  CHECK(ok_native, what);
  snprintf(what, sizeof what, "%s: native centre matches the model", name);
  CHECK(ok_centre, what);
  SDL_DestroySurface(a);
  return ok_sides && ok_centre && ok_native;
}

/* ---------------------------------------------------------------- failure-injection producers ------------------------------------------------ */
static unsigned producer_calls;
static int counting_producer(const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster, const GenesisFrameArtifact *native,
                             const GenesisFrameExtension *target, GenesisExtensionResult *out) {
  ++producer_calls;
  return genesis_vdp_produce_extension(vram, raster, native, target, out);
}
static GenesisExtensionReason injected_reason;
static int failing_producer(const uint8_t vram[GENESIS_VDP_VRAM_BYTES], const GenesisRasterFrameState *raster, const GenesisFrameArtifact *native,
                            const GenesisFrameExtension *target, GenesisExtensionResult *out) {
  (void)vram; (void)raster; (void)native; (void)target;
  memset(out, 0, sizeof *out);
  out->status = injected_reason != 0 ? GENESIS_EXT_UNAVAILABLE : GENESIS_EXT_NONE; /* reason 0: a producer that fails without saying why */
  out->reason = (uint8_t)injected_reason;
  return -1;
}

/* ---------------------------------------------------------------- guest invariance --------------------------------------------------------------- */
static int same_guest(const ScdMachine *a, const ScdMachine *b) {
  const GenesisRuntime *ha = (const GenesisRuntime *)a->vdp_host, *hb = (const GenesisRuntime *)b->vdp_host;
  return a->now == b->now && a->vdp_frames == b->vdp_frames && a->vdp_onset == b->vdp_onset && a->device_log == b->device_log &&
         a->vdp_dma_count == b->vdp_dma_count && a->vdp_dma_stall_ticks == b->vdp_dma_stall_ticks && a->main_dma_until == b->main_dma_until &&
         memcmp(&a->ga, &b->ga, sizeof a->ga) == 0 && memcmp(&a->gfx, &b->gfx, sizeof a->gfx) == 0 && memcmp(a->word_ram, b->word_ram, sizeof a->word_ram) == 0 &&
         memcmp(&ha->devices.vdp, &hb->devices.vdp, sizeof ha->devices.vdp) == 0 && memcmp(&a->stop, &b->stop, sizeof a->stop) == 0;
}

int main(void) {
  SDL_Surface *a;
  SvWidescreenCaps caps;
  uint64_t original_hash;
  const char *tmp = SDL_getenv("TMPDIR") ? SDL_getenv("TMPDIR") : "/tmp";
  char file[600];
  uint32_t acc, rej, tex;
  int i;
  snprintf(file, sizeof file, "%s/scd_ext_sdl_%d.cfg", tmp, (int)SDL_GetTicks());
  remove(file);
  SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv_unsafe("SDL_RENDER_DRIVER", "software", 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_SETTINGS", file, 1);
  SDL_setenv_unsafe("SEGARECOMP_VIEWER_UNTHROTTLED", "1", 1);
  m = (ScdMachine *)calloc(1U, sizeof *m);
  if (m == NULL) return 2;
  memset(&g_host, 0, sizeof g_host);
  fill_live_host(&live);

  /* ---- the registered host exposes the extension hook; the machine without an attached target does no extension work ---- */
  CHECK(live.extension != NULL, "the Sega CD viewer registers the Level 2 hook");
  fresh(m);
  CHECK(scd_vdp_present_enable(m) == 0, "presentation enabled");
  build_direct_scene(1);
  frame_no = 2U;
  run_to(m, ONSET(1));
  frame_begin();
  CHECK(adapter() != NULL && adapter()->request == 0U, "Original is the default: no extension requested");
  adapter()->producer = counting_producer;
  /* Original: zero extension work for many frames */
  for (i = 0; i < 4; ++i) CHECK(frame() == 0, "Original frame");
  CHECK(producer_calls == 0 && adapter()->produced == 0 && adapter()->failed == 0 && adapter()->result.status == GENESIS_EXT_NONE && sv_host_extension_side(sv()) == 0,
        "Original: the producer never runs, nothing is produced, nothing is shown");
  CHECK(check_output("Original", 320, 0, 0), "Original output");
  original_hash = capture_hash();
  sv_host_widescreen_caps(sv(), &caps);
  CHECK(caps.extended == SV_WS_AVAILABLE && caps.enhanced == SV_WS_UNAVAILABLE, "caps before any Level 2 frame: Level 2 capable, Level 3 unavailable");

  /* ---- Level 2 (requested live): real scenery both sides; truthfully Partial (no VRAM journal), never Available ---- */
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(frame() == 0, "Level 2 frame");
  CHECK(producer_calls == 1 && sv_host_extension_side(sv()) == 39, "Level 2: H40 gets 39 source pixels on each side");
  CHECK(adapter()->result.status == GENESIS_EXT_PARTIAL && (adapter()->result.flags & GENESIS_EXT_FLAG_NO_VRAM_JOURNAL) != 0,
        "Level 2: Partial with the no-VRAM-journal caveat (never forced Available)");
  CHECK(adapter()->result.sequence == last_onset && last_onset == frame_no, "the result is stamped with the presented Sega CD frame number");
  sv_host_widescreen_caps(sv(), &caps);
  CHECK(caps.extended == SV_WS_PARTIAL, "caps: Partial");
  check_output("Level 2 H40 direct scene", 320, 39, 1);
  CHECK(sv_host_widescreen_effective(sv()) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "effective mode is Level 2");

  /* ---- palette / priority / transparency are in the model (cases by cell column) and already probed; scrolling across the edges ---- */
  { static const int scrolls[][2] = {{5, 0}, {-7, 3}, {37, -100}, {511, 250}, {-511, 64}, {200, -39}};
    for (unsigned s = 0; s < sizeof scrolls / sizeof scrolls[0]; ++s) {
      char name[64];
      scene.hs_a = scrolls[s][0]; scene.hs_b = scrolls[s][1];
      write_scroll();
      CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "scrolled Level 2 frame");
      snprintf(name, sizeof name, "scroll A=%d B=%d", scene.hs_a, scene.hs_b);
      check_output(name, 320, 39, 1);
    }
    scene.hs_a = scene.hs_b = 0;
    write_scroll();
  }

  /* ---- Level 3 requested: Level 2 is what is shown; the request is kept ---- */
  set_mode(SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39 && sv_host_widescreen_effective(sv()) == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "Level 3 requested: Level 2 effective");
  check_output("Level 3 falls back to Level 2", 320, 39, 1);
  CHECK(sv_host_settings(sv())->video.widescreen == SV_WIDESCREEN_ENHANCED_GAMEPLAY, "the Level 3 preference is never overwritten");
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_4_3, SV_SCALE_FIT);
  CHECK(frame() == 0 && sv_host_extension_side(sv()) == 53, "4:3 pixel shape: 53 source pixels per side");
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);

  /* ---- back to Original: byte-identical to the Original output of the same frame content ---- */
  set_mode(SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  { const unsigned calls = producer_calls;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 0, "back to Original on the next frame");
    CHECK(producer_calls == calls && adapter()->result.status == GENESIS_EXT_NONE, "Original again: no extension work");
    CHECK(capture_hash() == original_hash, "Original after Level 2 is byte-identical to Original before"); }

  /* ---- H32 <-> H40 ---- */
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  vdp_reg(m, 12, 0x00);
  for (i = 0; i < 2; ++i) CHECK(frame() == 0, "frame after the H32 switch");
  CHECK(sv_host_extension_side(sv()) == 71, "H32 gets its own, larger extension (71)");
  check_output("Level 2 H32", 256, 71, 1);
  vdp_reg(m, 12, 0x81);
  for (i = 0; i < 2; ++i) CHECK(frame() == 0, "frame after returning to H40");
  CHECK(sv_host_extension_side(sv()) == 39, "and back to H40");

  /* ---- a mid-frame VDP change: palette from line 100 on (per-line CRAM) and Plane B name-table change at line 100 (no journal -> final VRAM) ---- */
  { const uint64_t base = ONSET(frame_no + 1U) - (UINT64_C(224) - 100U) * LINE_TICKS + 10U * MASTER;
    uint8_t native_row[GENESIS_FRAME_MAX_WIDTH * 3U];
    SDL_Surface *s;
    SvExtendedLayout lay;
    frame_begin();
    run_to(m, base);
    cram_word(m, 2, 7, 7, 7);                   /* index 2 becomes white from line 100 */
    put_be16(vdp_host()->devices.vdp.vram, 0xE000U + (3U * 64U + 1U) * 2U, 4U); /* Plane B cell (row 3, column 1) -> tile 4, mid-frame */
    scene.b[3][1].tile = 4U;
    CHECK(frame_finish() == 0, "frame with mid-frame VDP changes");
    CHECK(adapter()->result.status == GENESIS_EXT_PARTIAL && (adapter()->result.flags & GENESIS_EXT_FLAG_NO_VRAM_JOURNAL) != 0, "mid-frame change: Partial (no journal), not Available");
    /* The native picture and the extension agree on the same rule: per-line palette follows the change, tiles come from the frame's final VRAM */
    s = sv_host_capture_output(sv());
    CHECK(s != NULL && sv_layout_extended_dest(s->w, s->h, 320, 224, sv_host_extension_side(sv()), SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, "mid-frame: layout");
    if (s != NULL) {
      const int k = lay.native.w / 320;
      uint8_t before[3], after[3], want_before[3] = {0, 0, 0}, want_white[3] = {255, 255, 255};
      /* cell column 2 of Plane B is palette-0 index 3 ... column with index 2 is cell column 1 (tile 2); rows < 100 keep the old colour, rows >= 100 white */
      const int x = 8 + 4; /* inside cell column 1 (screen pixel 12), a Plane-B-only column? column 1 of a block of 8 has Plane A high priority: use column 5 */
      const int xb = 5 * 8 + 2; /* cell column 5 (c % 8 == 5: Plane A empty), B tile 1 + 5 % 4 = 2 -> index 2 */
      (void)x; (void)want_before;
      CHECK(genesis_vdp_frame_row_rgb888(last_native, 60, native_row) == 0, "native row 60");
      CHECK(pixel(s, lay.native.x + xb * k + k / 2, lay.surface.y + 60 * k + k / 2, before) && memcmp(before, &native_row[(size_t)xb * 3U], 3) == 0 && before[1] == 255 && before[0] == 0,
            "mid-frame: native row 60 keeps the old colour (green)");
      CHECK(pixel(s, lay.native.x + xb * k + k / 2, lay.surface.y + 150 * k + k / 2, after) && memcmp(after, want_white, 3) == 0, "mid-frame: native row 150 shows the new colour");
      { int mismatch = 0; /* the extension's rows follow the same per-line palette: the side column with Plane B index 2 on row 60 / 150 */
        for (int sx = -39; sx < 0; ++sx) {
          uint8_t at60[3], at150[3];
          ref_rgb(&scene, sx, 60, at60);
          ref_rgb(&scene, sx, 150, at150);
          if (at60[1] == 255 && at60[0] == 0 && at60[2] == 0) { /* index 2 */
            uint8_t g60[3], g150[3];
            pixel(s, lay.native.x + sx * k + k / 2, lay.surface.y + 60 * k + k / 2, g60);
            pixel(s, lay.native.x + sx * k + k / 2, lay.surface.y + 150 * k + k / 2, g150);
            if (memcmp(g60, at60, 3) != 0 || memcmp(g150, want_white, 3) != 0) mismatch = 1;
            (void)at150;
          }
        }
        CHECK(!mismatch, "mid-frame: the extension's side rows follow the same per-line palette as the native rows"); }
      SDL_DestroySurface(s);
    }
    /* restore the model (the CRAM entry returns for the next frames) */
    cram_word(m, 2, 0, 7, 0);
    scene.b[3][1].tile = 2U;
    put_be16(vdp_host()->devices.vdp.vram, 0xE000U + (3U * 64U + 1U) * 2U, 2U);
    for (i = 0; i < 2; ++i) CHECK(frame() == 0, "settling frame");
    (void)acc; (void)rej; (void)tex;
  }

  /* ---- failure injection: Original for that frame only, recover on the next valid frame ---- */
  { GenesisExtensionProducerFunction good = adapter()->producer;
    const size_t good_capacity = adapter()->capacity;
    SvWidescreenCaps c2;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "injection baseline: extended");
    /* 1. missing target: the extension is detached for a frame; the stale result of the previous frame never appears */
    frame_begin();
    scd_vdp_present_set_extension(m, NULL);
    CHECK(frame_finish() == 0 && sv_host_extension_side(sv()) == 0, "missing target: Original for that frame (stale result not shown)");
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "missing target: recovers on the next frame");
    /* 2. unsupported raster / 3. failed producer (no status) / 4. typed reasons */
    adapter()->producer = failing_producer;
    injected_reason = GENESIS_EXT_REASON_NO_RASTER;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 0 && adapter()->result.status == GENESIS_EXT_UNAVAILABLE && adapter()->result.reason == GENESIS_EXT_REASON_NO_RASTER,
          "unsupported raster: typed Unavailable, Original for that frame");
    sv_host_widescreen_caps(sv(), &c2);
    CHECK(c2.extended == SV_WS_UNAVAILABLE && sv_host_widescreen_effective(sv()) == SV_WIDESCREEN_ORIGINAL, "caps: Unavailable while it fails");
    injected_reason = 0;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 0 && adapter()->result.status == GENESIS_EXT_UNAVAILABLE && adapter()->result.reason == GENESIS_EXT_REASON_INVALID_REQUEST,
          "failed producer without a reason: invalid-request Unavailable, Original");
    CHECK(capture_hash() == original_hash, "the failed frame is the exact Original picture");
    adapter()->producer = good;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "failed producer: recovers on the next frame");
    /* 5. bad dimensions: an undersized target, and a plan that refuses */
    adapter()->capacity = 100;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 0 && adapter()->result.reason == GENESIS_EXT_REASON_TARGET_TOO_SMALL, "undersized target: Original for that frame");
    adapter()->capacity = good_capacity;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "undersized target: recovers");
    { uint8_t *keep = adapter()->pixels;
      adapter()->pixels = NULL;
      CHECK(frame() == 0 && sv_host_extension_side(sv()) == 0 && adapter()->result.reason == GENESIS_EXT_REASON_INVALID_REQUEST, "missing surface: Original for that frame");
      adapter()->pixels = keep; }
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "missing surface: recovers");
    /* 6. stale sequence: a result from another frame is refused by the presenter */
    frame_begin();
    run_to(m, ONSET(frame_no + 1U));
    ++frame_no;
    { uint64_t onset = 0;
      const GenesisFrameArtifact *f = scd_vdp_present_take(m, &onset);
      last_native = f;
      adapter()->result.sequence = onset - 1U;
      CHECK(f != NULL && live.present(live.ctx, f, onset) == 0 && sv_host_extension_side(sv()) == 0, "stale sequence: Original for that frame"); }
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "stale sequence: recovers on the next frame");
    /* 7. corrupt geometry in the result */
    frame_begin();
    run_to(m, ONSET(frame_no + 1U));
    ++frame_no;
    { uint64_t onset = 0;
      const GenesisFrameArtifact *f = scd_vdp_present_take(m, &onset);
      last_native = f;
      adapter()->result.pitch = 12;
      CHECK(f != NULL && live.present(live.ctx, f, onset) == 0 && sv_host_extension_side(sv()) == 0, "bad pitch: Original for that frame"); }
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39, "bad pitch: recovers");
    sv_host_extension_stats(sv(), &acc, &rej, &tex);
    CHECK(rej >= 2u, "rejections are counted");
  }

  /* ================================ part 2: background tiles produced by the graphics ASIC (stamp) and delivered by Word RAM DMA ================== */
  for (int h40 = 1; h40 >= 0; --h40) {
    const int native_w = h40 ? 320 : 256, side = h40 ? 39 : 71;
    char name[96];
    int built;
    fresh(m);
    scd_vdp_present_enable(m);
    run_to(m, ONSET(1));
    frame_no = 2U;
    set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
    frame_begin();
    built = build_stamp_dma_scene(h40);
    snprintf(name, sizeof name, "stamp scene built (ASIC output delivered by DMA) code %d", built);
    CHECK(built == 0, name);
    if (built != 0) continue;
    for (i = 0; i < 3; ++i) CHECK(frame() == 0, "stamp scene frame");
    CHECK(sv_host_extension_side(sv()) == side && adapter()->result.status == GENESIS_EXT_PARTIAL, "stamp scene: extended, Partial");
    snprintf(name, sizeof name, "stamp/DMA scene %s", h40 ? "H40" : "H32");
    check_output(name, native_w, side, 1);
    /* the scenery shows the colours the ASIC produced, not a default: both stamp colours occur on the left side */
    { SvExtendedLayout lay;
      SDL_Surface *s2 = sv_host_capture_output(sv());
      int seen3 = 0, seen5 = 0;
      CHECK(s2 != NULL && sv_layout_extended_dest(s2->w, s2->h, native_w, 224, side, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, "stamp scene layout");
      if (s2 != NULL) {
        const int k = lay.native.w / native_w;
        for (int x = -side; x < 0; ++x) {
          uint8_t p[3];
          if (!pixel(s2, lay.native.x + x * k + k / 2, lay.surface.y + 100 * k, p)) continue;
          if (p[2] == 255 && p[0] == 0 && p[1] == 0) seen3 = 1;       /* index 3 = blue */
          if (p[0] == 255 && p[2] == 255 && p[1] == 0) seen5 = 1;     /* index 5 = magenta */
        }
        CHECK(seen3 && seen5, "stamp scene: the ASIC-produced tile colours appear in the left scenery");
        SDL_DestroySurface(s2);
      }
    }
    /* the Word RAM changes after the DMA do not change the scenery: only the VRAM contents at frame time matter */
    memset(m->word_ram, 0xFF, sizeof m->word_ram);
    CHECK(frame() == 0, "frame after Word RAM changed");
    check_output("Word RAM rewritten after the DMA (VRAM unchanged)", native_w, side, 1);
    /* data absent: the tiles are cleared in VRAM (the DMA never happened): empty scenery is the backdrop, still typed Partial, native identical */
    memset(&vdp_host()->devices.vdp.vram[32], 0, 2048U);
    for (unsigned t = 1; t <= 64; ++t) scene.tiles[t].kind = T_EMPTY;
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == side, "absent tile data: still presented");
    check_output("absent tile data -> backdrop scenery", native_w, side, 1);
    CHECK(adapter()->result.status == GENESIS_EXT_PARTIAL, "absent data: Partial classification unchanged (never Available)");
  }

  /* ================================ part 3: ambiguous data -> typed Partial caveats ================================ */
  fresh(m);
  scd_vdp_present_enable(m);
  run_to(m, ONSET(1));
  frame_no = 2U;
  build_direct_scene(1);
  frame_begin();
  vdp_reg(m, 17, 0x02); /* the window owns the left columns: Plane A omitted off-screen on those rows */
  for (i = 0; i < 2; ++i) CHECK(frame() == 0, "window-edge frame");
  CHECK(adapter()->result.status == GENESIS_EXT_PARTIAL && (adapter()->result.flags & GENESIS_EXT_FLAG_WINDOW_EDGE) != 0 && sv_host_extension_side(sv()) == 39,
        "window over an edge: typed Partial caveat, still extended");
  { SvExtendedLayout lay;
    SDL_Surface *s3 = sv_host_capture_output(sv());
    int ok = 1;
    CHECK(s3 != NULL && sv_layout_extended_dest(s3->w, s3->h, 320, 224, 39, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER, &lay) == 0, "window layout");
    if (s3 != NULL) {
      const int k = lay.native.w / 320;
      Scene noa = scene;
      memset(noa.a, 0, sizeof noa.a);
      for (int x = -39; x < 0; ++x) { /* the left scenery has no Plane A (never repeated or guessed) */
        uint8_t got[3], want[3];
        ref_rgb(&noa, x, 100, want);
        if (!pixel(s3, lay.native.x + x * k + k / 2, lay.surface.y + 100 * k + k / 2, got) || memcmp(got, want, 3) != 0) ok = 0;
      }
      CHECK(ok, "window edge: Plane A is omitted left of the picture, Plane B scenery is exact");
      SDL_DestroySurface(s3);
    }
  }
  vdp_reg(m, 17, 0x00);
  vdp_reg(m, 11, 0x04); /* 2-cell column vertical scroll with differing columns */
  port(m, 0xC00004U, 4U, 0x40000010U);
  for (unsigned g = 0; g < 4; ++g) port(m, 0xC00000U, 2U, g * 8U);
  for (i = 0; i < 2; ++i) CHECK(frame() == 0, "column-scroll frame");
  CHECK(adapter()->result.status == GENESIS_EXT_PARTIAL && (adapter()->result.flags & GENESIS_EXT_FLAG_COLUMN_VSCROLL) != 0 && sv_host_extension_side(sv()) == 39,
        "per-column vertical scroll: typed Partial caveat, still extended");

  /* ================================ part 4: guest invariance, Original vs Level 2 (and live toggling) ================================ */
  {
    enum { FRAMES = 14 };
    static GenesisFrameArtifact ref_frames[FRAMES];
    static uint64_t ref_hash[FRAMES], ref_present_ticks[FRAMES];
    static ScdMachine *plain;
    static const SvWidescreenMode plan_modes[FRAMES] = {
        SV_WIDESCREEN_ORIGINAL, SV_WIDESCREEN_ORIGINAL, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_WIDESCREEN_EXTENDED_BACKGROUNDS,
        SV_WIDESCREEN_ENHANCED_GAMEPLAY, SV_WIDESCREEN_ORIGINAL, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_WIDESCREEN_ORIGINAL, SV_WIDESCREEN_ORIGINAL,
        SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_WIDESCREEN_ORIGINAL, SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_WIDESCREEN_EXTENDED_BACKGROUNDS};
    ScdMachine *machines[2];
    uint64_t produced_at_start = 0;
    unsigned run;
    plain = (ScdMachine *)calloc(1U, sizeof *plain);
    machines[0] = plain;
    machines[1] = m;
    for (run = 0; run < 2U; ++run) { /* run 0: no viewer at all; run 1: the viewer with live mode switches */
      uint64_t digest[FRAMES];
      m = machines[run];
      use_viewer = (int)run;
      fresh(m);
      scd_vdp_present_enable(m);
      run_to(m, ONSET(1));
      frame_no = 2U;
      set_mode(SV_WIDESCREEN_ORIGINAL, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
      if (run == 1U) { frame_begin(); produced_at_start = adapter()->produced; }
      CHECK(build_stamp_dma_scene(1) == 0, "invariance scene built");
      for (i = 0; i < FRAMES; ++i) {
        if (run == 1U) set_mode(plan_modes[i], SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
        if (frame_begin() != 0) { CHECK(0, "invariance frame begin"); break; }
        if (i == 7) { cram_word(m, 3, 1, 2, 3); port(m, 0xC00004U, 2U, 0x8700U | 5U); }  /* a guest-visible VDP event in the middle of the toggling */
        CHECK(frame_finish() == 0, "invariance frame");
        {
          uint64_t h = UINT64_C(1469598103934665603);
          const unsigned char *vdpb = (const unsigned char *)&((GenesisRuntime *)m->vdp_host)->devices.vdp;
          for (size_t b = 0; b < sizeof(GenesisVdpState); ++b) h = (h ^ vdpb[b]) * UINT64_C(1099511628211);
          for (size_t b = 0; b < sizeof m->word_ram; b += 7U) h = (h ^ m->word_ram[b]) * UINT64_C(1099511628211);
          h = (h ^ m->now) * UINT64_C(1099511628211); h = (h ^ m->vdp_frames) * UINT64_C(1099511628211); h = (h ^ m->device_log) * UINT64_C(1099511628211);
          h = (h ^ m->vdp_dma_count) * UINT64_C(1099511628211); h = (h ^ m->gfx.operations) * UINT64_C(1099511628211); h = (h ^ m->gfx.lines_rendered) * UINT64_C(1099511628211);
          h = (h ^ m->main_dma_until) * UINT64_C(1099511628211); h = (h ^ m->vdp_dma_stall_ticks) * UINT64_C(1099511628211);
          digest[i] = h;
        }
        if (run == 0U) { ref_frames[i] = *last_native; ref_hash[i] = digest[i]; ref_present_ticks[i] = m->now; }
        else {
          CHECK(digest[i] == ref_hash[i] && m->now == ref_present_ticks[i], "guest state (VDP, Word RAM, scheduler counters, DMA, ASIC) identical to the viewer-less run");
          CHECK(memcmp(last_native, &ref_frames[i], sizeof ref_frames[i]) == 0, "native frame artifact (digest included) identical to the viewer-less run");
          CHECK(sv_host_extension_side(sv()) == (plan_modes[i] == SV_WIDESCREEN_ORIGINAL ? 0 : 39), "the mode switched live applies to exactly this frame");
        }
      }
    }
    CHECK(same_guest(plain, m), "final guest state identical between the viewer-less run and the live-toggled Level 2 run");
    CHECK(adapter()->produced - produced_at_start == 8U, "extension work happened only on the frames that requested it");
    scd_vdp_release(plain);
    free(plain);
    m = machines[1];
    use_viewer = 1;
  }

  /* ================================ part 5: incomplete initial capture ================================ */
  fresh(m);
  scd_vdp_present_enable(m);
  build_direct_scene(1);
  set_mode(SV_WIDESCREEN_EXTENDED_BACKGROUNDS, SV_ASPECT_ORIGINAL, SV_SCALE_INTEGER);
  frame_no = 0U;
  { const uint64_t produced_before = adapter()->produced, failed_before = adapter()->failed;
    frame_begin();
    CHECK(frame_finish() == 1, "onset 1 (the power-on run-in): no frame is presented");
    CHECK(adapter()->produced == produced_before && adapter()->failed == failed_before, "incomplete first frame: no extension is produced for it");
    CHECK(frame() == 0 && sv_host_extension_side(sv()) == 39 && adapter()->result.sequence == last_onset, "the first complete frame is extended and stamped with its own number"); }

  SDL_RemovePath(file);
  genesis_sdl3_viewer_close(g_host.sdl);
  memset(&g_host, 0, sizeof g_host);

  /* ================================ part 6: an older-style viewer (the extension is never attached) ================================ */
  { GenesisSdl3Viewer *old = genesis_sdl3_viewer_open("older-style Sega CD viewer", 3);
    GenesisViewerHost oh;
    ScdLiveHost no_hook;
    SvHost *h;
    SvWidescreenCaps oc;
    uint64_t base = 0;
    CHECK(old != NULL, "older-style viewer opens");
    if (old != NULL) {
      memset(&no_hook, 0, sizeof no_hook);
      CHECK(no_hook.extension == NULL, "an older host has no extension hook");
      genesis_sdl3_viewer_host(old, &oh);
      h = genesis_sdl3_viewer_sv_host(old);
      { SvSettings st = *sv_host_settings(h);
        st.video.widescreen = SV_WIDESCREEN_EXTENDED_BACKGROUNDS;
        sv_host_apply_settings(h, &st); }
      fresh(m);
      scd_vdp_present_enable(m);
      build_direct_scene(1);
      run_to(m, ONSET(1));
      frame_no = 2U;
      for (i = 0; i < 3; ++i) {
        uint64_t onset = 0;
        const GenesisFrameArtifact *f;
        oh.window_closed(oh.ctx);
        scd_vdp_present_set_extension(m, NULL);
        run_to(m, ONSET(frame_no + 1U));
        ++frame_no;
        f = scd_vdp_present_take(m, &onset);
        CHECK(f != NULL && oh.present(oh.ctx, f) == 0, "older-style present");
        if (i == 0) { sv_host_toast(h, ""); a = sv_host_capture_output(h); if (a != NULL) { base = surface_hash(a); SDL_DestroySurface(a); } }
      }
      sv_host_widescreen_caps(h, &oc);
      CHECK(oc.extended == SV_WS_UNAVAILABLE && oc.enhanced == SV_WS_UNAVAILABLE, "older-style: Level 2 requested but reported Unavailable");
      CHECK(sv_host_extension_side(h) == 0 && sv_host_widescreen_effective(h) == SV_WIDESCREEN_ORIGINAL, "older-style: presents the Original picture");
      sv_host_toast(h, "");
      a = sv_host_capture_output(h);
      CHECK(a != NULL && surface_hash(a) == base, "older-style output is stable Original");
      if (a != NULL) SDL_DestroySurface(a);
      CHECK(sv_host_settings(h)->video.widescreen == SV_WIDESCREEN_EXTENDED_BACKGROUNDS, "the requested mode is kept");
      genesis_sdl3_viewer_close(old);
    }
  }

  (void)original_hash;
  remove(file);
  scd_vdp_release(m);
  free(m);
  if (failures != 0) { printf("sega_cd_viewer_extension_sdl_test: %d failure(s)\n", failures); return 1; }
  printf("sega_cd_viewer_extension_sdl_test: ok\n");
  return 0;
}
