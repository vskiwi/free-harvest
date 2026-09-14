/*
 * T-Dongle-S3 LCD: SPI bus, esp_lcd panel, backlight. The only file that
 * knows which GPIO the glass hangs off.
 *
 * Pixels handed to hr_display_hw_flush() are RGB565 in the byte order the
 * panel reads (high byte first); hr_gfx keeps its frame buffer that way so
 * nothing is converted at flush time.
 */
#ifndef HR_DISPLAY_HW_H
#define HR_DISPLAY_HW_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Bring the panel up: backlight pin driven OFF first (it floats on at
 * power-up), SPI2, ST7735 reset + init, orientation, display on. Returns
 * false if the panel could not be initialised; the caller then runs without
 * a display rather than aborting.
 */
bool hr_display_hw_init(void);

/* Backlight level, 0 = off, 100 = full. Handles the active-low MOSFET. */
void hr_display_hw_backlight(uint8_t pct);

/*
 * Panel output on/off (ST7735 DISPON/DISPOFF). GRAM is kept, and
 * hr_display_hw_flush() still writes to it while off, so switching back on
 * shows the current frame at once. Call with the backlight at 0 to avoid a
 * flash of whatever the panel latches while it wakes.
 */
void hr_display_hw_panel_on(bool on);

/*
 * Push a rectangle of pixels (row-major, w*h entries, wire byte order).
 * Blocks until the DMA transfer has completed, so the buffer may be reused
 * on return. Returns false on a transfer error.
 */
bool hr_display_hw_flush(int x, int y, int w, int h, const uint16_t *pixels);

#endif /* HR_DISPLAY_HW_H */
