#include "sms_render.h"

#include <string.h>

/* Mode 4 renderer: see sms_render.h and machine contract section 9.8. Rules cite MacDonald's VDP document (MD-VDP) and
 * SMS Power! through the contract; nothing is derived from a reference emulator's source. */

#define R0_VSCROLL_LOCK 0x80u
#define R0_HSCROLL_LOCK 0x40u
#define R0_LEFT_BLANK 0x20u
#define R0_SPRITE_SHIFT 0x08u
#define R1_DISPLAY 0x40u
#define R1_SPRITE_8X16 0x02u
#define R1_SPRITE_ZOOM 0x01u

void sms_renderer_init(SmsRenderer *r, SmsVdp *vdp, SmsFrameRecord *records, uint32_t capacity) {
  memset(r, 0, sizeof *r);
  r->vdp = vdp;
  r->records = records;
  r->record_capacity = capacity;
}

static void hook(void *context, const SmsVdp *vdp, uint32_t line, uint64_t cycles) {
  SmsRenderer *r = (SmsRenderer *)context;
  (void)cycles;
  sms_render_line(r, vdp, line, vdp->frame);
}

void sms_renderer_attach(SmsRenderer *r) {
  r->vdp->line_hook = hook;
  r->vdp->line_hook_context = r;
}

/* 4bpp planar row of a pattern: colour index 0-15 of pixel `col` (0 = leftmost) */
static unsigned pattern_pixel(const uint8_t *vram, unsigned pattern, unsigned row, unsigned col) {
  const uint8_t *p = &vram[((pattern * 32u) + row * 4u) & (SMS_VDP_VRAM_SIZE - 1u)];
  const unsigned bit = 7u - col;
  return ((p[0] >> bit) & 1u) | (((p[1] >> bit) & 1u) << 1) | (((p[2] >> bit) & 1u) << 2) | (((p[3] >> bit) & 1u) << 3);
}

typedef struct SpriteHit {
  uint8_t x_index; /* SAT X/N pair index */
  uint8_t row;     /* row within the (zoomed-down) sprite */
} SpriteHit;

void sms_extension_reset(SmsExtension *ext) {
  if (ext == NULL) return;
  ext->side = 0u;
  ext->ok_lines = 0u;
  ext->geometry_fail = 0u;
  ext->first_reason = SMS_EXT_REASON_NONE;
  memset(&ext->result, 0, sizeof ext->result);
  ext->unsupported_frames = 0u;
}

const char *sms_ext_class_name(SmsExtClass cls) {
  switch (cls) {
    case SMS_EXT_NONE: return "none";
    case SMS_EXT_WRAPPED: return "wrapped";
    case SMS_EXT_DISTINCT: return "distinct";
    case SMS_EXT_UNAVAILABLE: return "unavailable";
  }
  return "invalid";
}

const char *sms_ext_reason_name(SmsExtReason reason) {
  switch (reason) {
    case SMS_EXT_REASON_NONE: return "none";
    case SMS_EXT_REASON_LEFT_MASK: return "left_column_mask";
    case SMS_EXT_REASON_VSCROLL_LOCK: return "vscroll_lock";
    case SMS_EXT_REASON_GEOMETRY: return "geometry";
    case SMS_EXT_REASON_INCOMPLETE: return "incomplete_frame";
  }
  return "invalid";
}

/* line 0: latch the requested side for the frame height (a mid-frame request change applies to the next frame) */
static void ext_begin_frame(SmsExtension *e, unsigned height) {
  const uint32_t want = height == 224u ? e->request_side_224 : e->request_side_192;
  e->ok_lines = 0u;
  e->first_reason = SMS_EXT_REASON_NONE;
  e->geometry_fail = (uint8_t)(want > SMS_EXT_MAX_SIDE);
  e->side = e->geometry_fail ? 0u : want;
}

static void ext_fail_line(SmsExtension *e, unsigned line, SmsExtReason reason) {
  memset(e->work_left[line], 0, sizeof e->work_left[line]);
  memset(e->work_right[line], 0, sizeof e->work_right[line]);
  if (e->first_reason == SMS_EXT_REASON_NONE) e->first_reason = reason;
}

/* One line of extension from the line's own background samples: the sample at screen x outside [0, 256) is the one at x & 255 (name-table wrap),
 * so the left side is the last `side` source samples and the right side the first `side`. `color`/`pal` NULL = display disabled (backdrop). */
