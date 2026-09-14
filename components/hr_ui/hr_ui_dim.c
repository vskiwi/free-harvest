/*
 * hr_ui_dim - backlight inactivity state machine. See hr_ui_dim.h.
 */
#include "hr_ui_dim.h"

#include <string.h>

void hr_ui_dim_cfg_default(hr_ui_dim_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->dim_after_ms = HR_UI_DIM_DEFAULT_DIM_MS;
    cfg->off_after_ms = HR_UI_DIM_DEFAULT_OFF_MS;
    cfg->dim_pct = HR_UI_DIM_DEFAULT_DIM_PCT;
    cfg->alert_keep_on = true;
}

void hr_ui_dim_init(hr_ui_dim_state_t *d, unsigned long now_ms)
{
    memset(d, 0, sizeof(*d));
    d->level = HR_UI_DIM_ACTIVE;
    d->last_event_ms = now_ms;
    d->last_phase = HR_PHASE_UNKNOWN;
    d->last_alert = HR_UI_ALERT_NONE;
}

void hr_ui_dim_wake(hr_ui_dim_state_t *d, unsigned long now_ms)
{
    d->last_event_ms = now_ms;
    d->level = HR_UI_DIM_ACTIVE;
}

bool hr_ui_dim_button(hr_ui_dim_state_t *d, unsigned long now_ms)
{
    const bool was_off = d->level == HR_UI_DIM_OFF;
    hr_ui_dim_wake(d, now_ms);
    return !was_off;
}

static bool screen_is_override(hr_ui_screen_t s)
{
    return s == HR_UI_SCREEN_INFO || s == HR_UI_SCREEN_RAW;
}

/*
 * Compare this snapshot with the previous one and report whether anything
 * happened that deserves the user's eyes. Updates the bookkeeping.
 */
static bool detect_event(hr_ui_dim_state_t *d, const hr_ui_state_t *st,
                         const hr_ui_model_t *m)
{
    bool event = false;

    if (d->seeded) {
        /* Screen changed - except the INFO/RAW override timing out and
         * returning to the main screen, which is the button press from ten
         * seconds ago ending, not news. */
        if (st->screen != d->last_screen &&
            !(screen_is_override(d->last_screen) &&
              !screen_is_override(st->screen) &&
              st->screen != HR_UI_SCREEN_ALERT)) {
            event = true;
        }
        /* Dryer phase: start, freeze -> dry, complete, back to idle. A
         * STAT that merely updates temperature or vacuum keeps the same
         * phase and is not an event. */
        if (m->tel.valid != d->last_tel_valid ||
            (m->tel.valid && m->tel.phase != d->last_phase)) {
            event = true;
        }
        if (st->alert != d->last_alert) {
            event = true;
        }
        if (m->link_up != d->last_link_up) {
            event = true;
        }
        if (m->wifi != d->last_wifi) {
            event = true;
        }
        if (m->mqtt_connected != d->last_mqtt_connected) {
            event = true;
        }
    }

    d->seeded = true;
    d->last_screen = st->screen;
    d->last_tel_valid = m->tel.valid;
    d->last_phase = m->tel.phase;
    d->last_alert = st->alert;
    d->last_link_up = m->link_up;
    d->last_wifi = m->wifi;
    d->last_mqtt_connected = m->mqtt_connected;
    return event;
}

/* The brightest level the current situation forbids going below. */
static hr_ui_dim_level_t floor_level(const hr_ui_dim_cfg_t *cfg,
                                     const hr_ui_state_t *st,
                                     const hr_ui_model_t *m)
{
    hr_ui_dim_level_t floor = HR_UI_DIM_OFF;

    if (st->alert != HR_UI_ALERT_NONE) {
        if (st->dismissed != st->alert && cfg->alert_keep_on) {
            /* Alert on screen, not yet acknowledged. */
            return HR_UI_DIM_ACTIVE;
        }
        /* Showing but keep-on disabled, or acknowledged: dim, never off. */
        floor = HR_UI_DIM_DIM;
    }
    /* Provisioning: the AP name and address have to stay readable. */
    if (m->wifi == HR_UI_WIFI_AP_SETUP || st->screen == HR_UI_SCREEN_PROVISION) {
        if (floor > HR_UI_DIM_DIM) {
            floor = HR_UI_DIM_DIM;
        }
    }
    return floor;
}

hr_ui_dim_level_t hr_ui_dim_update(hr_ui_dim_state_t *d,
                                   const hr_ui_dim_cfg_t *cfg,
                                   const hr_ui_state_t *st,
                                   const hr_ui_model_t *m,
                                   unsigned long now_ms)
{
    if (detect_event(d, st, m)) {
        d->last_event_ms = now_ms;
    }

    hr_ui_dim_level_t level = HR_UI_DIM_ACTIVE;
    if (cfg->enabled && cfg->dim_after_ms != 0) {
        const unsigned long idle = now_ms - d->last_event_ms;
        if (idle >= cfg->dim_after_ms) {
            level = HR_UI_DIM_DIM;
        }
        if (cfg->off_after_ms != 0 && idle >= cfg->off_after_ms) {
            level = HR_UI_DIM_OFF;
        }
    }

    const hr_ui_dim_level_t floor = floor_level(cfg, st, m);
    if (level > floor) {
        level = floor;
    }
    d->level = level;
    return level;
}

uint8_t hr_ui_dim_backlight_pct(const hr_ui_dim_cfg_t *cfg,
                                hr_ui_dim_level_t level)
{
    switch (level) {
    case HR_UI_DIM_ACTIVE:
        return 100;
    case HR_UI_DIM_DIM:
        return cfg->dim_pct == 0 ? 1 : (cfg->dim_pct > 99 ? 99 : cfg->dim_pct);
    default:
        return 0;
    }
}

const char *hr_ui_dim_level_name(hr_ui_dim_level_t level)
{
    switch (level) {
    case HR_UI_DIM_ACTIVE: return "ACTIVE";
    case HR_UI_DIM_DIM:    return "DIM";
    case HR_UI_DIM_OFF:    return "OFF";
    default:               return "?";
    }
}
