/*
 * hr_ui_model - screen selection, alert detection, LED table and number
 * formatting for the dongle display. See hr_ui_model.h.
 */
#include "hr_ui_model.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void hr_ui_model_init(hr_ui_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->tel.phase = HR_PHASE_UNKNOWN;
    m->tel.phase_pct = -1;
    m->tel.freeze_eta_s = -1;
}

void hr_ui_state_init(hr_ui_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->screen = HR_UI_SCREEN_BOOT;
    st->last_phase = HR_PHASE_UNKNOWN;
    st->last_run_phase = HR_PHASE_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Phases during which the machine is doing something with a batch. */
static bool phase_is_run(hr_phase_t p)
{
    switch (p) {
    case HR_PHASE_PREPARING:
    case HR_PHASE_TRANSITION:
    case HR_PHASE_RUNNING:
    case HR_PHASE_FREEZING:
    case HR_PHASE_DRYING:
    case HR_PHASE_FINAL_DRY:
        return true;
    default:
        return false;
    }
}

/* STAT types the decoder knows about. Anything else is "never seen". */
static bool stat_type_known(int type)
{
    switch (type) {
    case 1: case 2: case 4: case 5: case 6: case 7:
    case 15: case 17: case 31: case 43:
        return true;
    default:
        return false;
    }
}

static bool have_dryer(const hr_ui_model_t *m)
{
    return m->link_up && m->tel.valid;
}

/* IDLE / RUN / COMPLETE / NO_DRYER from telemetry alone. */
static hr_ui_screen_t main_screen(const hr_ui_model_t *m)
{
    if (!have_dryer(m)) {
        return HR_UI_SCREEN_NO_DRYER;
    }
    switch (m->tel.phase) {
    case HR_PHASE_COMPLETE:
        return HR_UI_SCREEN_COMPLETE;
    case HR_PHASE_IDLE:
    case HR_PHASE_RECIPE:
    case HR_PHASE_UNKNOWN:
        return HR_UI_SCREEN_IDLE;
    default:
        return HR_UI_SCREEN_RUN;
    }
}

/* ------------------------------------------------------------------ */
/* Alert conditions                                                    */
/* ------------------------------------------------------------------ */

static hr_ui_alert_t evaluate_alert(hr_ui_state_t *st, const hr_ui_model_t *m,
                                    unsigned long now_ms)
{
    /* Remember the last time the dryer was seen running, and what it did. */
    if (m->link_up && m->tel.valid && phase_is_run(m->tel.phase)) {
        st->last_run_ms = now_ms;
        st->last_run_phase = m->tel.phase;
        st->last_run_elapsed_s = m->tel.phase_elapsed_s;
    }
    if (m->link_up) {
        st->link_lost_ms = 0;
    } else if (st->link_lost_ms == 0) {
        st->link_lost_ms = now_ms;
    }

    /*
     * Counters that only matter when they MOVE. The first observation seeds
     * them, so a counter that was already non-zero when the display came up
     * does not raise an alarm about something that happened long ago.
     */
    if (!st->counters_seeded) {
        st->counters_seeded = true;
        st->last_frames_bad = m->frames_bad;
        st->last_capture_dropped = m->capture_dropped;
    } else {
        if (m->frames_bad != st->last_frames_bad) {
            st->last_frames_bad = m->frames_bad;
            st->frames_bad_grew_ms = now_ms;
        }
        if (m->capture_dropped != st->last_capture_dropped) {
            st->last_capture_dropped = m->capture_dropped;
            st->capture_grew_ms = now_ms;
        }
    }

    /*
     * Link lost mid-batch. Latched: it stays up until the link returns, not
     * merely for five minutes after the last frame - a dryer that stays
     * silent for an hour is more worrying, not less.
     */
    const bool run_recent = st->last_run_ms != 0 &&
                            now_ms - st->last_run_ms < HR_UI_RUN_RECENT_MS;
    if (!m->link_up &&
        (run_recent || st->alert == HR_UI_ALERT_LINK_LOST)) {
        return HR_UI_ALERT_LINK_LOST;
    }
    if (have_dryer(m) && m->tel.type == 15) {
        return HR_UI_ALERT_DIAGNOSTICS;
    }
    if (m->heap_free != 0 && m->heap_free < HR_UI_LOW_HEAP_BYTES) {
        return HR_UI_ALERT_LOW_HEAP;
    }
    if (now_ms < HR_UI_RESET_ALERT_MS &&
        (strcmp(m->reset_reason, "panic") == 0 ||
         strcmp(m->reset_reason, "brownout") == 0)) {
        return HR_UI_ALERT_RESET;
    }
    if (st->frames_bad_grew_ms != 0 &&
        now_ms - st->frames_bad_grew_ms < HR_UI_GROWTH_WINDOW_MS) {
        return HR_UI_ALERT_BAD_FRAMES;
    }
    if (st->capture_grew_ms != 0 &&
        now_ms - st->capture_grew_ms < HR_UI_GROWTH_WINDOW_MS) {
        return HR_UI_ALERT_CAPTURE_DROP;
    }
    if (have_dryer(m) && !stat_type_known(m->tel.type)) {
        return HR_UI_ALERT_UNKNOWN_SCREEN;
    }
    return HR_UI_ALERT_NONE;
}

bool hr_ui_alert_active(const hr_ui_state_t *st)
{
    return st->alert != HR_UI_ALERT_NONE;
}

const char *hr_ui_alert_title(hr_ui_alert_t a)
{
    switch (a) {
    case HR_UI_ALERT_LINK_LOST:      return "LINK LOST";
    case HR_UI_ALERT_DIAGNOSTICS:    return "DIAGNOSTICS";
    case HR_UI_ALERT_LOW_HEAP:       return "LOW MEMORY";
    case HR_UI_ALERT_RESET:          return "UNEXPECTED RESET";
    case HR_UI_ALERT_BAD_FRAMES:     return "BAD FRAMES";
    case HR_UI_ALERT_CAPTURE_DROP:   return "LOG DROPPED";
    case HR_UI_ALERT_UNKNOWN_SCREEN: return "UNKNOWN SCREEN";
    default:                         return "";
    }
}

/* ------------------------------------------------------------------ */
/* Screen selection                                                    */
/* ------------------------------------------------------------------ */

hr_ui_screen_t hr_ui_select(hr_ui_state_t *st, const hr_ui_model_t *m,
                            unsigned long now_ms)
{
    /* Wi-Fi transitions drive the two timed screens. */
    if (m->wifi != st->last_wifi) {
        if (m->wifi == HR_UI_WIFI_CONNECTING) {
            st->connecting_since_ms = now_ms;
        } else if (m->wifi == HR_UI_WIFI_CONNECTED &&
                   (st->last_wifi == HR_UI_WIFI_CONNECTING ||
                    st->last_wifi == HR_UI_WIFI_AP_SETUP)) {
            /* A join we watched happen. Finding the network already up
             * when the display starts is not news worth ten seconds. */
            st->connected_since_ms = now_ms;
        }
        st->last_wifi = m->wifi;
    }
    /* Phase transitions: the COMPLETE banner flash. */
    if (m->tel.valid && m->tel.phase != st->last_phase) {
        if (m->tel.phase == HR_PHASE_COMPLETE) {
            st->complete_since_ms = now_ms;
        }
        st->last_phase = m->tel.phase;
    }

    hr_ui_alert_t alert = evaluate_alert(st, m, now_ms);
    if (alert != st->alert) {
        st->alert = alert;
        st->alert_since_ms = now_ms;
        /* A different (or no) alert: an old acknowledgement no longer
         * applies. */
        if (st->dismissed != alert) {
            st->dismissed = HR_UI_ALERT_NONE;
        }
    }

    hr_ui_screen_t screen;

    if (now_ms - m->boot_ms < HR_UI_BOOT_MS) {
        screen = HR_UI_SCREEN_BOOT;
    } else if (st->alert != HR_UI_ALERT_NONE && st->dismissed != st->alert) {
        screen = HR_UI_SCREEN_ALERT;
    } else if (st->override_until_ms != 0 && now_ms < st->override_until_ms) {
        screen = st->override;
    } else if (m->wifi == HR_UI_WIFI_CONNECTED && st->connected_since_ms != 0 &&
               now_ms - st->connected_since_ms < HR_UI_GOT_IP_SHOW_MS) {
        /* Freshly connected: show the address for a moment, whatever else
         * is going on - it is the one thing the user needs to write down. */
        screen = HR_UI_SCREEN_CONNECTING;
    } else if (!have_dryer(m)) {
        switch (m->wifi) {
        case HR_UI_WIFI_AP_SETUP:
            screen = HR_UI_SCREEN_PROVISION;
            break;
        case HR_UI_WIFI_AP_CLOSED:
            screen = HR_UI_SCREEN_AP_CLOSED;
            break;
        case HR_UI_WIFI_CONNECTING:
            /* Wait a while for the network, then stop pretending the dryer
             * matters less than the router. */
            screen = (now_ms - st->connecting_since_ms < HR_UI_CONNECTING_MAX_MS)
                         ? HR_UI_SCREEN_CONNECTING
                         : HR_UI_SCREEN_NO_DRYER;
            break;
        default:
            screen = HR_UI_SCREEN_NO_DRYER;
            break;
        }
    } else {
        screen = main_screen(m);
    }

    if (st->override_until_ms != 0 && now_ms >= st->override_until_ms) {
        st->override_until_ms = 0;
    }
    st->screen = screen;
    return screen;
}

/* ------------------------------------------------------------------ */
/* Button                                                              */
/* ------------------------------------------------------------------ */

void hr_ui_button(hr_ui_state_t *st, hr_ui_button_event_t ev,
                  unsigned long now_ms)
{
    st->last_activity_ms = now_ms;

    if (ev == HR_UI_BUTTON_LONG) {
        st->backlight_off = !st->backlight_off;
        return;
    }

    /* SHORT */
    if (st->alert != HR_UI_ALERT_NONE && st->dismissed != st->alert) {
        st->dismissed = st->alert;
        return;
    }
    const bool overriding = st->override_until_ms != 0 &&
                            now_ms < st->override_until_ms;
    if (!overriding) {
        st->override = HR_UI_SCREEN_INFO;
    } else if (st->override == HR_UI_SCREEN_INFO) {
        st->override = HR_UI_SCREEN_RAW;
    } else {
        st->override_until_ms = 0; /* back to the main screen */
        return;
    }
    st->override_until_ms = now_ms + HR_UI_OVERRIDE_MS;
    if (st->override_until_ms == 0) {
        st->override_until_ms = 1;
    }
}

/* ------------------------------------------------------------------ */
/* LED                                                                 */
/* ------------------------------------------------------------------ */

static hr_ui_led_t led(uint8_t r, uint8_t g, uint8_t b, uint8_t pct,
                       hr_ui_led_pattern_t pattern, unsigned period_ms)
{
    hr_ui_led_t l;
    l.r = r;
    l.g = g;
    l.b = b;
    l.brightness_pct = pct > HR_UI_LED_MAX_PCT ? HR_UI_LED_MAX_PCT : pct;
    l.pattern = pattern;
    l.period_ms = period_ms;
    return l;
}

hr_ui_led_t hr_ui_led_for(hr_ui_screen_t screen, const hr_ui_model_t *m,
                          unsigned long now_ms)
{
    (void)now_ms;
    if (screen == HR_UI_SCREEN_INFO || screen == HR_UI_SCREEN_RAW) {
        /* The LED keeps mirroring the machine while the user reads. */
        screen = main_screen(m);
    }
    switch (screen) {
    case HR_UI_SCREEN_BOOT:
        return led(255, 255, 255, 20, HR_UI_LED_SOLID, 0);
    case HR_UI_SCREEN_PROVISION:
        return led(0, 60, 255, 25, HR_UI_LED_BREATHE, 2000);
    case HR_UI_SCREEN_AP_CLOSED:
        return led(255, 0, 0, 20, HR_UI_LED_BLINK, 2000);
    case HR_UI_SCREEN_CONNECTING:
        return led(255, 200, 0, 25, HR_UI_LED_BLINK, 500);
    case HR_UI_SCREEN_NO_DRYER:
        return led(160, 0, 255, 20, HR_UI_LED_BREATHE, 3000);
    case HR_UI_SCREEN_IDLE:
        return led(48, 224, 96, 10, HR_UI_LED_SOLID, 0);
    case HR_UI_SCREEN_RUN: {
        uint8_t r, g, b;
        hr_ui_phase_rgb(m->tel.phase, &r, &g, &b);
        return led(r, g, b, 15, HR_UI_LED_SOLID, 0);
    }
    case HR_UI_SCREEN_COMPLETE:
        return led(48, 224, 96, 25, HR_UI_LED_BREATHE, 2000);
    case HR_UI_SCREEN_ALERT:
        return led(255, 0, 0, 30, HR_UI_LED_BLINK, 1000);
    default:
        return led(0, 0, 0, 0, HR_UI_LED_OFF, 0);
    }
}

uint8_t hr_ui_led_level(const hr_ui_led_t *l, unsigned long now_ms)
{
    const unsigned pct = l->brightness_pct;
    switch (l->pattern) {
    case HR_UI_LED_SOLID:
        return (uint8_t)pct;
    case HR_UI_LED_BLINK: {
        if (l->period_ms == 0) {
            return (uint8_t)pct;
        }
        return (now_ms % l->period_ms) < l->period_ms / 2 ? (uint8_t)pct : 0;
    }
    case HR_UI_LED_BREATHE: {
        if (l->period_ms == 0) {
            return (uint8_t)pct;
        }
        const unsigned half = l->period_ms / 2;
        unsigned t = (unsigned)(now_ms % l->period_ms);
        if (t >= half) {
            t = l->period_ms - t;
        }
        /* Triangle between pct/5 and pct: a breath, not a blink. */
        const unsigned floor_pct = pct / 5;
        return (uint8_t)(floor_pct + (pct - floor_pct) * t / half);
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

void hr_ui_fmt_hm(unsigned long secs, char *out, size_t cap)
{
    unsigned long h = secs / 3600;
    unsigned long mi = (secs % 3600) / 60;
    if (h > 0) {
        snprintf(out, cap, "%luh%02lum", h, mi);
    } else {
        snprintf(out, cap, "%lum", mi);
    }
}

void hr_ui_fmt_mmss(unsigned long secs, char *out, size_t cap)
{
    unsigned long mi = secs / 60;
    if (mi > 99) {
        mi = 99;
        secs = 59;
    }
    snprintf(out, cap, "%02lu:%02lu", mi, secs % 60);
}

void hr_ui_fmt_hhmm(unsigned long secs, char *out, size_t cap)
{
    unsigned long h = (secs / 3600) % 100;
    unsigned long mi = (secs % 3600) / 60;
    snprintf(out, cap, "%02lu:%02lu", h, mi);
}

void hr_ui_fmt_uptime(unsigned long secs, char *out, size_t cap)
{
    unsigned long d = secs / 86400;
    unsigned long h = (secs % 86400) / 3600;
    unsigned long mi = (secs % 3600) / 60;
    if (d > 0) {
        snprintf(out, cap, "%lud %02lu:%02lu", d, h, mi);
    } else {
        snprintf(out, cap, "%02lu:%02lu", h, mi);
    }
}

void hr_ui_fmt_temp(long f, bool metric, char *out, size_t cap)
{
    if (metric) {
        long num = (f - 32) * 5;
        /* Round to nearest, away from zero at .5 */
        long c = (num + (num >= 0 ? 4 : -4)) / 9;
        snprintf(out, cap, "%ld" HR_UI_DEG "C", c);
    } else {
        snprintf(out, cap, "%ld" HR_UI_DEG "F", f);
    }
}

void hr_ui_fmt_vac(long um, bool valid, char *out, size_t cap)
{
    if (!valid || um <= 0) {
        snprintf(out, cap, "atm");
    } else {
        snprintf(out, cap, "%ld mT", um);
    }
}

void hr_ui_fmt_bytes_mb(size_t used, size_t cap_bytes, char *out, size_t cap)
{
    /* Tenths of a megabyte, truncated. */
    unsigned long u10 = (unsigned long)(used / (1024UL * 1024UL / 10UL));
    unsigned long c10 = (unsigned long)(cap_bytes / (1024UL * 1024UL / 10UL));
    snprintf(out, cap, "%lu.%lu/%lu.%luMB", u10 / 10, u10 % 10, c10 / 10,
             c10 % 10);
}

const char *hr_ui_phase_label_short(hr_phase_t p)
{
    switch (p) {
    case HR_PHASE_IDLE:        return "READY";
    case HR_PHASE_PREPARING:   return "PREPARING";
    case HR_PHASE_TRANSITION:  return "LOAD TRAYS";
    case HR_PHASE_RUNNING:     return "RUNNING";
    case HR_PHASE_FREEZING:    return "FREEZING";
    case HR_PHASE_DRYING:      return "DRYING";
    case HR_PHASE_FINAL_DRY:   return "FINAL DRY";
    case HR_PHASE_COMPLETE:    return "COMPLETE";
    case HR_PHASE_DIAGNOSTICS: return "DIAGNOSTICS";
    case HR_PHASE_RECIPE:      return "RECIPE";
    default:                   return "---";
    }
}

uint16_t hr_ui_phase_color(hr_phase_t p)
{
    switch (p) {
    case HR_PHASE_PREPARING:  return HR_UI_C_PREP;
    case HR_PHASE_FREEZING:   return HR_UI_C_FREEZE;
    case HR_PHASE_DRYING:     return HR_UI_C_DRY;
    case HR_PHASE_FINAL_DRY:  return HR_UI_C_FINAL;
    case HR_PHASE_COMPLETE:   return HR_UI_C_GREEN;
    case HR_PHASE_IDLE:       return HR_UI_C_IDLE;
    case HR_PHASE_RUNNING:    return HR_UI_C_PREP;
    case HR_PHASE_TRANSITION: return HR_UI_C_YELLOW;
    default:                  return HR_UI_C_GREY;
    }
}

void hr_ui_phase_rgb(hr_phase_t p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (p) {
    case HR_PHASE_PREPARING:
    case HR_PHASE_RUNNING:    *r = 0x40; *g = 0xC0; *b = 0xFF; break;
    case HR_PHASE_FREEZING:   *r = 0x30; *g = 0x60; *b = 0xFF; break;
    case HR_PHASE_DRYING:     *r = 0xFF; *g = 0x90; *b = 0x20; break;
    case HR_PHASE_FINAL_DRY:  *r = 0xFF; *g = 0x50; *b = 0x20; break;
    case HR_PHASE_COMPLETE:   *r = 0x30; *g = 0xE0; *b = 0x60; break;
    case HR_PHASE_IDLE:       *r = 0x60; *g = 0xA0; *b = 0x80; break;
    case HR_PHASE_TRANSITION: *r = 0xFF; *g = 0xD0; *b = 0x20; break;
    default:                  *r = 0x80; *g = 0x80; *b = 0x80; break;
    }
}