static void ext_emit_line(SmsExtension *e, const SmsVdp *v, unsigned line, unsigned r0, const uint8_t *color, const uint8_t *pal,
                          uint8_t backdrop) {
  const unsigned side = e->side;
  unsigned i;
  if (color != NULL && (r0 & R0_LEFT_BLANK) != 0u) {
    ext_fail_line(e, line, SMS_EXT_REASON_LEFT_MASK);
    return;
  }
  if (color != NULL && (r0 & R0_VSCROLL_LOCK) != 0u) {
    ext_fail_line(e, line, SMS_EXT_REASON_VSCROLL_LOCK);
    return;
  }
  for (i = 0; i < side; ++i) {
    const unsigned l = SMS_FB_WIDTH - side + i, rr = i;
    e->work_left[line][i] = color == NULL || pal[l] == 255u ? backdrop : (uint8_t)(v->cram[(unsigned)pal[l] * 16u + color[l]] & 0x3Fu);
    e->work_right[line][i] = color == NULL || pal[rr] == 255u ? backdrop : (uint8_t)(v->cram[(unsigned)pal[rr] * 16u + color[rr]] & 0x3Fu);
  }
  ++e->ok_lines;
}

static void ext_finish_frame(SmsRenderer *r) {
  SmsExtension *e = r->ext;
  SmsExtResult *res = &e->result;
  memset(res, 0, sizeof *res);
  res->sequence = r->frames_completed;
  res->height = r->height;
  if (e->geometry_fail) {
    res->cls = SMS_EXT_UNAVAILABLE;
    res->reason = SMS_EXT_REASON_GEOMETRY;
  } else if (e->side == 0u) {
    res->cls = SMS_EXT_NONE;
  } else if (e->ok_lines == r->height) {
    res->cls = SMS_EXT_WRAPPED; /* see the block comment in sms_render.h: no Mode 4 line can be DISTINCT */
    res->side = e->side;
    memcpy(e->left, e->work_left, sizeof e->left);
    memcpy(e->right, e->work_right, sizeof e->right);
  } else {
    res->cls = SMS_EXT_UNAVAILABLE;
    res->reason = e->first_reason != SMS_EXT_REASON_NONE ? e->first_reason : SMS_EXT_REASON_INCOMPLETE;
  }
}

static void finish_frame(SmsRenderer *r) {
  SmsSha256 sha;
  SmsFrameRecord *rec;
  memcpy(r->last, r->pixels, (size_t)SMS_FB_WIDTH * r->height);
  r->last_height = r->height;
  r->last_frame = r->vdp->frame;
  ++r->frames_completed;
  r->in_frame = 0;
  if (r->records != NULL && r->record_count < r->record_capacity) {
    rec = &r->records[r->record_count++];
    rec->frame = r->vdp->frame;
    rec->height = r->height;
    rec->trace_count = r->vdp->trace_count;
    sms_sha256_init(&sha);
    sms_sha256_update(&sha, r->last, (size_t)SMS_FB_WIDTH * r->height);
    sms_sha256_final(&sha, rec->sha256);
  } else {
    ++r->record_dropped;
  }
  if (r->ext != NULL) ext_finish_frame(r);
}

