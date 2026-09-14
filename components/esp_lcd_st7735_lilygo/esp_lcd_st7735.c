/*
 * esp_lcd panel driver for the ST7735 / ST7735S - see esp_lcd_st7735.h for
 * provenance (LilyGo T-Dongle-S3 examples, MIT) and the list of changes.
 *
 * SPDX-License-Identifier: MIT
 */
#include "esp_lcd_st7735.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <sys/cdefs.h>

static const char *TAG = "st7735";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; /* current MADCTL, so mirror/swap compose */
    uint8_t colmod_val; /* current COLMOD */
    const st7735_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} st7735_panel_t;

static esp_err_t panel_st7735_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735_draw_bitmap(esp_lcd_panel_t *panel, int x_start,
                                          int y_start, int x_end, int y_end,
                                          const void *color_data);
static esp_err_t panel_st7735_invert_color(esp_lcd_panel_t *panel, bool invert);
static esp_err_t panel_st7735_mirror(esp_lcd_panel_t *panel, bool mirror_x,
                                     bool mirror_y);
static esp_err_t panel_st7735_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7735_set_gap(esp_lcd_panel_t *panel, int x_gap,
                                      int y_gap);
static esp_err_t panel_st7735_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_st7735(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *cfg,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st7735_panel_t *p = NULL;

    ESP_GOTO_ON_FALSE(io && cfg && ret_panel, ESP_ERR_INVALID_ARG, err, TAG,
                      "invalid argument");
    p = calloc(1, sizeof(st7735_panel_t));
    ESP_GOTO_ON_FALSE(p, ESP_ERR_NO_MEM, err, TAG, "no mem for st7735 panel");

    if (cfg->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << cfg->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG,
                          "configure GPIO for RST line failed");
    }

    switch (cfg->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        p->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        p->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported RGB element order");
        break;
    }

    switch (cfg->bits_per_pixel) {
    case 16: /* RGB565 */
        p->colmod_val = 0x55;
        p->fb_bits_per_pixel = 16;
        break;
    case 18: /* RGB666, three bytes per pixel on the wire */
        p->colmod_val = 0x66;
        p->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG,
                          "unsupported pixel width");
        break;
    }

    p->io = io;
    p->reset_gpio_num = cfg->reset_gpio_num;
    p->reset_level = cfg->flags.reset_active_high;
    if (cfg->vendor_config) {
        const st7735_vendor_config_t *v = cfg->vendor_config;
        p->init_cmds = v->init_cmds;
        p->init_cmds_size = v->init_cmds_size;
    }
    p->base.del = panel_st7735_del;
    p->base.reset = panel_st7735_reset;
    p->base.init = panel_st7735_init;
    p->base.draw_bitmap = panel_st7735_draw_bitmap;
    p->base.invert_color = panel_st7735_invert_color;
    p->base.set_gap = panel_st7735_set_gap;
    p->base.mirror = panel_st7735_mirror;
    p->base.swap_xy = panel_st7735_swap_xy;
    p->base.disp_on_off = panel_st7735_disp_on_off;
    *ret_panel = &p->base;
    ESP_LOGD(TAG, "new st7735 panel @%p", p);
    return ESP_OK;

err:
    if (p) {
        if (cfg->reset_gpio_num >= 0) {
            gpio_reset_pin(cfg->reset_gpio_num);
        }
        free(p);
    }
    return ret;
}

static esp_err_t panel_st7735_del(esp_lcd_panel_t *panel)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7735 panel @%p", p);
    free(p);
    return ESP_OK;
}

static esp_err_t panel_st7735_reset(esp_lcd_panel_t *panel)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, LCD_CMD_SWRESET,
                                                      NULL, 0),
                            TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20)); /* spec: >= 5 ms before the next */
    }
    return ESP_OK;
}

/*
 * "Green tab" 80x160 sequence, as shipped by LilyGo for this glass. MADCTL
 * and COLMOD are sent separately from the values chosen at creation, so
 * they are not in this table. INVON is: this IPS panel shows inverted
 * colours without it.
 */
