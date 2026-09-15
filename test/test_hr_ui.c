#include "hr_ui_dim.h"
#include "hr_ui_model.h"
#include "test_util.h"

#include <string.h>

/* A model with the boot splash over, no Wi-Fi, no dryer. */
static void fresh(hr_ui_model_t *m, hr_ui_state_t *st)
{
    hr_ui_model_init(m);
    hr_ui_state_init(st);
    m->heap_free = 150000;
    snprintf(m->reset_reason, sizeof(m->reset_reason), "poweron");
}

static void dryer_running(hr_ui_model_t *m, hr_phase_t phase)
{
    m->link_up = true;
    m->usb_mounted = true;
    m->tel.valid = true;
    m->tel.type = phase == HR_PHASE_FREEZING ? 4 : 5;
    m->tel.phase = phase;
    m->tel.temp_f = -18;
    m->tel.phase_elapsed_s = 862;
    m->tel.batch_elapsed_s = 65040;
}

static void test_boot_then_provision(void)
{
    TEST_CASE("BOOT for 3 s, then PROVISION while the setup AP is open");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    m.wifi = HR_UI_WIFI_AP_SETUP;
    CHECK_INT(hr_ui_select(&st, &m, 0), HR_UI_SCREEN_BOOT);
    CHECK_INT(hr_ui_select(&st, &m, 2999), HR_UI_SCREEN_BOOT);
    CHECK_INT(hr_ui_select(&st, &m, 3000), HR_UI_SCREEN_PROVISION);

    m.wifi = HR_UI_WIFI_AP_CLOSED;
    CHECK_INT(hr_ui_select(&st, &m, 400000), HR_UI_SCREEN_AP_CLOSED);
}

static void test_provision_connecting_main(void)
{
    TEST_CASE("PROVISION -> CONNECTING -> IP shown 10 s -> main");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    m.wifi = HR_UI_WIFI_AP_SETUP;
    CHECK_INT(hr_ui_select(&st, &m, 3000), HR_UI_SCREEN_PROVISION);

    m.wifi = HR_UI_WIFI_CONNECTING;
    CHECK_INT(hr_ui_select(&st, &m, 4000), HR_UI_SCREEN_CONNECTING);
    /* Still connecting after 30 s: the dryer matters more than the router. */
    CHECK_INT(hr_ui_select(&st, &m, 4000 + 30000), HR_UI_SCREEN_NO_DRYER);

    m.wifi = HR_UI_WIFI_CONNECTED;
    snprintf(m.ip, sizeof(m.ip), "192.168.1.42");
    CHECK_INT(hr_ui_select(&st, &m, 40000), HR_UI_SCREEN_CONNECTING);
    CHECK_INT(hr_ui_select(&st, &m, 49999), HR_UI_SCREEN_CONNECTING);
    CHECK_INT(hr_ui_select(&st, &m, 50000), HR_UI_SCREEN_NO_DRYER);

    /* Once the dryer speaks, the main screen follows the phase. */
    m.link_up = true;
    m.tel.valid = true;
    m.tel.type = 1;
    m.tel.phase = HR_PHASE_IDLE;
    CHECK_INT(hr_ui_select(&st, &m, 51000), HR_UI_SCREEN_IDLE);
    m.tel.phase = HR_PHASE_COMPLETE;
    m.tel.type = 7;
    CHECK_INT(hr_ui_select(&st, &m, 52000), HR_UI_SCREEN_COMPLETE);
    CHECK_INT(st.complete_since_ms, 52000);
}

static void test_no_dryer_vs_run(void)
{
    TEST_CASE("NO_DRYER without frames; RUN wins over Wi-Fi setup");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    /* Network already up when the display starts: no 10 s IP banner. */
    m.wifi = HR_UI_WIFI_CONNECTED;
    m.usb_mounted = true;
    CHECK_INT(hr_ui_select(&st, &m, 100000), HR_UI_SCREEN_NO_DRYER);

    /* Link up but no STAT decoded yet is still "no dryer". */
    m.link_up = true;
    CHECK_INT(hr_ui_select(&st, &m, 101000), HR_UI_SCREEN_NO_DRYER);

    dryer_running(&m, HR_PHASE_FREEZING);
    CHECK_INT(hr_ui_select(&st, &m, 102000), HR_UI_SCREEN_RUN);

    /* Wi-Fi state does not block telemetry. */
    m.wifi = HR_UI_WIFI_AP_SETUP;
    CHECK_INT(hr_ui_select(&st, &m, 103000), HR_UI_SCREEN_RUN);
    m.wifi = HR_UI_WIFI_AP_CLOSED;
    CHECK_INT(hr_ui_select(&st, &m, 104000), HR_UI_SCREEN_RUN);

    /* Preparing / transition / drying all count as RUN. */
    m.tel.phase = HR_PHASE_PREPARING;
    CHECK_INT(hr_ui_select(&st, &m, 105000), HR_UI_SCREEN_RUN);
    m.tel.phase = HR_PHASE_TRANSITION;
    CHECK_INT(hr_ui_select(&st, &m, 105000), HR_UI_SCREEN_RUN);
    m.tel.phase = HR_PHASE_FINAL_DRY;
    CHECK_INT(hr_ui_select(&st, &m, 105000), HR_UI_SCREEN_RUN);
}

