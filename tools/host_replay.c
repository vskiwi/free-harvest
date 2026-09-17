/*
 * host_replay - run a capture through the protocol core and the display
 * state machine on a PC, with no ESP-IDF and no hardware.
 *
 * Reads a Free Harvest capture (format v2, as downloaded from /api/capture,
 * or the "<rel_ms>\t<frame>" script written by tools/replay_capture.py
 * --to-frames), feeds every frame the dryer sent into hr_session exactly as
 * hr_usb.c would, advances the clock between frames the way the main loop
 * does, and after each step asks hr_ui_select() which screen the dongle would
 * be showing. Screen changes, LED changes and every frame the session sends
 * back are printed with their timestamps; where the capture also contains the
 * adapter's side ('<' lines) the verbs are compared at the end.
 *
 * What this checks without a dryer:
 *   - the parser and session on real frame shapes (handshake, REQINFO ->
 *     WIFIINFO, STAT decoding, link timeout on a gap);
 *   - the screen state machine: BOOT -> PROVISION/CONNECTING -> NO_DRYER ->
 *     IDLE -> RUN (per phase) -> COMPLETE, ALERT on a mid-run link loss,
 *     INFO/RAW on a simulated button press and their auto-return;
 *   - that nothing here can transmit anything but what hr_session decides to.
 *
 * Build and run: tools/host_replay.sh [capture] [options]
 *   --wifi-at MS      pretend the station got an IP at this uptime
 *   --press MS        simulate a short button press at this uptime (repeatable)
 *   --long-press MS   simulate a long press (night mode toggle)
 *   --quiet           print only screen transitions and the summary
 */
#include "hr_session.h"
#include "hr_telemetry.h"
#include "hr_ui_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 20000
#define MAX_PRESSES 16

typedef struct {
    unsigned long ms;
    char dir;
    char *payload;
} line_t;

static line_t s_lines[MAX_LINES];
static size_t s_nlines;

/* ---- what the session transmits ------------------------------------- */
typedef struct {
    char verb[HR_MAX_VERB];
    unsigned count;
} verb_count_t;
static verb_count_t s_tx_verbs[64];
static size_t s_ntx_verbs;
static unsigned long s_tx_frames;
static unsigned long s_now_ms;
static bool s_quiet;

static void count_verb(verb_count_t *tab, size_t *n, const char *verb)
{
    for (size_t i = 0; i < *n; i++) {
        if (strcmp(tab[i].verb, verb) == 0) {
            tab[i].count++;
            return;
        }
    }
    if (*n < 64) {
        snprintf(tab[*n].verb, HR_MAX_VERB, "%s", verb);
        tab[*n].count = 1;
        (*n)++;
    }
}

static void tx_verb_of(const char *frame, char dir, char *out, size_t cap)
{
    size_t i = 0;
    char sep = (dir == '>') ? ',' : ' ';
    while (frame[i] && frame[i] != sep && frame[i] != '\r' && i + 1 < cap) {
        out[i] = frame[i];
        i++;
    }
    out[i] = '\0';
}

static bool host_tx(const char *data, size_t len, void *user)
{
    (void)user;
    char line[HR_MAX_FRAME];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, data, n);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
        n--;
    }
    line[n] = '\0';
    char verb[HR_MAX_VERB];
    tx_verb_of(line, '<', verb, sizeof(verb));
    count_verb(s_tx_verbs, &s_ntx_verbs, verb);
    s_tx_frames++;
    if (!s_quiet) {
        printf("%9lu  <  %s\n", s_now_ms, line);
    }
    return true;
}

/* ---- capture reading -------------------------------------------------- */
static bool read_capture(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return false;
    }
    char buf[HR_MAX_FRAME + 128];
    while (fgets(buf, sizeof(buf), f) != NULL && s_nlines < MAX_LINES) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n == 0 || buf[0] == '#') {
            continue;
        }
        /* split on tabs */
        char *col[4] = {0};
        int nc = 0;
        char *p = buf;
        col[nc++] = p;
        while (nc < 4 && (p = strchr(p, '\t')) != NULL) {
            *p++ = '\0';
            col[nc++] = p;
        }
        line_t *l = &s_lines[s_nlines];
        if (nc == 4) {
            l->ms = strtoul(col[0], NULL, 10);
            l->dir = col[2][0];
            l->payload = strdup(col[3]);
        } else if (nc == 2) {
            l->ms = strtoul(col[0], NULL, 10);
            l->dir = (col[1][0] == '~') ? '~' : '>';
            l->payload = strdup(col[1]);
        } else {
            continue;
        }
        if (l->payload == NULL) {
            break;
        }
        s_nlines++;
    }
    fclose(f);
    return s_nlines > 0;
}

