/*
 * Screen layouts. See hr_display_screens.h.
 *
 * Geometry: 160x80. Glyphs are 6x8 (26 columns), 12x16 (13 columns) or
 * 18x24 (8 columns). The five zones are only a flushing convenience - a
 * screen may put content wherever it reads best.
 */
#include "hr_display_screens.h"

#include "hr_gfx.h"

#include <stdio.h>
#include <string.h>

#define X_MARGIN 2
#define X_RIGHT  (HR_GFX_W - X_MARGIN)

/* Text rows for the 8 px font, chosen to sit inside the zones. */
#define Y_STATUS   1
#define Y_LINE1    28
#define Y_LINE2    38
#define Y_FOOT1    62
#define Y_FOOT2    71

static const int s_zones[HR_ZONE_COUNT][2] = {
    {HR_ZONE_STATUS_Y, HR_ZONE_STATUS_H},
    {HR_ZONE_TITLE_Y, HR_ZONE_TITLE_H},
    {HR_ZONE_BIG_Y, HR_ZONE_BIG_H},
    {HR_ZONE_PROGRESS_Y, HR_ZONE_PROGRESS_H},
    {HR_ZONE_FOOTER_Y, HR_ZONE_FOOTER_H},
};

void hr_screens_zone(int i, int *y, int *h)
{
    *y = s_zones[i][0];
    *h = s_zones[i][1];
}

static bool blink(unsigned long now_ms, unsigned period_ms)
{
    return (now_ms % period_ms) < period_ms / 2;
}

/* Copy at most `n` glyphs of `s` into `out` (cap includes the NUL). */
static void clip(const char *s, size_t n, char *out, size_t cap)
{
    if (n >= cap) {
        n = cap - 1;
    }
    snprintf(out, cap, "%.*s", (int)n, s);
}

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */

static void status_bar(const hr_ui_state_t *st, const hr_ui_model_t *m,
                       unsigned long now_ms, const char *right)
{
    hr_gfx_fill_rect(0, HR_ZONE_STATUS_Y, HR_GFX_W, HR_ZONE_STATUS_H,
                     HR_UI_C_BLACK);

    /* U: the dryer-facing USB link. */
    uint16_t cu;
    const bool bad_recent = st->frames_bad_grew_ms != 0 &&
                            now_ms - st->frames_bad_grew_ms <
                                HR_UI_GROWTH_WINDOW_MS;
    if (bad_recent) {
        cu = blink(now_ms, 1000) ? HR_UI_C_RED : HR_UI_C_DKGREY;
    } else if (!m->usb_mounted) {
        cu = HR_UI_C_GREY;
    } else if (!m->link_up) {
        cu = HR_UI_C_YELLOW;
    } else {
        cu = HR_UI_C_GREEN;
    }
    /* W: home network. */
    uint16_t cw;
    switch (m->wifi) {
    case HR_UI_WIFI_CONNECTED:  cw = HR_UI_C_GREEN; break;
    case HR_UI_WIFI_CONNECTING: cw = HR_UI_C_YELLOW; break;
    default:                    cw = HR_UI_C_GREY; break;
    }
    /* M: broker. */
    uint16_t cm = !m->mqtt_configured ? HR_UI_C_GREY
                  : m->mqtt_connected ? HR_UI_C_GREEN
                                      : HR_UI_C_YELLOW;

    int x = X_MARGIN;
    x = hr_gfx_text6x8(x, Y_STATUS, "U", cu, HR_UI_C_BLACK) + 2;
    x = hr_gfx_text6x8(x, Y_STATUS, "W", cw, HR_UI_C_BLACK) + 2;
    x = hr_gfx_text6x8(x, Y_STATUS, "M", cm, HR_UI_C_BLACK) + 2;
    if (m->wifi == HR_UI_WIFI_CONNECTED && m->rssi_dbm != 0) {
        /* Four signal bars, 2 px wide, rising to the right. */
        int bars = m->rssi_dbm > -55 ? 4 : m->rssi_dbm > -65 ? 3
                 : m->rssi_dbm > -75 ? 2 : 1;
        for (int i = 0; i < 4; i++) {
            int h = 2 + i * 2;
            hr_gfx_fill_rect(x + i * 3, Y_STATUS + 8 - h, 2, h,
                             i < bars ? HR_UI_C_GREEN : HR_UI_C_DKGREY);
        }
        x += 14;
    }
    if (hr_ui_alert_active(st)) {
        hr_gfx_text6x8(x + 2, Y_STATUS, "!", HR_UI_C_RED, HR_UI_C_BLACK);
    }
    if (right) {
        hr_gfx_text_right(X_RIGHT, Y_STATUS, right, 1, HR_UI_C_WHITE,
                          HR_UI_C_BLACK);
    }
}