static void test_no_ip_then_regained(void)
{
    TEST_CASE("NO_IP does not block RUN; the address regained after it is shown");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    m.wifi = HR_UI_WIFI_CONNECTED;
    dryer_running(&m, HR_PHASE_FREEZING);
    CHECK_INT(hr_ui_select(&st, &m, 100000), HR_UI_SCREEN_RUN);

    /* Lease lost: still associated, no address. Telemetry keeps the screen. */
    m.wifi = HR_UI_WIFI_NO_IP;
    snprintf(m.ip, sizeof(m.ip), "0.0.0.0");
    CHECK_INT(hr_ui_select(&st, &m, 101000), HR_UI_SCREEN_RUN);

    /* Without a dryer it is just "waiting", not the connecting banner. */
    fresh(&m, &st);
    m.usb_mounted = true;
    m.wifi = HR_UI_WIFI_NO_IP;
    CHECK_INT(hr_ui_select(&st, &m, 102000), HR_UI_SCREEN_NO_DRYER);

    /* Address back (possibly a new one): show it for the usual 10 s. */
    m.wifi = HR_UI_WIFI_CONNECTED;
    snprintf(m.ip, sizeof(m.ip), "192.168.1.77");
    CHECK_INT(hr_ui_select(&st, &m, 103000), HR_UI_SCREEN_CONNECTING);
    CHECK_INT(hr_ui_select(&st, &m, 112999), HR_UI_SCREEN_CONNECTING);
    CHECK_INT(hr_ui_select(&st, &m, 113000), HR_UI_SCREEN_NO_DRYER);
}

static void test_alert_link_lost_and_dismiss(void)
{
    TEST_CASE("link lost mid-batch raises ALERT; short press dismisses");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    m.wifi = HR_UI_WIFI_CONNECTED;
    dryer_running(&m, HR_PHASE_DRYING);
    CHECK_INT(hr_ui_select(&st, &m, 200000), HR_UI_SCREEN_RUN);
    CHECK(!hr_ui_alert_active(&st));

    /* 45 s later the session times out. */
    m.link_up = false;
    CHECK_INT(hr_ui_select(&st, &m, 245000), HR_UI_SCREEN_ALERT);
    CHECK_INT(st.alert, HR_UI_ALERT_LINK_LOST);
    CHECK_STR(hr_ui_alert_title(st.alert), "LINK LOST");
    CHECK_INT(st.last_run_phase, HR_PHASE_DRYING);
    CHECK_INT(st.link_lost_ms, 245000);

    /* ALERT beats INFO: a short press dismisses rather than cycling. */
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 246000);
    CHECK_INT(st.dismissed, HR_UI_ALERT_LINK_LOST);
    CHECK_INT(hr_ui_select(&st, &m, 246000), HR_UI_SCREEN_NO_DRYER);
    CHECK(hr_ui_alert_active(&st)); /* status-bar "!" stays */

    /* Latched: still an alert an hour later, still dismissed. */
    CHECK_INT(hr_ui_select(&st, &m, 245000 + 3600000), HR_UI_SCREEN_NO_DRYER);
    CHECK_INT(st.alert, HR_UI_ALERT_LINK_LOST);

    /* Link back: alert clears, acknowledgement forgotten. */
    m.link_up = true;
    CHECK_INT(hr_ui_select(&st, &m, 245000 + 3600000 + 1000), HR_UI_SCREEN_RUN);
    CHECK_INT(st.alert, HR_UI_ALERT_NONE);
    CHECK_INT(st.dismissed, HR_UI_ALERT_NONE);
}

