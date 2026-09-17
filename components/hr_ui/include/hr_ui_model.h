/*
 * hr_ui_model - which screen to show, what colour the LED is, and how the
 * numbers are spelled. The logic behind the T-Dongle-S3 display, kept apart
 * from the drawing.
 *
 * Pure C11, no ESP-IDF, no drawing: the renderer in main/hr_display.c hands
 * this module a snapshot of the adapter (hr_ui_model_t), gets back a screen
 * id and an LED instruction, and draws. Everything here is exercised by the
 * host tests in test/test_hr_ui.c, the same way hr_protocol is.
 *
 * This module is read-only with respect to the dryer: it has no transmit
 * callback, no session pointer, and no verb table. The button can move
 * between screens and switch the backlight - nothing else, by construction.
 */
#ifndef HR_UI_MODEL_H
#define HR_UI_MODEL_H

#include "hr_telemetry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Snapshot of the adapter, as the display sees it                     */
/* ------------------------------------------------------------------ */

typedef enum {
    HR_UI_WIFI_NONE = 0,   /* driver not up yet */
    HR_UI_WIFI_AP_SETUP,   /* setup hotspot open, waiting for credentials */
    HR_UI_WIFI_AP_CLOSED,  /* 5-minute window over, nothing joined */
    HR_UI_WIFI_CONNECTING, /* trying the stored network */
    HR_UI_WIFI_CONNECTED,  /* station up with an IP */
    /* Associated but no IPv4 address for a while (DHCP lease lost or never
     * granted); the adapter is restarting DHCP / rejoining by itself.
     * Not connected for every purpose the screen cares about. */
    HR_UI_WIFI_NO_IP,
    /* Associated with an address, but the router has stopped answering the
     * station (weak signal: it still hears the router, the router no longer
     * hears it). The adapter is rejoining by itself. Not connected either. */
    HR_UI_WIFI_UNREACHABLE,
} hr_ui_wifi_t;

/* Longest slice of the raw STAT body the RAW screen can show: 5 x 26. */
#define HR_UI_RAW_MAX 136

typedef struct {
    bool valid;              /* at least one STAT decoded this boot */
    int  type;               /* STAT type discriminator */
    hr_phase_t phase;        /* as decided by the phase tracker */
    long temp_f;
    bool pressure_valid;     /* false = placeholder / atmosphere */
    long vacuum_um;          /* microns when pressure_valid */
    long batch_elapsed_s;
    long phase_elapsed_s;
    long phase_pct;          /* 0..100, -1 when the phase has none */
    long prep_remaining_s;   /* type 17 countdown */
    long freeze_eta_s;       /* -1 when unknown */
    char mode[16];
    char last_stat[HR_UI_RAW_MAX];
    unsigned long rx_ms;     /* uptime when the last STAT arrived */
} hr_ui_telemetry_t;

typedef struct {
    unsigned long uptime_ms; /* now, milliseconds since boot */
    unsigned long boot_ms;   /* uptime at which the UI came up (BOOT splash) */

    hr_ui_wifi_t wifi;
    char ssid[33];
    char ip[16];
    char ap_ssid[33];
    int  rssi_dbm;           /* 0 when not associated */
    long ap_remaining_s;     /* seconds left in the setup window, 0 = closed */

    bool mqtt_configured;
    bool mqtt_connected;

    bool usb_mounted;
    unsigned usb_mounts;
    unsigned long usb_rx_bytes;
    bool link_up;
    unsigned long frames_bad;
    unsigned long capture_dropped;
    size_t capture_used;
    size_t capture_cap;

    char machine_name[32];
    char fw_version[24];     /* dryer firmware, from UID */
    char reset_reason[12];
    unsigned heap_free;

    /*
     * Present temperatures in Celsius. The owner's setting (hr_units.h,
     * Settings > Temperature unit); tel.temp_f itself is always the dryer's
     * degrees F and converts only when spelled (hr_ui_fmt_temp).
     */
    bool metric;

    hr_ui_telemetry_t tel;
} hr_ui_model_t;

/* Zero a model to safe defaults (no Wi-Fi, no dryer, unknown phase). */
void hr_ui_model_init(hr_ui_model_t *m);

