#ifndef SEGARECOMP_VIEWER_SV_FONT_H
#define SEGARECOMP_VIEWER_SV_FONT_H

/*
 * The viewer's bitmap font (SDL free): glyph metrics and a 1-bit atlas generated offline by tools/gen_viewer_font.py from the project's licensed fonts
 * (ProggyClean for body text, Silkscreen Bold for headings). Text width and wrapping are computed from the real glyph advances (never from a fixed
 * character cell), at any whole-number scale. sv_host.c turns the atlas into one texture; nothing is loaded from disk.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum SvFontId { SV_FONT_BODY = 0, SV_FONT_HEAD = 1, SV_FONT_COUNT } SvFontId;

#define SV_FONT_FIRST 32
#define SV_FONT_LAST 126
#define SV_FONT_GLYPHS (SV_FONT_LAST - SV_FONT_FIRST + 1)

typedef struct SvGlyph {
  short x, y, w, h;   /* rectangle in the atlas (pixels) */
  short xoff, yoff;   /* offset of the rectangle from the pen position / baseline */
  short advance;
} SvGlyph;

typedef struct SvFontFace {
  short size;         /* native pixel size */
  short ascent, descent;
  SvGlyph glyphs[SV_FONT_GLYPHS];
} SvFontFace;

extern const int sv_font_atlas_w, sv_font_atlas_h;
extern const unsigned char sv_font_atlas_bits[]; /* 1 bit per pixel, row-major, MSB first, rows padded to whole bytes */
extern const SvFontFace sv_font_faces[SV_FONT_COUNT];

/* The glyph of an ASCII byte (anything else is '?'). */
const SvGlyph *sv_font_glyph(SvFontId face, unsigned char ch);
/* Width in atlas pixels (multiply by the draw scale). Handles UTF-8 by drawing each non-ASCII byte as '?': names stay bounded and printable. */
int sv_font_text_width(SvFontId face, const char *text);
int sv_font_text_width_n(SvFontId face, const char *text, int bytes);
/* Native line height (ascent + descent + 1 of leading). */
int sv_font_line_height(SvFontId face);
/* Number of leading bytes of `text` that fit into `max_px` (scaled by `scale`); never splits inside nothing: bytes are glyphs. */
int sv_font_fit(SvFontId face, const char *text, int scale, int max_px);
/* Copies `text` into `out`, shortened with "..." so that it fits `max_px` at `scale`. Returns the resulting width. */
int sv_font_ellipsize(SvFontId face, const char *text, int scale, int max_px, char *out, int cap);
/* Word-wraps `text` into at most `max_lines` lines of at most `max_px` pixels (scaled). Line i is text[start[i] .. start[i]+len[i]). Returns the line count; a
 * final line that did not fit is cut and marked by the caller (see `truncated`, may be NULL). */
int sv_font_wrap(SvFontId face, const char *text, int scale, int max_px, int max_lines, int *start, int *len, int *truncated);

#ifdef __cplusplus
}
#endif
#endif