static void test_alert_priority_and_kinds(void)
{
    TEST_CASE("alert kinds and priority");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    m.wifi = HR_UI_WIFI_CONNECTED;
    m.link_up = true;
    m.tel.valid = true;
    m.tel.type = 1;
    m.tel.phase = HR_PHASE_IDLE;

    /* Idle dryer that goes silent is NOT an alert - just NO_DRYER. */
    CHECK_INT(hr_ui_select(&st, &m, 100000), HR_UI_SCREEN_IDLE);
    m.link_up = false;
    CHECK_INT(hr_ui_select(&st, &m, 150000), HR_UI_SCREEN_NO_DRYER);
    m.link_up = true;

    /* Counters are seeded on first sight, alarm only on growth. */
    m.frames_bad = 7;
    hr_ui_state_init(&st);
    CHECK_INT(hr_ui_select(&st, &m, 100000), HR_UI_SCREEN_IDLE);
    m.frames_bad = 8;
    CHECK_INT(hr_ui_select(&st, &m, 101000), HR_UI_SCREEN_ALERT);
    CHECK_INT(st.alert, HR_UI_ALERT_BAD_FRAMES);
    /* ...and expires a minute after the last growth. */
    CHECK_INT(hr_ui_select(&st, &m, 101000 + 59999), HR_UI_SCREEN_ALERT);
    CHECK_INT(hr_ui_select(&st, &m, 101000 + 60000), HR_UI_SCREEN_IDLE);
    CHECK_INT(st.alert, HR_UI_ALERT_NONE);

    /* Capture drops, same shape. */
    m.capture_dropped = 1;
    CHECK_INT(hr_ui_select(&st, &m, 200000), HR_UI_SCREEN_ALERT);
    CHECK_INT(st.alert, HR_UI_ALERT_CAPTURE_DROP);
    /* Diagnostics screen outranks it. */
    m.tel.type = 15;
    m.tel.phase = HR_PHASE_DIAGNOSTICS;
    CHECK_INT(hr_ui_select(&st, &m, 201000), HR_UI_SCREEN_ALERT);
    CHECK_INT(st.alert, HR_UI_ALERT_DIAGNOSTICS);
    /* Dismissed: the dryer's screen shows through, "!" stays. */
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 201500);
    CHECK_INT(hr_ui_select(&st, &m, 202000), HR_UI_SCREEN_RUN);
    CHECK(hr_ui_alert_active(&st));
    /* A different kind un-dismisses. */
    m.tel.type = 1;
    m.tel.phase = HR_PHASE_IDLE;
    m.heap_free = 10000;
    CHECK_INT(hr_ui_select(&st, &m, 203000), HR_UI_SCREEN_ALERT);
    CHECK_INT(st.alert, HR_UI_ALERT_LOW_HEAP);
    CHECK_INT(st.dismissed, HR_UI_ALERT_NONE);
    m.heap_free = 150000;

    /* Unknown STAT type, soft warning. */
    m.tel.type = 44;
    CHECK_INT(hr_ui_select(&st, &m, 300000), HR_UI_SCREEN_ALERT);
    CHECK_INT(st.alert, HR_UI_ALERT_UNKNOWN_SCREEN);
    m.tel.type = 1;

    /* Panic in the first minute after boot. */
    hr_ui_state_init(&st);
    snprintf(m.reset_reason, sizeof(m.reset_reason), "panic");
    CHECK_INT(hr_ui_select(&st, &m, 5000), HR_UI_SCREEN_ALERT);
    CHECK_INT(st.alert, HR_UI_ALERT_RESET);
    CHECK_INT(hr_ui_select(&st, &m, 60000), HR_UI_SCREEN_IDLE);
}

static void test_info_raw_cycle_and_timeout(void)
{
    TEST_CASE("short press cycles main -> INFO -> RAW -> main; 10 s timeout");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    m.wifi = HR_UI_WIFI_CONNECTED;
    dryer_running(&m, HR_PHASE_FREEZING);
    CHECK_INT(hr_ui_select(&st, &m, 100000), HR_UI_SCREEN_RUN);

    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 100100);
    CHECK_INT(hr_ui_select(&st, &m, 100100), HR_UI_SCREEN_INFO);
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 100200);
    CHECK_INT(hr_ui_select(&st, &m, 100200), HR_UI_SCREEN_RAW);
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 100300);
    CHECK_INT(hr_ui_select(&st, &m, 100300), HR_UI_SCREEN_RUN);

    /* Auto-return after 10 s. */
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 101000);
    CHECK_INT(hr_ui_select(&st, &m, 110999), HR_UI_SCREEN_INFO);
    CHECK_INT(hr_ui_select(&st, &m, 111000), HR_UI_SCREEN_RUN);
    /* And the next press starts again at INFO, not RAW. */
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 112000);
    CHECK_INT(hr_ui_select(&st, &m, 112000), HR_UI_SCREEN_INFO);

    /* INFO also works with no dryer at all. */
    hr_ui_state_init(&st);
    m.link_up = false;
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 200000);
    CHECK_INT(hr_ui_select(&st, &m, 200000), HR_UI_SCREEN_INFO);
}

