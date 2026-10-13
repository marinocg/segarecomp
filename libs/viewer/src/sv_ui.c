/* Bitmap-font text and stepped (pixel-chamfered) frame primitives of the viewer UI. No font file, no font library: the atlas is generated offline into
 * sv_font_data.c and turned into one nearest-filtered texture. Everything is whole-number scaled so the text stays crisp at any display density. */
#include <stdlib.h>
#include <string.h>

#include "sv_internal.h"

static SDL_Texture *font_texture(SvHost *h) {
  if (h->font_tex == NULL) {
    const int w = sv_font_atlas_w, hh = sv_font_atlas_h, row_bytes = (w + 7) / 8;
    SDL_Surface *surface = SDL_CreateSurface(w, hh, SDL_PIXELFORMAT_RGBA32);
    int x, y;
    if (surface == NULL) return NULL;
    for (y = 0; y < hh; ++y) {
      Uint8 *row = (Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch;
      for (x = 0; x < w; ++x) {
        const int on = (sv_font_atlas_bits[y * row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1;
        row[x * 4 + 0] = 255; row[x * 4 + 1] = 255; row[x * 4 + 2] = 255; row[x * 4 + 3] = on ? 255 : 0;
      }
    }
    h->font_tex = SDL_CreateTextureFromSurface(h->renderer, surface);
    SDL_DestroySurface(surface);
    if (h->font_tex != NULL) {
      SDL_SetTextureScaleMode(h->font_tex, SDL_SCALEMODE_NEAREST);
      SDL_SetTextureBlendMode(h->font_tex, SDL_BLENDMODE_BLEND);
    }
  }
  return h->font_tex;
}

int sv_ui_text_w(SvFontId face, int scale, const char *text) { return sv_font_text_width(face, text) * (scale < 1 ? 1 : scale); }
int sv_ui_line_h(SvFontId face, int scale) { return sv_font_line_height(face) * (scale < 1 ? 1 : scale); }

void sv_ui_text(SvHost *h, SvFontId face, int scale, int x, int y, SDL_Color c, const char *text) {
  SDL_Texture *tex = font_texture(h);
  const SvFontFace *f;
  int pen = x, i;
  if (tex == NULL || text == NULL) return;
  if (scale < 1) scale = 1;
  f = &sv_font_faces[face < 0 || face >= SV_FONT_COUNT ? SV_FONT_BODY : face];
  SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
  SDL_SetTextureAlphaMod(tex, c.a);
  for (i = 0; text[i] != '\0'; ++i) {
    const SvGlyph *g = sv_font_glyph(face, (unsigned char)text[i]);
    if (g->w > 0 && g->h > 0) {
      SDL_FRect src, dst;
      src.x = (float)g->x; src.y = (float)g->y; src.w = (float)g->w; src.h = (float)g->h;
      dst.x = (float)(pen + g->xoff * scale);
      dst.y = (float)(y + (f->ascent + g->yoff) * scale);
      dst.w = (float)(g->w * scale); dst.h = (float)(g->h * scale);
      SDL_RenderTexture(h->renderer, tex, &src, &dst);
    }
    pen += g->advance * scale;
  }
}

void sv_ui_text_shadow(SvHost *h, SvFontId face, int scale, int x, int y, SDL_Color c, const char *text) {
  const SDL_Color shadow = {2, 6, 18, (Uint8)(c.a > 200 ? 220 : c.a)};
  sv_ui_text(h, face, scale, x + scale, y + scale, shadow, text);
  sv_ui_text(h, face, scale, x, y, c, text);
}

/* A rectangle whose four corners are cut in `k`-pixel steps (pixel-art "chamfer", the repository's restrained geometry). */
void sv_ui_chamfer(SvHost *h, int x, int y, int w, int hh, int k, SDL_Color c) {
  if (w < 2 * k || hh < 2 * k || k < 1) { sv_host_fill(h, x, y, w, hh, c); return; }
  sv_host_fill(h, x + k, y, w - 2 * k, hh, c);
  sv_host_fill(h, x, y + k, w, hh - 2 * k, c);
  if (k >= 2) { /* second step: the diagonal corner pixel */
    sv_host_fill(h, x + k / 2, y + k / 2, k - k / 2, k - k / 2, c);
    sv_host_fill(h, x + w - k, y + k / 2, k - k / 2, k - k / 2, c);
    sv_host_fill(h, x + k / 2, y + hh - k, k - k / 2, k - k / 2, c);
    sv_host_fill(h, x + w - k, y + hh - k, k - k / 2, k - k / 2, c);
  }
}

/* Branded panel frame: soft stepped glow, dark outline, edge colour, lit top-left line, fill. */
void sv_ui_frame(SvHost *h, int x, int y, int w, int hh, int u, SDL_Color fill, SDL_Color edge, SDL_Color light, int glow) {
  const SDL_Color outline = {4, 10, 26, 255};
  if (glow) {
    SDL_Color g1 = {46, 104, 214, 26}, g2 = {46, 104, 214, 14};
    sv_ui_chamfer(h, x - 3 * u, y - 3 * u, w + 6 * u, hh + 6 * u, 3 * u, g2);
    sv_ui_chamfer(h, x - 2 * u, y - 2 * u, w + 4 * u, hh + 4 * u, 3 * u, g1);
  }
  sv_ui_chamfer(h, x - u, y - u, w + 2 * u, hh + 2 * u, 3 * u, outline);
  sv_ui_chamfer(h, x, y, w, hh, 3 * u, edge);
  sv_ui_chamfer(h, x + u, y + u, w - 2 * u, hh - 2 * u, 2 * u, fill);
  sv_host_fill(h, x + 3 * u, y + u, w - 6 * u, u, light);          /* lit top edge */
  sv_host_fill(h, x + u, y + 3 * u, u, hh - 6 * u, light);         /* lit left edge */
}

SDL_Texture *sv_ui_wordmark(SvHost *h) {
  if (!h->wordmark_tried) {
    h->wordmark_tried = 1;
    h->wordmark = sv_host_load_asset(h, "wordmark.png");
  }
  return h->wordmark;
}

void sv_ui_destroy(SvHost *h) {
  if (h->font_tex != NULL) SDL_DestroyTexture(h->font_tex);
  if (h->wordmark != NULL) SDL_DestroyTexture(h->wordmark);
  h->font_tex = NULL;
  h->wordmark = NULL;
}