/* ------------------------------------------------------------------ */
/* Screens                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    HR_UI_SCREEN_BOOT = 0,
    HR_UI_SCREEN_PROVISION,
    HR_UI_SCREEN_AP_CLOSED,
    HR_UI_SCREEN_CONNECTING,
    HR_UI_SCREEN_NO_DRYER,
    HR_UI_SCREEN_IDLE,
    HR_UI_SCREEN_RUN,
    HR_UI_SCREEN_COMPLETE,
    HR_UI_SCREEN_ALERT,
    HR_UI_SCREEN_INFO,
    HR_UI_SCREEN_RAW,
} hr_ui_screen_t;

/* Why the ALERT screen is up, most serious first. */
typedef enum {
    HR_UI_ALERT_NONE = 0,
    HR_UI_ALERT_LINK_LOST,   /* dryer went silent while a batch was running */
    HR_UI_ALERT_DIAGNOSTICS, /* STAT type 15: the dryer is about to drop USB */
    HR_UI_ALERT_LOW_HEAP,    /* under 20 KB free */
    HR_UI_ALERT_RESET,       /* panic / brownout, first 60 s after boot */
    HR_UI_ALERT_BAD_FRAMES,  /* parser rejects grew in the last minute */
    HR_UI_ALERT_CAPTURE_DROP,/* capture queue overflowed in the last minute */
    HR_UI_ALERT_UNKNOWN_SCREEN, /* STAT type never seen before (44) */
} hr_ui_alert_t;

typedef enum {
    HR_UI_BUTTON_SHORT = 0,  /* released before 1.5 s */
    HR_UI_BUTTON_LONG,       /* held 1.5 s */
} hr_ui_button_event_t;

/* Timing constants of the state machine, exposed so tests can use them. */
#define HR_UI_BOOT_MS          3000UL   /* splash */
#define HR_UI_GOT_IP_SHOW_MS   10000UL  /* show the new IP this long */
#define HR_UI_CONNECTING_MAX_MS 30000UL /* then stop waiting for Wi-Fi */
#define HR_UI_OVERRIDE_MS      10000UL  /* INFO / RAW auto-return */
#define HR_UI_RUN_RECENT_MS    300000UL /* link loss counts as an alert */
#define HR_UI_GROWTH_WINDOW_MS 60000UL  /* "grew in the last minute" */
#define HR_UI_RESET_ALERT_MS   60000UL
#define HR_UI_LOW_HEAP_BYTES   20480U
#define HR_UI_COMPLETE_FLASH_MS 3000UL  /* inverted COMPLETE banner */

/*
 * Persistent UI state. Owned by the renderer, mutated only by the functions
 * below. Zero-initialise once.
 */
typedef struct {
    hr_ui_screen_t screen;      /* last result of hr_ui_select() */
    hr_ui_alert_t  alert;       /* currently active alert, or NONE */
    hr_ui_alert_t  dismissed;   /* alert the user has acknowledged */
    unsigned long  alert_since_ms;

    /* INFO / RAW shown by button, until this uptime. 0 = none. */
    hr_ui_screen_t override;
    unsigned long  override_until_ms;

    bool backlight_off;         /* night mode, toggled by a long press */
    unsigned long last_activity_ms;

    /* Bookkeeping for the alert conditions. */
    hr_ui_wifi_t  last_wifi;
    unsigned long connecting_since_ms;
    unsigned long connected_since_ms;
    unsigned long last_run_ms;      /* last time telemetry showed a run */
    unsigned long link_lost_ms;     /* when link_up went false, 0 = up */
    hr_phase_t    last_run_phase;
    long          last_run_elapsed_s;
    unsigned long last_frames_bad;
    unsigned long frames_bad_grew_ms;
    unsigned long last_capture_dropped;
    unsigned long capture_grew_ms;
    bool          counters_seeded;
    hr_phase_t    last_phase;
    unsigned long complete_since_ms; /* phase turned COMPLETE at this time */
} hr_ui_state_t;

void hr_ui_state_init(hr_ui_state_t *st);

/*
 * Decide the screen for this snapshot. Call on every tick and every model
 * change; it is cheap and idempotent for the same inputs.
 *
 * Priority (docs/04 section 5): ALERT > INFO/RAW (button, timed) >
 * PROVISION / AP_CLOSED > NO_DRYER > main (IDLE / RUN / COMPLETE). Wi-Fi
 * state never hides telemetry: a dryer that is talking wins over a setup
 * hotspot that nobody has joined.
 */
hr_ui_screen_t hr_ui_select(hr_ui_state_t *st, const hr_ui_model_t *m,
                            unsigned long now_ms);

/*
 * Button gesture. SHORT dismisses an alert if one is showing, otherwise
 * cycles main -> INFO -> RAW -> main. LONG toggles night mode (backlight
 * and LED off). That is the complete list; the button has no other effect.
 */
void hr_ui_button(hr_ui_state_t *st, hr_ui_button_event_t ev,
                  unsigned long now_ms);