static void test_button_touches_only_ui_state(void)
{
    TEST_CASE("button changes screens and backlight, nothing else");
    hr_ui_model_t m, m_before;
    hr_ui_state_t st, before;
    fresh(&m, &st);
    m.wifi = HR_UI_WIFI_CONNECTED;
    dryer_running(&m, HR_PHASE_DRYING);
    hr_ui_select(&st, &m, 100000);
    m_before = m;

    /* LONG: only backlight_off and last_activity_ms move. */
    before = st;
    hr_ui_button(&st, HR_UI_BUTTON_LONG, 100500);
    CHECK(st.backlight_off);
    CHECK_INT(st.last_activity_ms, 100500);
    before.backlight_off = st.backlight_off;
    before.last_activity_ms = st.last_activity_ms;
    CHECK(memcmp(&before, &st, sizeof(st)) == 0);
    hr_ui_button(&st, HR_UI_BUTTON_LONG, 101000);
    CHECK(!st.backlight_off);
    /* Night mode does not change the screen decision. */
    CHECK_INT(hr_ui_select(&st, &m, 101000), HR_UI_SCREEN_RUN);

    /* SHORT: only the override fields and last_activity_ms move. */
    before = st;
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 102000);
    before.override = st.override;
    before.override_until_ms = st.override_until_ms;
    before.last_activity_ms = st.last_activity_ms;
    CHECK(memcmp(&before, &st, sizeof(st)) == 0);

    /* The model is never written by the UI. */
    CHECK(memcmp(&m_before, &m, sizeof(m)) == 0);
}

static void test_led_table(void)
{
    TEST_CASE("LED mirrors the screen, capped at 30 %");
    hr_ui_model_t m;
    hr_ui_state_t st;
    fresh(&m, &st);
    hr_ui_led_t l;

    l = hr_ui_led_for(HR_UI_SCREEN_BOOT, &m, 0);
    CHECK_INT(l.pattern, HR_UI_LED_SOLID);
    CHECK(l.r == 255 && l.g == 255 && l.b == 255);
    CHECK_INT(l.brightness_pct, 20);

    l = hr_ui_led_for(HR_UI_SCREEN_ALERT, &m, 0);
    CHECK_INT(l.pattern, HR_UI_LED_BLINK);
    CHECK_INT(l.period_ms, 1000);
    CHECK(l.r == 255 && l.g == 0 && l.b == 0);
    CHECK(l.brightness_pct <= HR_UI_LED_MAX_PCT);
    CHECK_INT(hr_ui_led_level(&l, 0), 30);
    CHECK_INT(hr_ui_led_level(&l, 499), 30);
    CHECK_INT(hr_ui_led_level(&l, 500), 0);
    CHECK_INT(hr_ui_led_level(&l, 1000), 30);

    l = hr_ui_led_for(HR_UI_SCREEN_PROVISION, &m, 0);
    CHECK_INT(l.pattern, HR_UI_LED_BREATHE);
    CHECK(hr_ui_led_level(&l, 0) < hr_ui_led_level(&l, l.period_ms / 2));
    CHECK_INT(hr_ui_led_level(&l, l.period_ms / 2), l.brightness_pct);
    CHECK(hr_ui_led_level(&l, 0) > 0); /* breathes, never fully dark */

    l = hr_ui_led_for(HR_UI_SCREEN_CONNECTING, &m, 0);
    CHECK_INT(l.pattern, HR_UI_LED_BLINK);
    CHECK_INT(l.period_ms, 500);

    l = hr_ui_led_for(HR_UI_SCREEN_IDLE, &m, 0);
    CHECK_INT(l.brightness_pct, 10);

    dryer_running(&m, HR_PHASE_DRYING);
    l = hr_ui_led_for(HR_UI_SCREEN_RUN, &m, 0);
    CHECK(l.r == 0xFF && l.g == 0x90 && l.b == 0x20);
    CHECK_INT(l.brightness_pct, 15);
    /* INFO over a running dryer keeps the phase colour. */
    l = hr_ui_led_for(HR_UI_SCREEN_INFO, &m, 0);
    CHECK(l.r == 0xFF && l.g == 0x90 && l.b == 0x20);

    l = hr_ui_led_for(HR_UI_SCREEN_COMPLETE, &m, 0);
    CHECK_INT(l.pattern, HR_UI_LED_BREATHE);
    CHECK(l.g > l.r && l.g > l.b);

    /* Every entry respects the cap. */
    for (int s = HR_UI_SCREEN_BOOT; s <= HR_UI_SCREEN_RAW; s++) {
        l = hr_ui_led_for((hr_ui_screen_t)s, &m, 0);
        CHECK(l.brightness_pct <= HR_UI_LED_MAX_PCT);
    }
}