void sms_render_line(SmsRenderer *r, const SmsVdp *v, uint32_t line, uint64_t frame) {
  const SmsVdpMode mode = sms_vdp_mode(v);
  uint8_t bg_color[SMS_FB_WIDTH], bg_prio[SMS_FB_WIDTH], bg_pal[SMS_FB_WIDTH];
  uint8_t spr_color[SMS_FB_WIDTH];
  uint8_t *out;
  const unsigned r0 = v->reg[0], r1 = v->reg[1];
  unsigned x;
  (void)frame;
  if (mode == SMS_VDP_MODE_UNSUPPORTED) {
    /* never rendered: T004 reports the typed stop; the extension does not accept the mode either */
    if (line == 0u && r->ext != NULL && (r->ext->request_side_192 != 0u || r->ext->request_side_224 != 0u)) ++r->ext->unsupported_frames;
    return;
  }
  if (line == 0u) {
    r->height = sms_vdp_active_lines(mode);
    r->vscroll = v->reg[9];
    r->in_frame = 1u;
    if (r->ext != NULL) ext_begin_frame(r->ext, r->height);
  }
  if (!r->in_frame || line >= r->height) return;
  out = &r->pixels[line * SMS_FB_WIDTH];
  {
    const uint8_t backdrop = (uint8_t)(v->cram[16u + (v->reg[7] & 15u)] & 0x3Fu);
    const int ext_on = r->ext != NULL && r->ext->side != 0u;
    if ((r1 & R1_DISPLAY) == 0u) {
      memset(out, backdrop, SMS_FB_WIDTH);
      if (ext_on) ext_emit_line(r->ext, v, line, r0, NULL, NULL, backdrop);
    } else {
      /* ---- background ---- */
      const int tall = r->height == 224u;
      const unsigned name_base = tall ? (((v->reg[2] & 0x0Cu) << 10) | 0x700u) : ((v->reg[2] & 0x0Eu) << 10);
      const unsigned hscroll = ((r0 & R0_HSCROLL_LOCK) != 0u && line < 16u) ? 0u : v->reg[8];
      const unsigned fine = hscroll & 7u;
      for (x = 0; x < SMS_FB_WIDTH; ++x) {
        const unsigned screen_col = x >> 3;
        unsigned vs = ((r0 & R0_VSCROLL_LOCK) != 0u && screen_col >= 24u) ? 0u : r->vscroll;
        unsigned ry, row, col, pixel_in_tile, entry, pattern, prow, color;
        const uint8_t *nt;
        /* the source pixel column: screen x minus the scroll, wrapping over the 32-column table */
        const unsigned src = (x - hscroll) & 255u;
        if (x < fine) {
          bg_color[x] = 0;
          bg_prio[x] = 0;
          bg_pal[x] = 255u; /* fine-scroll gap: backdrop (U8) */
          continue;
        }
        ry = tall ? ((line + vs) & 255u) : ((line + vs) % 224u);
        row = ry >> 3;
        col = src >> 3;
        pixel_in_tile = src & 7u;
        nt = &v->vram[(name_base + row * 64u + col * 2u) & (SMS_VDP_VRAM_SIZE - 1u)];
        entry = (unsigned)nt[0] | ((unsigned)nt[1] << 8);
        pattern = entry & 0x1FFu;
        prow = ry & 7u;
        if ((entry & 0x400u) != 0u) prow = 7u - prow;
        if ((entry & 0x200u) != 0u) pixel_in_tile = 7u - pixel_in_tile;
        color = pattern_pixel(v->vram, pattern, prow, pixel_in_tile);
        bg_color[x] = (uint8_t)color;
        bg_prio[x] = (uint8_t)((entry >> 12) & 1u);
        bg_pal[x] = (uint8_t)((entry >> 11) & 1u);
      }
      if (ext_on) ext_emit_line(r->ext, v, line, r0, bg_color, bg_pal, backdrop); /* background only; sprites are not extended */
      /* ---- sprites ---- */
      memset(spr_color, 0, sizeof spr_color);
      {
        const unsigned sat = (v->reg[5] & 0x7Eu) << 7;
        const unsigned zoom = (r1 & R1_SPRITE_ZOOM) != 0u ? 2u : 1u;
        const unsigned h = ((r1 & R1_SPRITE_8X16) != 0u ? 16u : 8u) * zoom;
        const unsigned pattern_base = (v->reg[6] & 0x04u) != 0u ? 256u : 0u;
        SpriteHit hits[SMS_SPRITES_PER_LINE];
        unsigned count = 0, i;
        int overflow = 0, collision = 0;
        for (i = 0; i < 64u; ++i) {
          const unsigned y = v->vram[(sat + i) & (SMS_VDP_VRAM_SIZE - 1u)];
          unsigned rel;
          if (!tall && y == 0xD0u) break; /* terminator: 192-line mode only */
          rel = (line - (y + 1u)) & 255u;
          if (rel >= h) continue;
          if (count == SMS_SPRITES_PER_LINE) {
            overflow = 1;
            break;
          }
          hits[count].x_index = (uint8_t)i;
          hits[count].row = (uint8_t)(rel / zoom);
          ++count;
        }
        for (i = 0; i < count; ++i) {
          const unsigned base = (sat + 0x80u + 2u * hits[i].x_index) & (SMS_VDP_VRAM_SIZE - 1u);
          const int sx = (int)v->vram[base] - ((r0 & R0_SPRITE_SHIFT) != 0u ? 8 : 0);
          unsigned tile = v->vram[base + 1u];
          unsigned px;
          if ((r1 & R1_SPRITE_8X16) != 0u) tile &= 0xFEu;
          tile += pattern_base;
          {
            const unsigned row = hits[i].row;
            const unsigned t = tile + (row >> 3);
            for (px = 0; px < 8u * zoom; ++px) {
              const int dx = sx + (int)px;
              unsigned color;
              if (dx < 0 || dx > 255) continue;
              color = pattern_pixel(v->vram, t, row & 7u, px / zoom);
              if (color == 0u) continue;
              if (spr_color[dx] != 0u) collision = 1;
              else spr_color[dx] = (uint8_t)color;
            }
          }
        }
        sms_vdp_set_sprite_flags(r->vdp, overflow, collision);
      }
      /* ---- compose ---- */
      for (x = 0; x < SMS_FB_WIDTH; ++x) {
        uint8_t value;
        if (bg_pal[x] == 255u) value = backdrop;
        else value = (uint8_t)(v->cram[(unsigned)bg_pal[x] * 16u + bg_color[x]] & 0x3Fu);
        if (spr_color[x] != 0u && !(bg_prio[x] != 0u && bg_color[x] != 0u && bg_pal[x] != 255u))
          value = (uint8_t)(v->cram[16u + spr_color[x]] & 0x3Fu);
        out[x] = value;
      }
      if ((r0 & R0_LEFT_BLANK) != 0u) memset(out, backdrop, 8);
    }
  }
  if (line + 1u == r->height) finish_frame(r);
}

void sms_renderer_last_sha256(const SmsRenderer *r, uint8_t out[32]) {
  SmsSha256 sha;
  sms_sha256_init(&sha);
  sms_sha256_update(&sha, r->last, (size_t)SMS_FB_WIDTH * r->last_height);
  sms_sha256_final(&sha, out);
}
