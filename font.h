#ifndef GLEEP_FONT_H
#define GLEEP_FONT_H

void draw_text(int x, int y, int scale, unsigned int color, const char *text);
int  text_width(int scale, const char *text);

#endif