static void test_format_helpers(void)
{
    TEST_CASE("format helpers");
    char b[32];

    hr_ui_fmt_hm(65040, b, sizeof(b));
    CHECK_STR(b, "18h04m");
    hr_ui_fmt_hm(14 * 60 + 22, b, sizeof(b));
    CHECK_STR(b, "14m");
    hr_ui_fmt_hm(0, b, sizeof(b));
    CHECK_STR(b, "0m");
    hr_ui_fmt_hm(94440, b, sizeof(b));
    CHECK_STR(b, "26h14m");

    hr_ui_fmt_mmss(899, b, sizeof(b));
    CHECK_STR(b, "14:59");
    hr_ui_fmt_mmss(5, b, sizeof(b));
    CHECK_STR(b, "00:05");
    hr_ui_fmt_mmss(100000, b, sizeof(b));
    CHECK_STR(b, "99:59");

    hr_ui_fmt_hhmm(4 * 3600 + 32 * 60 + 7, b, sizeof(b));
    CHECK_STR(b, "04:32");
    hr_ui_fmt_hhmm(0, b, sizeof(b));
    CHECK_STR(b, "00:00");

    hr_ui_fmt_uptime(3 * 86400 + 4 * 3600 + 12 * 60, b, sizeof(b));
    CHECK_STR(b, "3d 04:12");
    hr_ui_fmt_uptime(4 * 3600 + 12 * 60, b, sizeof(b));
    CHECK_STR(b, "04:12");

    hr_ui_fmt_temp(-18, false, b, sizeof(b));
    CHECK_STR(b, "-18" HR_UI_DEG "F");
    hr_ui_fmt_temp(72, false, b, sizeof(b));
    CHECK_STR(b, "72" HR_UI_DEG "F");
    hr_ui_fmt_temp(-18, true, b, sizeof(b));
    CHECK_STR(b, "-28" HR_UI_DEG "C");
    hr_ui_fmt_temp(32, true, b, sizeof(b));
    CHECK_STR(b, "0" HR_UI_DEG "C");
    hr_ui_fmt_temp(212, true, b, sizeof(b));
    CHECK_STR(b, "100" HR_UI_DEG "C");
    hr_ui_fmt_temp(124, true, b, sizeof(b));
    CHECK_STR(b, "51" HR_UI_DEG "C");

    hr_ui_fmt_vac(435, true, b, sizeof(b));
    CHECK_STR(b, "435 mT");
    hr_ui_fmt_vac(0, false, b, sizeof(b));
    CHECK_STR(b, "atm");
    hr_ui_fmt_vac(148066, false, b, sizeof(b));
    CHECK_STR(b, "atm");

    hr_ui_fmt_bytes_mb(1258291, 12478464, b, sizeof(b));
    CHECK_STR(b, "1.2/11.9MB");
    hr_ui_fmt_bytes_mb(0, 3 * 1024 * 1024, b, sizeof(b));
    CHECK_STR(b, "0.0/3.0MB");

    CHECK_STR(hr_ui_phase_label_short(HR_PHASE_FREEZING), "FREEZING");
    CHECK_STR(hr_ui_phase_label_short(HR_PHASE_IDLE), "READY");
    CHECK_STR(hr_ui_phase_label_short(HR_PHASE_FINAL_DRY), "FINAL DRY");
    CHECK_STR(hr_ui_phase_label_short(HR_PHASE_TRANSITION), "LOAD TRAYS");
    CHECK_STR(hr_ui_phase_label_short(HR_PHASE_UNKNOWN), "---");

    CHECK_INT(hr_ui_phase_color(HR_PHASE_FREEZING), HR_RGB565(0x30, 0x60, 0xFF));
    CHECK_INT(hr_ui_phase_color(HR_PHASE_DRYING), HR_RGB565(0xFF, 0x90, 0x20));
    CHECK_INT(HR_RGB565(0xFF, 0, 0), 0xF800);
    CHECK_INT(HR_RGB565(0, 0xFF, 0), 0x07E0);
    CHECK_INT(HR_RGB565(0, 0, 0xFF), 0x001F);

    /* Labels fit the 16 px title line: 13 columns at 12 px per glyph. */
    for (int p = HR_PHASE_UNKNOWN; p <= HR_PHASE_COMPLETE; p++) {
        CHECK(strlen(hr_ui_phase_label_short((hr_phase_t)p)) <= 13);
    }
}

/* ------------------------------------------------------------------ */
/* Backlight inactivity state machine                                  */
/* ------------------------------------------------------------------ */

/* select + dim update in one go, the way the ui task does it. */
static hr_ui_dim_level_t tick(hr_ui_dim_state_t *d, const hr_ui_dim_cfg_t *c,
                              hr_ui_state_t *st, const hr_ui_model_t *m,
                              unsigned long now)
{
    hr_ui_select(st, m, now);
    return hr_ui_dim_update(d, c, st, m, now);
}

