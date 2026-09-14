/*
 * T-Dongle-S3 LCD bring-up. See hr_display_hw.h.
 *
 * Orientation, for the "verify on hardware" list: landscape 160x80 with the
 * USB plug on the left is gap (1, 26), swap_xy, mirror(x=false, y=true),
 * exactly what LilyGo's lcd.ino does. CONFIG_HR_UI_ROTATION_180 inverts both
 * mirror flags. Colour inversion is ON and the element order is BGR: the
 * glass is an IPS panel that reads black as white without INVON.
 */
#include "hr_display_hw.h"

#include "board_t_dongle_s3.h"
#include "esp_lcd_st7735.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "hr_lcd";

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES     LEDC_TIMER_8_BIT
#define BL_LEDC_FULL    256u /* duty range is [0, 2^res] */
#define BL_LEDC_HZ      1000

#define FLUSH_TIMEOUT_MS 200

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static bool s_ledc_ready;

/* Runs in ISR context when the colour DMA completes. */
static bool on_color_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

/*
 * Backlight off, as the very first thing the firmware does to the pin.
 * The gate floats at reset and the panel lights at random brightness until
 * something drives it; the vendor's factory sketch does the same.
 */
static void backlight_pin_off(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << HR_PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(HR_PIN_LCD_BL, 1); /* active low: 1 = off */
}

static bool backlight_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        return false;
    }
    ledc_channel_config_t ch = {
        .gpio_num = HR_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = BL_LEDC_FULL, /* pin high the whole period = still off */
        .hpoint = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) {
        return false;
    }
    s_ledc_ready = true;
    return true;
}

void hr_display_hw_backlight(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    if (!s_ledc_ready) {
        gpio_set_level(HR_PIN_LCD_BL, pct == 0 ? 1 : 0);
        return;
    }
    /* The MOSFET conducts while the pin is LOW, so the HIGH time is the
     * complement of the requested brightness. */
    uint32_t duty = BL_LEDC_FULL - (BL_LEDC_FULL * pct) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

bool hr_display_hw_init(void)
{
    backlight_pin_off();

    s_done = xSemaphoreCreateBinary();
    if (s_done == NULL) {
        return false;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = HR_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = HR_PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HR_LCD_WIDTH * HR_LCD_HEIGHT * 2,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = HR_PIN_LCD_CS,
        .dc_gpio_num = HR_PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = HR_LCD_SPI_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io,
                                   &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = HR_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7735(s_io, &dev, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7735: %s", esp_err_to_name(err));
        return false;
    }

    bool mirror_x = false, mirror_y = true;
#if CONFIG_HR_UI_ROTATION_180
    mirror_x = !mirror_x;
    mirror_y = !mirror_y;
#endif
    if (esp_lcd_panel_reset(s_panel) != ESP_OK ||
        esp_lcd_panel_init(s_panel) != ESP_OK ||
        esp_lcd_panel_invert_color(s_panel, true) != ESP_OK ||
        esp_lcd_panel_set_gap(s_panel, 1, 26) != ESP_OK ||
        esp_lcd_panel_swap_xy(s_panel, true) != ESP_OK ||
        esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y) != ESP_OK ||
        esp_lcd_panel_disp_on_off(s_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "panel init sequence failed");
        return false;
    }

    if (!backlight_pwm_init()) {
        ESP_LOGW(TAG, "LEDC unavailable; backlight is on/off only");
    }
    ESP_LOGI(TAG, "ST7735 %dx%d up, SPI %u MHz%s", HR_LCD_WIDTH,
             HR_LCD_HEIGHT, (unsigned)(HR_LCD_SPI_HZ / 1000000),
             mirror_x ? ", rotated 180" : "");
    return true;
}

bool hr_display_hw_flush(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (s_panel == NULL || w <= 0 || h <= 0) {
        return false;
    }
    /* Drain a stale token, then wait for this transfer's own. */
    xSemaphoreTake(s_done, 0);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h,
                                              pixels);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "draw_bitmap: %s", esp_err_to_name(err));
        return false;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(FLUSH_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "flush timed out");
        return false;
    }
    return true;
}
