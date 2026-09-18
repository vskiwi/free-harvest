/*
 * hr_unitsync - see hr_unitsync.h.
 *
 * Everything here runs on the main loop (or the httpd task for a manual
 * sync); the only shared state with the USB RX task is inside hr_dryerfiles,
 * which locks its own. The small state below is therefore guarded by a mutex
 * of its own only because the httpd task may call hr_unitsync_now()/tick()
 * while the main loop ticks.
 */
#include "hr_unitsync.h"

#include "hr_batchstore.h"   /* hr_time_known / hr_time_now */
#include "hr_capture.h"
#include "hr_temp.h"
#include "hr_units.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "hr_unitsync";

#define UNIT_FILE      "HRTempFC.txt"
#define UNIT_RETRY_MS  30000UL   /* one more try after a failed read */
#define UNIT_ATTEMPTS  2
#define UNIT_READ_MS   12000UL   /* past the client's own 3 x 3 s retries:
                                    a read that has not ended by then failed */
#define UNIT_BUSY_MS   60000UL   /* how long a due sync waits behind the page */
#define UNIT_REC_MAX   64        /* the record is 11 bytes; anything bigger
                                    is not it */

static SemaphoreHandle_t s_lock;
static struct {
    unsigned long due_ms;     /* 0 = nothing scheduled */
    unsigned long first_due_ms;
    const char *why;
    int attempts;
    bool reading;             /* hr_dryerfiles is reading UNIT_FILE for us */
    unsigned long started_ms;
    char buf[UNIT_REC_MAX + 1];
    size_t len;
    bool overflow;
    unsigned long syncs, fails;
} s;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static unsigned long now_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000);
}

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/* Start the read. Lock held. */
static hr_df_result_t start_locked(void)
{
    hr_df_result_t r = hr_dryerfiles_read(UNIT_FILE, true);
    if (r != HR_DF_OK) {
        return r;
    }
    s.reading = true;
    s.started_ms = now_ms();
    s.len = 0;
    s.buf[0] = '\0';
    s.overflow = false;
    s.due_ms = 0;
    s.attempts++;
    ESP_LOGI(TAG, "reading %s off the dryer (%s, attempt %d)", UNIT_FILE,
             s.why ? s.why : "?", s.attempts);
    hr_capture_event("unit sync read %s (%s, attempt %d)", UNIT_FILE,
                     s.why ? s.why : "?", s.attempts);
    return HR_DF_OK;
}

/* Take what has arrived; true once the transfer has ended. Lock held. */
static bool drain_locked(void)
{
    bool more = true;
    for (int i = 0; i < 4 && more; i++) {
        char tmp[256];
        size_t n = hr_dryerfiles_take(tmp, sizeof(tmp), &more);
        if (n == 0) {
            break;
        }
        size_t room = sizeof(s.buf) - 1 - s.len;
        if (n > room) {
            s.overflow = true;
            n = room;
        }
        memcpy(s.buf + s.len, tmp, n);
        s.len += n;
        s.buf[s.len] = '\0';
    }
    return !more;
}

/* The read ended: parse, apply, log. Lock NOT held (NVS write). */
static void finish(bool ok_transfer, hr_files_err_t err)
{
    char rec[UNIT_REC_MAX + 1];
    size_t len;
    bool overflow;
    LOCK();
    memcpy(rec, s.buf, sizeof(rec));
    len = s.len;
    overflow = s.overflow;
    s.reading = false;
    UNLOCK();

    int parsed = HR_TEMP_DRYER_UNKNOWN;
    if (ok_transfer && !overflow) {
        parsed = hr_tempfc_parse(rec, len);
    }
    /* printable copy of the record for the log */
    for (char *p = rec; *p; p++) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7f) {
            *p = '.';
        }
    }
    if (parsed != HR_TEMP_DRYER_UNKNOWN) {
        uint32_t epoch = hr_time_known() ? hr_time_now() : 0;
        hr_units_set_dryer_unit(parsed, epoch);
        hr_capture_event("unit sync ok %s -> %s", rec,
                         parsed == HR_TEMP_C ? "C" : "F");
        LOCK();
        s.syncs++;
        s.attempts = 0;
        UNLOCK();
        return;
    }
    LOCK();
    s.fails++;
    const bool retry = s.attempts < UNIT_ATTEMPTS;
    if (retry) {
        s.due_ms = now_ms() + UNIT_RETRY_MS;
        s.first_due_ms = s.due_ms;
    }
    UNLOCK();
    ESP_LOGW(TAG, "panel unit sync failed (%s%s): record \"%s\"%s",
             ok_transfer ? "unparsed" : hr_files_err_str(err),
             overflow ? ", too long" : "", rec,
             retry ? "; retry in 30 s" : "");
    hr_capture_event("unit sync failed %s \"%s\"",
                     ok_transfer ? "unparsed" : hr_files_err_str(err), rec);
}