static void status_bar_uptime(const hr_ui_state_t *st, const hr_ui_model_t *m,
                              unsigned long now_ms)
{
    char t[8];
    hr_ui_fmt_hhmm(m->uptime_ms / 1000, t, sizeof(t));
    status_bar(st, m, now_ms, t);
}

/* ------------------------------------------------------------------ */
/* Shared pieces                                                       */
/* ------------------------------------------------------------------ */

/* Title line at 16 px, in colour, with an optional right-aligned tail. */
static void title(const char *text, uint16_t fg, uint16_t bg,
                  const char *right)
{
    hr_gfx_fill_rect(0, HR_ZONE_TITLE_Y, HR_GFX_W, HR_ZONE_TITLE_H, bg);
    int x = hr_gfx_text_big(X_MARGIN, HR_ZONE_TITLE_Y, text, fg, bg);
    if (right && *right) {
        /* Big if it fits beside the title, small otherwise. */
        if (x + 4 + hr_gfx_text_width(right, 2) <= X_RIGHT) {
            hr_gfx_text_right(X_RIGHT, HR_ZONE_TITLE_Y, right, 2,
                              HR_UI_C_WHITE, bg);
        } else {
            hr_gfx_text_right(X_RIGHT, HR_ZONE_TITLE_Y + 4, right, 1,
                              HR_UI_C_WHITE, bg);
        }
    }
}

/*
 * The 24 px readings line: temperature left, vacuum right. Vacuum drops to
 * 12 px and loses its unit if the pair would not fit.
 */
static void readings(const hr_ui_model_t *m, uint16_t fg, uint16_t bg)
{
    char temp[12], vac[16];
    hr_ui_fmt_temp(m->tel.temp_f, false, temp, sizeof(temp));
    hr_gfx_fill_rect(0, HR_ZONE_BIG_Y, HR_GFX_W, HR_ZONE_BIG_H, bg);
    int x = hr_gfx_text_huge(X_MARGIN, HR_ZONE_BIG_Y, temp, fg, bg);

    if (m->tel.pressure_valid) {
        snprintf(vac, sizeof(vac), "%ldmT", m->tel.vacuum_um);
        if (x + 6 + hr_gfx_text_width(vac, 2) > X_RIGHT) {
            snprintf(vac, sizeof(vac), "%ld", m->tel.vacuum_um);
        }
    } else {
        snprintf(vac, sizeof(vac), "atm");
    }
    hr_gfx_text_right(X_RIGHT, HR_ZONE_BIG_Y + 4, vac, 2, fg, bg);
}

static void footer_clear(uint16_t bg)
{
    hr_gfx_fill_rect(0, HR_ZONE_FOOTER_Y, HR_GFX_W, HR_ZONE_FOOTER_H, bg);
}

static void progress_clear(uint16_t bg)
{
    hr_gfx_fill_rect(0, HR_ZONE_PROGRESS_Y, HR_GFX_W, HR_ZONE_PROGRESS_H, bg);
}

/* Bar across the progress zone with a percentage at the right. */
static void progress_pct(int pct, uint16_t color, const char *label)
{
    progress_clear(HR_UI_C_BLACK);
    char t[8];
    if (label == NULL) {
        snprintf(t, sizeof(t), "%3d%%", pct);
        label = t;
    }
    const int bar_w = HR_GFX_W - X_MARGIN * 2 - hr_gfx_text_width(label, 1) - 4;
    hr_gfx_progress_bar(X_MARGIN, HR_ZONE_PROGRESS_Y + 1, bar_w, 8, pct, color);
    hr_gfx_text_right(X_RIGHT, HR_ZONE_PROGRESS_Y + 1, label, 1, HR_UI_C_WHITE,
                      HR_UI_C_BLACK);
}