/* Running dryer, Wi-Fi up, splash over, dim state seeded at t0. */
static void dim_running(hr_ui_model_t *m, hr_ui_state_t *st,
                        hr_ui_dim_state_t *d, hr_ui_dim_cfg_t *c,
                        unsigned long t0)
{
    fresh(m, st);
    hr_ui_dim_cfg_default(c);
    m->wifi = HR_UI_WIFI_CONNECTED;
    m->mqtt_configured = true;
    m->mqtt_connected = true;
    dryer_running(m, HR_PHASE_FREEZING);
    hr_ui_dim_init(d, t0);
    CHECK_INT(tick(d, c, st, m, t0), HR_UI_DIM_ACTIVE);
    CHECK_INT(st->screen, HR_UI_SCREEN_RUN);
}

static void test_dim_defaults_and_timeouts(void)
{
    TEST_CASE("dim: defaults; ACTIVE -> DIM at 30 s -> OFF at 300 s");
    hr_ui_model_t m;
    hr_ui_state_t st;
    hr_ui_dim_state_t d;
    hr_ui_dim_cfg_t c;
    dim_running(&m, &st, &d, &c, 100000);

    CHECK(c.enabled);
    CHECK_INT(c.dim_after_ms, 30000);
    CHECK_INT(c.off_after_ms, 300000);
    CHECK_INT(c.dim_pct, 10);
    CHECK(c.alert_keep_on);
    CHECK_INT(hr_ui_dim_backlight_pct(&c, HR_UI_DIM_ACTIVE), 100);
    CHECK_INT(hr_ui_dim_backlight_pct(&c, HR_UI_DIM_DIM), 10);
    CHECK_INT(hr_ui_dim_backlight_pct(&c, HR_UI_DIM_OFF), 0);
    CHECK_STR(hr_ui_dim_level_name(HR_UI_DIM_DIM), "DIM");

    CHECK_INT(tick(&d, &c, &st, &m, 129999), HR_UI_DIM_ACTIVE);
    CHECK_INT(tick(&d, &c, &st, &m, 130000), HR_UI_DIM_DIM);
    CHECK_INT(tick(&d, &c, &st, &m, 399999), HR_UI_DIM_DIM);
    CHECK_INT(tick(&d, &c, &st, &m, 400000), HR_UI_DIM_OFF);
    CHECK_INT(tick(&d, &c, &st, &m, 4000000), HR_UI_DIM_OFF);

    /* off_after 0: dims, never switches off. */
    c.off_after_ms = 0;
    CHECK_INT(tick(&d, &c, &st, &m, 4000001), HR_UI_DIM_DIM);
    /* off_after <= dim_after: straight to OFF. */
    c.off_after_ms = 30000;
    hr_ui_dim_wake(&d, 5000000);
    CHECK_INT(tick(&d, &c, &st, &m, 5029999), HR_UI_DIM_ACTIVE);
    CHECK_INT(tick(&d, &c, &st, &m, 5030000), HR_UI_DIM_OFF);
    /* Master switch off: always ACTIVE. */
    c.enabled = false;
    CHECK_INT(tick(&d, &c, &st, &m, 9000000), HR_UI_DIM_ACTIVE);
    c.enabled = true;
    c.dim_after_ms = 0; /* "never dim" spelled as 0 */
    CHECK_INT(tick(&d, &c, &st, &m, 9000001), HR_UI_DIM_ACTIVE);
}