/* ---- the run ---------------------------------------------------------- */
static const char *screen_name(hr_ui_screen_t s)
{
    switch (s) {
    case HR_UI_SCREEN_BOOT:       return "BOOT";
    case HR_UI_SCREEN_PROVISION:  return "PROVISION";
    case HR_UI_SCREEN_AP_CLOSED:  return "AP_CLOSED";
    case HR_UI_SCREEN_CONNECTING: return "CONNECTING";
    case HR_UI_SCREEN_NO_DRYER:   return "NO_DRYER";
    case HR_UI_SCREEN_IDLE:       return "IDLE";
    case HR_UI_SCREEN_RUN:        return "RUN";
    case HR_UI_SCREEN_COMPLETE:   return "COMPLETE";
    case HR_UI_SCREEN_ALERT:      return "ALERT";
    case HR_UI_SCREEN_INFO:       return "INFO";
    case HR_UI_SCREEN_RAW:        return "RAW";
    default:                      return "?";
    }
}

static const char *pattern_name(hr_ui_led_pattern_t p)
{
    switch (p) {
    case HR_UI_LED_OFF:     return "off";
    case HR_UI_LED_SOLID:   return "solid";
    case HR_UI_LED_BREATHE: return "breathe";
    case HR_UI_LED_BLINK:   return "blink";
    default:                return "?";
    }
}

typedef struct {
    hr_session_t session;
    hr_phase_tracker_t tracker;
    hr_ui_model_t model;
    hr_ui_state_t ui;
    hr_ui_screen_t last_screen;
    hr_ui_led_t last_led;
    unsigned transitions;
    unsigned long wifi_at_ms;
    unsigned screens_seen[HR_UI_SCREEN_RAW + 1];
} rig_t;

static void on_inbound(const hr_frame_t *f, void *user)
{
    rig_t *r = (rig_t *)user;
    hr_telemetry_t tel;
    if (hr_telemetry_from_stat(f, &tel)) {
        hr_phase_tracker_update(&r->tracker, &tel, s_now_ms);
        hr_ui_telemetry_t *t = &r->model.tel;
        t->valid = true;
        t->type = tel.type;
        t->phase = hr_phase_of_tracked(&tel, &r->tracker);
        t->temp_f = tel.temperature_f;
        t->pressure_valid = tel.pressure_valid;
        t->vacuum_um = tel.pressure_microns;
        t->batch_elapsed_s = tel.batch_elapsed_s;
        t->phase_elapsed_s = tel.phase_elapsed_s;
        t->phase_pct = tel.phase_pct;
        t->prep_remaining_s = tel.prep_remaining_s;
        t->purge_remaining_s = tel.purge_remaining_s;
        t->purge_pump_on = tel.purge_pump_on;
        t->freeze_eta_s = hr_freeze_eta_s(&r->tracker, &tel);
        snprintf(t->mode, sizeof(t->mode), "%s", tel.mode);
        snprintf(t->last_stat, sizeof(t->last_stat), "%s",
                 r->session.info.last_stat);
        t->rx_ms = s_now_ms;
    }
}

static void refresh_model(rig_t *r)
{
    hr_ui_model_t *m = &r->model;
    m->uptime_ms = s_now_ms;
    if (r->wifi_at_ms != 0 && s_now_ms >= r->wifi_at_ms) {
        m->wifi = HR_UI_WIFI_CONNECTED;
        snprintf(m->ssid, sizeof(m->ssid), "MyNetwork");
        snprintf(m->ip, sizeof(m->ip), "192.168.1.42");
        m->rssi_dbm = -61;
        m->ap_remaining_s = 0;
    } else if (r->wifi_at_ms != 0) {
        m->wifi = HR_UI_WIFI_CONNECTING;
        snprintf(m->ssid, sizeof(m->ssid), "MyNetwork");
        m->ap_remaining_s = (long)(300 - (long)(s_now_ms / 1000));
        if (m->ap_remaining_s < 0) {
            m->ap_remaining_s = 0;
        }
    } else {
        m->wifi = s_now_ms < 300000UL ? HR_UI_WIFI_AP_SETUP
                                      : HR_UI_WIFI_AP_CLOSED;
        m->ap_remaining_s = s_now_ms < 300000UL
                                ? (long)(300 - (long)(s_now_ms / 1000)) : 0;
    }
    m->usb_mounted = true;
    m->usb_mounts = 1;
    m->link_up = (r->session.link == HR_LINK_UP);
    m->usb_rx_bytes = r->session.frames_in * 60;
    m->frames_bad = r->session.stream.frames_bad;
    snprintf(m->machine_name, sizeof(m->machine_name), "%s",
             r->session.info.serial);
    snprintf(m->fw_version, sizeof(m->fw_version), "%s",
             r->session.info.fw_version);
    m->heap_free = 140000;
    m->capture_cap = 11900000;
    m->capture_used = (size_t)(r->session.frames_in * 60);
}