/* ------------------------------------------------------------------ */
/* Screens                                                             */
/* ------------------------------------------------------------------ */

static void screen_boot(const hr_ui_model_t *m, const char *adapter_fw)
{
    (void)m;
    char line[32];
    hr_gfx_clear(HR_UI_C_BLACK);
    snprintf(line, sizeof(line), "free-harvest v%s", adapter_fw);
    hr_gfx_text6x8(X_MARGIN, Y_STATUS, line, HR_UI_C_GREY, HR_UI_C_BLACK);
    hr_gfx_text_big(X_MARGIN, 20, "Harvest Right", HR_UI_C_WHITE, HR_UI_C_BLACK);
    hr_gfx_text_big(X_MARGIN, 38, "dongle", HR_UI_C_GREEN, HR_UI_C_BLACK);
    hr_gfx_text6x8(X_MARGIN, Y_FOOT2, "starting up...", HR_UI_C_GREY,
                   HR_UI_C_BLACK);
}

static void screen_provision(const hr_ui_state_t *st, const hr_ui_model_t *m,
                             unsigned long now_ms)
{
    char t[16];
    hr_gfx_clear(HR_UI_C_BLACK);
    hr_ui_fmt_mmss((unsigned long)(m->ap_remaining_s > 0 ? m->ap_remaining_s : 0),
                   t, sizeof(t));
    status_bar(st, m, now_ms, t);
    title("SETUP Wi-Fi", HR_UI_C_YELLOW, HR_UI_C_BLACK, NULL);
    hr_gfx_text6x8(X_MARGIN, Y_LINE1, "join AP:", HR_UI_C_WHITE, HR_UI_C_BLACK);
    if (hr_gfx_text_width(m->ap_ssid, 2) <= HR_GFX_W - X_MARGIN * 2) {
        hr_gfx_text_big(X_MARGIN, 36, m->ap_ssid, HR_UI_C_WHITE, HR_UI_C_BLACK);
    } else {
        hr_gfx_text6x8(X_MARGIN, Y_LINE2 + 2, m->ap_ssid, HR_UI_C_WHITE,
                       HR_UI_C_BLACK);
    }
    hr_gfx_text6x8(X_MARGIN, 54, "open http://192.168.4.1", HR_UI_C_WHITE,
                   HR_UI_C_BLACK);
    int pct = (int)(m->ap_remaining_s * 100 / 300);
    hr_gfx_progress_bar(X_MARGIN, 66, HR_GFX_W - X_MARGIN * 2, 8, pct,
                        HR_UI_C_YELLOW);
}

static void screen_ap_closed(const hr_ui_state_t *st, const hr_ui_model_t *m,
                             unsigned long now_ms)
{
    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);
    title("AP CLOSED", HR_UI_C_RED, HR_UI_C_BLACK, NULL);
    hr_gfx_text6x8(X_MARGIN, Y_LINE1, "setup window (5 min) over",
                   HR_UI_C_WHITE, HR_UI_C_BLACK);
    hr_gfx_text6x8(X_MARGIN, Y_LINE2, "no network joined", HR_UI_C_GREY,
                   HR_UI_C_BLACK);
    hr_gfx_text6x8(X_MARGIN, Y_FOOT1, "replug the stick to run",
                   HR_UI_C_WHITE, HR_UI_C_BLACK);
    hr_gfx_text6x8(X_MARGIN, Y_FOOT2, "setup again", HR_UI_C_WHITE,
                   HR_UI_C_BLACK);
}

