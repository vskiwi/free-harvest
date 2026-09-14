/*
 * hr_ui task: model copy, screen selection, dirty-zone flushing, LED,
 * button, backlight. See hr_display.h.
 */
#include "hr_display.h"

#include "hr_http.h" /* FREEHARVEST_VERSION */

#if CONFIG_HR_DISPLAY
#include "hr_display_hw.h"
#include "hr_display_screens.h"
#include "hr_gfx.h"
#endif

#if CONFIG_HR_LED
#include "hr_led.h"
#else
static inline void hr_led_init(void) {}
static inline void hr_led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t pct)
{
    (void)r; (void)g; (void)b; (void)pct;
}
#endif
#if CONFIG_HR_BUTTON
#include "hr_button.h"
#else
static inline void hr_button_init(void) {}
static inline bool hr_button_poll(unsigned long now_ms, hr_ui_button_event_t *ev)
{
    (void)now_ms; (void)ev;
    return false;
}
#endif

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "hr_display";

/* Below TinyUSB (5) and httpd (5); above the main loop (1). */
#define UI_TASK_PRIO   3
#define UI_TASK_STACK  6144
#define UI_POLL_MS     20     /* button sampling / LED animation */
#define UI_TICK_MS     1000   /* redraw for clocks and blinking */
#define UI_PULSE_MS    120    /* LED flash on a new STAT */
#define UI_BLINK_REDRAW_MS 500 /* screens with a 2 Hz element */

static SemaphoreHandle_t s_lock;
static hr_ui_model_t s_model;        /* shared: written by posts */
static volatile bool s_dirty;        /* something changed since last draw */
static volatile unsigned long s_pulse_ms;
static hr_ui_state_t s_state;
static int s_backlight_pct = -1;
#if CONFIG_HR_DISPLAY
static bool s_have_panel;
static uint32_t s_zone_hash[HR_ZONE_COUNT];
static hr_ui_screen_t s_last_screen = HR_UI_SCREEN_BOOT;
static bool s_first_draw = true;
#endif

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ */
/* Posting into the model                                              */
/* ------------------------------------------------------------------ */

void hr_display_post_telemetry(const hr_telemetry_t *t, hr_phase_t phase,
                               long freeze_eta_s, const char *last_stat)
{
    if (s_lock == NULL || t == NULL || !t->valid) {
        return;
    }
    const unsigned long now = now_ms();
    /* Called on the TinyUSB task: the ui task only ever holds the lock for a
     * struct copy, but never let USB servicing wait on the display. A sample
     * dropped here is redrawn from the next STAT seconds later. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    hr_ui_telemetry_t *tel = &s_model.tel;
    tel->valid = true;
    tel->type = t->type;
    tel->phase = phase;
    tel->temp_f = t->temperature_f;
    tel->pressure_valid = t->pressure_valid;
    tel->vacuum_um = t->pressure_microns;
    tel->batch_elapsed_s = t->batch_elapsed_s;
    tel->phase_elapsed_s = t->phase_elapsed_s;
    /* Only the phases that report a percentage get a bar. */
    tel->phase_pct = (t->type == 4 || t->type == 5 || t->type == 6)
                         ? t->phase_pct : -1;
    tel->prep_remaining_s = t->prep_active ? t->prep_remaining_s : 0;
    tel->freeze_eta_s = freeze_eta_s;
    snprintf(tel->mode, sizeof(tel->mode), "%s", t->mode);
    if (last_stat) {
        snprintf(tel->last_stat, sizeof(tel->last_stat), "%s", last_stat);
    }
    tel->rx_ms = now;
    s_dirty = true;
    s_pulse_ms = now;
    xSemaphoreGive(s_lock);
}

