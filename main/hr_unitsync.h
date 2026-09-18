/*
 * hr_unitsync - keep the adapter's screen in the unit the DRYER's panel shows.
 *
 * NOT IN UPSTREAM. Upstream 1.2.2 lets the browser ask the dryer for its
 * panel unit (Settings > Temperature unit > "Use the dryer's setting") and
 * keeps the answer in that browser. A board with a screen of its own
 * (T-Dongle-S3, hr_units "auto") needs the same answer on the adapter, and
 * without anyone clicking: after every link-up, when a batch starts, and on
 * request (POST /api/units/sync).
 *
 * The read itself is upstream's file client, used as any other caller of
 * hr_dryerfiles would: hr_dryerfiles_read("HRTempFC.txt") starts a
 * background transfer, and this module drains the ring from the main loop
 * with hr_dryerfiles_take() until the transfer ends, then parses the 11-byte
 * record (hr_tempfc_parse) and hands the unit to hr_units. One request on the
 * wire, ~100 ms, reads only. It shares the one transfer slot with the web
 * page: a listing or a read in progress makes the sync wait for the next
 * tick; while the sync runs, the page sees "busy" for that tenth of a second.
 *
 * The running-batch rule is deliberately bypassed (force): one 11-byte
 * request does not disturb a batch (measured on 6.0.644170, STAT cadence
 * unchanged), and the owner may well change the panel's unit while loading
 * trays - which is exactly when the screen should follow.
 */
#ifndef HR_UNITSYNC_H
#define HR_UNITSYNC_H

#include "hr_dryerfiles.h"

#include <stdbool.h>
#include <stddef.h>

/* Ask for a sync `delay_ms` from now (a later call replaces an earlier one).
 * `why` is for the log only and must be a string literal. */
void hr_unitsync_schedule(unsigned long delay_ms, const char *why);

/* Start a sync right now. HR_DF_OK if the request went out; otherwise why
 * not (HR_DF_BUSY: the file client is doing something else - try later). */
hr_df_result_t hr_unitsync_now(void);

/*
 * Drive the sync: start what is due, drain what has arrived, apply the
 * result. From the MAIN LOOP (it may write NVS via hr_units); also called
 * by the /api/units/sync handler while it waits. Call after
 * hr_dryerfiles_tick() with the same link state.
 */
void hr_unitsync_tick(bool link_up);

/* True while a sync is scheduled, in flight, or waiting to be applied. */
bool hr_unitsync_busy(void);

typedef struct {
    const char *state;       /* "idle" / "due" / "reading" */
    unsigned long syncs;     /* successful reads since boot */
    unsigned long fails;     /* reads that ended without a unit */
} hr_unitsync_stats_t;

void hr_unitsync_stats(hr_unitsync_stats_t *out);

#endif /* HR_UNITSYNC_H */