static void screen_connecting(const hr_ui_state_t *st, const hr_ui_model_t *m,
                              unsigned long now_ms)
{
    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);
    title("Wi-Fi", HR_UI_C_WHITE, HR_UI_C_BLACK, NULL);
    char line[40];
    snprintf(line, sizeof(line), "%.25s", m->ssid[0] ? m->ssid : "(no network)");
    hr_gfx_text6x8(X_MARGIN, Y_LINE1, line, HR_UI_C_WHITE, HR_UI_C_BLACK);
    if (m->wifi == HR_UI_WIFI_CONNECTED) {
        if (hr_gfx_text_width(m->ip, 2) <= HR_GFX_W - X_MARGIN * 2) {
            hr_gfx_text_big(X_MARGIN, Y_LINE2, m->ip, HR_UI_C_GREEN,
                            HR_UI_C_BLACK);
        } else {
            hr_gfx_text6x8(X_MARGIN, Y_LINE2 + 4, m->ip, HR_UI_C_GREEN,
                           HR_UI_C_BLACK);
        }
        hr_gfx_text6x8(X_MARGIN, Y_FOOT1, "connected - open this IP",
                       HR_UI_C_GREY, HR_UI_C_BLACK);
        hr_gfx_text6x8(X_MARGIN, Y_FOOT2, "in a browser", HR_UI_C_GREY,
                       HR_UI_C_BLACK);
    } else {
        hr_gfx_text6x8(X_MARGIN, Y_LINE2,
                       blink(now_ms, 1000) ? "connecting..." : "connecting",
                       HR_UI_C_YELLOW, HR_UI_C_BLACK);
    }
}

static void screen_no_dryer(const hr_ui_state_t *st, const hr_ui_model_t *m,
                            unsigned long now_ms)
{
    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);
    title("WAITING", HR_UI_C_WHITE, HR_UI_C_BLACK, NULL);
    hr_gfx_text6x8(X_MARGIN + 7 * 12 + 4, HR_ZONE_TITLE_Y + 4, "for dryer",
                   HR_UI_C_GREY, HR_UI_C_BLACK);

    /* The three failures that look identical from the outside. */
    const char *why;
    if (!m->usb_mounted) {
        why = "USB: not enumerated";
    } else if (m->usb_rx_bytes == 0) {
        why = "USB: enumerated, silent";
    } else if (!m->link_up && m->frames_bad > 0) {
        why = "USB: bad frames";
    } else if (!m->link_up) {
        why = "USB: link down, no frames";
    } else {
        why = "USB: linked, no STAT yet";
    }
    hr_gfx_text6x8(X_MARGIN, Y_LINE1, why, HR_UI_C_WHITE, HR_UI_C_BLACK);

    char line[40];
    if (m->usb_rx_bytes < 10000) {
        snprintf(line, sizeof(line), "mounts %u  rx %lu B", m->usb_mounts,
                 m->usb_rx_bytes);
    } else {
        snprintf(line, sizeof(line), "mounts %u  rx %lu KB", m->usb_mounts,
                 m->usb_rx_bytes / 1024);
    }
    hr_gfx_text6x8(X_MARGIN, Y_LINE2, line, HR_UI_C_GREY, HR_UI_C_BLACK);

    if (m->wifi == HR_UI_WIFI_CONNECTED) {
        hr_gfx_text6x8(X_MARGIN, Y_FOOT1, m->ip, HR_UI_C_WHITE, HR_UI_C_BLACK);
    } else if (m->wifi == HR_UI_WIFI_AP_SETUP) {
        snprintf(line, sizeof(line), "AP %.22s", m->ap_ssid);
        hr_gfx_text6x8(X_MARGIN, Y_FOOT1, line, HR_UI_C_GREY, HR_UI_C_BLACK);
    } else {
        hr_gfx_text6x8(X_MARGIN, Y_FOOT1, "no Wi-Fi", HR_UI_C_GREY,
                       HR_UI_C_BLACK);
    }
    if (m->machine_name[0]) {
        snprintf(line, sizeof(line), "%.26s", m->machine_name);
        hr_gfx_text6x8(X_MARGIN, Y_FOOT2, line, HR_UI_C_GREY, HR_UI_C_BLACK);
    }
}