/* True while an alert is active, even if dismissed (status-bar "!"). */
bool hr_ui_alert_active(const hr_ui_state_t *st);

/* Short title for the ALERT screen, e.g. "LINK LOST". Never NULL. */
const char *hr_ui_alert_title(hr_ui_alert_t a);

/* ------------------------------------------------------------------ */
/* LED                                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    HR_UI_LED_OFF = 0,
    HR_UI_LED_SOLID,
    HR_UI_LED_BREATHE, /* triangle wave, period_ms */
    HR_UI_LED_BLINK,   /* 50 % square wave, period_ms */
} hr_ui_led_pattern_t;

typedef struct {
    uint8_t r, g, b;              /* colour at full level */
    uint8_t brightness_pct;       /* peak, never above HR_UI_LED_MAX_PCT */
    hr_ui_led_pattern_t pattern;
    unsigned period_ms;           /* for BREATHE / BLINK */
} hr_ui_led_t;

/* Hard ceiling: the LED sits in a closed case next to the antenna. */
#define HR_UI_LED_MAX_PCT 30

hr_ui_led_t hr_ui_led_for(hr_ui_screen_t screen, const hr_ui_model_t *m,
                          unsigned long now_ms);

/*
 * Instantaneous brightness (0..brightness_pct) of a pattern at `now_ms`, so
 * the hardware layer only has to apply a number.
 */
uint8_t hr_ui_led_level(const hr_ui_led_t *led, unsigned long now_ms);

/* ------------------------------------------------------------------ */
/* Text formatting                                                     */
/* ------------------------------------------------------------------ */

/* The 6x8 font puts a degree sign at code 127. */
#define HR_UI_DEG "\x7f"

/* "18h04m", or "14m" under an hour. */
void hr_ui_fmt_hm(unsigned long secs, char *out, size_t cap);
/* "14:59" minutes:seconds, for countdowns. Caps at 99:59. */
void hr_ui_fmt_mmss(unsigned long secs, char *out, size_t cap);
/* "04:32" hours:minutes for the status bar. Wraps at 100 h. */
void hr_ui_fmt_hhmm(unsigned long secs, char *out, size_t cap);
/* "3d 04:12" or "04:12" for the INFO screen. */
void hr_ui_fmt_uptime(unsigned long secs, char *out, size_t cap);
/* "-18°F" / "-28°C" (integer Celsius). */
void hr_ui_fmt_temp(long f, bool metric, char *out, size_t cap);
/* "atm" when the reading is not a vacuum, else "435 mT". */
void hr_ui_fmt_vac(long um, bool valid, char *out, size_t cap);
/* "1.2/11.9MB" for the capture partition. */
void hr_ui_fmt_bytes_mb(size_t used, size_t cap_bytes, char *out, size_t cap);

/* Upper-case label that fits the 16 px title line: "FREEZING", "READY". */
const char *hr_ui_phase_label_short(hr_phase_t p);

/* Phase colour, RGB565 (docs/04 section 4.6). */
uint16_t hr_ui_phase_color(hr_phase_t p);
/* Same colour as 8-bit RGB components, for the LED. */
void hr_ui_phase_rgb(hr_phase_t p, uint8_t *r, uint8_t *g, uint8_t *b);

/* RGB565 helpers and the palette. */
#define HR_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define HR_UI_C_BLACK   HR_RGB565(0x00, 0x00, 0x00)
#define HR_UI_C_WHITE   HR_RGB565(0xFF, 0xFF, 0xFF)
#define HR_UI_C_GREY    HR_RGB565(0x80, 0x80, 0x80)
#define HR_UI_C_DKGREY  HR_RGB565(0x30, 0x30, 0x30)
#define HR_UI_C_RED     HR_RGB565(0xFF, 0x30, 0x30)
#define HR_UI_C_GREEN   HR_RGB565(0x30, 0xE0, 0x60)
#define HR_UI_C_YELLOW  HR_RGB565(0xFF, 0xD0, 0x20)
#define HR_UI_C_PREP    HR_RGB565(0x40, 0xC0, 0xFF)
#define HR_UI_C_FREEZE  HR_RGB565(0x30, 0x60, 0xFF)
#define HR_UI_C_DRY     HR_RGB565(0xFF, 0x90, 0x20)
#define HR_UI_C_FINAL   HR_RGB565(0xFF, 0x50, 0x20)
#define HR_UI_C_IDLE    HR_RGB565(0x60, 0xA0, 0x80)

#ifdef __cplusplus
}
#endif

#endif /* HR_UI_MODEL_H */