void hr_display_post_status(const hr_display_status_t *s)
{
    if (s_lock == NULL || s == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hr_ui_model_t *m = &s_model;
    /* Redraw only when something visible moved; the caller posts 4x/s. */
    bool changed =
        m->wifi != s->wifi || m->link_up != s->link_up ||
        m->usb_mounted != s->usb_mounted || m->usb_mounts != s->usb_mounts ||
        m->mqtt_configured != s->mqtt_configured ||
        m->mqtt_connected != s->mqtt_connected ||
        m->frames_bad != s->frames_bad ||
        m->capture_dropped != s->capture_dropped ||
        strcmp(m->ip, s->ip) != 0 || strcmp(m->ssid, s->ssid) != 0 ||
        strcmp(m->machine_name, s->machine_name) != 0;
    m->wifi = s->wifi;
    snprintf(m->ssid, sizeof(m->ssid), "%s", s->ssid);
    snprintf(m->ip, sizeof(m->ip), "%s", s->ip);
    snprintf(m->ap_ssid, sizeof(m->ap_ssid), "%s", s->ap_ssid);
    m->rssi_dbm = s->rssi_dbm;
    m->ap_remaining_s = s->ap_remaining_s;
    m->mqtt_configured = s->mqtt_configured;
    m->mqtt_connected = s->mqtt_connected;
    m->usb_mounted = s->usb_mounted;
    m->usb_mounts = s->usb_mounts;
    m->usb_rx_bytes = s->usb_rx_bytes;
    m->link_up = s->link_up;
    m->frames_bad = s->frames_bad;
    m->capture_dropped = s->capture_dropped;
    m->capture_used = s->capture_used;
    m->capture_cap = s->capture_cap;
    snprintf(m->machine_name, sizeof(m->machine_name), "%s", s->machine_name);
    snprintf(m->fw_version, sizeof(m->fw_version), "%s", s->fw_version);
    snprintf(m->reset_reason, sizeof(m->reset_reason), "%s", s->reset_reason);
    m->heap_free = s->heap_free;
    if (changed) {
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw(hr_ui_screen_t screen, const hr_ui_model_t *m,
                 unsigned long now)
{
#if !CONFIG_HR_DISPLAY
    (void)screen;
    (void)m;
    (void)now;
#else
    if (!s_have_panel) {
        return;
    }
    hr_screens_render(screen, &s_state, m, now, FREEHARVEST_VERSION);

    /* Full push on a screen change or the first frame; otherwise only the
     * bands whose content hash moved, so nothing visibly flickers. */
    if (s_first_draw || screen != s_last_screen) {
        for (int i = 0; i < HR_ZONE_COUNT; i++) {
            int y, h;
            hr_screens_zone(i, &y, &h);
            s_zone_hash[i] = hr_gfx_hash_rows(y, h);
        }
        hr_gfx_flush_all();
        s_first_draw = false;
        s_last_screen = screen;
        return;
    }
    for (int i = 0; i < HR_ZONE_COUNT; i++) {
        int y, h;
        hr_screens_zone(i, &y, &h);
        uint32_t hsh = hr_gfx_hash_rows(y, h);
        if (hsh != s_zone_hash[i]) {
            s_zone_hash[i] = hsh;
            hr_gfx_flush_rect(0, y, HR_GFX_W, h);
        }
    }
#endif
}

static void set_backlight(int pct)
{
    if (pct != s_backlight_pct) {
        s_backlight_pct = pct;
#if CONFIG_HR_DISPLAY
        if (s_have_panel) {
            hr_display_hw_backlight((uint8_t)pct);
        }
#endif
    }
}

/* Screens with a sub-second element need redrawing faster than 1 Hz. */
static bool screen_blinks(hr_ui_screen_t screen, const hr_ui_model_t *m)
{
    return screen == HR_UI_SCREEN_ALERT || screen == HR_UI_SCREEN_CONNECTING ||
           (screen == HR_UI_SCREEN_RUN && m->tel.phase == HR_PHASE_TRANSITION);
}

static void ui_task(void *arg)
{
    (void)arg;
    hr_ui_model_t m;
    unsigned long last_draw = 0;
    hr_ui_screen_t screen = HR_UI_SCREEN_BOOT;

    for (;;) {
        const unsigned long now = now_ms();

        hr_ui_button_event_t ev;
        if (hr_button_poll(now, &ev)) {
            hr_ui_button(&s_state, ev, now);
            s_dirty = true;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_model.uptime_ms = now;
        m = s_model;
        const bool dirty = s_dirty;
        s_dirty = false;
        xSemaphoreGive(s_lock);

        const unsigned interval = screen_blinks(screen, &m) ? UI_BLINK_REDRAW_MS
                                                            : UI_TICK_MS;
        if (dirty || now - last_draw >= interval) {
            screen = hr_ui_select(&s_state, &m, now);
            draw(screen, &m, now);
            last_draw = now;
        }

        /* Night mode: screen and LED dark. Otherwise full backlight. */
        set_backlight(s_state.backlight_off ? 0 : 100);

        hr_ui_led_t led = hr_ui_led_for(screen, &m, now);
        uint8_t level = hr_ui_led_level(&led, now);
        if (screen == HR_UI_SCREEN_RUN && now - s_pulse_ms < UI_PULSE_MS) {
            level = HR_UI_LED_MAX_PCT; /* one frame arrived: blink brighter */
        }
        if (s_state.backlight_off) {
            level = 0;
        }
        hr_led_set(led.r, led.g, led.b, level);

        vTaskDelay(pdMS_TO_TICKS(UI_POLL_MS));
    }
}

void hr_display_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "no mutex; display disabled");
        return;
    }
    hr_ui_model_init(&s_model);
    s_model.boot_ms = now_ms();
    snprintf(s_model.ap_ssid, sizeof(s_model.ap_ssid), "%s", CONFIG_HR_AP_SSID);
    hr_ui_state_init(&s_state);

    hr_led_init();
    hr_button_init();
#if CONFIG_HR_DISPLAY
    hr_gfx_init();
    s_have_panel = hr_display_hw_init();
    if (!s_have_panel) {
        ESP_LOGW(TAG, "LCD not initialised; running LED/button only");
    }
#endif

    if (xTaskCreate(ui_task, "hr_ui", UI_TASK_STACK, NULL, UI_TASK_PRIO,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the UI task");
        return;
    }
#if CONFIG_HR_DISPLAY
    /* Panel is initialised and the first frame is about to go out. */
    if (s_have_panel) {
        hr_display_hw_backlight(100);
        s_backlight_pct = 100;
    }
#endif
    ESP_LOGI(TAG, "display task running");
}