static void screen_idle(const hr_ui_state_t *st, const hr_ui_model_t *m,
                        unsigned long now_ms)
{
    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);
    title(hr_ui_phase_label_short(m->tel.phase),
          m->tel.phase == HR_PHASE_IDLE ? HR_UI_C_GREEN : HR_UI_C_GREY,
          HR_UI_C_BLACK, NULL);
    readings(m, HR_UI_C_WHITE, HR_UI_C_BLACK);

    char line[40];
    footer_clear(HR_UI_C_BLACK);
    snprintf(line, sizeof(line), "%.26s",
             m->machine_name[0] ? m->machine_name : "Harvest Right");
    hr_gfx_text6x8(X_MARGIN, Y_FOOT1, line, HR_UI_C_WHITE, HR_UI_C_BLACK);
    if (m->fw_version[0]) {
        snprintf(line, sizeof(line), "fw %.14s  %.8s", m->fw_version,
                 m->tel.mode);
    } else {
        snprintf(line, sizeof(line), "%.16s", m->tel.mode);
    }
    hr_gfx_text6x8(X_MARGIN, Y_FOOT2, line, HR_UI_C_GREY, HR_UI_C_BLACK);
}

static void screen_run(const hr_ui_state_t *st, const hr_ui_model_t *m,
                       unsigned long now_ms)
{
    const hr_phase_t phase = m->tel.phase;
    const uint16_t pc = hr_ui_phase_color(phase);
    char t[24], line[40];

    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);

    if (phase == HR_PHASE_TRANSITION) {
        /* The machine is waiting for a human to load the trays. */
        hr_gfx_text_big(4, HR_ZONE_TITLE_Y + 3, "LOAD TRAYS", HR_UI_C_YELLOW,
                        HR_UI_C_BLACK);
        hr_gfx_text6x8(4, 31, "trays in, drain valve shut,", HR_UI_C_WHITE,
                       HR_UI_C_BLACK);
        hr_gfx_text6x8(4, 41, "then press Continue on the", HR_UI_C_WHITE,
                       HR_UI_C_BLACK);
        hr_gfx_text6x8(4, 51, "dryer's own panel", HR_UI_C_WHITE,
                       HR_UI_C_BLACK);
        hr_ui_fmt_temp(m->tel.temp_f, false, t, sizeof(t));
        hr_gfx_text6x8(4, 66, t, HR_UI_C_GREY, HR_UI_C_BLACK);
        if (blink(now_ms, 1000)) {
            hr_gfx_frame(0, HR_ZONE_TITLE_Y, HR_GFX_W, HR_GFX_H - HR_ZONE_TITLE_Y,
                         2, HR_UI_C_YELLOW);
        }
        return;
    }

    hr_ui_fmt_hm((unsigned long)m->tel.phase_elapsed_s, t, sizeof(t));
    title(hr_ui_phase_label_short(phase), pc, HR_UI_C_BLACK, t);

    if (phase == HR_PHASE_PREPARING) {
        /* Countdown in place of the vacuum - the pump is off anyway. */
        char temp[12];
        hr_ui_fmt_temp(m->tel.temp_f, false, temp, sizeof(temp));
        hr_gfx_fill_rect(0, HR_ZONE_BIG_Y, HR_GFX_W, HR_ZONE_BIG_H, HR_UI_C_BLACK);
        hr_gfx_text_huge(X_MARGIN, HR_ZONE_BIG_Y, temp, HR_UI_C_WHITE,
                         HR_UI_C_BLACK);
        hr_ui_fmt_mmss((unsigned long)(m->tel.prep_remaining_s > 0
                                           ? m->tel.prep_remaining_s : 0),
                       t, sizeof(t));
        /* 24 px if it leaves a gap after the temperature, else 16 px. */
        if (hr_gfx_text_width(temp, 3) + 6 + hr_gfx_text_width(t, 3) <=
            HR_GFX_W - X_MARGIN * 2) {
            hr_gfx_text_right(X_RIGHT, HR_ZONE_BIG_Y, t, 3, pc, HR_UI_C_BLACK);
        } else {
            hr_gfx_text_right(X_RIGHT, HR_ZONE_BIG_Y + 4, t, 2, pc,
                              HR_UI_C_BLACK);
        }
        int pct = (int)((900 - m->tel.prep_remaining_s) * 100 / 900);
        progress_pct(pct, pc, "pre-cool");
    } else {
        readings(m, HR_UI_C_WHITE, HR_UI_C_BLACK);
        if (m->tel.phase_pct >= 0 && m->tel.phase_pct <= 100) {
            progress_pct((int)m->tel.phase_pct, pc, NULL);
        } else {
            progress_clear(HR_UI_C_BLACK);
        }
    }

    footer_clear(HR_UI_C_BLACK);
    hr_ui_fmt_hm((unsigned long)m->tel.batch_elapsed_s, t, sizeof(t));
    snprintf(line, sizeof(line), "batch %s", t);
    hr_gfx_text6x8(X_MARGIN, Y_FOOT1, line, HR_UI_C_WHITE, HR_UI_C_BLACK);
    if (phase == HR_PHASE_FREEZING && m->tel.freeze_eta_s >= 0) {
        hr_ui_fmt_hm((unsigned long)m->tel.freeze_eta_s, t, sizeof(t));
        snprintf(line, sizeof(line), "ETA %s", t);
        hr_gfx_text_right(X_RIGHT, Y_FOOT1, line, 1, HR_UI_C_WHITE,
                          HR_UI_C_BLACK);
    }
    snprintf(line, sizeof(line), "%.14s  %.10s", m->tel.mode, m->machine_name);
    hr_gfx_text6x8(X_MARGIN, Y_FOOT2, line, HR_UI_C_GREY, HR_UI_C_BLACK);
}

