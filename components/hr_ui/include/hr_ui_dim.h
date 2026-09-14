/*
 * hr_ui_dim - backlight inactivity state machine for the dongle display.
 *
 * The stick lives in the dryer's USB port around the clock, so the panel
 * must not sit at full brightness forever: ACTIVE -> DIM after a while
 * without events -> OFF later still. The hardware side (LEDC ramp, panel
 * DISPOFF, LED) is in main/hr_display.c; this module only decides the
 * level, from a snapshot of the model and the UI state, at a time the
 * caller passes in - so the host tests never sleep.
 *
 * What counts as an EVENT (wakes to ACTIVE, restarts the timers):
 *   - a button press (hr_ui_dim_button)
 *   - the dryer phase changing (start, freezing -> drying, complete, ...)
 *   - an alert appearing, changing or clearing
 *   - the USB link going down or coming back
 *   - Wi-Fi state changing (setup AP, connecting, connected)
 *   - MQTT connecting or dropping
 *   - the screen changing for any other reason (boot splash over, IP banner)
 * What does NOT: ordinary telemetry (temperature, vacuum, elapsed time)
 * and the INFO/RAW override timing out. The frame buffer keeps updating
 * while DIM, so the dim screen still shows live numbers.
 *
 * Floors (the level never drops below them):
 *   - alert showing:    ACTIVE if cfg.alert_keep_on, else DIM
 *   - alert dismissed but still active (status-bar "!"): DIM
 *   - setup AP open (provisioning):                      DIM
 *
 * Like the rest of hr_ui this is output-only: no session, no transmit
 * callback, nothing that can reach the dryer.
 */
#ifndef HR_UI_DIM_H
#define HR_UI_DIM_H

#include "hr_ui_model.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HR_UI_DIM_ACTIVE = 0, /* full brightness */
    HR_UI_DIM_DIM,        /* cfg.dim_pct, still readable up close */
    HR_UI_DIM_OFF,        /* backlight off, panel may be switched off */
} hr_ui_dim_level_t;

typedef struct {
    bool enabled;               /* master switch; false = always ACTIVE */
    unsigned long dim_after_ms; /* idle time before DIM; 0 = never dim */
    unsigned long off_after_ms; /* idle time before OFF; 0 = never off.
                                 * Counted from the last event, like
                                 * dim_after_ms. A value <= dim_after_ms
                                 * goes straight from ACTIVE to OFF. */
    uint8_t dim_pct;            /* backlight while DIM, 1..99 */
    bool alert_keep_on;         /* true: a showing alert holds ACTIVE */
} hr_ui_dim_cfg_t;

/* Fill a configuration with the documented defaults (30 s / 300 s / 10 %). */
void hr_ui_dim_cfg_default(hr_ui_dim_cfg_t *cfg);

#define HR_UI_DIM_DEFAULT_DIM_MS  30000UL
#define HR_UI_DIM_DEFAULT_OFF_MS  300000UL
#define HR_UI_DIM_DEFAULT_DIM_PCT 10

/* LED ceiling while the screen is OFF and the LED follows the display. */
#define HR_UI_LED_DIM_PCT 5

typedef struct {
    hr_ui_dim_level_t level;
    unsigned long last_event_ms;

    /* What the previous update saw, to detect the events listed above. */
    bool seeded;
    hr_ui_screen_t last_screen;
    hr_phase_t last_phase;
    bool last_tel_valid;
    bool last_link_up;
    hr_ui_wifi_t last_wifi;
    bool last_mqtt_connected;
    hr_ui_alert_t last_alert;
} hr_ui_dim_state_t;

/* ACTIVE, with the idle timer started at now_ms. */
void hr_ui_dim_init(hr_ui_dim_state_t *d, unsigned long now_ms);

/*
 * Decide the level for this tick. Call after hr_ui_select() so that
 * st->screen and st->alert are current. Idempotent for the same inputs.
 */
hr_ui_dim_level_t hr_ui_dim_update(hr_ui_dim_state_t *d,
                                   const hr_ui_dim_cfg_t *cfg,
                                   const hr_ui_state_t *st,
                                   const hr_ui_model_t *m,
                                   unsigned long now_ms);

/* Restart the idle timer and go ACTIVE (any user activity). */
void hr_ui_dim_wake(hr_ui_dim_state_t *d, unsigned long now_ms);

/*
 * A button press arrived. Wakes the display and returns true if the press
 * should ALSO be handed to hr_ui_button(). When the screen is OFF the first
 * press only wakes it - the user cannot see what a short press would
 * dismiss or a long press would toggle - so this returns false.
 */
bool hr_ui_dim_button(hr_ui_dim_state_t *d, unsigned long now_ms);

/* Backlight percentage for a level: 100, cfg->dim_pct or 0. */
uint8_t hr_ui_dim_backlight_pct(const hr_ui_dim_cfg_t *cfg,
                                hr_ui_dim_level_t level);

/* "ACTIVE" / "DIM" / "OFF", for logs. */
const char *hr_ui_dim_level_name(hr_ui_dim_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* HR_UI_DIM_H */