static const st7735_lcd_init_cmd_t init_default[] = {
    {ST7735_FRMCTR1, (const uint8_t[]){0x05, 0x3A, 0x3A}, 3, 0},
    {ST7735_FRMCTR2, (const uint8_t[]){0x05, 0x3A, 0x3A}, 3, 0},
    {ST7735_FRMCTR3, (const uint8_t[]){0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A}, 6, 0},
    {ST7735_INVCTR,  (const uint8_t[]){0x03}, 1, 0},
    {ST7735_PWCTR1,  (const uint8_t[]){0x62, 0x02, 0x04}, 3, 0},
    {ST7735_PWCTR2,  (const uint8_t[]){0xC0}, 1, 0},
    {ST7735_PWCTR3,  (const uint8_t[]){0x0D, 0x00}, 2, 0},
    {ST7735_PWCTR4,  (const uint8_t[]){0x8D, 0x6A}, 2, 0},
    {ST7735_PWCTR5,  (const uint8_t[]){0x8D, 0xEE}, 2, 0},
    {ST7735_VMCTR1,  (const uint8_t[]){0x0E}, 1, 0},
    {ST7735_INVON,   NULL, 0, 0},
    {ST7735_GMCTRP1, (const uint8_t[]){0x10, 0x0E, 0x02, 0x03, 0x0E, 0x07, 0x02, 0x07,
                                       0x0A, 0x12, 0x27, 0x37, 0x00, 0x0D, 0x0E, 0x10}, 16, 0},
    {ST7735_GMCTRN1, (const uint8_t[]){0x10, 0x0E, 0x03, 0x03, 0x0F, 0x06, 0x02, 0x08,
                                       0x0A, 0x13, 0x26, 0x36, 0x00, 0x0D, 0x0E, 0x10}, 16, 0},
    {ST7735_NORON,   NULL, 0, 10},
    {ST7735_DISPON,  NULL, 0, 100},
};

static esp_err_t panel_st7735_init(esp_lcd_panel_t *panel)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    esp_lcd_panel_io_handle_t io = p->io;

    /* The panel wakes up asleep; leave sleep before anything else. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0),
                        TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL,
                                                  (uint8_t[]){p->madctl_val}, 1),
                        TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD,
                                                  (uint8_t[]){p->colmod_val}, 1),
                        TAG, "send command failed");

    const st7735_lcd_init_cmd_t *cmds = init_default;
    uint16_t n = sizeof(init_default) / sizeof(init_default[0]);
    if (p->init_cmds) {
        cmds = p->init_cmds;
        n = p->init_cmds_size;
    }

    for (uint16_t i = 0; i < n; i++) {
        /* Keep our shadow copies in step with a vendor table that sets
         * these itself, so later mirror/swap calls compose correctly. */
        if (cmds[i].cmd == LCD_CMD_MADCTL && cmds[i].data_bytes >= 1) {
            p->madctl_val = ((const uint8_t *)cmds[i].data)[0];
        } else if (cmds[i].cmd == LCD_CMD_COLMOD && cmds[i].data_bytes >= 1) {
            p->colmod_val = ((const uint8_t *)cmds[i].data)[0];
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, cmds[i].cmd,
                                                      cmds[i].data,
                                                      cmds[i].data_bytes),
                            TAG, "send command failed");
        if (cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(cmds[i].delay_ms));
        }
    }
    ESP_LOGD(TAG, "init sequence sent (%u commands)", (unsigned)n);
    return ESP_OK;
}

static esp_err_t panel_st7735_draw_bitmap(esp_lcd_panel_t *panel, int x_start,
                                          int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) &&
           "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = p->io;

    x_start += p->x_gap;
    x_end += p->x_gap;
    y_start += p->y_gap;
    y_end += p->y_gap;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET,
                                                  (uint8_t[]){
                                                      (x_start >> 8) & 0xFF,
                                                      x_start & 0xFF,
                                                      ((x_end - 1) >> 8) & 0xFF,
                                                      (x_end - 1) & 0xFF,
                                                  },
                                                  4),
                        TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET,
                                                  (uint8_t[]){
                                                      (y_start >> 8) & 0xFF,
                                                      y_start & 0xFF,
                                                      ((y_end - 1) >> 8) & 0xFF,
                                                      (y_end - 1) & 0xFF,
                                                  },
                                                  4),
                        TAG, "send command failed");
    size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) *
                 p->fb_bits_per_pixel / 8;
    return esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);
}

static esp_err_t panel_st7735_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    return esp_lcd_panel_io_tx_param(p->io,
                                     invert ? LCD_CMD_INVON : LCD_CMD_INVOFF,
                                     NULL, 0);
}

static esp_err_t panel_st7735_mirror(esp_lcd_panel_t *panel, bool mirror_x,
                                     bool mirror_y)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    if (mirror_x) {
        p->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        p->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    return esp_lcd_panel_io_tx_param(p->io, LCD_CMD_MADCTL,
                                     (uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_st7735_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    if (swap_axes) {
        p->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    return esp_lcd_panel_io_tx_param(p->io, LCD_CMD_MADCTL,
                                     (uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_st7735_set_gap(esp_lcd_panel_t *panel, int x_gap,
                                      int y_gap)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7735_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7735_panel_t *p = __containerof(panel, st7735_panel_t, base);
    return esp_lcd_panel_io_tx_param(p->io,
                                     on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF,
                                     NULL, 0);
}