static void screen_complete(const hr_ui_state_t *st, const hr_ui_model_t *m,
                            unsigned long now_ms)
{
    /* Inverted for a moment when the phase flips to COMPLETE. */
    const bool flash = st->complete_since_ms != 0 &&
                       now_ms - st->complete_since_ms < HR_UI_COMPLETE_FLASH_MS;
    const uint16_t bg = flash ? HR_UI_C_GREEN : HR_UI_C_BLACK;
    const uint16_t fg = flash ? HR_UI_C_BLACK : HR_UI_C_WHITE;
    const uint16_t accent = flash ? HR_UI_C_BLACK : HR_UI_C_GREEN;
    char t[24], line[40];

    hr_gfx_clear(bg);
    status_bar_uptime(st, m, now_ms);
    title("COMPLETE", accent, bg, NULL);
    hr_gfx_text_right(X_RIGHT, HR_ZONE_TITLE_Y + 4, "done", 1, accent, bg);
    readings(m, fg, bg);
    hr_ui_fmt_hm((unsigned long)m->tel.batch_elapsed_s, t, sizeof(t));
    snprintf(line, sizeof(line), "total %s", t);
    hr_gfx_text6x8(X_MARGIN, HR_ZONE_PROGRESS_Y + 1, line, fg, bg);
    hr_gfx_text6x8(X_MARGIN, Y_FOOT1, "defrost / warm / +2h on", flash ? fg : HR_UI_C_GREY, bg);
    hr_gfx_text6x8(X_MARGIN, Y_FOOT2, "the dryer's panel", flash ? fg : HR_UI_C_GREY, bg);
}

