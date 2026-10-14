#include <string.h>

#include "sv_font.h"

const SvGlyph *sv_font_glyph(SvFontId face, unsigned char ch) {
  if (face < 0 || face >= SV_FONT_COUNT) face = SV_FONT_BODY;
  if (ch < SV_FONT_FIRST || ch > SV_FONT_LAST) ch = '?';
  return &sv_font_faces[face].glyphs[ch - SV_FONT_FIRST];
}

int sv_font_text_width_n(SvFontId face, const char *text, int bytes) {
  int w = 0, i;
  if (text == NULL) return 0;
  for (i = 0; i < bytes && text[i] != '\0'; ++i) w += sv_font_glyph(face, (unsigned char)text[i])->advance;
  return w;
}

int sv_font_text_width(SvFontId face, const char *text) { return text == NULL ? 0 : sv_font_text_width_n(face, text, (int)strlen(text)); }

int sv_font_line_height(SvFontId face) {
  const SvFontFace *f = &sv_font_faces[face < 0 || face >= SV_FONT_COUNT ? SV_FONT_BODY : face];
  return f->ascent + f->descent + 1;
}

int sv_font_fit(SvFontId face, const char *text, int scale, int max_px) {
  int w = 0, i;
  if (text == NULL || scale < 1) return 0;
  for (i = 0; text[i] != '\0'; ++i) {
    const int adv = sv_font_glyph(face, (unsigned char)text[i])->advance * scale;
    if (w + adv > max_px) break;
    w += adv;
  }
  return i;
}

int sv_font_ellipsize(SvFontId face, const char *text, int scale, int max_px, char *out, int cap) {
  const int len = text != NULL ? (int)strlen(text) : 0;
  int keep, w;
  if (cap <= 0) return 0;
  if (text == NULL || scale < 1) { out[0] = '\0'; return 0; }
  if (sv_font_text_width(face, text) * scale <= max_px) {
    keep = len < cap - 1 ? len : cap - 1;
    memcpy(out, text, (size_t)keep);
    out[keep] = '\0';
    return sv_font_text_width_n(face, out, keep) * scale;
  }
  {
    const int dots = sv_font_glyph(face, '.')->advance * 3 * scale;
    keep = sv_font_fit(face, text, scale, max_px - dots);
    if (keep > cap - 4) keep = cap - 4;
    if (keep < 0) keep = 0;
    memcpy(out, text, (size_t)keep);
    memcpy(out + keep, "...", 4);
    w = sv_font_text_width_n(face, out, keep + 3) * scale;
  }
  return w;
}

int sv_font_wrap(SvFontId face, const char *text, int scale, int max_px, int max_lines, int *start, int *len, int *truncated) {
  int n = 0, pos = 0;
  const int total = text != NULL ? (int)strlen(text) : 0;
  if (truncated != NULL) *truncated = 0;
  if (text == NULL || max_lines < 1 || scale < 1) return 0;
  while (pos < total && n < max_lines) {
    int line_end, fit, last_space;
    while (pos < total && text[pos] == ' ') ++pos;
    if (pos >= total) break;
    fit = sv_font_fit(face, text + pos, scale, max_px);
    if (fit < 1) fit = 1;
    if (pos + fit >= total) line_end = total;
    else {
      last_space = fit;
      while (last_space > 0 && text[pos + last_space] != ' ') --last_space;
      line_end = last_space > 0 ? pos + last_space : pos + fit; /* a single over-long word is hard-cut */
    }
    start[n] = pos;
    len[n] = line_end - pos;
    ++n;
    pos = line_end;
  }
  while (pos < total && text[pos] == ' ') ++pos;
  if (pos < total && truncated != NULL) *truncated = 1;
  return n;
}