static void test_dim_telemetry_does_not_wake(void)
{
    TEST_CASE("dim: temperature/vacuum/elapsed ticks do not wake, phase does");
    hr_ui_model_t m;
    hr_ui_state_t st;
    hr_ui_dim_state_t d;
    hr_ui_dim_cfg_t c;
    dim_running(&m, &st, &d, &c, 100000);

    /* A STAT every few seconds for ten minutes, numbers moving. */
    for (unsigned long t = 101000; t <= 700000; t += 5000) {
        m.tel.temp_f -= 1;
        m.tel.vacuum_um += 3;
        m.tel.phase_elapsed_s += 5;
        m.tel.batch_elapsed_s += 5;
        m.tel.rx_ms = t;
        m.rssi_dbm = (int)(-50 - (t / 5000) % 7);
        m.heap_free = 150000 + (unsigned)(t % 1000);
        hr_ui_dim_update(&d, &c, &st, &m, t); /* also fine without select */
    }
    CHECK_INT(tick(&d, &c, &st, &m, 700001), HR_UI_DIM_OFF);
    CHECK_INT(d.last_event_ms, 100000);

    /* Phase change wakes: freezing -> drying. */
    m.tel.phase = HR_PHASE_DRYING;
    m.tel.type = 5;
    CHECK_INT(tick(&d, &c, &st, &m, 700002), HR_UI_DIM_ACTIVE);
    CHECK_INT(d.last_event_ms, 700002);
    CHECK_INT(tick(&d, &c, &st, &m, 730002), HR_UI_DIM_DIM);

    /* Batch complete wakes (screen RUN -> COMPLETE, phase too). */
    m.tel.phase = HR_PHASE_COMPLETE;
    m.tel.type = 7;
    CHECK_INT(tick(&d, &c, &st, &m, 731000), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_COMPLETE);
    CHECK_INT(tick(&d, &c, &st, &m, 731000 + 300000), HR_UI_DIM_OFF);

    /* Wi-Fi state change wakes; so does MQTT dropping; so does the USB
     * link going away (here: idle dryer, no alert). */
    m.wifi = HR_UI_WIFI_CONNECTING;
    CHECK_INT(tick(&d, &c, &st, &m, 1040000), HR_UI_DIM_ACTIVE);
    CHECK_INT(tick(&d, &c, &st, &m, 1340000), HR_UI_DIM_OFF);
    m.mqtt_connected = false;
    CHECK_INT(tick(&d, &c, &st, &m, 1340001), HR_UI_DIM_ACTIVE);
    CHECK_INT(tick(&d, &c, &st, &m, 1640001), HR_UI_DIM_OFF);
    m.tel.phase = HR_PHASE_IDLE;
    m.tel.type = 1;
    tick(&d, &c, &st, &m, 1640002);
    tick(&d, &c, &st, &m, 1940002 + HR_UI_RUN_RECENT_MS);
    CHECK_INT(d.level, HR_UI_DIM_OFF);
    m.link_up = false;
    CHECK_INT(tick(&d, &c, &st, &m, 1940003 + HR_UI_RUN_RECENT_MS),
              HR_UI_DIM_ACTIVE);
    CHECK_INT(st.alert, HR_UI_ALERT_NONE); /* idle dryer silent: no alert */
    m.link_up = true;
    CHECK_INT(tick(&d, &c, &st, &m, 1940004 + HR_UI_RUN_RECENT_MS),
              HR_UI_DIM_ACTIVE);
}

static void test_dim_alert_floor(void)
{
    TEST_CASE("dim: alert wakes and keeps ACTIVE; dismissed -> DIM, never OFF");
    hr_ui_model_t m;
    hr_ui_state_t st;
    hr_ui_dim_state_t d;
    hr_ui_dim_cfg_t c;
    dim_running(&m, &st, &d, &c, 100000);
    CHECK_INT(tick(&d, &c, &st, &m, 400000), HR_UI_DIM_OFF);

    /* Link lost mid-run: ALERT screen, screen wakes and stays awake. */
    m.link_up = false;
    CHECK_INT(tick(&d, &c, &st, &m, 400001), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_ALERT);
    CHECK_INT(tick(&d, &c, &st, &m, 400001 + 3600000), HR_UI_DIM_ACTIVE);

    /* Acknowledged: may dim, but not go off while the alert is latched. */
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 4000002);
    CHECK_INT(tick(&d, &c, &st, &m, 4000002), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_NO_DRYER);
    /* (the button press itself is a wake in the real task; emulate) */
    hr_ui_dim_wake(&d, 4000002);
    CHECK_INT(tick(&d, &c, &st, &m, 4030002), HR_UI_DIM_DIM);
    CHECK_INT(tick(&d, &c, &st, &m, 4030002 + 3600000), HR_UI_DIM_DIM);

    /* Link back: alert clears (an event), then normal timing resumes. */
    m.link_up = true;
    CHECK_INT(tick(&d, &c, &st, &m, 8000000), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.alert, HR_UI_ALERT_NONE);
    CHECK_INT(tick(&d, &c, &st, &m, 8300000), HR_UI_DIM_OFF);

    /* alert_keep_on = false: a showing alert still floors at DIM. */
    c.alert_keep_on = false;
    m.link_up = false;
    CHECK_INT(tick(&d, &c, &st, &m, 8300001), HR_UI_DIM_ACTIVE); /* woke */
    CHECK_INT(st.screen, HR_UI_SCREEN_ALERT);
    CHECK_INT(tick(&d, &c, &st, &m, 8330001), HR_UI_DIM_DIM);
    CHECK_INT(tick(&d, &c, &st, &m, 8330001 + 3600000), HR_UI_DIM_DIM);
}