static void screen_alert(const hr_ui_state_t *st, const hr_ui_model_t *m,
                         unsigned long now_ms)
{
    char l1[40] = "", l2[40] = "", t[16], title_s[24];

    switch (st->alert) {
    case HR_UI_ALERT_LINK_LOST: {
        unsigned long silent_s =
            st->link_lost_ms ? (now_ms - st->link_lost_ms) / 1000 : 0;
        if (silent_s >= 3600) {
            hr_ui_fmt_hm(silent_s, t, sizeof(t));
        } else {
            hr_ui_fmt_mmss(silent_s, t, sizeof(t));
        }
        snprintf(l1, sizeof(l1), "dryer silent %s", t);
        hr_ui_fmt_hm((unsigned long)st->last_run_elapsed_s, t, sizeof(t));
        snprintf(l2, sizeof(l2), "was: %s %s",
                 hr_ui_phase_label_short(st->last_run_phase), t);
        break;
    }
    case HR_UI_ALERT_DIAGNOSTICS:
        snprintf(l1, sizeof(l1), "dryer on diagnostics screen");
        snprintf(l2, sizeof(l2), "USB link will drop");
        break;
    case HR_UI_ALERT_LOW_HEAP:
        snprintf(l1, sizeof(l1), "heap %uk free", m->heap_free / 1024);
        snprintf(l2, sizeof(l2), "adapter may restart");
        break;
    case HR_UI_ALERT_RESET:
        snprintf(l1, sizeof(l1), "reset reason: %s", m->reset_reason);
        snprintf(l2, sizeof(l2), "see /api/log");
        break;
    case HR_UI_ALERT_BAD_FRAMES:
        snprintf(l1, sizeof(l1), "parser rejects: %lu", m->frames_bad);
        snprintf(l2, sizeof(l2), "bytes kept in /api/log");
        break;
    case HR_UI_ALERT_CAPTURE_DROP:
        snprintf(l1, sizeof(l1), "capture drops: %lu", m->capture_dropped);
        snprintf(l2, sizeof(l2), "flash log has gaps");
        break;
    case HR_UI_ALERT_UNKNOWN_SCREEN:
        snprintf(l1, sizeof(l1), "STAT type %d never seen", m->tel.type);
        snprintf(l2, sizeof(l2), "please share the capture");
        break;
    default:
        break;
    }

    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);
    snprintf(title_s, sizeof(title_s), "! %s", hr_ui_alert_title(st->alert));
    if (hr_gfx_text_width(title_s, 2) > HR_GFX_W - 8) {
        hr_gfx_text6x8(4, HR_ZONE_TITLE_Y + 7, title_s, HR_UI_C_RED,
                       HR_UI_C_BLACK);
    } else {
        hr_gfx_text_big(4, HR_ZONE_TITLE_Y + 3, title_s, HR_UI_C_RED,
                        HR_UI_C_BLACK);
    }
    hr_gfx_text6x8(4, 31, l1, HR_UI_C_WHITE, HR_UI_C_BLACK);
    hr_gfx_text6x8(4, 41, l2, HR_UI_C_WHITE, HR_UI_C_BLACK);
    hr_gfx_text6x8(4, 68, "press to dismiss", HR_UI_C_GREY, HR_UI_C_BLACK);
    /* 2 Hz frame: the same blink as the LED. */
    hr_gfx_frame(0, HR_ZONE_TITLE_Y, HR_GFX_W, HR_GFX_H - HR_ZONE_TITLE_Y, 2,
                 blink(now_ms, 500) ? HR_UI_C_RED : HR_UI_C_DKGREY);
}

static void screen_info(const hr_ui_state_t *st, const hr_ui_model_t *m,
                        unsigned long now_ms, const char *adapter_fw)
{
    char line[40], a[16], b[16];
    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);

    /* IP, big when it fits (it does for anything under 14 characters). */
    const char *ip = m->wifi == HR_UI_WIFI_CONNECTED ? m->ip : "no IP";
    if (hr_gfx_text_width(ip, 2) <= HR_GFX_W - X_MARGIN * 2) {
        hr_gfx_text_big(X_MARGIN, HR_ZONE_TITLE_Y, ip, HR_UI_C_WHITE,
                        HR_UI_C_BLACK);
    } else {
        hr_gfx_text6x8(X_MARGIN, HR_ZONE_TITLE_Y + 4, ip, HR_UI_C_WHITE,
                       HR_UI_C_BLACK);
    }

    int y = 27; /* six 9 px rows end exactly at the bottom edge */
    if (m->wifi == HR_UI_WIFI_CONNECTED) {
        clip(m->ssid, 13, a, sizeof(a));
        snprintf(line, sizeof(line), "SSID %s %ddBm", a, m->rssi_dbm);
    } else if (m->wifi == HR_UI_WIFI_AP_SETUP) {
        clip(m->ap_ssid, 20, a, sizeof(a));
        snprintf(line, sizeof(line), "AP %s", a);
    } else if (m->wifi == HR_UI_WIFI_CONNECTING) {
        clip(m->ssid, 16, a, sizeof(a));
        snprintf(line, sizeof(line), "joining %s", a);
    } else {
        snprintf(line, sizeof(line), "Wi-Fi: none");
    }
    hr_gfx_text6x8(X_MARGIN, y, line, HR_UI_C_WHITE, HR_UI_C_BLACK);
    y += 9;

    snprintf(line, sizeof(line), "MQTT %s",
             !m->mqtt_configured ? "not set"
             : m->mqtt_connected ? "connected" : "not connected");
    hr_gfx_text6x8(X_MARGIN, y, line, HR_UI_C_WHITE, HR_UI_C_BLACK);
    y += 9;

    hr_ui_fmt_bytes_mb(m->capture_used, m->capture_cap, a, sizeof(a));
    snprintf(line, sizeof(line), "log %s drops %lu", a, m->capture_dropped);
    hr_gfx_text6x8(X_MARGIN, y, line, HR_UI_C_WHITE, HR_UI_C_BLACK);
    y += 9;

    hr_ui_fmt_uptime(m->uptime_ms / 1000, b, sizeof(b));
    snprintf(line, sizeof(line), "heap %uk  up %s", m->heap_free / 1024, b);
    hr_gfx_text6x8(X_MARGIN, y, line, HR_UI_C_WHITE, HR_UI_C_BLACK);
    y += 9;

    snprintf(line, sizeof(line), "v%s  reset %s", adapter_fw, m->reset_reason);
    hr_gfx_text6x8(X_MARGIN, y, line, HR_UI_C_GREY, HR_UI_C_BLACK);
    y += 9;

    snprintf(line, sizeof(line), "usb mounts %u  bad %lu", m->usb_mounts,
             m->frames_bad);
    hr_gfx_text6x8(X_MARGIN, y, line, HR_UI_C_GREY, HR_UI_C_BLACK);
}

