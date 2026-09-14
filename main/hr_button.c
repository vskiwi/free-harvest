/*
 * GPIO0 button, debounced by polling. See hr_button.h.
 */
#include "hr_button.h"

#include "board_t_dongle_s3.h"

#include "driver/gpio.h"

#define BUTTON_IGNORE_AFTER_BOOT_MS 100UL
#define BUTTON_DEBOUNCE_MS          30UL
#define BUTTON_LONG_MS              1500UL

static bool s_ready;
static bool s_stable_down;      /* debounced state */
static bool s_raw_down;         /* last raw sample */
static unsigned long s_raw_since_ms;
static unsigned long s_down_since_ms;
static bool s_long_sent;        /* LONG fired for this press */

void hr_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << HR_PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        /* The board has a 10 k pull-up; the internal one is belt and
         * braces for a pin that also strapped the boot mode. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    s_ready = gpio_config(&io) == ESP_OK;
}

bool hr_button_poll(unsigned long now_ms, hr_ui_button_event_t *ev)
{
    if (!s_ready || now_ms < BUTTON_IGNORE_AFTER_BOOT_MS) {
        return false;
    }
    const bool raw_down = gpio_get_level(HR_PIN_BUTTON) == 0; /* active low */

    if (raw_down != s_raw_down) {
        s_raw_down = raw_down;
        s_raw_since_ms = now_ms;
    }
    if (raw_down != s_stable_down &&
        now_ms - s_raw_since_ms >= BUTTON_DEBOUNCE_MS) {
        s_stable_down = raw_down;
        if (s_stable_down) {
            s_down_since_ms = now_ms;
            s_long_sent = false;
        } else if (!s_long_sent) {
            /* Released before the long threshold. */
            *ev = HR_UI_BUTTON_SHORT;
            return true;
        }
    }
    if (s_stable_down && !s_long_sent &&
        now_ms - s_down_since_ms >= BUTTON_LONG_MS) {
        /* Fire while still held, so the user sees the effect and lets go. */
        s_long_sent = true;
        *ev = HR_UI_BUTTON_LONG;
        return true;
    }
    return false;
}
