/*
 * hr_dryerfiles - the dryer's own files (batch CSV logs) in the web panel.
 *
 * ESP-side glue around hr_files (components/hr_protocol): owns the transfer
 * state machine, the side buffer the stream collects FDFILEBLOCK frames in,
 * a one-block mailbox between the USB RX task and the main loop, the cached
 * copies on the capture partition, and the on/off switch in NVS.
 *
 * Threads:
 *   USB RX task   hr_dryerfiles_on_frame() / the block sink - copy only,
 *                 never flash (hr_capture.h explains why).
 *   main loop     hr_dryerfiles_tick() - sends requests, writes blocks to
 *                 SPIFFS, finishes files.
 *   httpd         refresh / fetch / cancel / snapshot / stream - start,
 *                 stop and read; the dryer is never spoken to from here.
 *
 * Safety: every request to the dryer is a read (FDFILES / FILEREAD, both
 * in the SAFE allow-list). They still go out only when the feature is
 * switched on, the link is up, and the dryer is NOT running a batch unless
 * the caller says `force` - a kilobyte a quarter-second on the USB link is
 * not something to try for the first time mid-run.
 *
 * Compiled only with CONFIG_HR_BATCH_HISTORY.
 */
#ifndef HR_DRYERFILES_H
#define HR_DRYERFILES_H

#include "hr_files.h"
#include "hr_session.h"

#include <stdbool.h>
#include <stddef.h>

/* Files kept on the capture partition, newest fetched last. */
#define HR_DF_CACHE_MAX 3
/* List entries remembered from the last FDFILES sweep. */
#define HR_DF_LIST_MAX  40

typedef enum {
    HR_DF_OK = 0,
    HR_DF_DISABLED,   /* switched off (settings) */
    HR_DF_BUSY,       /* a transfer is running */
    HR_DF_LINK,       /* no dryer link */
    HR_DF_RUNNING,    /* dryer is running a batch and force was not given */
    HR_DF_BADNAME,    /* not a name we will put on the wire */
    HR_DF_STORAGE,    /* capture partition unavailable or full */
    HR_DF_TOOBIG,     /* over HR_FILES_MAX_SIZE */
} hr_df_result_t;

typedef struct {
    char name[HR_FILES_NAME_MAX];
    long size;
    bool cached;
} hr_df_entry_t;

/* A consistent copy of everything the UI shows, taken under the lock. */
typedef struct {
    bool enabled;
    bool link_up;
    bool dryer_running;
    hr_files_state_t state;
    hr_files_err_t err;
    char name[HR_FILES_NAME_MAX];     /* file being read / last read */
    long block;
    long received;
    long size;
    int pct;
    unsigned long age_ms;             /* since the transfer finished; 0 if running */
    unsigned long list_age_ms;        /* since the list completed; -1 never */
    bool list_valid;
    unsigned nlist;
    hr_df_entry_t list[HR_DF_LIST_MAX];
    unsigned ncached;
    char cached[HR_DF_CACHE_MAX][HR_FILES_NAME_MAX];
    /* lifetime counters, for the log and the live probe */
    unsigned long requests, timeouts, blocks_ok, blocks_bad, transfers,
                  blocks_in, big_dropped;
} hr_df_snapshot_t;

void hr_dryerfiles_init(hr_session_t *s);

bool hr_dryerfiles_enabled(void);
bool hr_dryerfiles_set_enabled(bool on);   /* persists; false if NVS failed */

/* From the frame observer (USB RX task). Looks only at FDFILELIST. */
void hr_dryerfiles_on_frame(const hr_frame_t *f);

/* From the main loop. `dryer_running` is the phase tracker's verdict. */
void hr_dryerfiles_tick(unsigned long now_ms, bool link_up, bool dryer_running);

/* Ask the dryer for its file list (names containing `pattern`; NULL = ".csv"). */
hr_df_result_t hr_dryerfiles_refresh(const char *pattern, bool force);

/* Read `name` from the dryer into the cache. */
hr_df_result_t hr_dryerfiles_fetch(const char *name, bool force);

void hr_dryerfiles_cancel(void);

void hr_dryerfiles_snapshot(hr_df_snapshot_t *out);

/*
 * Cached copy of `name`: the path on the capture partition if the name is
 * acceptable and the file is complete, else false. The caller brackets its
 * reads with hr_capture_fs_enter()/leave().
 */
bool hr_dryerfiles_cached_path(const char *name, char *path, size_t cap);

/*
 * Stream the cached CSV as JSON series for the chart:
 *   {"name":..,"n":N,"cols":{...},"t":[minutes from first row],
 *    "shelf":[F],"room":[F],"top":[F],"bot":[F],"mtorr":[..],"htr":[0/1],
 *    "process":[".."]}
 * `write` is called with successive chunks; returns false to stop. Returns
 * false if the file is missing or has no header.
 */
typedef bool (*hr_df_write_fn)(const char *chunk, size_t n, void *user);
bool hr_dryerfiles_stream_json(const char *name, hr_df_write_fn write,
                               void *user);

const char *hr_df_result_str(hr_df_result_t r);

#endif /* HR_DRYERFILES_H */
