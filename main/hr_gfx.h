/*
 * Minimal software renderer for the 160x80 dongle display: one RGB565 frame
 * buffer in internal RAM, rectangles, a 6x8 ASCII font (scaled x2 and x3
 * for the title and the big numbers), a progress bar, and zone flushes.
 *
 * Colours are plain RGB565 (HR_RGB565 in hr_ui_model.h). The buffer stores
 * them byte-swapped, which is the order the panel wants, so a flush is a
 * straight DMA of the rows.
 */
#ifndef HR_GFX_INCLUDED_H
#define HR_GFX_INCLUDED_H

#include <stdbool.h>
#include <stdint.h>

#define HR_GFX_W 160
#define HR_GFX_H 80

/* Glyph cell of the built-in font, before scaling. */
#define HR_GFX_FONT_W 6
#define HR_GFX_FONT_H 8

void hr_gfx_init(void);

void hr_gfx_clear(uint16_t color);
void hr_gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
/* Hollow frame `t` pixels thick, inside the given rectangle. */
void hr_gfx_frame(int x, int y, int w, int h, int t, uint16_t color);

/*
 * Text. Every call paints the background of each glyph cell too, so a zone
 * redraw overwrites what was there without a separate clear. Returns the x
 * position after the last glyph. Glyphs outside the buffer are clipped.
 */
int hr_gfx_text6x8(int x, int y, const char *s, uint16_t fg, uint16_t bg);
/* 12x16 (scale 2) and 18x24 (scale 3). */
int hr_gfx_text_big(int x, int y, const char *s, uint16_t fg, uint16_t bg);
int hr_gfx_text_huge(int x, int y, const char *s, uint16_t fg, uint16_t bg);
int hr_gfx_text_scaled(int x, int y, const char *s, int scale, uint16_t fg,
                       uint16_t bg);
/* Pixel width of `s` at the given scale. */
int hr_gfx_text_width(const char *s, int scale);
/* Right-aligned variant: the text ends at x_right. */
int hr_gfx_text_right(int x_right, int y, const char *s, int scale,
                      uint16_t fg, uint16_t bg);

/* Bar with a 1 px frame; pct 0..100 fills left to right. */
void hr_gfx_progress_bar(int x, int y, int w, int h, int pct, uint16_t color);

/* Push the whole buffer, or a band of full-width rows (x/w are ignored:
 * the panel is fed row by row, and every dirty zone spans the width). */
void hr_gfx_flush_all(void);
void hr_gfx_flush_rect(int x, int y, int w, int h);

/* Cheap content hash of a band of rows, for change detection. */
uint32_t hr_gfx_hash_rows(int y, int h);

#endif /* HR_GFX_INCLUDED_H */
