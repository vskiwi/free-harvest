/*
 * esp_lcd panel driver for the Sitronix ST7735 / ST7735S.
 *
 * Adapted from LilyGo's T-Dongle-S3 example
 * (https://github.com/Xinyuan-LilyGO/T-Dongle-S3,
 * examples/lcd/bsp_lcd/esp_lcd_st7735.{c,h}). That repository is MIT
 * licensed; the file itself carries no header, so the attribution lives
 * here. Changes for this project: built against the ESP-IDF v6 esp_lcd API
 * (rgb_ele_order instead of color_space, disp_on_off only), the init
 * sequence is a table of commands the same way esp_lcd's own ST7789 driver
 * does it, and the chatter has been cut to debug level.
 *
 * The default init sequence is the "green tab" 80x160 set that this board's
 * glass needs, INVON included. Gap, mirror and axis swap are the caller's
 * business (see main/hr_display_hw.c for the values that fit the dongle).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ST7735 command set (system + panel function lists). */
#define ST7735_SWRESET 0x01
#define ST7735_SLPIN   0x10
#define ST7735_SLPOUT  0x11
#define ST7735_PTLON   0x12
#define ST7735_NORON   0x13
#define ST7735_INVOFF  0x20
#define ST7735_INVON   0x21
#define ST7735_GAMSET  0x26
#define ST7735_DISPOFF 0x28
#define ST7735_DISPON  0x29
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_MADCTL  0x36
#define ST7735_COLMOD  0x3A
#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR  0xB4
#define ST7735_PWCTR1  0xC0
#define ST7735_PWCTR2  0xC1
#define ST7735_PWCTR3  0xC2
#define ST7735_PWCTR4  0xC3
#define ST7735_PWCTR5  0xC4
#define ST7735_VMCTR1  0xC5
#define ST7735_VMOFCTR 0xC7
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

/* One entry of an initialisation sequence. */
typedef struct {
    int cmd;               /* command byte */
    const void *data;      /* parameters, may be NULL */
    size_t data_bytes;     /* number of parameter bytes */
    unsigned int delay_ms; /* pause after the command */
} st7735_lcd_init_cmd_t;

/*
 * Optional vendor configuration, passed through
 * esp_lcd_panel_dev_config_t.vendor_config. NULL selects the built-in
 * 80x160 sequence.
 */
typedef struct {
    const st7735_lcd_init_cmd_t *init_cmds; /* static const array */
    uint16_t init_cmds_size;
} st7735_vendor_config_t;

/*
 * Create an ST7735 panel on an existing panel IO. Honours reset_gpio_num,
 * rgb_ele_order, bits_per_pixel (16 or 18) and flags.reset_active_high.
 */
esp_err_t esp_lcd_new_panel_st7735(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