static void screen_raw(const hr_ui_state_t *st, const hr_ui_model_t *m,
                       unsigned long now_ms)
{
    hr_gfx_clear(HR_UI_C_BLACK);
    status_bar_uptime(st, m, now_ms);
    if (!m->tel.valid || m->tel.last_stat[0] == '\0') {
        hr_gfx_text6x8(X_MARGIN, 14, "no STAT received yet", HR_UI_C_GREY,
                       HR_UI_C_BLACK);
        return;
    }
    /* Five rows of 26 glyphs, wrapped hard. */
    const char *s = m->tel.last_stat;
    size_t len = strlen(s);
    char row[27];
    for (int i = 0; i < 5 && (size_t)i * 26 < len; i++) {
        clip(s + i * 26, 26, row, sizeof(row));
        hr_gfx_text6x8(X_MARGIN, 12 + i * 9, row, HR_UI_C_WHITE, HR_UI_C_BLACK);
    }
    char age[24];
    hr_ui_fmt_mmss((m->uptime_ms - m->tel.rx_ms) / 1000, age, sizeof(age));
    hr_gfx_text_right(X_RIGHT, Y_FOOT2, age, 1, HR_UI_C_GREY, HR_UI_C_BLACK);
}

void hr_screens_render(hr_ui_screen_t screen, const hr_ui_state_t *st,
                       const hr_ui_model_t *m, unsigned long now_ms,
                       const char *adapter_fw)
{
    switch (screen) {
    case HR_UI_SCREEN_BOOT:       screen_boot(m, adapter_fw); break;
    case HR_UI_SCREEN_PROVISION:  screen_provision(st, m, now_ms); break;
    case HR_UI_SCREEN_AP_CLOSED:  screen_ap_closed(st, m, now_ms); break;
    case HR_UI_SCREEN_CONNECTING: screen_connecting(st, m, now_ms); break;
    case HR_UI_SCREEN_NO_DRYER:   screen_no_dryer(st, m, now_ms); break;
    case HR_UI_SCREEN_IDLE:       screen_idle(st, m, now_ms); break;
    case HR_UI_SCREEN_RUN:        screen_run(st, m, now_ms); break;
    case HR_UI_SCREEN_COMPLETE:   screen_complete(st, m, now_ms); break;
    case HR_UI_SCREEN_ALERT:      screen_alert(st, m, now_ms); break;
    case HR_UI_SCREEN_INFO:       screen_info(st, m, now_ms, adapter_fw); break;
    case HR_UI_SCREEN_RAW:        screen_raw(st, m, now_ms); break;
    default:                      hr_gfx_clear(HR_UI_C_BLACK); break;
    }
}
