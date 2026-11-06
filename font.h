#ifndef FONT_H
#define FONT_H

/* Gleep Remote - built-in 5x7 bitmap font.
 * No system fonts, no ScePgf, no external files: glyphs are drawn
 * as tiny filled rectangles via vita2d, so it works anywhere.

 * draw_text(): render a string.
 *   scale = pixel scale (1.0 = 5x7 px glyphs, 2.0 = 10x14, 1.5 = in-between)
 *   x, y  = top-left corner of the first glyph, in screen pixels
 *   color = RGBA8(...) value
 *
 * text_width(): width in pixels the string will occupy at the given
 * scale (5 px per glyph + 1 px spacing). Use it for centering.
 */

void draw_text(float scale, int x, int y, unsigned int color, const char *text);
int  text_width(float scale, const char *text);

#endif /* FONT_H */