static void test_dim_provisioning_never_off(void)
{
    TEST_CASE("dim: setup AP open -> DIM at most, never OFF");
    hr_ui_model_t m;
    hr_ui_state_t st;
    hr_ui_dim_state_t d;
    hr_ui_dim_cfg_t c;
    fresh(&m, &st);
    hr_ui_dim_cfg_default(&c);
    m.wifi = HR_UI_WIFI_AP_SETUP;
    m.ap_remaining_s = 300;
    hr_ui_dim_init(&d, 0);
    CHECK_INT(tick(&d, &c, &st, &m, 0), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_BOOT);
    /* Splash over -> PROVISION is a screen change: still ACTIVE at 3 s. */
    CHECK_INT(tick(&d, &c, &st, &m, 3000), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_PROVISION);
    CHECK_INT(tick(&d, &c, &st, &m, 33000), HR_UI_DIM_DIM);
    CHECK_INT(tick(&d, &c, &st, &m, 3000 + 300000), HR_UI_DIM_DIM);
    CHECK_INT(tick(&d, &c, &st, &m, 3000 + 3600000), HR_UI_DIM_DIM);

    /* Window closed: an event, then the normal path to OFF is open. */
    m.wifi = HR_UI_WIFI_AP_CLOSED;
    m.ap_remaining_s = 0;
    CHECK_INT(tick(&d, &c, &st, &m, 4000000), HR_UI_DIM_ACTIVE);
    CHECK_INT(tick(&d, &c, &st, &m, 4300000), HR_UI_DIM_OFF);

    /* Someone joins and reopens setup: awake again, floored at DIM. */
    m.wifi = HR_UI_WIFI_AP_SETUP;
    CHECK_INT(tick(&d, &c, &st, &m, 4300001), HR_UI_DIM_ACTIVE);
    CHECK_INT(tick(&d, &c, &st, &m, 4300001 + 600000), HR_UI_DIM_DIM);
}

static void test_dim_button_when_off(void)
{
    TEST_CASE("dim: press on a dark screen only wakes; otherwise wakes AND acts");
    hr_ui_model_t m;
    hr_ui_state_t st;
    hr_ui_dim_state_t d;
    hr_ui_dim_cfg_t c;
    dim_running(&m, &st, &d, &c, 100000);

    /* ACTIVE: forwarded (INFO comes up) and the timer restarts. */
    CHECK_INT(tick(&d, &c, &st, &m, 120000), HR_UI_DIM_ACTIVE);
    CHECK(hr_ui_dim_button(&d, 120000));
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 120000);
    CHECK_INT(tick(&d, &c, &st, &m, 120000), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_INFO);
    CHECK_INT(tick(&d, &c, &st, &m, 149999), HR_UI_DIM_ACTIVE);
    /* INFO timing out and returning to RUN is not an event. */
    CHECK_INT(tick(&d, &c, &st, &m, 150000), HR_UI_DIM_DIM);
    CHECK_INT(st.screen, HR_UI_SCREEN_RUN);

    /* DIM: forwarded too - the screen is readable. */
    CHECK(hr_ui_dim_button(&d, 150001));
    CHECK_INT(d.level, HR_UI_DIM_ACTIVE);
    CHECK_INT(tick(&d, &c, &st, &m, 150001), HR_UI_DIM_ACTIVE);

    /* OFF: the first press only wakes, nothing is forwarded. */
    CHECK_INT(tick(&d, &c, &st, &m, 450001), HR_UI_DIM_OFF);
    hr_ui_state_t before = st;
    CHECK(!hr_ui_dim_button(&d, 450002));
    CHECK_INT(d.level, HR_UI_DIM_ACTIVE);
    CHECK(memcmp(&before, &st, sizeof(st)) == 0); /* caller did not act */
    CHECK_INT(tick(&d, &c, &st, &m, 450002), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_RUN);
    /* The second press is a normal one. */
    CHECK(hr_ui_dim_button(&d, 451000));
    hr_ui_button(&st, HR_UI_BUTTON_SHORT, 451000);
    CHECK_INT(tick(&d, &c, &st, &m, 451000), HR_UI_DIM_ACTIVE);
    CHECK_INT(st.screen, HR_UI_SCREEN_INFO);
}

int main(void)
{
    test_boot_then_provision();
    test_provision_connecting_main();
    test_no_dryer_vs_run();
    test_no_ip_then_regained();
    test_alert_link_lost_and_dismiss();
    test_alert_priority_and_kinds();
    test_info_raw_cycle_and_timeout();
    test_button_touches_only_ui_state();
    test_led_table();
    test_format_helpers();
    test_dim_defaults_and_timeouts();
    test_dim_telemetry_does_not_wake();
    test_dim_alert_floor();
    test_dim_provisioning_never_off();
    test_dim_button_when_off();
    return TEST_REPORT();
}
