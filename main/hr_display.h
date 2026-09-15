/*
 * Status display for the T-Dongle-S3: owns the hr_ui task, the LED and the
 * button, and renders the screens decided by components/hr_ui.
 *
 * Threading: callers only COPY into a mutex-guarded model here. Drawing
 * happens on the hr_ui task alone - never on the TinyUSB RX task that
 * delivers telemetry, never on httpd. This is the same rule hr_capture
 * follows and for the same reason (the TinyUSB stack is small).
 *
 * Output only. Nothing in this module or the ones it drives (hr_gfx,
 * hr_display_hw, hr_led, hr_button) holds a session pointer or a transmit
 * callback; there is no way from here to the dryer.
 */
#ifndef HR_DISPLAY_H
#define HR_DISPLAY_H

#include "hr_telemetry.h"
#include "hr_ui_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Everything the main loop knows that the screen shows. Posted every tick. */
typedef struct {
    hr_ui_wifi_t wifi;
    char ssid[33];
    char ip[16];
    char ap_ssid[33];
    int  rssi_dbm;
    long ap_remaining_s;

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
    char fw_version[24];
    char reset_reason[12];
    unsigned heap_free;
    bool temp_metric;        /* show temperatures in Celsius (hr_units.h) */
} hr_display_status_t;

/*
 * Bring up panel, LED and button and start the hr_ui task. Call AFTER
 * hr_usb_init() and hr_capture_init(): the SPI DMA buffers come from the
 * same pool TinyUSB takes its endpoint memory from, and TinyUSB must win.
 * Safe to call when the panel is absent - the task then drives LED and
 * button only.
 */
void hr_display_init(void);

/*
 * New decoded STAT. Called from the frame observer on the USB task: copies
 * the fields under the lock, flags a redraw and an LED pulse, returns.
 * `phase` and `freeze_eta_s` come from the caller's phase tracker;
 * `last_stat` is the raw frame body for the RAW screen (may be NULL).
 */
void hr_display_post_telemetry(const hr_telemetry_t *t, hr_phase_t phase,
                               long freeze_eta_s, const char *last_stat);

/* Connection / counter snapshot from the main loop, every 250 ms. */
void hr_display_post_status(const hr_display_status_t *s);

#endif /* HR_DISPLAY_H */
