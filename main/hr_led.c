/*
 * APA102 bit-bang. One LED, two wires, a 32-bit frame: there is nothing here
 * worth a SPI peripheral.
 *
 * Wire format: 4 x 0x00 start frame, then per LED 0xE0 | brightness(5 bit),
 * BLUE, GREEN, RED (this part is wired BGR), then an end frame of at least
 * n/2 clock pulses - four 0xFF bytes for one LED is plenty. Data is sampled
 * on the rising edge of CLK, MSB first.
 */
#include "hr_led.h"

#include "board_t_dongle_s3.h"
#include "hr_ui_model.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"

static bool s_ready;
static uint8_t s_last[4];

static inline void tick(void)
{
    /* ~1 MHz is comfortable for the APA102's 30 MHz-rated input. */
    esp_rom_delay_us(1);
}

static void shift_byte(uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(HR_PIN_LED_DATA, (v >> i) & 1);
        tick();
        gpio_set_level(HR_PIN_LED_CLK, 1);
        tick();
        gpio_set_level(HR_PIN_LED_CLK, 0);
    }
}

void hr_led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << HR_PIN_LED_DATA) | (1ULL << HR_PIN_LED_CLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        return;
    }
    gpio_set_level(HR_PIN_LED_DATA, 0);
    gpio_set_level(HR_PIN_LED_CLK, 0);
    s_ready = true;
    hr_led_set(0, 0, 0, 0);
}

void hr_led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness_pct)
{
    if (!s_ready) {
        return;
    }
    if (brightness_pct > HR_UI_LED_MAX_PCT) {
        brightness_pct = HR_UI_LED_MAX_PCT;
    }
    /* 5-bit global current: 30 % -> 9/31. Below 1 % the LED is simply off. */
    uint8_t level = (uint8_t)((brightness_pct * 31u + 50u) / 100u);
    if (brightness_pct == 0) {
        level = 0;
    } else if (level == 0) {
        level = 1;
    }
    uint8_t frame[4] = {(uint8_t)(0xE0 | level), b, g, r};
    if (level == 0) {
        frame[1] = frame[2] = frame[3] = 0;
    }
    /* Skip the wire when nothing changed; the task calls this every 20 ms. */
    if (frame[0] == s_last[0] && frame[1] == s_last[1] &&
        frame[2] == s_last[2] && frame[3] == s_last[3]) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        s_last[i] = frame[i];
    }

    for (int i = 0; i < 4; i++) {
        shift_byte(0x00);
    }
    for (int i = 0; i < 4; i++) {
        shift_byte(frame[i]);
    }
    for (int i = 0; i < 4; i++) {
        shift_byte(0xFF);
    }
}