static void step(rig_t *r)
{
    hr_session_tick(&r->session, s_now_ms);
    hr_phase_tracker_tick(&r->tracker, s_now_ms);
    refresh_model(r);
    hr_ui_screen_t s = hr_ui_select(&r->ui, &r->model, s_now_ms);
    hr_ui_led_t led = hr_ui_led_for(s, &r->model, s_now_ms);
    if (s != r->last_screen) {
        r->transitions++;
        r->screens_seen[s]++;
        const hr_ui_telemetry_t *t = &r->model.tel;
        char extra[96] = "";
        if (s == HR_UI_SCREEN_RUN || s == HR_UI_SCREEN_COMPLETE ||
            s == HR_UI_SCREEN_IDLE) {
            char temp[16], vac[16], hm[16];
            hr_ui_fmt_temp(t->temp_f, false, temp, sizeof(temp));
            hr_ui_fmt_vac(t->vacuum_um, t->pressure_valid, vac, sizeof(vac));
            hr_ui_fmt_hm((unsigned long)t->batch_elapsed_s, hm, sizeof(hm));
            snprintf(extra, sizeof(extra), " %s %s %s batch %s",
                     hr_ui_phase_label_short(t->phase), temp, vac, hm);
        } else if (s == HR_UI_SCREEN_ALERT) {
            snprintf(extra, sizeof(extra), " %s",
                     hr_ui_alert_title(r->ui.alert));
        }
        printf("%9lu  == screen %-10s led %02x%02x%02x %s %u%%%s\n", s_now_ms,
               screen_name(s), led.r, led.g, led.b, pattern_name(led.pattern),
               led.brightness_pct, extra);
        r->last_screen = s;
    }
    r->last_led = led;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    unsigned long wifi_at = 0;
    unsigned long presses[MAX_PRESSES], longs[MAX_PRESSES];
    size_t np = 0, nl = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wifi-at") == 0 && i + 1 < argc) {
            wifi_at = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            if (np < MAX_PRESSES) presses[np++] = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--long-press") == 0 && i + 1 < argc) {
            if (nl < MAX_PRESSES) longs[nl++] = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--quiet") == 0) {
            s_quiet = true;
        } else {
            path = argv[i];
        }
    }
    if (path == NULL) {
        fprintf(stderr, "usage: host_replay CAPTURE [--wifi-at MS] "
                        "[--press MS]... [--long-press MS]... [--quiet]\n");
        return 2;
    }
    if (!read_capture(path)) {
        fprintf(stderr, "no frames in %s\n", path);
        return 2;
    }

    rig_t r;
    memset(&r, 0, sizeof(r));
    r.wifi_at_ms = wifi_at;
    r.last_screen = (hr_ui_screen_t)-1;
    hr_session_init(&r.session, host_tx, NULL);
    hr_session_set_observer(&r.session, on_inbound, &r);
    hr_phase_tracker_init(&r.tracker);
    hr_ui_model_init(&r.model);
    hr_ui_state_init(&r.ui);
    snprintf(r.model.ap_ssid, sizeof(r.model.ap_ssid), "HR-Adapter-Setup");
    snprintf(r.model.reset_reason, sizeof(r.model.reset_reason), "poweron");
    hr_session_set_wifi(&r.session, 1, 0, "", "HR_3c8427e1b4f0");

    /* expected adapter-side verbs, from the capture's '<' lines */
    verb_count_t want[64];
    size_t nwant = 0;
    unsigned long rx_frames = 0;

    /* clock: start at the first line's time minus the BOOT splash */
    unsigned long t0 = s_lines[0].ms;
    s_now_ms = t0 > 500 ? t0 - 500 : 0;
    r.model.boot_ms = s_now_ms;
    step(&r);

    size_t pi = 0, li = 0;
    for (size_t i = 0; i < s_nlines; i++) {
        const line_t *l = &s_lines[i];
        /* advance the clock in <=1000 ms steps as the main loop would */
        while (s_now_ms + 1000 < l->ms) {
            s_now_ms += 1000;
            while (pi < np && presses[pi] <= s_now_ms) {
                printf("%9lu  ** button short press\n", s_now_ms);
                hr_ui_button(&r.ui, HR_UI_BUTTON_SHORT, s_now_ms);
                pi++;
            }
            while (li < nl && longs[li] <= s_now_ms) {
                printf("%9lu  ** button long press (night mode)\n", s_now_ms);
                hr_ui_button(&r.ui, HR_UI_BUTTON_LONG, s_now_ms);
                li++;
            }
            step(&r);
        }
        s_now_ms = l->ms;
        if (l->dir == '>') {
            rx_frames++;
            if (!s_quiet) {
                printf("%9lu  >  %s\n", s_now_ms, l->payload);
            }
            char wire[HR_MAX_FRAME + 2];
            snprintf(wire, sizeof(wire), "%s\r", l->payload);
            /* feed in two chunks, as CDC reads split frames arbitrarily */
            size_t len = strlen(wire), half = len / 2;
            hr_session_rx(&r.session, wire, half, s_now_ms);
            hr_session_rx(&r.session, wire + half, len - half, s_now_ms);
            /* a network-side "wifi connected" flag feeds the WIFIINFO reply */
            if (wifi_at != 0 && s_now_ms >= wifi_at) {
                hr_session_set_wifi(&r.session, 5, 81, "MyNetwork",
                                    "HR_3c8427e1b4f0");
                hr_session_set_cloud_auto(&r.session, true);
            }
        } else if (l->dir == '<') {
            char verb[HR_MAX_VERB];
            tx_verb_of(l->payload, '<', verb, sizeof(verb));
            count_verb(want, &nwant, verb);
        } else if (l->dir == '!' && !s_quiet) {
            printf("%9lu  !  %s\n", s_now_ms, l->payload);
        } else if (l->dir == '?' && !s_quiet) {
            printf("%9lu  ?  %s\n", s_now_ms, l->payload);
        } else if (l->dir == 'e') {
            /* "enc <len> <frame>": the 6.0.644170 encoded transport. Fed
             * verbatim and WITHOUT a CR - it has no terminator - split in
             * two like the plaintext frames, so the framer's length count
             * is what reassembles it. */
            const char *frame = strchr(l->payload, ' ');
            frame = frame ? strchr(frame + 1, ' ') : NULL;
            if (frame != NULL && frame[1] != '\0') {
                frame++;
                if (!s_quiet) {
                    printf("%9lu  e  %s\n", s_now_ms, l->payload);
                }
                size_t len = strlen(frame), half = len / 2;
                hr_session_rx(&r.session, frame, half, s_now_ms);
                hr_session_rx(&r.session, frame + half, len - half, s_now_ms);
            }
        }
        step(&r);
    }
    /* run the clock on so a trailing gap can time the link out */
    for (int k = 0; k < 60; k++) {
        s_now_ms += 1000;
        step(&r);
    }

    printf("\n=== summary ===\n");
    printf("dryer frames fed: %lu   parsed ok: %lu   rejected: %lu   "
           "unknown verbs: %lu\n",
           rx_frames, r.session.stream.frames_ok, r.session.stream.frames_bad,
           r.session.unknown_verbs);
    if (r.session.stream.enc_frames || r.session.stream.enc_bad) {
        printf("encoded frames (\")S\" transport): %lu whole, %lu bytes, "
               "%lu abandoned\n",
               r.session.stream.enc_frames, r.session.stream.enc_bytes,
               r.session.stream.enc_bad);
    }
    printf("frames the session sent: %lu   screen transitions: %u\n",
           s_tx_frames, r.transitions);
    printf("screens visited:");
    for (int s = 0; s <= HR_UI_SCREEN_RAW; s++) {
        if (r.screens_seen[s]) {
            printf(" %s(x%u)", screen_name((hr_ui_screen_t)s),
                   r.screens_seen[s]);
        }
    }
    printf("\n");
    int rc = 0;
    if (nwant > 0) {
        printf("adapter-side verbs, capture vs session:\n");
        for (size_t i = 0; i < nwant; i++) {
            unsigned have = 0;
            for (size_t j = 0; j < s_ntx_verbs; j++) {
                if (strcmp(s_tx_verbs[j].verb, want[i].verb) == 0) {
                    have = s_tx_verbs[j].count;
                }
            }
            /* The hello burst and STATE heartbeat come from main.c, not from
             * the session, so only the session's own replies are compared. */
            bool session_owned = strcmp(want[i].verb, "WIFIINFO") == 0;
            printf("  %-10s capture %4u   session %4u%s\n", want[i].verb,
                   want[i].count, have,
                   !session_owned ? "   (main-loop frame, not compared)"
                   : (have == want[i].count ? "" : "   <-- differs"));
            if (session_owned && have != want[i].count) {
                rc = 1;
            }
        }
    }
    printf("%s\n", rc == 0 ? "OK" : "DIFFERENCES");
    for (size_t i = 0; i < s_nlines; i++) {
        free(s_lines[i].payload);
    }
    return rc;
}