void hr_unitsync_schedule(unsigned long delay_ms, const char *why)
{
    ensure_lock();
    LOCK();
    unsigned long due = now_ms() + delay_ms;
    if (due == 0) {
        due = 1;
    }
    s.due_ms = due;
    s.first_due_ms = due;
    s.attempts = 0;
    s.why = why;
    UNLOCK();
    ESP_LOGI(TAG, "panel unit sync in %lu ms (%s)", delay_ms, why ? why : "");
}

hr_df_result_t hr_unitsync_now(void)
{
    ensure_lock();
    if (!hr_dryerfiles_enabled()) {
        return HR_DF_DISABLED;
    }
    LOCK();
    hr_df_result_t r;
    if (s.reading) {
        r = HR_DF_BUSY;
    } else {
        s.attempts = 0;
        s.why = "manual";
        r = start_locked();
    }
    UNLOCK();
    return r;
}

void hr_unitsync_tick(bool link_up)
{
    ensure_lock();
    bool ended = false, ok = false;
    hr_files_err_t err = HR_FILES_OK;

    LOCK();
    const unsigned long now = now_ms();
    if (!link_up) {
        s.due_ms = 0;            /* the next link-up schedules its own */
        if (s.reading) {
            ended = true;        /* hr_dryerfiles ends it too; report it */
            err = HR_FILES_ERR_LINK;
        }
    } else if (s.reading) {
        const bool done = drain_locked();
        hr_df_snapshot_t snap;
        hr_dryerfiles_snapshot(&snap);
        /* Is the client still on OUR file? A cancel from the page, or the
         * page starting its own read after ours ended, both show here. */
        const bool ours = strcmp(snap.file, UNIT_FILE) == 0;
        if (done || !ours) {
            ended = true;
            ok = ours && snap.state == HR_FILES_DONE;
            err = ours ? snap.err : HR_FILES_ERR_CANCELLED;
        } else if (now - s.started_ms > UNIT_READ_MS) {
            ended = true;
            err = HR_FILES_ERR_TIMEOUT;
            hr_dryerfiles_cancel();
        }
    } else if (s.due_ms != 0 && (long)(now - s.due_ms) >= 0) {
        if (!hr_dryerfiles_enabled()) {
            s.due_ms = 0;        /* nothing to do until someone turns it on */
        } else {
            hr_df_result_t r = start_locked();
            if (r == HR_DF_BUSY) {
                /* the page is listing or reading: come back next tick, but
                 * not forever */
                if (now - s.first_due_ms > UNIT_BUSY_MS) {
                    ESP_LOGW(TAG, "file client busy for a minute; giving up "
                                  "on this panel-unit sync");
                    s.due_ms = 0;
                }
            } else if (r != HR_DF_OK) {
                ESP_LOGW(TAG, "panel unit sync not started: %s",
                         hr_df_result_str(r));
                s.due_ms = 0;
            }
        }
    }
    UNLOCK();

    if (ended) {
        finish(ok, err);
    }
}

bool hr_unitsync_busy(void)
{
    ensure_lock();
    LOCK();
    const bool b = s.reading || s.due_ms != 0;
    UNLOCK();
    return b;
}

void hr_unitsync_stats(hr_unitsync_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    ensure_lock();
    LOCK();
    out->state = s.reading ? "reading" : s.due_ms ? "due" : "idle";
    out->syncs = s.syncs;
    out->fails = s.fails;
    UNLOCK();
}
